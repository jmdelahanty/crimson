#include "gui/analysis_timeline_trace_plot.h"

#include "implot.h"

#include <algorithm>
#include <cmath>
#include <limits>

std::optional<double> sampleTimeFromFrame(int32_t frame, double video_fps) {
    if (frame < 0 || video_fps <= 0.0) {
        return std::nullopt;
    }
    return static_cast<double>(frame) / video_fps;
}

void extendTraceRange(const AnalysisTimelineTrace& trace,
                      double& x_min,
                      double& x_max,
                      double& y_min,
                      double& y_max) {
    for (double value : trace.xs) {
        if (!std::isfinite(value)) {
            continue;
        }
        x_min = std::min(x_min, value);
        x_max = std::max(x_max, value);
    }
    for (double value : trace.ys) {
        if (!std::isfinite(value)) {
            continue;
        }
        y_min = std::min(y_min, value);
        y_max = std::max(y_max, value);
    }
}

void drawCurrentTimeMarker(double current_time, const char* label) {
    if (current_time < 0.0) {
        return;
    }
    ImPlotRect limits = ImPlot::GetPlotLimits();
    double current_line_x[2] = {current_time, current_time};
    double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
    ImPlot::PlotLine(label, current_line_x, current_line_y, 2);
}

bool drawAnalysisTracePlotRow(
    const AnalysisTimelineTracePlotRow& row,
    const TimelineScrollState& scroll_state,
    const AnalysisTimelineXAxisLimits* linked_x_limits,
    bool embedded_in_subplots) {
    if (row.traces.empty()) {
        if (!embedded_in_subplots) {
            ImGui::TextDisabled("%s unavailable", row.title.c_str());
        }
        return false;
    }

    double x_min = std::numeric_limits<double>::infinity();
    double x_max = -std::numeric_limits<double>::infinity();
    double y_min = std::numeric_limits<double>::infinity();
    double y_max = -std::numeric_limits<double>::infinity();
    for (const auto& trace : row.traces) {
        extendTraceRange(trace, x_min, x_max, y_min, y_max);
    }
    if (!std::isfinite(x_min) || !std::isfinite(x_max) ||
        !std::isfinite(y_min) || !std::isfinite(y_max)) {
        if (!embedded_in_subplots) {
            ImGui::TextDisabled("%s has no finite samples",
                                row.title.c_str());
        }
        return false;
    }
    if (x_min == x_max) {
        x_min -= 0.5;
        x_max += 0.5;
    }
    if (y_min == y_max) {
        y_min -= 1.0;
        y_max += 1.0;
    }
    const double y_span = std::max(1e-6, y_max - y_min);
    y_min -= y_span * 0.1;
    y_max += y_span * 0.1;

    if (ImPlot::BeginPlot(row.title.c_str(),
                          embedded_in_subplots ? ImVec2(-1.0f, 0.0f)
                                               : ImVec2(-1.0f, 230.0f))) {
        ImPlot::SetupAxes(embedded_in_subplots ? nullptr : "Time (s)",
                          row.y_axis_label.c_str());
        if (linked_x_limits != nullptr && linked_x_limits->valid &&
            linked_x_limits->max > linked_x_limits->min) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    linked_x_limits->min,
                                    linked_x_limits->max,
                                    linked_x_limits->force ? ImGuiCond_Always
                                                           : ImGuiCond_Once);
        } else if (scroll_state.enabled && row.current_time >= 0.0) {
            const double half_span = static_cast<double>(
                std::max(0.1f, scroll_state.window_half_span_s));
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    row.current_time - half_span,
                                    row.current_time + half_span,
                                    ImGuiCond_Always);
        } else if (!scroll_state.enabled && scroll_state.prev_enabled) {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max,
                                    ImGuiCond_Always);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max,
                                    ImGuiCond_Once);
        }
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Once);
        for (const auto& trace : row.traces) {
            if (trace.has_color) {
                ImPlot::SetNextLineStyle(trace.color, trace.line_width);
            }
            ImPlot::PlotLine(trace.label.c_str(),
                             trace.xs.data(),
                             trace.ys.data(),
                             static_cast<int>(trace.xs.size()));
        }
        drawCurrentTimeMarker(
            row.current_time,
            row.current_marker_id.empty() ? "##current_time"
                                          : row.current_marker_id.c_str());
        ImPlot::EndPlot();
    }
    return true;
}

void drawAnalysisTracePlot(const char* title,
                           const char* y_axis_label,
                           const std::vector<AnalysisTimelineTrace>& traces,
                           const TimelineScrollState& scroll_state,
                           double current_time,
                           const char* current_marker_id) {
    AnalysisTimelineTracePlotRow row;
    row.title = title != nullptr ? title : "Trace";
    row.y_axis_label = y_axis_label != nullptr ? y_axis_label : "";
    row.traces = traces;
    row.current_time = current_time;
    row.current_marker_id =
        current_marker_id != nullptr ? current_marker_id : "##current_time";
    drawAnalysisTracePlotRow(row, scroll_state, nullptr, false);
}
