#include "gui/movement_timeline_window.h"

#include "imgui.h"
#include "implot.h"
#include "ui_path_config.h"
#include "zarr_loader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <vector>

namespace {

const ZarrDetectionData::MovementSeries* renderMovementDatasetUI(
    ZarrDetectionLoader& zarr_loader,
    const char* combo_label) {
    size_t series_count = zarr_loader.getMovementSeriesCount();
    size_t selected_index = zarr_loader.getSelectedMovementSeriesIndex();
    const auto* selected_series = zarr_loader.getMovementSeries(selected_index);

    if (series_count > 1) {
        std::ostringstream summary;
        if (selected_series) {
            summary << selected_series->category << "/"
                    << selected_series->run_name << " (track "
                    << selected_series->track_id << ")";
        } else {
            summary << "Select dataset";
        }
        if (ImGui::BeginCombo(combo_label, summary.str().c_str())) {
            for (size_t i = 0; i < series_count; ++i) {
                const auto* series = zarr_loader.getMovementSeries(i);
                if (!series) {
                    continue;
                }
                std::ostringstream label;
                label << series->category << "/" << series->run_name
                      << " (track " << series->track_id << ")";
                bool is_selected = (i == selected_index);
                if (ImGui::Selectable(label.str().c_str(), is_selected)) {
                    if (zarr_loader.selectMovementSeries(i)) {
                        selected_index = i;
                        selected_series = zarr_loader.getMovementSeries(i);
                    }
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    } else if (selected_series) {
        ImGui::Text("Dataset: %s/%s (track %s)",
                    selected_series->category.c_str(),
                    selected_series->run_name.c_str(),
                    selected_series->track_id.c_str());
    }

    if (selected_series) {
        if (!selected_series->detection_variant.empty() ||
            !selected_series->source_detect_run.empty()) {
            if (!selected_series->source_detect_run.empty()) {
                ImGui::Text("Variant: %s | Source run: %s",
                            selected_series->detection_variant.empty()
                                ? "unknown"
                                : selected_series->detection_variant.c_str(),
                            selected_series->source_detect_run.c_str());
            } else {
                ImGui::Text("Variant: %s",
                            selected_series->detection_variant.empty()
                                ? "unknown"
                                : selected_series->detection_variant.c_str());
            }
        }
        if (selected_series->fps > 0.0 ||
            selected_series->smoothing_seconds > 0.0) {
            if (selected_series->fps > 0.0 &&
                selected_series->smoothing_seconds > 0.0) {
                ImGui::Text("FPS: %.2f | Smoothing: %.2f s",
                            selected_series->fps,
                            selected_series->smoothing_seconds);
            } else if (selected_series->fps > 0.0) {
                ImGui::Text("FPS: %.2f", selected_series->fps);
            } else {
                ImGui::Text("Smoothing: %.2f s",
                            selected_series->smoothing_seconds);
            }
        }
        if (selected_series->video_width > 0 &&
            selected_series->video_height > 0) {
            ImGui::Text("Camera size: %dx%d",
                        selected_series->video_width,
                        selected_series->video_height);
        }
    }

    return selected_series;
}

}  // namespace

void drawMovementTimelineWindow(const MovementTimelineWindowContext& context,
                                MovementTimelineWindowState& state) {
    if (!ImGui::Begin("Speed & Distance Timeline")) {
        ImGui::End();
        return;
    }

    const auto* selected_series =
        renderMovementDatasetUI(context.zarr_loader, "Dataset");

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

    bool smoothed_available = !smoothed_speed.empty();
    bool instant_available = !instant_speed.empty();
    bool distance_available = !distance_mm.empty();
    bool heading_sample_available =
        !heading_degrees.empty() || !smoothed_heading_degrees.empty();
    bool heading_per_second_available =
        !heading_per_second_degrees.empty() &&
        !heading_per_second_time.empty() &&
        heading_per_second_degrees.size() == heading_per_second_time.size();

    if (!selected_series || time_data.empty() ||
        (!smoothed_available && !instant_available && !distance_available &&
         !heading_sample_available && !heading_per_second_available)) {
        ImGui::TextUnformatted("No movement data available.");
        ImGui::End();
        return;
    }

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
        ImGui::Text("Movement Run: %s/%s | Track: %s",
                    context.zarr_loader.getMovementCategory().c_str(),
                    context.zarr_loader.getMovementRunName().c_str(),
                    context.zarr_loader.getMovementTrackId().c_str());
    } else {
        ImGui::Text("Movement Run: %s | Track: %s",
                    context.zarr_loader.getMovementRunName().c_str(),
                    context.zarr_loader.getMovementTrackId().c_str());
    }
    ImGui::Text("Data points: %zu", time_data.size());

    ImGui::Checkbox("Scrolling Window (±s)##movement",
                    &context.scroll_state.enabled);
    if (context.scroll_state.enabled) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Half-span##movement_window_span",
                         &context.scroll_state.window_half_span_s,
                         0.1f,
                         0.5f,
                         60.0f,
                         "%.1f s");
        context.scroll_state.window_half_span_s =
            std::max(0.1f, context.scroll_state.window_half_span_s);
    }

    ImGui::Checkbox("Show Smoothed Speed", &state.show_smoothed);
    ImGui::SameLine();
    ImGui::Checkbox("Show Instantaneous Speed", &state.show_instantaneous);

    ImGui::SeparatorText("Heading Options");
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
    ImGui::BeginDisabled(!context.zarr_loader.hasEyeVergenceFrame());
    ImGui::Checkbox("Show Vergence", &state.show_vergence);
    ImGui::EndDisabled();

    std::vector<double> time_plot;
    std::vector<double> smoothed_plot;
    std::vector<double> instant_plot;
    time_plot.reserve(time_data.size());
    smoothed_plot.reserve(smoothed_speed.size());
    if (!instant_speed.empty()) {
        instant_plot.reserve(instant_speed.size());
    }

    for (size_t i = 0; i < time_data.size(); ++i) {
        time_plot.push_back(static_cast<double>(time_data[i]));
        if (smoothed_available && i < smoothed_speed.size()) {
            smoothed_plot.push_back(static_cast<double>(smoothed_speed[i]));
        }
        if (instant_available && i < instant_speed.size()) {
            instant_plot.push_back(static_cast<double>(instant_speed[i]));
        }
    }

    constexpr double kMmPerPlotUnit = 10.0;
    std::vector<double> distance_time;
    std::vector<double> distance_units;
    double sum_distance = 0.0;
    double max_distance_mm = 0.0;
    double min_distance_mm = std::numeric_limits<double>::infinity();
    size_t valid_distance_count = 0;

    size_t distance_samples = std::min(time_data.size(), distance_mm.size());
    distance_time.reserve(distance_samples);
    distance_units.reserve(distance_samples);
    for (size_t i = 0; i < distance_samples; ++i) {
        float raw_distance = distance_mm[i];
        if (!IsFiniteFloat(raw_distance)) {
            continue;
        }
        double t = static_cast<double>(time_data[i]);
        double value_mm = static_cast<double>(raw_distance);
        distance_time.push_back(t);
        distance_units.push_back(value_mm / kMmPerPlotUnit);
        sum_distance += value_mm;
        max_distance_mm = std::max(max_distance_mm, value_mm);
        min_distance_mm = std::min(min_distance_mm, value_mm);
        ++valid_distance_count;
    }

    std::vector<double> heading_time_raw;
    std::vector<double> heading_raw_plot;
    std::vector<double> heading_time_smoothed;
    std::vector<double> heading_smoothed_plot;
    heading_time_raw.reserve(std::min(time_data.size(), heading_degrees.size()));
    heading_raw_plot.reserve(std::min(time_data.size(), heading_degrees.size()));
    heading_time_smoothed.reserve(
        std::min(time_data.size(), smoothed_heading_degrees.size()));
    heading_smoothed_plot.reserve(
        std::min(time_data.size(), smoothed_heading_degrees.size()));

    auto headingSampleAllowed = [&](size_t index) {
        if (!heading_keypoint_success.empty() &&
            index < heading_keypoint_success.size()) {
            return heading_keypoint_success[index] != 0;
        }
        return true;
    };

    double heading_y_min = std::numeric_limits<double>::infinity();
    double heading_y_max = -std::numeric_limits<double>::infinity();
    auto extend_heading_range = [&](double value) {
        heading_y_min = std::min(heading_y_min, value);
        heading_y_max = std::max(heading_y_max, value);
    };

    size_t raw_samples = std::min(time_data.size(), heading_degrees.size());
    for (size_t i = 0; i < raw_samples; ++i) {
        if (!headingSampleAllowed(i)) {
            continue;
        }
        float heading_raw_val = heading_degrees[i];
        if (!IsFiniteFloat(heading_raw_val)) {
            continue;
        }
        double t = static_cast<double>(time_data[i]);
        double v = static_cast<double>(heading_raw_val);
        heading_time_raw.push_back(t);
        heading_raw_plot.push_back(v);
        extend_heading_range(v);
    }

    size_t smoothed_samples =
        std::min(time_data.size(), smoothed_heading_degrees.size());
    for (size_t i = 0; i < smoothed_samples; ++i) {
        if (!headingSampleAllowed(i)) {
            continue;
        }
        float heading_smooth_val = smoothed_heading_degrees[i];
        if (!IsFiniteFloat(heading_smooth_val)) {
            continue;
        }
        double t = static_cast<double>(time_data[i]);
        double v = static_cast<double>(heading_smooth_val);
        heading_time_smoothed.push_back(t);
        heading_smoothed_plot.push_back(v);
        extend_heading_range(v);
    }

    std::vector<double> heading_per_second_time_plot;
    std::vector<double> heading_per_second_plot;
    std::vector<double> heading_per_second_resultant_plot;
    if (heading_per_second_available) {
        size_t per_samples =
            std::min(heading_per_second_degrees.size(),
                     heading_per_second_time.size());
        heading_per_second_time_plot.reserve(per_samples);
        heading_per_second_plot.reserve(per_samples);
        if (!heading_per_second_resultant.empty()) {
            heading_per_second_resultant_plot.reserve(per_samples);
        }
        for (size_t i = 0; i < per_samples; ++i) {
            float heading_val = heading_per_second_degrees[i];
            float heading_time_val = heading_per_second_time[i];
            if (!IsFiniteFloat(heading_val) ||
                !IsFiniteFloat(heading_time_val)) {
                continue;
            }
            double t = static_cast<double>(heading_time_val);
            double v = static_cast<double>(heading_val);
            heading_per_second_time_plot.push_back(t);
            heading_per_second_plot.push_back(v);
            extend_heading_range(v);
            if (!heading_per_second_resultant.empty() &&
                i < heading_per_second_resultant.size()) {
                float resultant = heading_per_second_resultant[i];
                if (IsFiniteFloat(resultant)) {
                    heading_per_second_resultant_plot.push_back(
                        static_cast<double>(resultant));
                } else {
                    heading_per_second_resultant_plot.push_back(
                        std::numeric_limits<double>::quiet_NaN());
                }
            }
        }
        if (!heading_per_second_resultant.empty() &&
            heading_per_second_resultant_plot.size() !=
                heading_per_second_plot.size()) {
            heading_per_second_resultant_plot.resize(
                heading_per_second_plot.size(),
                std::numeric_limits<double>::quiet_NaN());
        }
    }

    std::vector<double> vergence_time_plot;
    std::vector<double> vergence_value_plot;
    size_t vergence_valid_count = 0;
    double vergence_sum = 0.0;
    double vergence_min = std::numeric_limits<double>::infinity();
    double vergence_max = -std::numeric_limits<double>::infinity();
    if (context.zarr_loader.hasEyeVergenceFrame()) {
        const auto& verg_time =
            context.zarr_loader.getEyeVergenceFrameTimeSeconds();
        const auto& verg_values =
            context.zarr_loader.getEyeVergenceFrameSignedDeg();
        const auto& verg_valid =
            context.zarr_loader.getEyeVergenceFrameValidMask();
        size_t count = std::min(verg_time.size(), verg_values.size());
        vergence_time_plot.reserve(count);
        vergence_value_plot.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            double t = static_cast<double>(verg_time[i]);
            double value = static_cast<double>(verg_values[i]);
            bool valid = verg_valid.empty() ||
                         (i < verg_valid.size() && verg_valid[i] != 0);
            vergence_time_plot.push_back(t);
            if (valid && std::isfinite(value)) {
                vergence_value_plot.push_back(value);
                vergence_min = std::min(vergence_min, value);
                vergence_max = std::max(vergence_max, value);
                vergence_sum += value;
                ++vergence_valid_count;
            } else {
                vergence_value_plot.push_back(
                    std::numeric_limits<double>::quiet_NaN());
            }
        }
    }

    auto compute_heading_axis = [&](double& min_out, double& max_out) {
        if (heading_y_min == std::numeric_limits<double>::infinity() ||
            heading_y_max == -std::numeric_limits<double>::infinity()) {
            min_out = -180.0;
            max_out = 180.0;
        } else {
            min_out = heading_y_min;
            max_out = heading_y_max;
            double span = std::max(10.0, max_out - min_out);
            double padding = std::max(5.0, span * 0.1);
            min_out -= padding;
            max_out += padding;
            if (min_out >= max_out) {
                min_out -= 1.0;
                max_out += 1.0;
            }
        }
    };

    double vergence_axis_min = -60.0;
    double vergence_axis_max = 60.0;
    if (!vergence_time_plot.empty()) {
        if (vergence_min == std::numeric_limits<double>::infinity() ||
            vergence_max == -std::numeric_limits<double>::infinity()) {
            vergence_axis_min = -60.0;
            vergence_axis_max = 60.0;
        } else {
            double span = std::max(5.0, vergence_max - vergence_min);
            double padding = span * 0.1;
            vergence_axis_min = vergence_min - padding;
            vergence_axis_max = vergence_max + padding;
            if (vergence_axis_min >= vergence_axis_max) {
                vergence_axis_min -= 1.0;
                vergence_axis_max += 1.0;
            }
        }
    }

    auto computeCurrentTime = [&]() -> double {
        if (context.current_frame_num < 0) {
            return -1.0;
        }
        double resolved_time = -1.0;

        if (!frame_indices.empty()) {
            auto it = std::find(frame_indices.begin(),
                                frame_indices.end(),
                                context.current_frame_num);
            if (it != frame_indices.end()) {
                size_t idx = std::distance(frame_indices.begin(), it);
                if (idx < time_data.size()) {
                    resolved_time = static_cast<double>(time_data[idx]);
                }
            } else {
                auto upper =
                    std::lower_bound(frame_indices.begin(),
                                     frame_indices.end(),
                                     context.current_frame_num);
                if (upper != frame_indices.end() && upper != frame_indices.begin()) {
                    auto lower = upper - 1;
                    size_t lower_idx =
                        std::distance(frame_indices.begin(), lower);
                    size_t upper_idx =
                        std::distance(frame_indices.begin(), upper);

                    if (upper_idx < time_data.size() &&
                        lower_idx < time_data.size()) {
                        int32_t f0 = *lower;
                        int32_t f1 = *upper;
                        float t0 = time_data[lower_idx];
                        float t1 = time_data[upper_idx];
                        float delta_f = static_cast<float>(f1 - f0);
                        if (delta_f != 0.0f) {
                            float alpha = static_cast<float>(
                                              context.current_frame_num - f0) /
                                          delta_f;
                            resolved_time =
                                static_cast<double>(t0 + alpha * (t1 - t0));
                        }
                    }
                } else if (upper == frame_indices.begin() && !time_data.empty()) {
                    resolved_time = static_cast<double>(time_data.front());
                } else if (upper == frame_indices.end() && !time_data.empty()) {
                    resolved_time = static_cast<double>(time_data.back());
                }
            }
        }

        if (resolved_time < 0.0 && context.video_fps > 0.0) {
            double estimated_time =
                static_cast<double>(context.current_frame_num) /
                context.video_fps;
            if (!time_data.empty()) {
                double min_time = static_cast<double>(time_data.front());
                double max_time = static_cast<double>(time_data.back());
                resolved_time = std::clamp(estimated_time, min_time, max_time);
            } else {
                resolved_time = estimated_time;
            }
        }

        return resolved_time;
    };

    double current_time_line = computeCurrentTime();
    double time_axis_min = time_plot.front();
    double time_axis_max = time_plot.back();
    if (time_axis_max <= time_axis_min) {
        time_axis_max = time_axis_min + 0.5;
    }
    bool has_time_span = time_axis_max > time_axis_min;
    double half_span_seconds = static_cast<double>(std::max(
        0.1f, context.scroll_state.window_half_span_s));
    bool use_time_window =
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
            double pad = std::max(0.1, half_span_seconds);
            window_min = std::max(time_axis_min, current_time_line - pad);
            window_max = std::min(time_axis_max, current_time_line + pad);
            if (window_max <= window_min) {
                window_min = std::max(time_axis_min, time_axis_max - pad);
                window_max = time_axis_max;
            }
        }
    }
    bool reset_time_axis =
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

    ImVec2 subplot_size = ImVec2(-1, 920);
    if (!time_plot.empty() &&
        ImPlot::BeginSubplots("##movement_plots",
                              4,
                              1,
                              subplot_size,
                              ImPlotSubplotFlags_LinkAllX |
                                  ImPlotSubplotFlags_NoTitle)) {
        if (ImPlot::BeginPlot("##speed_plot")) {
            ImPlot::SetupAxes(nullptr, "Speed (mm/s)");
            apply_time_axis_limits(ImGuiCond_Once);

            double max_speed = 0.0;
            if (state.show_smoothed && !smoothed_plot.empty()) {
                max_speed = std::max(
                    max_speed,
                    *std::max_element(smoothed_plot.begin(), smoothed_plot.end()));
            }
            if (state.show_instantaneous && !instant_plot.empty()) {
                max_speed = std::max(
                    max_speed,
                    *std::max_element(instant_plot.begin(), instant_plot.end()));
            }
            double y_max_speed = (max_speed > 0.0) ? max_speed * 1.1 : 1.0;
            ImPlot::SetupAxisLimits(ImAxis_Y1,
                                    0.0,
                                    y_max_speed,
                                    ImGuiCond_Once);

            if (state.show_smoothed && !smoothed_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), 2.0f);
                ImPlot::PlotLine("Smoothed Speed",
                                 time_plot.data(),
                                 smoothed_plot.data(),
                                 static_cast<int>(time_plot.size()));
            }

            if (state.show_instantaneous && !instant_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.5f, 0.2f, 0.6f), 1.0f);
                ImPlot::PlotLine("Instantaneous Speed",
                                 time_plot.data(),
                                 instant_plot.data(),
                                 static_cast<int>(instant_plot.size()));
            }

            if (current_time_line >= 0.0) {
                ImPlotRect limits = ImPlot::GetPlotLimits();
                double current_line_x[2] = {current_time_line, current_time_line};
                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                ImPlot::PlotLine("##current_time_speed",
                                 current_line_x,
                                 current_line_y,
                                 2);
            }

            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("##heading_plot")) {
            ImPlot::SetupAxes(nullptr, "Heading (deg)");
            if (!time_plot.empty()) {
                apply_time_axis_limits(ImGuiCond_Once);
            }
            double heading_axis_min;
            double heading_axis_max;
            compute_heading_axis(heading_axis_min, heading_axis_max);
            ImPlot::SetupAxisLimits(ImAxis_Y1,
                                    heading_axis_min,
                                    heading_axis_max,
                                    ImGuiCond_Once);

            bool drew_heading = false;
            if (state.show_heading_raw && !heading_raw_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.35f, 0.2f, 0.9f), 1.5f);
                ImPlot::PlotLine("Heading (raw)",
                                 heading_time_raw.data(),
                                 heading_raw_plot.data(),
                                 static_cast<int>(heading_raw_plot.size()));
                drew_heading = true;
            }
            if (state.show_heading_smoothed && !heading_smoothed_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.7f, 0.4f, 1.0f, 1.0f), 2.0f);
                ImPlot::PlotLine("Heading (smoothed)",
                                 heading_time_smoothed.data(),
                                 heading_smoothed_plot.data(),
                                 static_cast<int>(heading_smoothed_plot.size()));
                drew_heading = true;
            }

            bool drew_per_second =
                state.show_heading_per_second &&
                !heading_per_second_plot.empty();
            if (drew_per_second) {
                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.8f, 0.8f, 1.0f), 2.0f);
                ImPlot::PlotLine("Heading (per-second)",
                                 heading_per_second_time_plot.data(),
                                 heading_per_second_plot.data(),
                                 static_cast<int>(heading_per_second_plot.size()));
            }

            bool drew_resultant =
                drew_per_second && !heading_per_second_resultant_plot.empty();
            if (drew_resultant) {
                ImPlot::SetupAxis(ImAxis_Y2, "Resultant");
                ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 1.0, ImGuiCond_Once);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.6f, 0.6f, 0.7f), 1.5f);
                ImPlot::PlotLine(
                    "Resultant",
                    heading_per_second_time_plot.data(),
                    heading_per_second_resultant_plot.data(),
                    static_cast<int>(heading_per_second_resultant_plot.size()));
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            }

            if ((drew_heading || drew_per_second) && current_time_line >= 0.0) {
                ImPlotRect limits = ImPlot::GetPlotLimits();
                double current_line_x[2] = {current_time_line, current_time_line};
                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                ImPlot::PlotLine("##current_time_heading",
                                 current_line_x,
                                 current_line_y,
                                 2);
            }

            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("##distance_plot")) {
            ImPlot::SetupAxes("Time (s)", "Distance (10 mm)");
            apply_time_axis_limits(ImGuiCond_Once);
            double y_max_units = (max_distance_mm > 0.0)
                                     ? (max_distance_mm * 1.1) / kMmPerPlotUnit
                                     : 1.0;
            if (y_max_units <= 0.0) {
                y_max_units = 1.0;
            }
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max_units, ImGuiCond_Once);

            if (!distance_time.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), 2.0f);
                ImPlot::PlotLine("Distance to Target (10 mm)",
                                 distance_time.data(),
                                 distance_units.data(),
                                 static_cast<int>(distance_time.size()));
            }

            if (current_time_line >= 0.0) {
                ImPlotRect limits = ImPlot::GetPlotLimits();
                double current_line_x[2] = {current_time_line, current_time_line};
                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                ImPlot::PlotLine("##current_time_distance",
                                 current_line_x,
                                 current_line_y,
                                 2);
            }

            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("##vergence_plot")) {
            ImPlot::SetupAxes(nullptr, "Vergence (deg)");
            if (!vergence_time_plot.empty()) {
                ImPlot::SetupAxisLimits(ImAxis_Y1,
                                        vergence_axis_min,
                                        vergence_axis_max,
                                        ImGuiCond_Once);
                if (state.show_vergence) {
                    ImVec4 vergence_color = ImVec4(0.85f, 0.2f, 0.7f, 1.0f);
                    ImPlot::SetNextLineStyle(vergence_color, 2.0f);
                    ImPlot::PlotLine("Vergence",
                                     vergence_time_plot.data(),
                                     vergence_value_plot.data(),
                                     static_cast<int>(vergence_time_plot.size()));
                }
            } else {
                ImGui::TextUnformatted("No vergence data available.");
            }
            ImPlot::EndPlot();
        }

        ImPlot::EndSubplots();
    }

    ImGui::SeparatorText("Speed Statistics");
    if (!smoothed_speed.empty()) {
        float avg_speed = std::accumulate(smoothed_speed.begin(),
                                          smoothed_speed.end(),
                                          0.0f) /
                          smoothed_speed.size();
        float max_spd = *std::max_element(smoothed_speed.begin(),
                                          smoothed_speed.end());
        ImGui::BulletText("Average Speed (smoothed): %.2f mm/s", avg_speed);
        ImGui::BulletText("Max Speed (smoothed): %.2f mm/s", max_spd);
    } else {
        ImGui::TextUnformatted("No smoothed speed data available.");
    }

    ImGui::SeparatorText("Distance Statistics");
    if (valid_distance_count == 0) {
        ImGui::TextUnformatted("No valid distance samples available.");
    } else {
        ImGui::Text("Valid points: %zu / %zu",
                    valid_distance_count,
                    distance_samples);
        double avg_distance =
            sum_distance / static_cast<double>(valid_distance_count);
        ImGui::BulletText("Average Distance: %.2f mm", avg_distance);
        ImGui::BulletText("Max Distance: %.2f mm", max_distance_mm);
        if (min_distance_mm < std::numeric_limits<double>::infinity()) {
            ImGui::BulletText("Min Distance: %.2f mm", min_distance_mm);
        }
    }

    ImGui::SeparatorText("Vergence Statistics");
    if (vergence_time_plot.empty()) {
        ImGui::TextUnformatted("No vergence data available.");
    } else if (vergence_valid_count == 0) {
        ImGui::TextUnformatted("No valid vergence samples.");
    } else {
        double avg_vergence =
            vergence_sum / static_cast<double>(vergence_valid_count);
        ImGui::BulletText("Valid samples: %zu", vergence_valid_count);
        ImGui::BulletText("Average Vergence: %.2f deg", avg_vergence);
        if (vergence_min != std::numeric_limits<double>::infinity() &&
            vergence_max != -std::numeric_limits<double>::infinity()) {
            ImGui::BulletText("Range: %.2f .. %.2f deg",
                              vergence_min,
                              vergence_max);
        }
    }

    ImGui::SeparatorText("Heading Statistics");
    const std::vector<double>* heading_for_stats = nullptr;
    if (!heading_smoothed_plot.empty()) {
        heading_for_stats = &heading_smoothed_plot;
    } else if (!heading_raw_plot.empty()) {
        heading_for_stats = &heading_raw_plot;
    }
    if (heading_for_stats && !heading_for_stats->empty()) {
        double sum_cos = 0.0;
        double sum_sin = 0.0;
        for (double deg : *heading_for_stats) {
            double rad = deg * static_cast<double>(M_PI) / 180.0;
            sum_cos += std::cos(rad);
            sum_sin += std::sin(rad);
        }
        size_t count = heading_for_stats->size();
        double mean_rad = std::atan2(sum_sin, sum_cos);
        double mean_deg = mean_rad * 180.0 / static_cast<double>(M_PI);
        double resultant =
            std::sqrt(sum_cos * sum_cos + sum_sin * sum_sin) /
            static_cast<double>(count);
        ImGui::Text("Valid samples: %zu", count);
        ImGui::BulletText("Circular Mean: %.1f deg", mean_deg);
        ImGui::BulletText("Mean Resultant Length: %.2f", resultant);
    } else {
        ImGui::TextUnformatted("No valid heading samples available.");
    }
    if (!heading_per_second_resultant_plot.empty()) {
        size_t finite_count = 0;
        double sum_res = 0.0;
        for (double value : heading_per_second_resultant_plot) {
            if (value == value) {
                sum_res += value;
                ++finite_count;
            }
        }
        if (finite_count > 0) {
            ImGui::BulletText("Average per-second resultant: %.2f",
                              sum_res / static_cast<double>(finite_count));
        }
    }

    ImGui::End();
}
