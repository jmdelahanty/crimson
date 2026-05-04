#include "gui/analysis_timeline_trace_plot.h"

#include "implot.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr size_t kDirectPlotPointLimit = 4096;
constexpr double kEnvelopeBucketsPerPixel = 1.0;

bool finitePoint(double x, double y) {
    return std::isfinite(x) && std::isfinite(y);
}

void appendEnvelopePoint(std::vector<double>& lod_x,
                         std::vector<double>& lod_y,
                         double x,
                         double y) {
    if (!lod_x.empty() && lod_x.back() == x && lod_y.back() == y) {
        return;
    }
    lod_x.push_back(x);
    lod_y.push_back(y);
}

}  // namespace

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

uint64_t plotAnalysisTimelineLine(const char* label,
                                  const double* xs,
                                  const double* ys,
                                  size_t count) {
    if (label == nullptr || xs == nullptr || ys == nullptr || count == 0) {
        return 0;
    }

    ImPlotRect limits = ImPlot::GetPlotLimits();
    size_t begin = 0;
    size_t end = count;
    if (std::isfinite(limits.X.Min) && std::isfinite(limits.X.Max) &&
        limits.X.Max > limits.X.Min) {
        const double x_min = limits.X.Min;
        const double x_max = limits.X.Max;
        const double* lower = std::lower_bound(xs, xs + count, x_min);
        const double* upper = std::upper_bound(xs, xs + count, x_max);
        begin = static_cast<size_t>(lower - xs);
        end = static_cast<size_t>(upper - xs);
        if (begin > 0) {
            --begin;
        }
        if (end < count) {
            ++end;
        }
        if (end <= begin) {
            return 0;
        }
    }

    const size_t visible_count = end - begin;
    const float plot_width_px = ImPlot::GetPlotSize().x;
    const size_t bucket_count = plot_width_px > 1.0f
                                    ? std::max<size_t>(
                                          1,
                                          static_cast<size_t>(
                                              plot_width_px *
                                              kEnvelopeBucketsPerPixel))
                                    : kDirectPlotPointLimit;
    if (visible_count <= kDirectPlotPointLimit ||
        visible_count <= bucket_count * 2) {
        ImPlot::PlotLine(label,
                         xs + begin,
                         ys + begin,
                         static_cast<int>(visible_count));
        return static_cast<uint64_t>(visible_count);
    }

    thread_local std::vector<double> lod_x;
    thread_local std::vector<double> lod_y;
    lod_x.clear();
    lod_y.clear();
    lod_x.reserve(std::min(visible_count, bucket_count * 2 + 2));
    lod_y.reserve(std::min(visible_count, bucket_count * 2 + 2));

    const size_t bucket_size =
        std::max<size_t>(1, (visible_count + bucket_count - 1) / bucket_count);
    for (size_t bucket_begin = begin; bucket_begin < end;
         bucket_begin += bucket_size) {
        const size_t bucket_end = std::min(end, bucket_begin + bucket_size);
        size_t min_index = bucket_end;
        size_t max_index = bucket_end;
        double min_value = std::numeric_limits<double>::infinity();
        double max_value = -std::numeric_limits<double>::infinity();
        for (size_t idx = bucket_begin; idx < bucket_end; ++idx) {
            if (!finitePoint(xs[idx], ys[idx])) {
                continue;
            }
            if (ys[idx] < min_value) {
                min_value = ys[idx];
                min_index = idx;
            }
            if (ys[idx] > max_value) {
                max_value = ys[idx];
                max_index = idx;
            }
        }
        if (min_index == bucket_end || max_index == bucket_end) {
            continue;
        }
        if (min_index == max_index) {
            appendEnvelopePoint(lod_x, lod_y, xs[min_index], ys[min_index]);
        } else if (min_index < max_index) {
            appendEnvelopePoint(lod_x, lod_y, xs[min_index], ys[min_index]);
            appendEnvelopePoint(lod_x, lod_y, xs[max_index], ys[max_index]);
        } else {
            appendEnvelopePoint(lod_x, lod_y, xs[max_index], ys[max_index]);
            appendEnvelopePoint(lod_x, lod_y, xs[min_index], ys[min_index]);
        }
    }

    if (lod_x.empty()) {
        return 0;
    }
    ImPlot::PlotLine(label,
                     lod_x.data(),
                     lod_y.data(),
                     static_cast<int>(lod_x.size()));
    return static_cast<uint64_t>(lod_x.size());
}

bool drawAnalysisTracePlotRow(
    const AnalysisTimelineTracePlotRow& row,
    const TimelineScrollState& scroll_state,
    const AnalysisTimelineXAxisLimits* linked_x_limits,
    bool embedded_in_subplots,
    uint64_t* submitted_points_out) {
    if (submitted_points_out != nullptr) {
        *submitted_points_out = 0;
    }
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
            const uint64_t submitted =
                plotAnalysisTimelineLine(trace.label.c_str(),
                                         trace.xs.data(),
                                         trace.ys.data(),
                                         trace.xs.size());
            if (submitted_points_out != nullptr) {
                *submitted_points_out += submitted;
            }
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
