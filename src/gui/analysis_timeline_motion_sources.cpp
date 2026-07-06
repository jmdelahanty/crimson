#include "gui/analysis_timeline_motion_sources.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace {

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
    if (bouts.source_swim_bout_candidate_id >= 0 &&
        swim_bouts.candidate_id >= 0 &&
        bouts.source_swim_bout_candidate_id != swim_bouts.candidate_id) {
        return false;
    }
    if (bouts.source_swim_bout_signal_id >= 0 &&
        swim_bouts.signal_id >= 0 &&
        bouts.source_swim_bout_signal_id != swim_bouts.signal_id) {
        return false;
    }
    return true;
}

}  // namespace

const ZarrDetectionData::MovementSeries* renderMovementDatasetUI(
    ZarrDetectionLoader& zarr_loader,
    const char* combo_label) {
    zarr_loader.updateDeferredMovementDataLoad();
    if (zarr_loader.isDeferredMovementDataLoadInProgress()) {
        const std::string& load_status =
            zarr_loader.getDeferredMovementLoadStatus();
        ImGui::TextDisabled("%s",
                            load_status.empty()
                                ? "Track kinematics are loading..."
                                : load_status.c_str());
    }
    const std::string& load_error =
        zarr_loader.getDeferredMovementLoadError();
    if (!load_error.empty()) {
        ImGui::TextWrapped("Movement load failed: %s",
                           load_error.c_str());
    }
    if (zarr_loader.hasDeferredMovementData()) {
        if (!zarr_loader.isDeferredMovementDataLoadInProgress()) {
            ImGui::TextDisabled("Track kinematics are available but not loaded.");
            if (ImGui::Button("Load Track Kinematics")) {
                std::string error;
                zarr_loader.startDeferredMovementDataLoad(&error);
            }
        }
    }

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
    if (bouts.is_compact_layout) {
        if (!bouts.signal_role.empty()) {
            label << ", role " << bouts.signal_role;
        }
        label << ", candidate " << bouts.candidate_id << " signal "
              << bouts.signal_id;
    }
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
        state.selected_swim_bout_candidate_id = -1;
        state.selected_swim_bout_signal_id = -1;
        return nullptr;
    }

    for (size_t index : compatible_indices) {
        const auto& candidate = swim_bouts[index];
        if (candidate.run_name == state.selected_swim_bout_run &&
            candidate.speed_level == state.selected_swim_bout_speed_level &&
            (!candidate.is_compact_layout ||
             (candidate.candidate_id ==
                  state.selected_swim_bout_candidate_id &&
              candidate.signal_id == state.selected_swim_bout_signal_id))) {
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
    state.selected_swim_bout_candidate_id = selected.candidate_id;
    state.selected_swim_bout_signal_id = selected.signal_id;
    return &selected;
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
