#include "gui/analysis_timeline_window.h"

#include "gui/analysis_timeline_eye_angle.h"
#include "gui/analysis_timeline_motion_controls.h"
#include "gui/analysis_timeline_motion_data.h"
#include "gui/analysis_timeline_motion_plot.h"
#include "gui/analysis_timeline_motion_summary.h"
#include "gui/analysis_timeline_stimulus_context.h"
#include "gui/analysis_timeline_tail_kinematics.h"
#include "imgui.h"
#include "zarr_loader.h"

#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

std::optional<double> currentTimeSeconds(const AnalysisTimelineWindowContext& context) {
    if (context.current_frame_num < 0 || context.video_fps <= 0.0) {
        return std::nullopt;
    }
    return static_cast<double>(context.current_frame_num) / context.video_fps;
}

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

uint64_t traceRowPointCount(const AnalysisTimelineTracePlotRow& row) {
    uint64_t count = 0;
    for (const auto& trace : row.traces) {
        count += static_cast<uint64_t>(trace.xs.size());
    }
    return count;
}

void addPreparedTraceRowStats(AnalysisTimelinePerfStats* perf,
                              const AnalysisTimelineTracePlotRow& row,
                              uint64_t& bucket) {
    if (perf == nullptr) {
        return;
    }
    const uint64_t points = traceRowPointCount(row);
    bucket += points;
    perf->prepared_points_total += points;
    perf->prepared_traces += static_cast<uint32_t>(row.traces.size());
}

}  // namespace

void drawAnalysisTimelineWindow(const AnalysisTimelineWindowContext& context,
                                AnalysisTimelineWindowState& state) {
    if (!ImGui::Begin("Analysis Timeline")) {
        ImGui::End();
        return;
    }

    AnalysisTimelinePerfStats* perf = context.perf_stats;
    const auto source_selector_start = std::chrono::steady_clock::now();
    const auto* selected_series =
        drawAnalysisTimelineMotionSourceSelector(context.zarr_loader);
    if (perf != nullptr) {
        perf->source_selector_ms +=
            durationMs(std::chrono::steady_clock::now() -
                       source_selector_start);
    }

    const auto& time_data = context.zarr_loader.getMovementTimeSeconds();
    const auto& smoothed_speed =
        context.zarr_loader.getMovementSmoothedSpeedMm();
    const auto& instant_speed =
        context.zarr_loader.getMovementInstantaneousSpeedMm();
    const auto& distance_mm =
        context.zarr_loader.getMovementDistanceToTargetMm();
    const auto& heading_degrees =
        context.zarr_loader.getMovementHeadingDegrees();
    const auto& smoothed_heading_degrees =
        context.zarr_loader.getMovementSmoothedHeadingDegrees();
    const auto& heading_keypoint_success =
        context.zarr_loader.getMovementHeadingKeypointSuccess();
    const auto& heading_per_second_degrees =
        context.zarr_loader.getMovementHeadingPerSecondDegrees();
    const auto& heading_per_second_resultant =
        context.zarr_loader.getMovementHeadingPerSecondResultant();
    const auto& heading_per_second_time =
        context.zarr_loader.getMovementHeadingPerSecondTimeSeconds();
    const auto& frame_indices = context.zarr_loader.getMovementFrameIndices();
    AnalysisTimelineMotionSelection motion_selection;
    motion_selection.selected_series = selected_series;

    bool smoothed_available = !smoothed_speed.empty();
    bool instant_available = !instant_speed.empty();
    bool distance_available = !distance_mm.empty();
    bool heading_sample_available =
        !heading_degrees.empty() || !smoothed_heading_degrees.empty();
    bool heading_per_second_available =
        !heading_per_second_degrees.empty() &&
        !heading_per_second_time.empty() &&
        heading_per_second_degrees.size() == heading_per_second_time.size();
    const std::string primary_speed_label =
        context.zarr_loader.getMovementPrimarySpeedLabel();
    const std::string primary_speed_units =
        context.zarr_loader.getMovementPrimarySpeedUnits();
    const std::string secondary_speed_label =
        context.zarr_loader.getMovementSecondarySpeedLabel();
    const std::string secondary_speed_units =
        context.zarr_loader.getMovementSecondarySpeedUnits();

    const bool has_track_timeline =
        selected_series != nullptr && !time_data.empty() &&
        (smoothed_available || instant_available || distance_available ||
         heading_sample_available || heading_per_second_available);
    const bool has_eye_angle_timeline =
        context.zarr_loader.hasEyeAngleAnalysisData();
    const bool has_tail_kinematics_timeline =
        context.zarr_loader.hasTailKinematicsData();
    const bool has_stimulus_context =
        context.zarr_loader.hasStimulusSteps() ||
        context.zarr_loader.hasStimulusEvents();

    if (!has_track_timeline && !has_eye_angle_timeline &&
        !has_tail_kinematics_timeline && !has_stimulus_context) {
        ImGui::TextUnformatted("No analysis timeline data available.");
        ImGui::End();
        return;
    }

    {
        const auto controls_start = std::chrono::steady_clock::now();
        ImGui::BeginDisabled(!has_stimulus_context);
        ImGui::Checkbox("Show stimulus context", &state.show_stimulus_context);
        ImGui::EndDisabled();
        if (!has_stimulus_context && state.show_stimulus_context) {
            ImGui::TextDisabled("Stimulus context unavailable.");
        }
        if (perf != nullptr) {
            perf->controls_ms +=
                durationMs(std::chrono::steady_clock::now() - controls_start);
        }
    }

    const double fallback_current_time =
        currentTimeSeconds(context).value_or(-1.0);

    if (has_track_timeline) {
        const auto motion_controls_start = std::chrono::steady_clock::now();
        motion_selection = drawAnalysisTimelineMotionControls({
            context.zarr_loader,
            context.scroll_state,
            selected_series,
            time_data.size(),
            smoothed_available,
            instant_available,
            distance_available,
            heading_sample_available,
            heading_per_second_available,
            primary_speed_label,
            primary_speed_units,
            secondary_speed_label,
        }, state);
        if (perf != nullptr) {
            perf->controls_ms +=
                durationMs(std::chrono::steady_clock::now() -
                           motion_controls_start);
        }

        AnalysisTimelineMotionPreparedData motion_data;
        {
            const auto prepare_start = std::chrono::steady_clock::now();
            motion_data = prepareAnalysisTimelineMotionData({
                time_data,
                frame_indices,
                smoothed_speed,
                instant_speed,
                distance_mm,
                heading_degrees,
                smoothed_heading_degrees,
                heading_keypoint_success,
                heading_per_second_degrees,
                heading_per_second_resultant,
                heading_per_second_time,
                motion_selection.selected_swim_bouts,
                smoothed_available,
                instant_available,
                heading_per_second_available,
                state.show_detector_response,
                context.video_fps,
            });
            if (perf != nullptr) {
                perf->prepare_motion_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               prepare_start);
                const uint64_t motion_points =
                    static_cast<uint64_t>(motion_data.smoothed_plot.size()) +
                    static_cast<uint64_t>(motion_data.instant_plot.size()) +
                    static_cast<uint64_t>(
                        motion_data.detector_value_plot.size()) +
                    static_cast<uint64_t>(
                        motion_data.heading_raw_plot.size()) +
                    static_cast<uint64_t>(
                        motion_data.heading_smoothed_plot.size()) +
                    static_cast<uint64_t>(
                        motion_data.heading_per_second_plot.size()) +
                    static_cast<uint64_t>(
                        motion_data.heading_per_second_resultant_plot.size()) +
                    static_cast<uint64_t>(motion_data.distance_units.size());
                perf->motion_points_prepared += motion_points;
                perf->prepared_points_total += motion_points;
            }
        }
        std::vector<AnalysisTimelineTracePlotRow> linked_extra_rows;
        AnalysisTimelineMotionSummaryContext motion_summary_context{
            state,
            context.scroll_state,
            motion_selection.selected_series,
            time_data,
            smoothed_speed,
            motion_data,
            fallback_current_time,
            primary_speed_label,
            primary_speed_units,
        };
        const auto position_start = std::chrono::steady_clock::now();
        if (auto position_row = buildAnalysisTimelineTrackPositionRow(
                motion_summary_context)) {
            if (perf != nullptr) {
                addPreparedTraceRowStats(perf,
                                         *position_row,
                                         perf->position_points_prepared);
            }
            linked_extra_rows.push_back(std::move(*position_row));
        }
        if (perf != nullptr) {
            perf->build_position_ms +=
                durationMs(std::chrono::steady_clock::now() - position_start);
        }
        if (has_eye_angle_timeline) {
            AnalysisTimelineEyeAngleContext eye_context{
                context.zarr_loader,
                context.scroll_state,
                context.current_frame_num,
                context.video_fps,
                fallback_current_time,
                context.perf_stats,
            };
            const auto eye_controls_start = std::chrono::steady_clock::now();
            drawAnalysisTimelineEyeAngleControls(eye_context, state);
            if (perf != nullptr) {
                perf->controls_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               eye_controls_start);
            }
            const auto eye_build_start = std::chrono::steady_clock::now();
            if (auto eye_row = buildAnalysisTimelineEyeAngleRow(eye_context,
                                                                state)) {
                if (perf != nullptr) {
                    addPreparedTraceRowStats(perf,
                                             *eye_row,
                                             perf->eye_points_prepared);
                }
                linked_extra_rows.push_back(std::move(*eye_row));
            }
            if (perf != nullptr) {
                perf->build_eye_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               eye_build_start);
            }
        }
        if (has_tail_kinematics_timeline) {
            AnalysisTimelineTailKinematicsContext tail_context{
                context.zarr_loader,
                context.scroll_state,
                context.current_frame_num,
                context.video_fps,
                fallback_current_time,
                context.perf_stats,
            };
            const auto tail_controls_start = std::chrono::steady_clock::now();
            drawAnalysisTimelineTailKinematicsControls(tail_context, state);
            if (perf != nullptr) {
                perf->controls_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               tail_controls_start);
            }
            const auto tail_build_start = std::chrono::steady_clock::now();
            auto tail_rows =
                buildAnalysisTimelineTailKinematicsRows(tail_context, state);
            if (perf != nullptr) {
                for (const auto& row : tail_rows) {
                    addPreparedTraceRowStats(perf,
                                             row,
                                             perf->tail_points_prepared);
                }
                perf->build_tail_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               tail_build_start);
            }
            linked_extra_rows.insert(linked_extra_rows.end(),
                                     std::make_move_iterator(
                                         tail_rows.begin()),
                                     std::make_move_iterator(tail_rows.end()));
        }
        const double current_time_line = drawAnalysisTimelineMotionPlots({
            state,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            time_data,
            frame_indices,
            motion_data.time_plot,
            motion_data.smoothed_plot,
            motion_data.instant_plot,
            motion_data.detector_time_plot,
            motion_data.detector_value_plot,
            motion_data.heading_time_raw,
            motion_data.heading_raw_plot,
            motion_data.heading_time_smoothed,
            motion_data.heading_smoothed_plot,
            motion_data.heading_per_second_time_plot,
            motion_data.heading_per_second_plot,
            motion_data.heading_per_second_resultant_plot,
            motion_data.distance_time,
            motion_data.distance_units,
            motion_selection.selected_swim_bouts,
            primary_speed_label,
            primary_speed_units,
            secondary_speed_label,
            secondary_speed_units,
            motion_data.heading_axis_min,
            motion_data.heading_axis_max,
            motion_data.max_distance_mm,
            &context.zarr_loader,
            state.show_stimulus_context && has_stimulus_context,
            &linked_extra_rows,
            context.perf_stats,
        });
        const auto summary_start = std::chrono::steady_clock::now();
        drawAnalysisTimelineMotionSummary({
            state,
            context.scroll_state,
            motion_selection.selected_series,
            time_data,
            smoothed_speed,
            motion_data,
            current_time_line,
            primary_speed_label,
            primary_speed_units,
        });
        if (perf != nullptr) {
            perf->summary_ms +=
                durationMs(std::chrono::steady_clock::now() - summary_start);
        }
    } else {
        ImGui::TextDisabled("Track kinematics traces unavailable.");
        if (state.show_stimulus_context && has_stimulus_context) {
            const auto stimulus_start = std::chrono::steady_clock::now();
            drawAnalysisTimelineStimulusContext({
                context.zarr_loader,
                context.scroll_state,
                context.current_frame_num,
                context.video_fps,
            });
            if (perf != nullptr) {
                perf->draw_stimulus_context_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               stimulus_start);
            }
        }
    }

    if (!has_track_timeline && has_eye_angle_timeline) {
        const auto eye_start = std::chrono::steady_clock::now();
        drawAnalysisTimelineEyeAngleSection({
            context.zarr_loader,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            fallback_current_time,
            context.perf_stats,
        }, state);
        if (perf != nullptr) {
            perf->standalone_eye_ms +=
                durationMs(std::chrono::steady_clock::now() - eye_start);
        }
    }

    if (!has_track_timeline && has_tail_kinematics_timeline) {
        const auto tail_start = std::chrono::steady_clock::now();
        drawAnalysisTimelineTailKinematicsSection({
            context.zarr_loader,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            fallback_current_time,
            context.perf_stats,
        }, state);
        if (perf != nullptr) {
            perf->standalone_tail_ms +=
                durationMs(std::chrono::steady_clock::now() - tail_start);
        }
    }

    ImGui::End();
}
