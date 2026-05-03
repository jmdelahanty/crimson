#include "gui/analysis_timeline_window.h"

#include "gui/analysis_timeline_eye_angle.h"
#include "gui/analysis_timeline_motion_data.h"
#include "gui/analysis_timeline_motion_plot.h"
#include "gui/analysis_timeline_motion_sources.h"
#include "gui/analysis_timeline_tail_kinematics.h"
#include "gui/analysis_timeline_trace_plot.h"
#include "imgui.h"
#include "zarr_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

namespace {

std::optional<double> currentTimeSeconds(const AnalysisTimelineWindowContext& context) {
    if (context.current_frame_num < 0 || context.video_fps <= 0.0) {
        return std::nullopt;
    }
    return static_cast<double>(context.current_frame_num) / context.video_fps;
}

void appendTrackPositionTrace(
    const std::vector<std::array<float, 2>>& positions,
    const std::vector<float>& time_seconds,
    const std::string& axis_label,
    size_t axis_index,
    const ImVec4& color,
    std::vector<AnalysisTimelineTrace>& traces) {
    if (positions.empty() || time_seconds.empty() || axis_index > 1) {
        return;
    }
    const size_t count = std::min(positions.size(), time_seconds.size());
    AnalysisTimelineTrace trace;
    trace.label = axis_label;
    trace.has_color = true;
    trace.color = color;
    trace.xs.reserve(count);
    trace.ys.reserve(count);
    for (size_t row = 0; row < count; ++row) {
        const float t = time_seconds[row];
        const float value = positions[row][axis_index];
        if (!std::isfinite(t) || !std::isfinite(value)) {
            continue;
        }
        trace.xs.push_back(static_cast<double>(t));
        trace.ys.push_back(static_cast<double>(value));
    }
    if (trace.xs.size() >= 2) {
        traces.push_back(std::move(trace));
    }
}

}  // namespace

void drawAnalysisTimelineWindow(const AnalysisTimelineWindowContext& context,
                                AnalysisTimelineWindowState& state) {
    if (!ImGui::Begin("Analysis Timeline")) {
        ImGui::End();
        return;
    }

    const auto* selected_series =
        renderMovementDatasetUI(context.zarr_loader, "Track Kinematics Source");

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
    const auto& swim_bout_series = context.zarr_loader.getSwimBoutSeries();
    const auto& bout_kinematics_series =
        context.zarr_loader.getBoutKinematicsSeries();
    std::vector<size_t> compatible_swim_bout_indices;
    if (selected_series) {
        compatible_swim_bout_indices =
            findCompatibleSwimBoutIndices(*selected_series, swim_bout_series);
    }
    const auto* selected_swim_bouts =
        selected_series
            ? resolveSelectedSwimBoutSeries(swim_bout_series,
                                            compatible_swim_bout_indices,
                                            state)
            : nullptr;
    std::vector<size_t> compatible_bout_kinematics_indices;
    if (selected_series && selected_swim_bouts) {
        compatible_bout_kinematics_indices =
            findCompatibleBoutKinematicsIndices(*selected_series,
                                                selected_swim_bouts,
                                                bout_kinematics_series);
    }
    const auto* selected_bout_kinematics =
        resolveSelectedBoutKinematicsSeries(bout_kinematics_series,
                                            compatible_bout_kinematics_indices,
                                            state);

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

    if (!has_track_timeline && !has_eye_angle_timeline &&
        !has_tail_kinematics_timeline) {
        ImGui::TextUnformatted("No analysis timeline data available.");
        ImGui::End();
        return;
    }

    if (has_track_timeline) {
    if (!smoothed_available && state.show_smoothed) {
        state.show_smoothed = false;
    }
    if (!instant_available && state.show_instantaneous) {
        state.show_instantaneous = false;
    }
    if (!heading_sample_available) {
        state.show_heading_raw = false;
        state.show_heading_smoothed = false;
    }
    if (!heading_per_second_available) {
        state.show_heading_per_second = false;
    }

    if (!context.zarr_loader.getMovementCategory().empty()) {
        ImGui::Text("Track Kinematics: %s/%s | Track: %s",
                    context.zarr_loader.getMovementCategory().c_str(),
                    context.zarr_loader.getMovementRunName().c_str(),
                    context.zarr_loader.getMovementTrackId().c_str());
    } else {
        ImGui::Text("Track Kinematics: %s | Track: %s",
                    context.zarr_loader.getMovementRunName().c_str(),
                    context.zarr_loader.getMovementTrackId().c_str());
    }
    if (!context.zarr_loader.getMovementSpeedLevel().empty()) {
        ImGui::Text("Speed level: %s | Primary units: %s",
                    context.zarr_loader.getMovementSpeedLevel().c_str(),
                    primary_speed_units.c_str());
    }
    ImGui::Text("Data points: %zu", time_data.size());

    ImGui::SeparatorText("Derived Swim-Bout Candidate");
    if (compatible_swim_bout_indices.empty()) {
        ImGui::TextDisabled("No compatible swim-bout candidates for this track/speed.");
    } else {
        const std::string selected_label =
            selected_swim_bouts ? swimBoutCandidateLabel(*selected_swim_bouts)
                                : "Select candidate";
        if (ImGui::BeginCombo("Candidate##swim_bout_candidate",
                              selected_label.c_str())) {
            for (size_t index : compatible_swim_bout_indices) {
                const auto& candidate = swim_bout_series[index];
                const std::string label = swimBoutCandidateLabel(candidate);
                const bool is_selected =
                    selected_swim_bouts == &candidate;
                if (ImGui::Selectable(label.c_str(), is_selected)) {
                    state.selected_swim_bout_run = candidate.run_name;
                    state.selected_swim_bout_speed_level =
                        candidate.speed_level;
                    state.selected_bout_kinematics_run.clear();
                    selected_swim_bouts = &candidate;
                    compatible_bout_kinematics_indices =
                        findCompatibleBoutKinematicsIndices(
                            *selected_series,
                            selected_swim_bouts,
                            bout_kinematics_series);
                    selected_bout_kinematics =
                        resolveSelectedBoutKinematicsSeries(
                            bout_kinematics_series,
                            compatible_bout_kinematics_indices,
                            state);
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (selected_swim_bouts) {
            ImGui::Text("Compatible candidates: %zu | Selected bouts: %zu",
                        compatible_swim_bout_indices.size(),
                        selected_swim_bouts->start_frame.size());
            if (!selected_swim_bouts->detection_signal_source_level.empty() ||
                !selected_swim_bouts->path_distance_source_level.empty()) {
                ImGui::Text("Detector source: %s | Physical metrics: %s",
                            selected_swim_bouts
                                    ->detection_signal_source_level.empty()
                                ? "direct speed level"
                                : selected_swim_bouts
                                      ->detection_signal_source_level.c_str(),
                            selected_swim_bouts
                                    ->path_distance_source_level.empty()
                                ? "candidate bouts"
                                : selected_swim_bouts
                                      ->path_distance_source_level.c_str());
            }
        }
    }

    ImGui::SeparatorText("Bout-Kinematics Candidate");
    if (selected_swim_bouts == nullptr ||
        compatible_bout_kinematics_indices.empty()) {
        ImGui::TextDisabled("No linked bout-kinematics metrics for the selected candidate.");
    } else {
        const char* combo_preview =
            selected_bout_kinematics
                ? selected_bout_kinematics->run_name.c_str()
                : "Select bout-kinematics run";
        if (ImGui::BeginCombo("Candidate##bout_kinematics_candidate",
                              combo_preview)) {
            for (size_t index : compatible_bout_kinematics_indices) {
                const auto& candidate = bout_kinematics_series[index];
                const bool is_selected =
                    selected_bout_kinematics == &candidate;
                std::ostringstream label;
                label << candidate.run_name;
                if (candidate.metrics_loaded) {
                    label << " (" << candidate.physical_active_duration_s.size()
                          << " bouts)";
                } else {
                    label << " (details not loaded)";
                }
                if (ImGui::Selectable(label.str().c_str(), is_selected)) {
                    state.selected_bout_kinematics_run = candidate.run_name;
                    selected_bout_kinematics = &candidate;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (selected_bout_kinematics) {
            if (!selected_bout_kinematics->metrics_loaded) {
                ImGui::TextDisabled(
                    "Per-bout physical metrics are deferred to keep startup responsive.");
                if (selected_bout_kinematics->metrics_load_failed &&
                    !selected_bout_kinematics->metrics_load_error.empty()) {
                    ImGui::TextWrapped("Load failed: %s",
                                       selected_bout_kinematics
                                           ->metrics_load_error.c_str());
                }
                if (ImGui::Button("Load Bout Metrics")) {
                    std::string error_message;
                    context.zarr_loader.ensureBoutKinematicsMetricsLoaded(
                        selected_bout_kinematics->run_name,
                        &error_message);
                }
            } else {
                const auto* valid_mask =
                    selected_bout_kinematics->physical_active_valid.empty()
                        ? nullptr
                        : &selected_bout_kinematics->physical_active_valid;
                const double mean_duration =
                    computeFiniteMean(
                        selected_bout_kinematics->physical_active_duration_s,
                        valid_mask);
                const double mean_path =
                    computeFiniteMean(
                        selected_bout_kinematics
                            ->physical_active_path_length_mm,
                        valid_mask);
                const double mean_speed =
                    computeFiniteMean(
                        selected_bout_kinematics
                            ->physical_active_mean_speed_mm_s,
                        valid_mask);
                const size_t valid_physical =
                    selected_bout_kinematics->physical_active_valid.empty()
                        ? selected_bout_kinematics
                              ->physical_active_duration_s.size()
                        : countValidMaskValues(
                              selected_bout_kinematics
                                  ->physical_active_valid);
                ImGui::Text("Physical-active valid: %zu/%zu",
                            valid_physical,
                            selected_bout_kinematics
                                ->physical_active_duration_s.size());
                if (std::isfinite(mean_duration) || std::isfinite(mean_path) ||
                    std::isfinite(mean_speed)) {
                    ImGui::Text("Mean duration %.3fs | path %.3f mm | speed %.3f mm/s",
                                std::isfinite(mean_duration) ? mean_duration : 0.0,
                                std::isfinite(mean_path) ? mean_path : 0.0,
                                std::isfinite(mean_speed) ? mean_speed : 0.0);
                }
            }
        }
    }

    ImGui::Checkbox("Scrolling Window (+/-s)##analysis_timeline",
                    &context.scroll_state.enabled);
    if (context.scroll_state.enabled) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Half-span##analysis_timeline_window_span",
                         &context.scroll_state.window_half_span_s,
                         0.1f,
                         0.5f,
                         60.0f,
                         "%.1f s");
        context.scroll_state.window_half_span_s =
            std::max(0.1f, context.scroll_state.window_half_span_s);
    }

    ImGui::Checkbox(primary_speed_label.c_str(), &state.show_smoothed);
    ImGui::SameLine();
    ImGui::Checkbox(secondary_speed_label.c_str(),
                    &state.show_instantaneous);

    ImGui::SeparatorText("Plot Layers");
    ImGui::BeginDisabled(!heading_sample_available);
    ImGui::Checkbox("Show Raw Heading", &state.show_heading_raw);
    ImGui::SameLine();
    ImGui::Checkbox("Show Smoothed Heading", &state.show_heading_smoothed);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!heading_per_second_available);
    ImGui::Checkbox("Show Heading (per-second)",
                    &state.show_heading_per_second);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_swim_bouts == nullptr);
    ImGui::Checkbox("Show Swim Bouts", &state.show_swim_bouts);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_swim_bouts == nullptr ||
                         !selected_swim_bouts->has_detector_trace);
    ImGui::Checkbox("Show Detector Response",
                    &state.show_detector_response);
    ImGui::EndDisabled();
    ImGui::Checkbox("Show Track Position", &state.show_track_position);
    ImGui::BeginDisabled(!state.show_track_position);
    ImGui::SameLine();
    ImGui::Checkbox("X##track_position_x", &state.show_track_position_x);
    ImGui::SameLine();
    ImGui::Checkbox("Y##track_position_y", &state.show_track_position_y);
    ImGui::EndDisabled();

    const auto motion_data = prepareAnalysisTimelineMotionData({
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
        selected_swim_bouts,
        smoothed_available,
        instant_available,
        heading_per_second_available,
        state.show_detector_response,
        context.video_fps,
    });
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
        selected_swim_bouts,
        primary_speed_label,
        primary_speed_units,
        secondary_speed_label,
        secondary_speed_units,
        motion_data.heading_axis_min,
        motion_data.heading_axis_max,
        motion_data.max_distance_mm,
    });
    if (state.show_track_position && selected_series != nullptr) {
        const bool use_mm_positions = !selected_series->positions_mm.empty();
        const auto& positions = use_mm_positions ? selected_series->positions_mm
                                                 : selected_series->positions_px;
        const char* units = use_mm_positions ? "mm" : "px";
        std::vector<AnalysisTimelineTrace> position_traces;
        if (state.show_track_position_x) {
            appendTrackPositionTrace(positions,
                                     time_data,
                                     std::string("X position (") + units + ")",
                                     0,
                                     ImVec4(0.25f, 0.72f, 1.0f, 1.0f),
                                     position_traces);
        }
        if (state.show_track_position_y) {
            appendTrackPositionTrace(positions,
                                     time_data,
                                     std::string("Y position (") + units + ")",
                                     1,
                                     ImVec4(1.0f, 0.62f, 0.18f, 1.0f),
                                     position_traces);
        }
        const std::string position_axis_label =
            std::string("Position (") + units + ")";
        drawAnalysisTracePlot("Track Position",
                              position_axis_label.c_str(),
                              position_traces,
                              context.scroll_state,
                              current_time_line,
                              "##current_time_track_position");
    }

    ImGui::SeparatorText("Speed Statistics");
    const auto speed_stats = computePrimarySpeedStats(smoothed_speed);
    if (speed_stats.has_primary_speed_data) {
        if (speed_stats.finite_count > 0) {
            ImGui::BulletText("Average %s: %.2f %s",
                              primary_speed_label.c_str(),
                              speed_stats.average,
                              primary_speed_units.c_str());
            ImGui::BulletText("Max %s: %.2f %s",
                              primary_speed_label.c_str(),
                              speed_stats.max,
                              primary_speed_units.c_str());
        } else {
            ImGui::TextUnformatted("No finite primary speed samples.");
        }
    } else {
        ImGui::TextUnformatted("No primary speed data available.");
    }

    ImGui::SeparatorText("Distance Statistics");
    if (motion_data.valid_distance_count == 0) {
        ImGui::TextUnformatted("No valid distance samples available.");
    } else {
        ImGui::Text("Valid points: %zu / %zu",
                    motion_data.valid_distance_count,
                    motion_data.distance_samples);
        double avg_distance =
            motion_data.sum_distance_mm /
            static_cast<double>(motion_data.valid_distance_count);
        ImGui::BulletText("Average Distance: %.2f mm", avg_distance);
        ImGui::BulletText("Max Distance: %.2f mm",
                          motion_data.max_distance_mm);
        if (motion_data.min_distance_mm <
            std::numeric_limits<double>::infinity()) {
            ImGui::BulletText("Min Distance: %.2f mm",
                              motion_data.min_distance_mm);
        }
    }

    ImGui::SeparatorText("Heading Statistics");
    const auto heading_stats = computeHeadingStats(motion_data);
    if (heading_stats.valid) {
        ImGui::Text("Valid samples: %zu", heading_stats.count);
        ImGui::BulletText("Circular Mean: %.1f deg",
                          heading_stats.circular_mean_deg);
        ImGui::BulletText("Mean Resultant Length: %.2f",
                          heading_stats.mean_resultant_length);
    } else {
        ImGui::TextUnformatted("No valid heading samples available.");
    }
    if (heading_stats.per_second_resultant_count > 0) {
        ImGui::BulletText("Average per-second resultant: %.2f",
                          heading_stats.average_per_second_resultant);
    }
    } else {
        ImGui::TextDisabled("Track kinematics traces unavailable.");
    }

    const double fallback_current_time =
        currentTimeSeconds(context).value_or(-1.0);

    if (has_eye_angle_timeline) {
        drawAnalysisTimelineEyeAngleSection({
            context.zarr_loader,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            fallback_current_time,
        }, state);
    }

    if (has_tail_kinematics_timeline) {
        drawAnalysisTimelineTailKinematicsSection({
            context.zarr_loader,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            fallback_current_time,
        }, state);
    }

    ImGui::End();
}
