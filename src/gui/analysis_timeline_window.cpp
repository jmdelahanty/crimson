#include "gui/analysis_timeline_window.h"

#include "gui/analysis_timeline_eye_angle.h"
#include "gui/analysis_timeline_motion_controls.h"
#include "gui/analysis_timeline_motion_data.h"
#include "gui/analysis_timeline_motion_plot.h"
#include "gui/analysis_timeline_tail_kinematics.h"
#include "gui/analysis_timeline_trace_plot.h"
#include "imgui.h"
#include "zarr_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
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
        drawAnalysisTimelineMotionSourceSelector(context.zarr_loader);

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

    if (!has_track_timeline && !has_eye_angle_timeline &&
        !has_tail_kinematics_timeline) {
        ImGui::TextUnformatted("No analysis timeline data available.");
        ImGui::End();
        return;
    }

    if (has_track_timeline) {
    motion_selection = drawAnalysisTimelineMotionControls({
        context.zarr_loader,
        context.scroll_state,
        selected_series,
        time_data.size(),
        smoothed_available,
        instant_available,
        heading_sample_available,
        heading_per_second_available,
        primary_speed_label,
        primary_speed_units,
        secondary_speed_label,
    }, state);

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
        motion_selection.selected_swim_bouts,
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
        motion_selection.selected_swim_bouts,
        primary_speed_label,
        primary_speed_units,
        secondary_speed_label,
        secondary_speed_units,
        motion_data.heading_axis_min,
        motion_data.heading_axis_max,
        motion_data.max_distance_mm,
    });
    if (state.show_track_position &&
        motion_selection.selected_series != nullptr) {
        const bool use_mm_positions =
            !motion_selection.selected_series->positions_mm.empty();
        const auto& positions =
            use_mm_positions ? motion_selection.selected_series->positions_mm
                             : motion_selection.selected_series->positions_px;
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
