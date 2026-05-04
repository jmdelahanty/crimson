#include "gui/analysis_timeline_tail_kinematics.h"

#include "gui/analysis_timeline_trace_plot.h"
#include "imgui.h"
#include "zarr_loader.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void appendTailTimelineTrace(
    const std::vector<float>& values,
    const std::vector<int32_t>& frame_index,
    double video_fps,
    const std::string& label,
    const std::string& units,
    std::vector<AnalysisTimelineTrace>& traces) {
    if (values.empty()) {
        return;
    }
    AnalysisTimelineTrace trace;
    trace.label = label;
    trace.units = units;
    trace.xs.reserve(values.size());
    trace.ys.reserve(values.size());
    for (size_t row = 0; row < values.size(); ++row) {
        const float value = values[row];
        if (!std::isfinite(value)) {
            continue;
        }
        std::optional<double> t;
        if (row < frame_index.size()) {
            t = sampleTimeFromFrame(frame_index[row], video_fps);
        } else {
            t = sampleTimeFromFrame(static_cast<int32_t>(row), video_fps);
        }
        if (!t.has_value()) {
            continue;
        }
        trace.xs.push_back(*t);
        trace.ys.push_back(static_cast<double>(value));
    }
    if (trace.xs.size() >= 2) {
        traces.push_back(std::move(trace));
    }
}

}  // namespace

void drawAnalysisTimelineTailKinematicsControls(
    const AnalysisTimelineTailKinematicsContext& context,
    AnalysisTimelineWindowState& state) {
    ImGui::SeparatorText("Tail-Kinematics Traces");
    const auto& tail = context.zarr_loader.getTailKinematicsData();
    ImGui::Text("Run: %s | Rows: %zu | Samples: %zu",
                tail.run_name.c_str(),
                tail.row_count,
                tail.sample_count);
    if (!tail.warning.empty()) {
        ImGui::TextWrapped("Warning: %s", tail.warning.c_str());
    }

    ImGui::Checkbox("Tail tip angle", &state.show_tail_tip_angle);
    ImGui::SameLine();
    ImGui::Checkbox("Tail tip lateral deflection",
                    &state.show_tail_tip_lateral_deflection);
    ImGui::SameLine();
    ImGui::Checkbox("Tail curvature", &state.show_tail_curvature);
}

std::vector<AnalysisTimelineTracePlotRow>
buildAnalysisTimelineTailKinematicsRows(
    const AnalysisTimelineTailKinematicsContext& context,
    AnalysisTimelineWindowState& state) {
    std::vector<AnalysisTimelineTracePlotRow> rows;
    const auto& tail = context.zarr_loader.getTailKinematicsData();

    const std::vector<int32_t>& tail_frame_index =
        !tail.frame_index.empty() ? tail.frame_index : tail.row_to_frame;
    double current_tail_time = context.fallback_current_time;
    if (auto current_row =
            context.zarr_loader.findTailKinematicsRowForFrame(
                context.current_frame_num)) {
        if (*current_row < tail_frame_index.size()) {
            current_tail_time =
                sampleTimeFromFrame(tail_frame_index[*current_row],
                                    context.video_fps)
                    .value_or(current_tail_time);
        }
    }

    if (state.show_tail_tip_angle) {
        std::vector<AnalysisTimelineTrace> traces;
        appendTailTimelineTrace(tail.tail_tip_angle_deg,
                                tail_frame_index,
                                context.video_fps,
                                "Tail Tip Angle",
                                "deg",
                                traces);
        if (!tail.max_abs_tail_angle_deg.empty()) {
            appendTailTimelineTrace(tail.max_abs_tail_angle_deg,
                                    tail_frame_index,
                                    context.video_fps,
                                    "Max Abs Tail Angle",
                                    "deg",
                                    traces);
        }
        if (!traces.empty()) {
            AnalysisTimelineTracePlotRow row;
            row.title = "Tail Angle";
            row.y_axis_label = "deg";
            row.traces = std::move(traces);
            row.current_time = current_tail_time;
            row.current_marker_id = "##current_time_tail_angle";
            row.row_weight = 1.0f;
            rows.push_back(std::move(row));
        }
    }
    if (state.show_tail_tip_lateral_deflection) {
        std::vector<AnalysisTimelineTrace> traces;
        appendTailTimelineTrace(tail.tail_tip_lateral_deflection_px,
                                tail_frame_index,
                                context.video_fps,
                                "Tail Tip Lateral Deflection",
                                "px",
                                traces);
        if (!traces.empty()) {
            AnalysisTimelineTracePlotRow row;
            row.title = "Tail Lateral Deflection";
            row.y_axis_label = "px";
            row.traces = std::move(traces);
            row.current_time = current_tail_time;
            row.current_marker_id = "##current_time_tail_deflection";
            row.row_weight = 1.0f;
            rows.push_back(std::move(row));
        }
    }
    if (state.show_tail_curvature) {
        std::vector<AnalysisTimelineTrace> traces;
        appendTailTimelineTrace(tail.max_abs_tail_curvature_px_inv,
                                tail_frame_index,
                                context.video_fps,
                                "Max Abs Tail Curvature",
                                "px^-1",
                                traces);
        if (!traces.empty()) {
            AnalysisTimelineTracePlotRow row;
            row.title = "Tail Curvature";
            row.y_axis_label = "px^-1";
            row.traces = std::move(traces);
            row.current_time = current_tail_time;
            row.current_marker_id = "##current_time_tail_curvature";
            row.row_weight = 1.0f;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

void drawAnalysisTimelineTailKinematicsSection(
    const AnalysisTimelineTailKinematicsContext& context,
    AnalysisTimelineWindowState& state) {
    drawAnalysisTimelineTailKinematicsControls(context, state);
    for (const auto& row :
         buildAnalysisTimelineTailKinematicsRows(context, state)) {
        drawAnalysisTracePlotRow(row, context.scroll_state);
    }
}
