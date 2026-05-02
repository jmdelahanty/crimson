#include "gui/analysis_timeline_window.h"

#include "imgui.h"
#include "implot.h"
#include "ui_path_config.h"
#include "zarr_loader.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
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
                    << selected_series->track_id;
            if (!selected_series->speed_level.empty()) {
                summary << ", " << selected_series->speed_level;
            }
            summary << ")";
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
                      << " (track " << series->track_id;
                if (!series->speed_level.empty()) {
                    label << ", " << series->speed_level;
                }
                label << ")";
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

std::string normalizeSpeedLevelName(std::string value) {
    if (value.rfind("speed_", 0) == 0) {
        value = value.substr(6);
    }
    return value;
}

int32_t parseTrackId(const std::string& track_id) {
    std::string value = track_id;
    if (value.rfind("id_", 0) == 0) {
        value = value.substr(3);
    }
    try {
        return static_cast<int32_t>(std::stoi(value));
    } catch (...) {
        return -1;
    }
}

bool isFiniteFloatValue(float value) {
    return std::isfinite(static_cast<double>(value));
}

bool speedLevelMatches(const std::string& candidate_level,
                       const std::string& selected_level) {
    const std::string candidate = normalizeSpeedLevelName(candidate_level);
    const std::string selected = normalizeSpeedLevelName(selected_level);
    return selected.empty() || candidate == selected;
}

bool speedSourceMatches(const std::string& source_level,
                        const std::string& selected_level) {
    const std::string source = normalizeSpeedLevelName(source_level);
    const std::string selected = normalizeSpeedLevelName(selected_level);
    return !source.empty() && !selected.empty() && source == selected;
}

bool pathReferencesSpeedLevel(const std::string& source_path,
                              const std::string& selected_level) {
    const std::string selected = normalizeSpeedLevelName(selected_level);
    if (source_path.empty() || selected.empty()) {
        return false;
    }
    std::string lower_path = source_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return lower_path.find("speed_" + selected) != std::string::npos ||
           lower_path.find("/" + selected + "/") != std::string::npos;
}

bool isCompatibleSwimBoutSeries(
    const ZarrDetectionData::MovementSeries& movement_series,
    const ZarrDetectionData::SwimBoutSeries& bouts) {
    const int32_t selected_track = parseTrackId(movement_series.track_id);
    const std::string selected_level =
        normalizeSpeedLevelName(movement_series.speed_level);
    if (!bouts.source_track_kinematics_run.empty() &&
        bouts.source_track_kinematics_run != movement_series.run_name) {
        return false;
    }
    if (bouts.source_track_kinematics_run.empty() &&
        bouts.run_name.find(movement_series.run_name) == std::string::npos) {
        return false;
    }
    if (bouts.track_id >= 0 && selected_track >= 0 &&
        bouts.track_id != selected_track) {
        return false;
    }
    if (selected_level.empty()) {
        return true;
    }
    if (speedLevelMatches(bouts.speed_level, selected_level)) {
        return true;
    }
    if (speedSourceMatches(bouts.detection_signal_source_level,
                           selected_level) ||
        speedSourceMatches(bouts.movement_metric_source_level,
                           selected_level) ||
        speedSourceMatches(bouts.path_distance_source_level,
                           selected_level)) {
        return true;
    }
    return pathReferencesSpeedLevel(bouts.detection_signal_source_path,
                                    selected_level);
}

std::vector<size_t> findCompatibleSwimBoutIndices(
    const ZarrDetectionData::MovementSeries& movement_series,
    const std::vector<ZarrDetectionData::SwimBoutSeries>& swim_bouts) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < swim_bouts.size(); ++i) {
        if (isCompatibleSwimBoutSeries(movement_series, swim_bouts[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

std::string swimBoutCandidateLabel(
    const ZarrDetectionData::SwimBoutSeries& bouts) {
    std::ostringstream label;
    label << bouts.run_name << " / " << bouts.speed_level;
    label << " (" << (bouts.detection_method.empty()
                          ? "method unknown"
                          : bouts.detection_method)
          << ", " << bouts.start_frame.size() << " bouts";
    if (isFiniteFloatValue(bouts.threshold_mm)) {
        label << ", threshold " << std::fixed << std::setprecision(3)
              << bouts.threshold_mm;
    }
    if (isFiniteFloatValue(bouts.exponential_tau_s)) {
        label << ", tau " << std::fixed << std::setprecision(3)
              << bouts.exponential_tau_s << "s";
    }
    if (bouts.is_latest_run) {
        label << ", latest";
    }
    if (bouts.is_default_level) {
        label << ", default";
    }
    label << ")";
    return label.str();
}

const ZarrDetectionData::SwimBoutSeries* resolveSelectedSwimBoutSeries(
    const std::vector<ZarrDetectionData::SwimBoutSeries>& swim_bouts,
    const std::vector<size_t>& compatible_indices,
    AnalysisTimelineWindowState& state) {
    if (compatible_indices.empty()) {
        state.selected_swim_bout_run.clear();
        state.selected_swim_bout_speed_level.clear();
        return nullptr;
    }

    for (size_t index : compatible_indices) {
        const auto& candidate = swim_bouts[index];
        if (candidate.run_name == state.selected_swim_bout_run &&
            candidate.speed_level == state.selected_swim_bout_speed_level) {
            return &candidate;
        }
    }

    size_t chosen = compatible_indices.front();
    for (size_t index : compatible_indices) {
        const auto& candidate = swim_bouts[index];
        if (candidate.is_latest_run && candidate.is_default_level) {
            chosen = index;
            break;
        }
        if (swim_bouts[chosen].is_latest_run &&
            !swim_bouts[chosen].is_default_level) {
            continue;
        }
        if (candidate.is_latest_run || candidate.is_default_level) {
            chosen = index;
        }
    }

    const auto& selected = swim_bouts[chosen];
    state.selected_swim_bout_run = selected.run_name;
    state.selected_swim_bout_speed_level = selected.speed_level;
    return &selected;
}

bool isCompatibleBoutKinematicsSeries(
    const ZarrDetectionData::MovementSeries& movement_series,
    const ZarrDetectionData::SwimBoutSeries& swim_bouts,
    const ZarrDetectionData::BoutKinematicsSeries& bouts) {
    const int32_t selected_track = parseTrackId(movement_series.track_id);
    if (!bouts.source_track_kinematics_run.empty() &&
        bouts.source_track_kinematics_run != movement_series.run_name) {
        return false;
    }
    if (bouts.source_track_id >= 0 && selected_track >= 0 &&
        bouts.source_track_id != selected_track) {
        return false;
    }
    if (!bouts.source_swim_bout_run.empty() &&
        bouts.source_swim_bout_run != swim_bouts.run_name) {
        return false;
    }
    if (bouts.source_swim_bout_run.empty()) {
        return false;
    }
    if (!bouts.source_swim_bout_speed_level.empty() &&
        normalizeSpeedLevelName(bouts.source_swim_bout_speed_level) !=
            normalizeSpeedLevelName(swim_bouts.speed_level)) {
        return false;
    }
    return true;
}

std::vector<size_t> findCompatibleBoutKinematicsIndices(
    const ZarrDetectionData::MovementSeries& movement_series,
    const ZarrDetectionData::SwimBoutSeries* swim_bouts,
    const std::vector<ZarrDetectionData::BoutKinematicsSeries>& bout_kinematics) {
    std::vector<size_t> indices;
    if (swim_bouts == nullptr) {
        return indices;
    }
    for (size_t i = 0; i < bout_kinematics.size(); ++i) {
        if (isCompatibleBoutKinematicsSeries(movement_series,
                                             *swim_bouts,
                                             bout_kinematics[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

const ZarrDetectionData::BoutKinematicsSeries*
resolveSelectedBoutKinematicsSeries(
    const std::vector<ZarrDetectionData::BoutKinematicsSeries>& bout_kinematics,
    const std::vector<size_t>& compatible_indices,
    AnalysisTimelineWindowState& state) {
    if (compatible_indices.empty()) {
        state.selected_bout_kinematics_run.clear();
        return nullptr;
    }
    for (size_t index : compatible_indices) {
        const auto& candidate = bout_kinematics[index];
        if (candidate.run_name == state.selected_bout_kinematics_run) {
            return &candidate;
        }
    }
    const auto& selected = bout_kinematics[compatible_indices.front()];
    state.selected_bout_kinematics_run = selected.run_name;
    return &selected;
}

double finiteMean(const std::vector<float>& values,
                  const std::vector<uint8_t>* valid_mask = nullptr) {
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        if (valid_mask && i < valid_mask->size() && (*valid_mask)[i] == 0) {
            continue;
        }
        if (!isFiniteFloatValue(values[i])) {
            continue;
        }
        sum += static_cast<double>(values[i]);
        ++count;
    }
    return count == 0 ? std::numeric_limits<double>::quiet_NaN()
                      : sum / static_cast<double>(count);
}

size_t validCount(const std::vector<uint8_t>& values) {
    return static_cast<size_t>(
        std::count_if(values.begin(), values.end(), [](uint8_t value) {
            return value != 0;
        }));
}

struct AnalysisTimelineTrace {
    std::string label;
    std::string units;
    std::vector<double> xs;
    std::vector<double> ys;
};

bool stringEndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

std::string unsmoothedEyeAngleFieldName(const std::string& field_name) {
    constexpr const char* kSmoothedSuffix = "_smoothed";
    if (!stringEndsWith(field_name, kSmoothedSuffix)) {
        return {};
    }
    return field_name.substr(0, field_name.size() -
                                      std::string(kSmoothedSuffix).size());
}

void addUniqueField(std::vector<std::string>& fields,
                    const std::string& field_name) {
    if (field_name.empty() ||
        std::find(fields.begin(), fields.end(), field_name) != fields.end()) {
        return;
    }
    fields.push_back(field_name);
}

const ZarrDetectionData::EyeAngleScalarField* resolveEyeAngleTimelineField(
    const ZarrDetectionLoader& loader,
    const std::string& requested_field,
    std::string& resolved_name) {
    resolved_name = requested_field;
    const auto* field = loader.findEyeAngleScalarField(requested_field);
    if (field != nullptr && (field->has_frame || field->has_roi)) {
        return field;
    }
    const std::string base = unsmoothedEyeAngleFieldName(requested_field);
    if (!base.empty()) {
        field = loader.findEyeAngleScalarField(base);
        if (field != nullptr && (field->has_frame || field->has_roi)) {
            resolved_name = base;
            return field;
        }
    }
    return nullptr;
}

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

void drawCurrentTimeMarker(double current_time,
                           const char* label = "##current_time") {
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
            ImPlot::PlotLine(trace.label.c_str(),
                             trace.xs.data(),
                             trace.ys.data(),
                             static_cast<int>(trace.xs.size()));
        }
        drawCurrentTimeMarker(current_time, current_marker_id);
        ImPlot::EndPlot();
    }
}

std::optional<double> currentTimeSeconds(const AnalysisTimelineWindowContext& context) {
    if (context.current_frame_num < 0 || context.video_fps <= 0.0) {
        return std::nullopt;
    }
    return static_cast<double>(context.current_frame_num) / context.video_fps;
}

std::vector<std::string> defaultEyeAngleTimelineFields(
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep) {
    std::vector<std::string> fields;
    for (const auto& field : rep.default_plot_fields) {
        addUniqueField(fields, field);
    }
    if (fields.empty()) {
        for (const auto& field : rep.primary_roi_fields) {
            addUniqueField(fields, field + "_smoothed");
            addUniqueField(fields, field);
        }
        for (const auto& field : rep.aggregate_roi_fields) {
            addUniqueField(fields, field + "_smoothed");
            addUniqueField(fields, field);
        }
    }
    return fields;
}

std::vector<AnalysisTimelineTrace> buildEyeAngleTimelineTraces(
    const ZarrDetectionLoader& loader,
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep,
    double video_fps) {
    std::vector<AnalysisTimelineTrace> traces;
    for (const auto& requested_field : defaultEyeAngleTimelineFields(rep)) {
        std::string resolved_name;
        const auto* field =
            resolveEyeAngleTimelineField(loader, requested_field, resolved_name);
        if (field == nullptr) {
            continue;
        }
        const bool use_frame_values =
            field->has_frame && !field->frame_values.empty();
        const auto& values =
            use_frame_values ? field->frame_values : field->roi_values;
        if (values.empty()) {
            continue;
        }

        AnalysisTimelineTrace trace;
        trace.label = field->display_name.empty() ? resolved_name
                                                  : field->display_name;
        if (resolved_name != requested_field) {
            trace.label += " (fallback)";
        }
        trace.units = field->units.empty() ? "deg" : field->units;
        trace.xs.reserve(values.size());
        trace.ys.reserve(values.size());
        for (size_t row = 0; row < values.size(); ++row) {
            const float value = values[row];
            if (!std::isfinite(value)) {
                continue;
            }
            std::optional<double> t;
            if (use_frame_values) {
                if (row < eye.frame_time_seconds.size() &&
                    std::isfinite(eye.frame_time_seconds[row])) {
                    t = static_cast<double>(eye.frame_time_seconds[row]);
                } else {
                    t = sampleTimeFromFrame(static_cast<int32_t>(row),
                                            video_fps);
                }
            } else if (row < eye.roi_time_seconds.size() &&
                       std::isfinite(eye.roi_time_seconds[row])) {
                t = static_cast<double>(eye.roi_time_seconds[row]);
            } else if (row < eye.roi_frame_indices.size()) {
                t = sampleTimeFromFrame(eye.roi_frame_indices[row], video_fps);
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
    return traces;
}

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
                if (!candidate.physical_active_duration_s.empty()) {
                    label << " (" << candidate.physical_active_duration_s.size()
                          << " bouts)";
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
            const auto* valid_mask =
                selected_bout_kinematics->physical_active_valid.empty()
                    ? nullptr
                    : &selected_bout_kinematics->physical_active_valid;
            const double mean_duration =
                finiteMean(selected_bout_kinematics->physical_active_duration_s,
                           valid_mask);
            const double mean_path =
                finiteMean(
                    selected_bout_kinematics->physical_active_path_length_mm,
                    valid_mask);
            const double mean_speed =
                finiteMean(
                    selected_bout_kinematics->physical_active_mean_speed_mm_s,
                    valid_mask);
            const size_t valid_physical =
                selected_bout_kinematics->physical_active_valid.empty()
                    ? selected_bout_kinematics
                          ->physical_active_duration_s.size()
                    : validCount(selected_bout_kinematics
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
    ImGui::BeginDisabled(!context.zarr_loader.hasEyeVergenceFrame());
    ImGui::Checkbox("Show Vergence", &state.show_vergence);
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

    auto frameToTime = [&](int32_t frame) -> std::optional<double> {
        if (frame < 0) {
            return std::nullopt;
        }
        if (!frame_indices.empty()) {
            auto upper = std::lower_bound(frame_indices.begin(),
                                          frame_indices.end(),
                                          frame);
            if (upper != frame_indices.end() && *upper == frame) {
                const size_t idx = static_cast<size_t>(
                    std::distance(frame_indices.begin(), upper));
                if (idx < time_data.size()) {
                    return static_cast<double>(time_data[idx]);
                }
            }
            if (upper != frame_indices.end() && upper != frame_indices.begin()) {
                auto lower = upper - 1;
                const size_t lower_idx = static_cast<size_t>(
                    std::distance(frame_indices.begin(), lower));
                const size_t upper_idx = static_cast<size_t>(
                    std::distance(frame_indices.begin(), upper));
                if (lower_idx < time_data.size() &&
                    upper_idx < time_data.size()) {
                    const int32_t f0 = *lower;
                    const int32_t f1 = *upper;
                    const double t0 = static_cast<double>(time_data[lower_idx]);
                    const double t1 = static_cast<double>(time_data[upper_idx]);
                    const int32_t delta_f = f1 - f0;
                    if (delta_f != 0) {
                        const double alpha =
                            static_cast<double>(frame - f0) /
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
        if (context.video_fps > 0.0) {
            return static_cast<double>(frame) / context.video_fps;
        }
        return std::nullopt;
    };

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

    std::vector<double> detector_time_plot;
    std::vector<double> detector_value_plot;
    if (selected_swim_bouts != nullptr &&
        state.show_detector_response &&
        selected_swim_bouts->has_detector_trace &&
        !selected_swim_bouts->detector_trace_values.empty()) {
        const size_t detector_count =
            selected_swim_bouts->detector_trace_values.size();
        detector_time_plot.reserve(detector_count);
        detector_value_plot.reserve(detector_count);
        for (size_t i = 0; i < detector_count; ++i) {
            const float value =
                selected_swim_bouts->detector_trace_values[i];
            if (!isFiniteFloatValue(value)) {
                continue;
            }
            std::optional<double> t;
            if (i < selected_swim_bouts
                        ->detector_trace_frame_indices.size()) {
                t = frameToTime(selected_swim_bouts
                                    ->detector_trace_frame_indices[i]);
            } else if (i < time_data.size()) {
                t = static_cast<double>(time_data[i]);
            }
            if (!t.has_value()) {
                continue;
            }
            detector_time_plot.push_back(*t);
            detector_value_plot.push_back(static_cast<double>(value));
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
        ImPlot::BeginSubplots("##analysis_timeline_plots",
                              4,
                              1,
                              subplot_size,
                              ImPlotSubplotFlags_LinkAllX |
                                  ImPlotSubplotFlags_NoTitle)) {
        if (ImPlot::BeginPlot("##speed_plot")) {
            std::string speed_axis_label = "Speed";
            if (!primary_speed_units.empty() &&
                primary_speed_units == secondary_speed_units) {
                speed_axis_label += " (" + primary_speed_units + ")";
            }
            ImPlot::SetupAxes(nullptr, speed_axis_label.c_str());
            apply_time_axis_limits(ImGuiCond_Once);

            double max_speed = 0.0;
            if (state.show_smoothed && !smoothed_plot.empty()) {
                for (double value : smoothed_plot) {
                    if (std::isfinite(value)) {
                        max_speed = std::max(max_speed, value);
                    }
                }
            }
            if (state.show_instantaneous && !instant_plot.empty()) {
                for (double value : instant_plot) {
                    if (std::isfinite(value)) {
                        max_speed = std::max(max_speed, value);
                    }
                }
            }
            if (!detector_value_plot.empty()) {
                for (double value : detector_value_plot) {
                    if (std::isfinite(value)) {
                        max_speed = std::max(max_speed, value);
                    }
                }
            }
            double y_max_speed = (max_speed > 0.0) ? max_speed * 1.1 : 1.0;
            ImPlot::SetupAxisLimits(ImAxis_Y1,
                                    0.0,
                                    y_max_speed,
                                    ImGuiCond_Once);

            if (state.show_swim_bouts && selected_swim_bouts != nullptr &&
                !selected_swim_bouts->start_frame.empty() &&
                selected_swim_bouts->start_frame.size() ==
                    selected_swim_bouts->end_frame.size()) {
                ImPlotRect limits = ImPlot::GetPlotLimits();
                ImDrawList* draw_list = ImPlot::GetPlotDrawList();
                const ImU32 bout_fill =
                    ImGui::GetColorU32(ImVec4(0.15f, 0.95f, 0.45f, 0.16f));
                const ImU32 bout_core_fill =
                    ImGui::GetColorU32(ImVec4(0.15f, 0.95f, 0.45f, 0.24f));
                const size_t bout_count =
                    selected_swim_bouts->start_frame.size();
                for (size_t i = 0; i < bout_count; ++i) {
                    const auto start_time =
                        frameToTime(selected_swim_bouts->start_frame[i]);
                    const auto end_time =
                        frameToTime(selected_swim_bouts->end_frame[i]);
                    if (!start_time.has_value() || !end_time.has_value() ||
                        *end_time < window_min ||
                        *start_time > window_max) {
                        continue;
                    }
                    const ImVec2 p0 = ImPlot::PlotToPixels(
                        ImPlotPoint(*start_time, limits.Y.Max));
                    const ImVec2 p1 = ImPlot::PlotToPixels(
                        ImPlotPoint(*end_time, limits.Y.Min));
                    draw_list->AddRectFilled(p0, p1, bout_fill, 0.0f);
                    if (i < selected_swim_bouts->core_start_frame.size() &&
                        i < selected_swim_bouts->core_end_frame.size()) {
                        auto core_start = frameToTime(
                            selected_swim_bouts->core_start_frame[i]);
                        auto core_end = frameToTime(
                            selected_swim_bouts->core_end_frame[i]);
                        if (core_start.has_value() && core_end.has_value()) {
                            const ImVec2 c0 = ImPlot::PlotToPixels(
                                ImPlotPoint(*core_start, limits.Y.Max));
                            const ImVec2 c1 = ImPlot::PlotToPixels(
                                ImPlotPoint(*core_end, limits.Y.Min));
                            draw_list->AddRectFilled(c0, c1, bout_core_fill,
                                                     0.0f);
                        }
                    }
                }
            }

            if (state.show_smoothed && !smoothed_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), 2.0f);
                ImPlot::PlotLine(primary_speed_label.c_str(),
                                 time_plot.data(),
                                 smoothed_plot.data(),
                                 static_cast<int>(time_plot.size()));
            }

            if (state.show_instantaneous && !instant_plot.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.5f, 0.2f, 0.6f), 1.0f);
                ImPlot::PlotLine(secondary_speed_label.c_str(),
                                 time_plot.data(),
                                 instant_plot.data(),
                                 static_cast<int>(instant_plot.size()));
            }

            if (!detector_time_plot.empty() &&
                detector_time_plot.size() == detector_value_plot.size()) {
                std::string detector_label =
                    selected_swim_bouts &&
                            !selected_swim_bouts->detector_trace_label.empty()
                        ? selected_swim_bouts->detector_trace_label
                        : "Detector response";
                if (selected_swim_bouts &&
                    !selected_swim_bouts->detector_trace_units.empty()) {
                    detector_label += " (" +
                                      selected_swim_bouts
                                          ->detector_trace_units +
                                      ", not physical speed)";
                } else {
                    detector_label += " (not physical speed)";
                }
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.85f, 0.2f, 0.85f),
                                         1.5f);
                ImPlot::PlotLine(detector_label.c_str(),
                                 detector_time_plot.data(),
                                 detector_value_plot.data(),
                                 static_cast<int>(detector_time_plot.size()));
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
            apply_time_axis_limits(ImGuiCond_Once);
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

                if (current_time_line >= 0.0) {
                    ImPlotRect limits = ImPlot::GetPlotLimits();
                    double current_line_x[2] = {current_time_line,
                                                current_time_line};
                    double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                    ImPlot::SetNextLineStyle(
                        ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                        3.0f);
                    ImPlot::PlotLine("##current_time_vergence",
                                     current_line_x,
                                     current_line_y,
                                     2);
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
        double sum_speed = 0.0;
        double max_speed = 0.0;
        size_t finite_speed_count = 0;
        for (float value : smoothed_speed) {
            if (!IsFiniteFloat(value)) {
                continue;
            }
            sum_speed += static_cast<double>(value);
            max_speed = std::max(max_speed, static_cast<double>(value));
            ++finite_speed_count;
        }
        if (finite_speed_count > 0) {
            const double avg_speed =
                sum_speed / static_cast<double>(finite_speed_count);
            ImGui::BulletText("Average %s: %.2f %s",
                              primary_speed_label.c_str(),
                              avg_speed,
                              primary_speed_units.c_str());
            ImGui::BulletText("Max %s: %.2f %s",
                              primary_speed_label.c_str(),
                              max_speed,
                              primary_speed_units.c_str());
        } else {
            ImGui::TextUnformatted("No finite primary speed samples.");
        }
    } else {
        ImGui::TextUnformatted("No primary speed data available.");
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
    } else {
        ImGui::TextDisabled("Track kinematics traces unavailable.");
    }

    const double fallback_current_time =
        currentTimeSeconds(context).value_or(-1.0);

    if (has_eye_angle_timeline) {
        ImGui::SeparatorText("Eye-Angle Traces");
        const auto& eye = context.zarr_loader.getEyeAngleAnalysisData();
        ImGui::Text("Run: %s", eye.run_name.c_str());
        if (eye.representations.empty()) {
            ImGui::TextDisabled("No eye-angle representations available.");
        } else {
            if (state.eye_angle_representation_index < 0 ||
                static_cast<size_t>(state.eye_angle_representation_index) >=
                    eye.representations.size()) {
                state.eye_angle_representation_index = 0;
                for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
                    if (eye.representations[idx].key ==
                        eye.default_representation) {
                        state.eye_angle_representation_index =
                            static_cast<int>(idx);
                        break;
                    }
                }
            }

            const auto& selected_rep = eye.representations
                [static_cast<size_t>(state.eye_angle_representation_index)];
            const char* rep_preview =
                selected_rep.display_name.empty()
                    ? selected_rep.key.c_str()
                    : selected_rep.display_name.c_str();
            if (ImGui::BeginCombo("Representation##analysis_eye_angle_rep",
                                  rep_preview)) {
                for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
                    const auto& rep = eye.representations[idx];
                    const bool selected =
                        static_cast<int>(idx) ==
                        state.eye_angle_representation_index;
                    const char* label =
                        rep.display_name.empty() ? rep.key.c_str()
                                                 : rep.display_name.c_str();
                    if (ImGui::Selectable(label, selected)) {
                        state.eye_angle_representation_index =
                            static_cast<int>(idx);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Show selected eye-angle representation",
                            &state.show_eye_angle_traces);
            if (state.show_eye_angle_traces) {
                double current_eye_time = fallback_current_time;
                if (auto current_row =
                        context.zarr_loader.findEyeAngleRowForFrame(
                            context.current_frame_num)) {
                    if (*current_row < eye.roi_time_seconds.size() &&
                        std::isfinite(eye.roi_time_seconds[*current_row])) {
                        current_eye_time = static_cast<double>(
                            eye.roi_time_seconds[*current_row]);
                    }
                } else if (context.current_frame_num >= 0 &&
                           static_cast<size_t>(context.current_frame_num) <
                               eye.frame_time_seconds.size() &&
                           std::isfinite(eye.frame_time_seconds
                                              [context.current_frame_num])) {
                    current_eye_time = static_cast<double>(
                        eye.frame_time_seconds[context.current_frame_num]);
                }

                const auto traces = buildEyeAngleTimelineTraces(
                    context.zarr_loader, eye, selected_rep, context.video_fps);
                std::string y_axis = "deg";
                if (!traces.empty() && !traces.front().units.empty()) {
                    y_axis = traces.front().units;
                }
                drawAnalysisTracePlot("Eye Angles",
                                      y_axis.c_str(),
                                      traces,
                                      context.scroll_state,
                                      current_eye_time,
                                      "##current_time_eye_angles");
            }
        }
    }

    if (has_tail_kinematics_timeline) {
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

        const std::vector<int32_t>& tail_frame_index =
            !tail.frame_index.empty() ? tail.frame_index : tail.row_to_frame;
        double current_tail_time = fallback_current_time;
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
            drawAnalysisTracePlot("Tail Angle",
                                  "deg",
                                  traces,
                                  context.scroll_state,
                                  current_tail_time,
                                  "##current_time_tail_angle");
        }
        if (state.show_tail_tip_lateral_deflection) {
            std::vector<AnalysisTimelineTrace> traces;
            appendTailTimelineTrace(tail.tail_tip_lateral_deflection_px,
                                    tail_frame_index,
                                    context.video_fps,
                                    "Tail Tip Lateral Deflection",
                                    "px",
                                    traces);
            drawAnalysisTracePlot("Tail Lateral Deflection",
                                  "px",
                                  traces,
                                  context.scroll_state,
                                  current_tail_time,
                                  "##current_time_tail_deflection");
        }
        if (state.show_tail_curvature) {
            std::vector<AnalysisTimelineTrace> traces;
            appendTailTimelineTrace(tail.max_abs_tail_curvature_px_inv,
                                    tail_frame_index,
                                    context.video_fps,
                                    "Max Abs Tail Curvature",
                                    "px^-1",
                                    traces);
            drawAnalysisTracePlot("Tail Curvature",
                                  "px^-1",
                                  traces,
                                  context.scroll_state,
                                  current_tail_time,
                                  "##current_time_tail_curvature");
        }
    }

    ImGui::End();
}
