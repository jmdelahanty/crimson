#include "gui/analysis_timeline_motion_controls.h"

#include "gui/analysis_timeline_motion_data.h"
#include "gui/analysis_timeline_motion_sources.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

const ZarrDetectionData::MovementSeries* drawAnalysisTimelineMotionSourceSelector(
    ZarrDetectionLoader& zarr_loader) {
    return renderMovementDatasetUI(zarr_loader, "Track Kinematics Source");
}

AnalysisTimelineMotionSelection drawAnalysisTimelineMotionControls(
    const AnalysisTimelineMotionControlsContext& context,
    AnalysisTimelineWindowState& state) {
    AnalysisTimelineMotionSelection selection;
    selection.selected_series = context.selected_series;
    if (selection.selected_series == nullptr) {
        return selection;
    }

    const auto& swim_bout_series = context.zarr_loader.getSwimBoutSeries();
    const auto& bout_kinematics_series =
        context.zarr_loader.getBoutKinematicsSeries();

    std::vector<size_t> compatible_swim_bout_indices =
        findCompatibleSwimBoutIndices(*selection.selected_series,
                                      swim_bout_series);
    selection.selected_swim_bouts =
        resolveSelectedSwimBoutSeries(swim_bout_series,
                                      compatible_swim_bout_indices,
                                      state);

    std::vector<size_t> compatible_bout_kinematics_indices;
    if (selection.selected_swim_bouts != nullptr) {
        compatible_bout_kinematics_indices =
            findCompatibleBoutKinematicsIndices(
                *selection.selected_series,
                selection.selected_swim_bouts,
                bout_kinematics_series);
    }
    selection.selected_bout_kinematics =
        resolveSelectedBoutKinematicsSeries(bout_kinematics_series,
                                            compatible_bout_kinematics_indices,
                                            state);

    if (!context.smoothed_available && state.show_smoothed) {
        state.show_smoothed = false;
    }
    if (!context.instant_available && state.show_instantaneous) {
        state.show_instantaneous = false;
    }
    if (!context.heading_sample_available) {
        state.show_heading_raw = false;
        state.show_heading_smoothed = false;
    }
    if (!context.heading_per_second_available) {
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
                    context.primary_speed_units.c_str());
    }
    ImGui::Text("Data points: %zu", context.data_point_count);

    ImGui::SeparatorText("Derived Swim-Bout Candidate");
    if (compatible_swim_bout_indices.empty()) {
        ImGui::TextDisabled(
            "No compatible swim-bout candidates for this track/speed.");
    } else {
        const std::string selected_label =
            selection.selected_swim_bouts
                ? swimBoutCandidateLabel(*selection.selected_swim_bouts)
                : "Select candidate";
        if (ImGui::BeginCombo("Candidate##swim_bout_candidate",
                              selected_label.c_str())) {
            for (size_t index : compatible_swim_bout_indices) {
                const auto& candidate = swim_bout_series[index];
                const std::string label = swimBoutCandidateLabel(candidate);
                const bool is_selected =
                    selection.selected_swim_bouts == &candidate;
                if (ImGui::Selectable(label.c_str(), is_selected)) {
                    state.selected_swim_bout_run = candidate.run_name;
                    state.selected_swim_bout_speed_level =
                        candidate.speed_level;
                    state.selected_swim_bout_candidate_id =
                        candidate.candidate_id;
                    state.selected_swim_bout_signal_id = candidate.signal_id;
                    state.selected_bout_kinematics_run.clear();
                    selection.selected_swim_bouts = &candidate;
                    compatible_bout_kinematics_indices =
                        findCompatibleBoutKinematicsIndices(
                            *selection.selected_series,
                            selection.selected_swim_bouts,
                            bout_kinematics_series);
                    selection.selected_bout_kinematics =
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
        if (selection.selected_swim_bouts) {
            ImGui::Text("Compatible candidates: %zu | Selected bouts: %zu",
                        compatible_swim_bout_indices.size(),
                        selection.selected_swim_bouts->start_frame.size());
            if (!selection.selected_swim_bouts
                     ->detection_signal_source_level.empty() ||
                !selection.selected_swim_bouts
                     ->path_distance_source_level.empty()) {
                ImGui::Text("Detector source: %s | Physical metrics: %s",
                            selection.selected_swim_bouts
                                    ->detection_signal_source_level.empty()
                                ? "direct speed level"
                                : selection.selected_swim_bouts
                                      ->detection_signal_source_level.c_str(),
                            selection.selected_swim_bouts
                                    ->path_distance_source_level.empty()
                                ? "candidate bouts"
                                : selection.selected_swim_bouts
                                      ->path_distance_source_level.c_str());
            }
        }
    }

    ImGui::SeparatorText("Bout-Kinematics Candidate");
    if (selection.selected_swim_bouts == nullptr ||
        compatible_bout_kinematics_indices.empty()) {
        ImGui::TextDisabled(
            "No linked bout-kinematics metrics for the selected candidate.");
    } else {
        const char* combo_preview =
            selection.selected_bout_kinematics
                ? selection.selected_bout_kinematics->run_name.c_str()
                : "Select bout-kinematics run";
        if (ImGui::BeginCombo("Candidate##bout_kinematics_candidate",
                              combo_preview)) {
            for (size_t index : compatible_bout_kinematics_indices) {
                const auto& candidate = bout_kinematics_series[index];
                const bool is_selected =
                    selection.selected_bout_kinematics == &candidate;
                std::ostringstream label;
                label << candidate.run_name;
                if (candidate.metrics_loaded) {
                    if (candidate.is_compact_layout) {
                        label << " (compact v2; movement "
                              << candidate.compact_movement_metric_count
                              << ", heading smoothed "
                              << candidate.compact_heading_smoothed_metric_count
                              << ", heading raw "
                              << candidate.compact_heading_raw_metric_count
                              << ", eye gaze "
                              << candidate.compact_eye_gaze_metric_count << ")";
                    } else {
                        label << " ("
                              << candidate.physical_active_duration_s.size()
                              << " bouts)";
                    }
                } else {
                    label << " (details not loaded)";
                }
                if (ImGui::Selectable(label.str().c_str(), is_selected)) {
                    state.selected_bout_kinematics_run = candidate.run_name;
                    selection.selected_bout_kinematics = &candidate;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (selection.selected_bout_kinematics) {
            if (!selection.selected_bout_kinematics->metrics_loaded) {
                ImGui::TextDisabled(
                    "Per-bout physical metrics are deferred to keep startup responsive.");
                if (selection.selected_bout_kinematics->metrics_load_failed &&
                    !selection.selected_bout_kinematics
                         ->metrics_load_error.empty()) {
                    ImGui::TextWrapped("Load failed: %s",
                                       selection.selected_bout_kinematics
                                           ->metrics_load_error.c_str());
                }
                if (ImGui::Button("Load Bout Metrics")) {
                    std::string error_message;
                    context.zarr_loader.ensureBoutKinematicsMetricsLoaded(
                        selection.selected_bout_kinematics->run_name,
                        &error_message);
                }
            } else {
                if (selection.selected_bout_kinematics->is_compact_layout) {
                    ImGui::Text("Compact v2 metrics: movement %zu | heading smoothed %zu | heading raw %zu | eye gaze %zu",
                                selection.selected_bout_kinematics
                                    ->compact_movement_metric_count,
                                selection.selected_bout_kinematics
                                    ->compact_heading_smoothed_metric_count,
                                selection.selected_bout_kinematics
                                    ->compact_heading_raw_metric_count,
                                selection.selected_bout_kinematics
                                    ->compact_eye_gaze_metric_count);
                }
                const auto* valid_mask =
                    selection.selected_bout_kinematics
                            ->physical_active_valid.empty()
                        ? nullptr
                        : &selection.selected_bout_kinematics
                               ->physical_active_valid;
                const double mean_duration =
                    computeFiniteMean(
                        selection.selected_bout_kinematics
                            ->physical_active_duration_s,
                        valid_mask);
                const double mean_path =
                    computeFiniteMean(
                        selection.selected_bout_kinematics
                            ->physical_active_path_length_mm,
                        valid_mask);
                const double mean_speed =
                    computeFiniteMean(
                        selection.selected_bout_kinematics
                            ->physical_active_mean_speed_mm_s,
                        valid_mask);
                const size_t valid_physical =
                    selection.selected_bout_kinematics
                            ->physical_active_valid.empty()
                        ? selection.selected_bout_kinematics
                              ->physical_active_duration_s.size()
                        : countValidMaskValues(
                              selection.selected_bout_kinematics
                                  ->physical_active_valid);
                ImGui::Text("Physical-active valid: %zu/%zu",
                            valid_physical,
                            selection.selected_bout_kinematics
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

    ImGui::Checkbox(context.primary_speed_label.c_str(), &state.show_smoothed);
    ImGui::SameLine();
    ImGui::Checkbox(context.secondary_speed_label.c_str(),
                    &state.show_instantaneous);

    ImGui::SeparatorText("Plot Layers");
    ImGui::BeginDisabled(!context.heading_sample_available);
    ImGui::Checkbox("Show Raw Heading", &state.show_heading_raw);
    ImGui::SameLine();
    ImGui::Checkbox("Show Smoothed Heading", &state.show_heading_smoothed);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.heading_per_second_available);
    ImGui::Checkbox("Show Heading (per-second)",
                    &state.show_heading_per_second);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selection.selected_swim_bouts == nullptr);
    ImGui::Checkbox("Show Swim Bouts", &state.show_swim_bouts);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selection.selected_swim_bouts == nullptr ||
                         !selection.selected_swim_bouts->has_detector_trace);
    ImGui::Checkbox("Show Detector Response",
                    &state.show_detector_response);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.distance_available);
    ImGui::Checkbox("Show Distance", &state.show_distance_trace);
    ImGui::EndDisabled();
    ImGui::Checkbox("Show Track Position", &state.show_track_position);
    ImGui::BeginDisabled(!state.show_track_position);
    ImGui::SameLine();
    ImGui::Checkbox("X##track_position_x", &state.show_track_position_x);
    ImGui::SameLine();
    ImGui::Checkbox("Y##track_position_y", &state.show_track_position_y);
    ImGui::EndDisabled();

    return selection;
}
