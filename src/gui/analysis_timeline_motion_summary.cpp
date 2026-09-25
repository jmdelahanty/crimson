#include "gui/analysis_timeline_motion_summary.h"

#include "gui/analysis_timeline_trace_plot.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

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

void drawSpeedStatistics(
    const AnalysisTimelineMotionSummaryContext& context) {
    ImGui::SeparatorText("Speed Statistics");
    const auto speed_stats = computePrimarySpeedStats(context.smoothed_speed);
    if (speed_stats.has_primary_speed_data) {
        if (speed_stats.finite_count > 0) {
            ImGui::BulletText("Average %s: %.2f %s",
                              context.primary_speed_label.c_str(),
                              speed_stats.average,
                              context.primary_speed_units.c_str());
            ImGui::BulletText("Max %s: %.2f %s",
                              context.primary_speed_label.c_str(),
                              speed_stats.max,
                              context.primary_speed_units.c_str());
        } else {
            ImGui::TextUnformatted("No finite primary speed samples.");
        }
    } else {
        ImGui::TextUnformatted("No primary speed data available.");
    }
}

void drawDistanceStatistics(
    const AnalysisTimelineMotionPreparedData& motion_data) {
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
}

void drawHeadingStatistics(
    const AnalysisTimelineMotionPreparedData& motion_data) {
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
}

}  // namespace

std::optional<AnalysisTimelineTracePlotRow> buildAnalysisTimelineTrackPositionRow(
    const AnalysisTimelineMotionSummaryContext& context) {
    if (!context.state.show_track_position ||
        context.selected_series == nullptr) {
        return std::nullopt;
    }

    const bool use_mm_positions =
        !context.selected_series->positions_mm.empty();
    const auto& positions = use_mm_positions
                                ? context.selected_series->positions_mm
                                : context.selected_series->positions_px;
    const char* units = use_mm_positions ? "mm" : "px";
    std::vector<AnalysisTimelineTrace> position_traces;
    if (context.state.show_track_position_x) {
        appendTrackPositionTrace(positions,
                                 context.time_data,
                                 std::string("X position (") + units + ")",
                                 0,
                                 ImVec4(0.25f, 0.72f, 1.0f, 1.0f),
                                 position_traces);
    }
    if (context.state.show_track_position_y) {
        appendTrackPositionTrace(positions,
                                 context.time_data,
                                 std::string("Y position (") + units + ")",
                                 1,
                                 ImVec4(1.0f, 0.62f, 0.18f, 1.0f),
                                 position_traces);
    }
    if (position_traces.empty()) {
        return std::nullopt;
    }
    AnalysisTimelineTracePlotRow row;
    row.title = "Track Position";
    row.y_axis_label = std::string("Position (") + units + ")";
    row.traces = std::move(position_traces);
    row.current_time = context.current_time_line;
    row.current_marker_id = "##current_time_track_position";
    row.row_weight = 1.0f;
    return row;
}

void drawAnalysisTimelineMotionSummary(
    const AnalysisTimelineMotionSummaryContext& context) {
    drawSpeedStatistics(context);
    if (context.state.show_distance_trace &&
        context.motion_data.valid_distance_count > 0) {
        drawDistanceStatistics(context.motion_data);
    }
    drawHeadingStatistics(context.motion_data);
}
