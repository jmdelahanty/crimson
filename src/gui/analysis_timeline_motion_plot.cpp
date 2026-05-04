#include "gui/analysis_timeline_motion_plot.h"

#include "gui/analysis_timeline_stimulus_context.h"
#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <iterator>
#include <optional>

namespace {

constexpr double kMmPerPlotUnit = 10.0;

std::optional<double> frameToTime(
    int32_t frame,
    const std::vector<int32_t>& frame_indices,
    const std::vector<float>& time_data,
    double video_fps) {
    if (frame < 0) {
        return std::nullopt;
    }
    if (!frame_indices.empty()) {
        auto upper = std::lower_bound(frame_indices.begin(),
                                      frame_indices.end(),
                                      frame);
        if (upper != frame_indices.end() && *upper == frame) {
            const size_t idx =
                static_cast<size_t>(std::distance(frame_indices.begin(), upper));
            if (idx < time_data.size()) {
                return static_cast<double>(time_data[idx]);
            }
        }
        if (upper != frame_indices.end() && upper != frame_indices.begin()) {
            auto lower = upper - 1;
            const size_t lower_idx =
                static_cast<size_t>(std::distance(frame_indices.begin(), lower));
            const size_t upper_idx =
                static_cast<size_t>(std::distance(frame_indices.begin(), upper));
            if (lower_idx < time_data.size() && upper_idx < time_data.size()) {
                const int32_t f0 = *lower;
                const int32_t f1 = *upper;
                const double t0 = static_cast<double>(time_data[lower_idx]);
                const double t1 = static_cast<double>(time_data[upper_idx]);
                const int32_t delta_f = f1 - f0;
                if (delta_f != 0) {
                    const double alpha = static_cast<double>(frame - f0) /
                                         static_cast<double>(delta_f);
                    return t0 + alpha * (t1 - t0);
                }
            }
        } else if (upper == frame_indices.begin() && !time_data.empty()) {
            return static_cast<double>(time_data.front());
        } else if (upper == frame_indices.end() && !time_data.empty()) {
            return static_cast<double>(time_data.back());
        }
    }
    if (video_fps > 0.0) {
        return static_cast<double>(frame) / video_fps;
    }
    return std::nullopt;
}

double currentTimeLine(const AnalysisTimelineMotionPlotContext& context) {
    if (context.current_frame_num < 0) {
        return -1.0;
    }
    double resolved_time = -1.0;

    if (!context.frame_indices.empty()) {
        auto it = std::find(context.frame_indices.begin(),
                            context.frame_indices.end(),
                            context.current_frame_num);
        if (it != context.frame_indices.end()) {
            size_t idx = std::distance(context.frame_indices.begin(), it);
            if (idx < context.source_time_seconds.size()) {
                resolved_time =
                    static_cast<double>(context.source_time_seconds[idx]);
            }
        } else {
            auto upper = std::lower_bound(context.frame_indices.begin(),
                                          context.frame_indices.end(),
                                          context.current_frame_num);
            if (upper != context.frame_indices.end() &&
                upper != context.frame_indices.begin()) {
                auto lower = upper - 1;
                size_t lower_idx =
                    std::distance(context.frame_indices.begin(), lower);
                size_t upper_idx =
                    std::distance(context.frame_indices.begin(), upper);

                if (upper_idx < context.source_time_seconds.size() &&
                    lower_idx < context.source_time_seconds.size()) {
                    int32_t f0 = *lower;
                    int32_t f1 = *upper;
                    float t0 = context.source_time_seconds[lower_idx];
                    float t1 = context.source_time_seconds[upper_idx];
                    float delta_f = static_cast<float>(f1 - f0);
                    if (delta_f != 0.0f) {
                        float alpha = static_cast<float>(
                                          context.current_frame_num - f0) /
                                      delta_f;
                        resolved_time =
                            static_cast<double>(t0 + alpha * (t1 - t0));
                    }
                }
            } else if (upper == context.frame_indices.begin() &&
                       !context.source_time_seconds.empty()) {
                resolved_time =
                    static_cast<double>(context.source_time_seconds.front());
            } else if (upper == context.frame_indices.end() &&
                       !context.source_time_seconds.empty()) {
                resolved_time =
                    static_cast<double>(context.source_time_seconds.back());
            }
        }
    }

    if (resolved_time < 0.0 && context.video_fps > 0.0) {
        double estimated_time =
            static_cast<double>(context.current_frame_num) / context.video_fps;
        if (!context.source_time_seconds.empty()) {
            double min_time =
                static_cast<double>(context.source_time_seconds.front());
            double max_time =
                static_cast<double>(context.source_time_seconds.back());
            resolved_time = std::clamp(estimated_time, min_time, max_time);
        } else {
            resolved_time = estimated_time;
        }
    }

    return resolved_time;
}

void drawMotionCurrentTimeMarker(double current_time, const char* label) {
    if (current_time < 0.0) {
        return;
    }
    ImPlotRect limits = ImPlot::GetPlotLimits();
    double current_line_x[2] = {current_time, current_time};
    double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
    ImPlot::PlotLine(label, current_line_x, current_line_y, 2);
}

}  // namespace

double drawAnalysisTimelineMotionPlots(
    const AnalysisTimelineMotionPlotContext& context) {
    const double current_time_line = currentTimeLine(context);
    if (context.time_plot.empty()) {
        return current_time_line;
    }

    double time_axis_min = context.time_plot.front();
    double time_axis_max = context.time_plot.back();
    if (time_axis_max <= time_axis_min) {
        time_axis_max = time_axis_min + 0.5;
    }
    const bool has_time_span = time_axis_max > time_axis_min;
    const double half_span_seconds = static_cast<double>(
        std::max(0.1f, context.scroll_state.window_half_span_s));
    const bool use_time_window =
        context.scroll_state.enabled && current_time_line >= 0.0 &&
        has_time_span;
    double window_min = time_axis_min;
    double window_max = time_axis_max;
    if (use_time_window) {
        window_min =
            std::max(time_axis_min, current_time_line - half_span_seconds);
        window_max =
            std::min(time_axis_max, current_time_line + half_span_seconds);
        if (window_max - window_min < 0.1) {
            const double pad = std::max(0.1, half_span_seconds);
            window_min = std::max(time_axis_min, current_time_line - pad);
            window_max = std::min(time_axis_max, current_time_line + pad);
            if (window_max <= window_min) {
                window_min = std::max(time_axis_min, time_axis_max - pad);
                window_max = time_axis_max;
            }
        }
    }
    const bool reset_time_axis =
        (!context.scroll_state.enabled && context.scroll_state.prev_enabled);

    auto apply_time_axis_limits = [&](ImGuiCond fallback_cond) {
        if (use_time_window) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    window_min,
                                    window_max,
                                    ImGuiCond_Always);
        } else if (reset_time_axis) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    time_axis_min,
                                    time_axis_max,
                                    ImGuiCond_Always);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    time_axis_min,
                                    time_axis_max,
                                    fallback_cond);
        }
    };
    const bool draw_stimulus_context =
        context.show_stimulus_context && context.stimulus_loader != nullptr &&
        (context.stimulus_loader->hasStimulusSteps() ||
         context.stimulus_loader->hasStimulusEvents());
    const bool draw_distance_plot =
        context.state.show_distance_trace && !context.distance_time.empty();
    const size_t extra_row_count =
        context.extra_trace_rows != nullptr
            ? context.extra_trace_rows->size()
            : 0;
    std::vector<float> row_ratios;
    row_ratios.reserve((draw_stimulus_context ? 1 : 0) + 2 +
                       (draw_distance_plot ? 1 : 0) +
                       extra_row_count);
    if (draw_stimulus_context) {
        row_ratios.push_back(0.70f);
    }
    row_ratios.push_back(1.15f);
    row_ratios.push_back(1.05f);
    if (draw_distance_plot) {
        row_ratios.push_back(1.0f);
    }
    if (context.extra_trace_rows != nullptr) {
        for (const auto& row : *context.extra_trace_rows) {
            row_ratios.push_back(std::max(0.5f, row.row_weight));
        }
    }
    const int subplot_rows = static_cast<int>(row_ratios.size());
    const float subplot_height =
        210.0f * static_cast<float>(subplot_rows) -
        (draw_stimulus_context ? 70.0f : 0.0f);
    const AnalysisTimelineXAxisLimits shared_x_limits{
        true,
        use_time_window ? window_min : time_axis_min,
        use_time_window ? window_max : time_axis_max,
        use_time_window || reset_time_axis,
    };

    if (ImPlot::BeginSubplots("##analysis_timeline_plots",
                              subplot_rows,
                              1,
                              ImVec2(-1, subplot_height),
                              ImPlotSubplotFlags_LinkAllX |
                                  ImPlotSubplotFlags_NoTitle,
                              row_ratios.data())) {
        if (draw_stimulus_context) {
            drawAnalysisTimelineStimulusContext({
                *context.stimulus_loader,
                context.scroll_state,
                context.current_frame_num,
                context.video_fps,
                true,
                true,
                use_time_window ? window_min : time_axis_min,
                use_time_window ? window_max : time_axis_max,
                use_time_window || reset_time_axis,
            });
        }

        if (ImPlot::BeginPlot("##speed_plot")) {
            std::string speed_axis_label = "Speed";
            if (!context.primary_speed_units.empty() &&
                context.primary_speed_units == context.secondary_speed_units) {
                speed_axis_label += " (" + context.primary_speed_units + ")";
            }
            ImPlot::SetupAxes(nullptr, speed_axis_label.c_str());
            apply_time_axis_limits(ImGuiCond_Once);

            double max_speed = 0.0;
            if (context.state.show_smoothed && !context.smoothed_plot.empty()) {
                for (double value : context.smoothed_plot) {
                    if (std::isfinite(value)) {
                        max_speed = std::max(max_speed, value);
                    }
                }
            }
            if (context.state.show_instantaneous &&
                !context.instant_plot.empty()) {
                for (double value : context.instant_plot) {
                    if (std::isfinite(value)) {
                        max_speed = std::max(max_speed, value);
                    }
                }
            }
            if (!context.detector_value_plot.empty()) {
                for (double value : context.detector_value_plot) {
                    if (std::isfinite(value)) {
                        max_speed = std::max(max_speed, value);
                    }
                }
            }
            const double y_max_speed =
                (max_speed > 0.0) ? max_speed * 1.1 : 1.0;
            ImPlot::SetupAxisLimits(ImAxis_Y1,
                                    0.0,
                                    y_max_speed,
                                    ImGuiCond_Once);

            if (context.state.show_swim_bouts &&
                context.selected_swim_bouts != nullptr &&
                !context.selected_swim_bouts->start_frame.empty() &&
                context.selected_swim_bouts->start_frame.size() ==
                    context.selected_swim_bouts->end_frame.size()) {
                ImPlotRect limits = ImPlot::GetPlotLimits();
                ImDrawList* draw_list = ImPlot::GetPlotDrawList();
                const ImVec2 plot_pos = ImPlot::GetPlotPos();
                const ImVec2 plot_size = ImPlot::GetPlotSize();
                const ImVec2 clip_min = plot_pos;
                const ImVec2 clip_max(plot_pos.x + plot_size.x,
                                      plot_pos.y + plot_size.y);
                const ImU32 bout_fill =
                    ImGui::GetColorU32(ImVec4(0.15f, 0.95f, 0.45f, 0.16f));
                const ImU32 bout_core_fill =
                    ImGui::GetColorU32(ImVec4(0.15f, 0.95f, 0.45f, 0.24f));
                const size_t bout_count =
                    context.selected_swim_bouts->start_frame.size();
                draw_list->PushClipRect(clip_min, clip_max, true);
                for (size_t i = 0; i < bout_count; ++i) {
                    const auto start_time =
                        frameToTime(context.selected_swim_bouts->start_frame[i],
                                    context.frame_indices,
                                    context.source_time_seconds,
                                    context.video_fps);
                    const auto end_time =
                        frameToTime(context.selected_swim_bouts->end_frame[i],
                                    context.frame_indices,
                                    context.source_time_seconds,
                                    context.video_fps);
                    if (!start_time.has_value() || !end_time.has_value() ||
                        *end_time < limits.X.Min ||
                        *start_time > limits.X.Max) {
                        continue;
                    }
                    const double visible_start =
                        std::clamp(*start_time, limits.X.Min, limits.X.Max);
                    const double visible_end =
                        std::clamp(*end_time, limits.X.Min, limits.X.Max);
                    if (visible_end <= visible_start) {
                        continue;
                    }
                    const ImVec2 p0 = ImPlot::PlotToPixels(
                        ImPlotPoint(visible_start, limits.Y.Max));
                    const ImVec2 p1 = ImPlot::PlotToPixels(
                        ImPlotPoint(visible_end, limits.Y.Min));
                    draw_list->AddRectFilled(p0, p1, bout_fill, 0.0f);
                    if (i < context.selected_swim_bouts
                                ->core_start_frame.size() &&
                        i < context.selected_swim_bouts
                                ->core_end_frame.size()) {
                        auto core_start = frameToTime(
                            context.selected_swim_bouts->core_start_frame[i],
                            context.frame_indices,
                            context.source_time_seconds,
                            context.video_fps);
                        auto core_end = frameToTime(
                            context.selected_swim_bouts->core_end_frame[i],
                            context.frame_indices,
                            context.source_time_seconds,
                            context.video_fps);
                        if (core_start.has_value() && core_end.has_value()) {
                            if (*core_end < limits.X.Min ||
                                *core_start > limits.X.Max) {
                                continue;
                            }
                            const double visible_core_start =
                                std::clamp(*core_start,
                                           limits.X.Min,
                                           limits.X.Max);
                            const double visible_core_end =
                                std::clamp(*core_end,
                                           limits.X.Min,
                                           limits.X.Max);
                            if (visible_core_end <= visible_core_start) {
                                continue;
                            }
                            const ImVec2 c0 = ImPlot::PlotToPixels(
                                ImPlotPoint(visible_core_start,
                                            limits.Y.Max));
                            const ImVec2 c1 = ImPlot::PlotToPixels(
                                ImPlotPoint(visible_core_end,
                                            limits.Y.Min));
                            draw_list->AddRectFilled(c0, c1, bout_core_fill,
                                                     0.0f);
                        }
                    }
                }
                draw_list->PopClipRect();
            }

            if (context.state.show_smoothed &&
                !context.smoothed_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), 2.0f);
                ImPlot::PlotLine(context.primary_speed_label.c_str(),
                                 context.time_plot.data(),
                                 context.smoothed_plot.data(),
                                 static_cast<int>(context.time_plot.size()));
            }

            if (context.state.show_instantaneous &&
                !context.instant_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.5f, 0.2f, 0.6f), 1.0f);
                ImPlot::PlotLine(context.secondary_speed_label.c_str(),
                                 context.time_plot.data(),
                                 context.instant_plot.data(),
                                 static_cast<int>(context.time_plot.size()));
            }

            if (!context.detector_time_plot.empty() &&
                context.detector_time_plot.size() ==
                    context.detector_value_plot.size()) {
                std::string detector_label =
                    context.selected_swim_bouts &&
                            !context.selected_swim_bouts
                                 ->detector_trace_label.empty()
                        ? context.selected_swim_bouts->detector_trace_label
                        : "Detector response";
                if (context.selected_swim_bouts &&
                    !context.selected_swim_bouts
                         ->detector_trace_units.empty()) {
                    detector_label += " (" +
                                      context.selected_swim_bouts
                                          ->detector_trace_units +
                                      ", not physical speed)";
                } else {
                    detector_label += " (not physical speed)";
                }
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.85f, 0.2f, 0.85f),
                                         1.5f);
                ImPlot::PlotLine(
                    detector_label.c_str(),
                    context.detector_time_plot.data(),
                    context.detector_value_plot.data(),
                    static_cast<int>(context.detector_time_plot.size()));
            }

            drawMotionCurrentTimeMarker(current_time_line,
                                        "##current_time_speed");
            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("##heading_plot")) {
            ImPlot::SetupAxes(nullptr, "Heading (deg)");
            apply_time_axis_limits(ImGuiCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1,
                                    context.heading_axis_min,
                                    context.heading_axis_max,
                                    ImGuiCond_Once);

            bool drew_heading = false;
            if (context.state.show_heading_raw &&
                !context.heading_raw_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.35f, 0.2f, 0.9f), 1.5f);
                ImPlot::PlotLine(
                    "Heading (raw)",
                    context.heading_time_raw.data(),
                    context.heading_raw_plot.data(),
                    static_cast<int>(context.heading_raw_plot.size()));
                drew_heading = true;
            }
            if (context.state.show_heading_smoothed &&
                !context.heading_smoothed_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.7f, 0.4f, 1.0f, 1.0f), 2.0f);
                ImPlot::PlotLine(
                    "Heading (smoothed)",
                    context.heading_time_smoothed.data(),
                    context.heading_smoothed_plot.data(),
                    static_cast<int>(context.heading_smoothed_plot.size()));
                drew_heading = true;
            }

            const bool drew_per_second =
                context.state.show_heading_per_second &&
                !context.heading_per_second_plot.empty();
            if (drew_per_second) {
                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.8f, 0.8f, 1.0f), 2.0f);
                ImPlot::PlotLine(
                    "Heading (per-second)",
                    context.heading_per_second_time_plot.data(),
                    context.heading_per_second_plot.data(),
                    static_cast<int>(context.heading_per_second_plot.size()));
            }

            const bool drew_resultant =
                drew_per_second &&
                !context.heading_per_second_resultant_plot.empty();
            if (drew_resultant) {
                ImPlot::SetupAxis(ImAxis_Y2, "Resultant");
                ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 1.0, ImGuiCond_Once);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.6f, 0.6f, 0.7f), 1.5f);
                ImPlot::PlotLine(
                    "Resultant",
                    context.heading_per_second_time_plot.data(),
                    context.heading_per_second_resultant_plot.data(),
                    static_cast<int>(
                        context.heading_per_second_resultant_plot.size()));
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            }

            if (drew_heading || drew_per_second) {
                drawMotionCurrentTimeMarker(current_time_line,
                                            "##current_time_heading");
            }
            ImPlot::EndPlot();
        }

        if (draw_distance_plot && ImPlot::BeginPlot("##distance_plot")) {
            ImPlot::SetupAxes("Time (s)", "Distance (10 mm)");
            apply_time_axis_limits(ImGuiCond_Once);
            double y_max_units = (context.max_distance_mm > 0.0)
                                     ? (context.max_distance_mm * 1.1) /
                                           kMmPerPlotUnit
                                     : 1.0;
            if (y_max_units <= 0.0) {
                y_max_units = 1.0;
            }
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max_units, ImGuiCond_Once);

            if (!context.distance_time.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), 2.0f);
                ImPlot::PlotLine(
                    "Distance to Target (10 mm)",
                    context.distance_time.data(),
                    context.distance_units.data(),
                    static_cast<int>(context.distance_time.size()));
            }

            drawMotionCurrentTimeMarker(current_time_line,
                                        "##current_time_distance");
            ImPlot::EndPlot();
        }

        if (context.extra_trace_rows != nullptr) {
            for (const auto& row : *context.extra_trace_rows) {
                drawAnalysisTracePlotRow(row,
                                         context.scroll_state,
                                         &shared_x_limits,
                                         true);
            }
        }

        ImPlot::EndSubplots();
    }

    return current_time_line;
}
