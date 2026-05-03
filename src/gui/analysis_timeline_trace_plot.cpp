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

void drawAnalysisTracePlot(const char* title,
                           const char* y_axis_label,
                           const std::vector<AnalysisTimelineTrace>& traces,
                           const TimelineScrollState& scroll_state,
                           double current_time,
                           const char* current_marker_id) {
    if (traces.empty()) {
        ImGui::TextDisabled("%s unavailable", title);
        return;
    }

    double x_min = std::numeric_limits<double>::infinity();
    double x_max = -std::numeric_limits<double>::infinity();
    double y_min = std::numeric_limits<double>::infinity();
    double y_max = -std::numeric_limits<double>::infinity();
    for (const auto& trace : traces) {
        extendTraceRange(trace, x_min, x_max, y_min, y_max);
    }
    if (!std::isfinite(x_min) || !std::isfinite(x_max) ||
        !std::isfinite(y_min) || !std::isfinite(y_max)) {
        ImGui::TextDisabled("%s has no finite samples", title);
        return;
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

    if (ImPlot::BeginPlot(title, ImVec2(-1.0f, 230.0f))) {
        ImPlot::SetupAxes("Time (s)", y_axis_label);
        if (scroll_state.enabled && current_time >= 0.0) {
            const double half_span = static_cast<double>(
                std::max(0.1f, scroll_state.window_half_span_s));
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    current_time - half_span,
                                    current_time + half_span,
                                    ImGuiCond_Always);
        } else if (!scroll_state.enabled && scroll_state.prev_enabled) {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max,
                                    ImGuiCond_Always);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max,
                                    ImGuiCond_Once);
        }
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Once);
        for (const auto& trace : traces) {
            if (trace.has_color) {
                ImPlot::SetNextLineStyle(trace.color, trace.line_width);
            }
            ImPlot::PlotLine(trace.label.c_str(),
                             trace.xs.data(),
                             trace.ys.data(),
                             static_cast<int>(trace.xs.size()));
        }
        drawCurrentTimeMarker(current_time, current_marker_id);
        ImPlot::EndPlot();
    }
}
