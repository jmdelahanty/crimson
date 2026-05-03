#include "gui/analysis_timeline_window.h"

#include "gui/analysis_timeline_eye_angle.h"
#include "gui/analysis_timeline_motion_controls.h"
#include "gui/analysis_timeline_motion_data.h"
#include "gui/analysis_timeline_motion_plot.h"
#include "gui/analysis_timeline_motion_summary.h"
#include "gui/analysis_timeline_tail_kinematics.h"
#include "imgui.h"
#include "zarr_loader.h"

#include <optional>
#include <string>

namespace {

std::optional<double> currentTimeSeconds(const AnalysisTimelineWindowContext& context) {
    if (context.current_frame_num < 0 || context.video_fps <= 0.0) {
        return std::nullopt;
    }
    return static_cast<double>(context.current_frame_num) / context.video_fps;
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
