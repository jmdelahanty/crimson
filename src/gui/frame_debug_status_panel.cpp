#include "gui/frame_debug_status_panel.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

const ZarrDetectionLoader::ReviewArtifactSummary* findReviewArtifact(
    const std::vector<ZarrDetectionLoader::ReviewArtifactSummary>& artifacts,
    ZarrDetectionLoader::ReviewArtifactKind kind) {
    for (const auto& artifact : artifacts) {
        if (artifact.kind == kind) {
            return &artifact;
        }
    }
    return nullptr;
}

void drawReviewArtifactBlock(
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    if (artifact == nullptr) {
        ImGui::TextDisabled("Review metadata unavailable");
        return;
    }
    if (!artifact->run_name.empty()) {
        ImGui::Text("Run: %s", artifact->run_name.c_str());
    }
    if (!artifact->has_review_status) {
        ImGui::TextDisabled("Review metadata unavailable");
        return;
    }

    const auto& review_state = artifact->review_state;
    ImVec4 status_color = (review_state == "approved")
                              ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                          : (review_state == "rejected")
                              ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                              : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
    ImGui::TextColored(status_color, "Review: %s", review_state.c_str());
    if (!artifact->review_intended_use.empty() ||
        !artifact->review_method.empty()) {
        ImGui::Text("Use: %s | Method: %s",
                    artifact->review_intended_use.c_str(),
                    artifact->review_method.c_str());
    }
    if (!artifact->review_timestamp.empty()) {
        ImGui::Text("Reviewed: %s", artifact->review_timestamp.c_str());
    }
    if (!artifact->review_reviewer.empty()) {
        ImGui::Text("Reviewer: %s", artifact->review_reviewer.c_str());
    }
    if (!artifact->review_notes.empty()) {
        ImGui::Text("Notes: %s", artifact->review_notes.c_str());
    }
}

std::string joinLabels(const std::vector<std::string>& labels) {
    if (labels.empty()) {
        return "<none>";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << labels[i];
    }
    return oss.str();
}

const char* maskPanelTitle(const FrameDebugWindowContext& context) {
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks() ||
                   context.zarr_loader.hasSubjectShapeData()
               ? "Subject Masks"
               : "Eye Masks";
}

const char* maskReviewTitle(const FrameDebugWindowContext& context) {
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks() ||
                   context.zarr_loader.hasSubjectShapeData()
               ? "Subject Mask Review:"
               : "Eye Mask Review:";
}

const char* unavailableMaskDetailsText(const FrameDebugWindowContext& context) {
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks() ||
                   context.zarr_loader.hasSubjectShapeData()
               ? "Current frame subject-mask details unavailable"
               : "Current frame eye-mask details unavailable";
}

std::string shortSubjectMaskComponentLabel(const std::string& label) {
    if (label == "subject_body") {
        return "Body";
    }
    if (label == "eye_left") {
        return "Left eye";
    }
    if (label == "eye_right") {
        return "Right eye";
    }
    if (label == "swim_bladder") {
        return "Swim bladder";
    }
    return label;
}

bool stringEndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string eyeUnsmoothedBaseField(const std::string& field_name) {
    constexpr const char* kSmoothedSuffix = "_smoothed";
    if (!stringEndsWith(field_name, kSmoothedSuffix)) {
        return {};
    }
    return field_name.substr(0, field_name.size() - std::strlen(kSmoothedSuffix));
}

bool maskHasComponent(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask,
    const std::string& component_name) {
    return std::any_of(
        mask.subject_mask_components.begin(),
        mask.subject_mask_components.end(),
        [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                SubjectMaskComponent& component) {
            return component.label == component_name;
        });
}

std::string componentSummary(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask) {
    std::ostringstream oss;
    bool first = true;
    for (const char* label :
         {"subject_body", "eye_left", "eye_right", "swim_bladder"}) {
        if (!maskHasComponent(mask, label)) {
            continue;
        }
        if (!first) {
            oss << ", ";
        }
        oss << shortSubjectMaskComponentLabel(label);
        first = false;
    }
    return first ? "<none>" : oss.str();
}

std::string firstEditableComponent(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask) {
    for (const char* label :
         {"subject_body", "eye_left", "eye_right", "swim_bladder"}) {
        if (maskHasComponent(mask, label)) {
            return label;
        }
    }
    if (!mask.subject_mask_components.empty()) {
        return mask.subject_mask_components.front().label;
    }
    return {};
}

bool loadSubjectMaskEditTarget(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state,
    int detection_index,
    int32_t roi_index,
    const std::string& component_name) {
    if (roi_index < 0 || component_name.empty()) {
        state.subject_mask_edit_status =
            "Preview load failed: no editable subject-mask target.";
        return false;
    }

    std::string error;
    if (state.subject_mask_edit_session.startFromLoadedRow(
            context.zarr_loader,
            static_cast<size_t>(roi_index),
            component_name,
            &error)) {
        state.subject_mask_edit_detection_index = detection_index;
        state.subject_mask_edit_component_name = component_name;
        const auto& target = state.subject_mask_edit_session.target();
        std::ostringstream oss;
        oss << "Preview loaded: detection=" << detection_index
            << " roi=" << target.roi_index
            << " component=" << target.component_name
            << " shape=" << target.rows << "x" << target.cols;
        state.subject_mask_edit_status = oss.str();
        return true;
    }

    state.subject_mask_edit_status = "Preview load failed: " + error;
    return false;
}

const char* headingSourceLabel(const KeypointHeadingComputationSpec& spec) {
    if (!spec.available) {
        return "Unavailable";
    }
    if (spec.legacy_fallback || spec.source == "legacy_3point") {
        return "Legacy fallback";
    }
    if (spec.source == "run_override") {
        return "Run override";
    }
    if (spec.source == "pose_schema") {
        return "Pose schema metadata";
    }
    if (spec.source == "deprecated_run_alias") {
        return "Deprecated run alias";
    }
    return spec.source.empty() ? "Resolved" : spec.source.c_str();
}

std::string formatPointSpec(const KeypointHeadingPointSpec& spec) {
    if (!spec.valid) {
        return "<invalid>";
    }
    if (spec.op == KeypointHeadingPointOp::Keypoint) {
        if (!spec.labels.empty()) {
            return "keypoint(" + spec.labels.front() + ")";
        }
        return "keypoint(?)";
    }
    if (spec.op == KeypointHeadingPointOp::Midpoint) {
        return "midpoint(" + joinLabels(spec.labels) + ")";
    }
    return "<none>";
}

void drawKeypointSkeletonSection(const FrameDebugWindowContext& context) {
    const auto& labels = context.zarr_loader.getKeypointLabels();
    const auto& edges = context.zarr_loader.getSkeletonEdges();
    ImGui::Text("Skeleton:");
    ImGui::Text("Keypoint count: %zu", labels.size());
    ImGui::Text("Edge count: %zu", edges.size());
    if (!labels.empty()) {
        ImGui::TextWrapped("Labels: %s", joinLabels(labels).c_str());
    } else {
        ImGui::TextDisabled("Labels unavailable");
    }
}

void drawHeadingContractSection(const FrameDebugWindowContext& context) {
    const auto& heading_spec = context.zarr_loader.getHeadingComputationSpec();
    ImGui::Text("Heading Contract:");
    ImGui::Text("Source: %s", headingSourceLabel(heading_spec));

    if (!heading_spec.available) {
        ImGui::TextDisabled("Heading metadata unavailable");
        return;
    }

    ImGui::Text("Enabled: %s", heading_spec.enabled ? "Yes" : "No");
    if (!heading_spec.enabled) {
        ImGui::TextDisabled("Heading updates are disabled by the resolved contract");
        return;
    }

    if (heading_spec.legacy_fallback) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.25f, 1.0f),
                           "Using legacy fallback inference");
    }
    ImGui::TextWrapped("Dependent keypoints: %s",
                       joinLabels(heading_spec.dependent_labels).c_str());
    ImGui::TextWrapped("Origin: %s",
                       formatPointSpec(heading_spec.origin).c_str());
    ImGui::TextWrapped("Direction: %s -> %s",
                       formatPointSpec(heading_spec.direction_from).c_str(),
                       formatPointSpec(heading_spec.direction_to).c_str());
}

void drawFrameOverviewSection(const FrameDebugWindowContext& context) {
    ImGui::Text("Inspecting Frame: %d", context.current_frame_num);
    if (context.zarr_loader.hasStimulusAlignment()) {
        if (auto current_stimulus_frame =
                context.zarr_loader.getStimulusFrameForCameraFrame(
                    context.current_frame_num)) {
            ImGui::Text("Stimulus frame: %d", *current_stimulus_frame);
        } else {
            ImGui::TextDisabled("Stimulus frame: not mapped");
        }
    }
    ImGui::Separator();
}

void drawDatasetSelectionSection(const FrameDebugWindowContext& context,
                                 FrameDebugWindowResult& result) {
    if (context.detection_dataset_labels.empty()) {
        return;
    }

    ImGui::Text("Detection dataset:");
    const int clamped_choice = std::min<int>(
        context.detection_dataset_choice,
        static_cast<int>(context.detection_dataset_labels.size()) - 1);
    const char* current_label =
        context.detection_dataset_labels[clamped_choice].c_str();
    if (ImGui::BeginCombo("##detection_dataset_combo", current_label)) {
        for (int i = 0;
             i < static_cast<int>(context.detection_dataset_labels.size());
             ++i) {
            bool selected = (i == context.detection_dataset_choice);
            if (ImGui::Selectable(context.detection_dataset_labels[i].c_str(),
                                  selected)) {
                result.requested_detection_dataset_index = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void drawDetectionSummarySection(const FrameDebugWindowContext& context) {
    if (!context.zarr_loader.hasDetectionData()) {
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.25f, 1.0f),
                           "[Zarr] Detection runs: unavailable (metadata/stimulus-only mode)");
        return;
    }

    if (!context.zarr_boxes.empty()) {
        if (context.frame_is_interpolated &&
            context.dataset_has_synthetic_boxes) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.7f, 0.0f, 1.0f),
                "[Zarr] Detections: Found %zu (INTERPOLATED)",
                context.zarr_boxes.size());
        } else if (context.frame_is_interpolated &&
                   !context.dataset_has_synthetic_boxes) {
            ImGui::TextColored(
                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                "[Zarr] Detections: Found %zu (original, interp available)",
                context.zarr_boxes.size());
        } else {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                               "[Zarr] Detections: Found %zu",
                               context.zarr_boxes.size());
        }

        if (context.detection_details != nullptr &&
            context.zarr_loader.hasScores() &&
            !context.detection_details->scores.empty()) {
            float max_score = *std::max_element(
                context.detection_details->scores.begin(),
                context.detection_details->scores.end());
            ImGui::Text("Max confidence: %.2f", max_score);
        }

        if (context.zarr_loader.hasClassIDs()) {
            ImGui::Text("Has class IDs: Yes");
        }

        if (context.zarr_loader.hasHeadingData()) {
            if (!context.dataset_has_synthetic_boxes &&
                context.detection_details != nullptr &&
                !context.detection_details->heading_valid.empty()) {
                size_t valid_headings =
                    std::count(context.detection_details->heading_valid.begin(),
                               context.detection_details->heading_valid.end(),
                               1);
                ImGui::Text("Heading vectors: %zu valid", valid_headings);
            } else {
                ImGui::Text(
                    "Heading vectors available (use original detections)");
            }
        }

        if (context.zarr_loader.hasInterpolation()) {
            ImGui::Text("Interpolation available: Yes");
            ImGui::Text("Current frame interpolated: %s",
                        context.frame_is_interpolated ? "Yes" : "No");
            ImGui::Text("Using interpolation: %s",
                        context.dataset_has_synthetic_boxes ? "Yes" : "No");
            ImGui::Text("Method: %s",
                        context.zarr_loader.getInterpolationMethod().c_str());
        }
    } else {
        if (context.zarr_loader.hasInterpolation() &&
            context.frame_is_interpolated) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                "[Zarr] Detections: None (frame is interpolated)");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                               "[Zarr] Detections: None");
        }
    }
}

void drawDetectionTab(
    const FrameDebugWindowContext& context,
    FrameDebugWindowResult& result,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    drawDatasetSelectionSection(context, result);
    if (artifact != nullptr || context.zarr_loader.hasDetectionData()) {
        ImGui::Separator();
        ImGui::Text("Detection Review:");
        drawReviewArtifactBlock(artifact);
    }
    ImGui::Separator();
    drawDetectionSummarySection(context);
}

void drawKeypointTab(
    const FrameDebugWindowContext& context,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    if (!context.zarr_loader.hasKeypointData() && artifact == nullptr) {
        ImGui::TextDisabled("Keypoint data unavailable");
        return;
    }

    if (context.zarr_loader.hasKeypointData()) {
        ImGui::Text("Run: %s",
                    context.zarr_loader.getKeypointsRunName().c_str());
        ImGui::Text("Refined keypoints: %s",
                    context.zarr_loader.isRefinedKeypoints() ? "Yes" : "No");
        ImGui::Separator();
        drawKeypointSkeletonSection(context);
        ImGui::Separator();
        drawHeadingContractSection(context);

        if (context.detection_details != nullptr &&
            context.detection_details->has_keypoints) {
            size_t detections_with_keypoints = 0;
            for (const auto& keypoints :
                 context.detection_details->keypoints_pixels) {
                if (!keypoints.empty()) {
                    ++detections_with_keypoints;
                }
            }
            ImGui::Text("Current frame detections with keypoints: %zu",
                        detections_with_keypoints);
            if (context.detection_details->keypoints_per_detection > 0) {
                ImGui::Text("Keypoints per detection: %zu",
                            context.detection_details->keypoints_per_detection);
            }
        }
    } else {
        ImGui::TextDisabled("Keypoint arrays unavailable for current dataset");
    }

    ImGui::Separator();
    ImGui::Text("Keypoint Review:");
    drawReviewArtifactBlock(artifact);
}

void drawSubjectMaskEditPreviewSection(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state) {
    if (!context.zarr_loader.eyeMasksUseRefinedSubjectMasks()) {
        return;
    }

    if (context.zarr_loader.getRefinedSubjectMaskOverlayComponents().empty()) {
        return;
    }

    if (state.subject_mask_edit_session.active() &&
        state.subject_mask_edit_session.target().zarr_path !=
            context.zarr_loader.getArchivePath()) {
        state.subject_mask_edit_session.clear();
        state.subject_mask_edit_status.clear();
    }

    ImGui::Separator();
    ImGui::Text("Subject Mask Editor:");
    ImGui::Checkbox("Enable canvas mask selection",
                    &state.subject_mask_canvas_pick_enabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When enabled, left-clicking visible subject masks in the camera view selects an edit target. Turn this off to leave canvas clicks available for keypoint editing or navigation while masks stay visible.");
    }

    const auto* masks =
        (context.detection_details != nullptr &&
         context.detection_details->includes_eye_masks)
            ? &context.detection_details->eye_masks
            : nullptr;
    std::vector<int> editable_target_indices;
    if (masks != nullptr) {
        for (int idx = 0; idx < static_cast<int>(masks->size()); ++idx) {
            if ((*masks)[static_cast<size_t>(idx)].roi_index >= 0) {
                editable_target_indices.push_back(idx);
            }
        }
    }

    if (masks == nullptr || editable_target_indices.empty()) {
        ImGui::TextDisabled("No editable subject-mask rows on this frame.");
    } else {
        const bool selected_target_on_frame = std::find(
            editable_target_indices.begin(),
            editable_target_indices.end(),
            state.subject_mask_edit_detection_index) !=
            editable_target_indices.end();
        if (!selected_target_on_frame) {
            state.subject_mask_edit_detection_index =
                editable_target_indices.front();
        }

        ImGuiTableFlags table_flags =
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##subject_mask_target_table",
                              4,
                              table_flags)) {
            ImGui::TableSetupColumn("Detection");
            ImGui::TableSetupColumn("ROI row");
            ImGui::TableSetupColumn("Components");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();

            for (int idx : editable_target_indices) {
                const auto& mask = (*masks)[static_cast<size_t>(idx)];
                const bool active_target =
                    state.subject_mask_edit_session.active() &&
                    state.subject_mask_edit_session.target().roi_index ==
                        mask.roi_index;
                const bool selected_row =
                    idx == state.subject_mask_edit_detection_index ||
                    active_target;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string label = "Detection " + std::to_string(idx);
                if (ImGui::Selectable(label.c_str(), selected_row)) {
                    std::string component_name =
                        state.subject_mask_edit_component_name;
                    if (!maskHasComponent(mask, component_name)) {
                        component_name = firstEditableComponent(mask);
                    }
                    loadSubjectMaskEditTarget(context,
                                              state,
                                              idx,
                                              mask.roi_index,
                                              component_name);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", mask.roi_index);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", componentSummary(mask).c_str());
                ImGui::TableSetColumnIndex(3);
                if (active_target &&
                    state.subject_mask_edit_session.dirty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f),
                                       "dirty");
                } else if (active_target) {
                    ImGui::Text("preview");
                } else {
                    ImGui::TextDisabled("clean");
                }
            }
            ImGui::EndTable();
        }

        const auto& selected_mask =
            (*masks)[static_cast<size_t>(
                state.subject_mask_edit_detection_index)];
        ImGui::Text("Component:");
        bool first_button = true;
        for (const char* label :
             {"subject_body", "eye_left", "eye_right", "swim_bladder"}) {
            const bool available = maskHasComponent(selected_mask, label);
            const bool selected =
                state.subject_mask_edit_component_name == label &&
                state.subject_mask_edit_session.active() &&
                state.subject_mask_edit_session.target().roi_index ==
                    selected_mask.roi_index;
            if (!first_button) {
                ImGui::SameLine();
            }
            first_button = false;
            ImGui::BeginDisabled(!available);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.34f, 0.34f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(0.42f, 0.42f, 0.24f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(0.48f, 0.48f, 0.28f, 1.0f));
            }
            if (ImGui::Button(shortSubjectMaskComponentLabel(label).c_str())) {
                loadSubjectMaskEditTarget(
                    context,
                    state,
                    state.subject_mask_edit_detection_index,
                    selected_mask.roi_index,
                    label);
            }
            if (selected) {
                ImGui::PopStyleColor(3);
            }
            ImGui::EndDisabled();
        }
    }

    if (state.subject_mask_edit_session.active()) {
        if (ImGui::Button("Reset Preview")) {
            state.subject_mask_edit_session.resetPreview();
            state.subject_mask_edit_status =
                "Preview reset to loaded mask row.";
        }

        const auto& target = state.subject_mask_edit_session.target();
        ImGui::Text("Active: %s row %d",
                    target.component_name.c_str(),
                    target.roi_index);
        ImGui::Text("Shape: %zux%zu | Dirty: %s",
                    target.rows,
                    target.cols,
                    state.subject_mask_edit_session.dirty() ? "yes" : "no");
        ImGui::TextDisabled("Save backend: PreviewOnly");
        ImGui::BeginDisabled(true);
        ImGui::Button("Save");
        ImGui::EndDisabled();
    }

    if (!state.subject_mask_edit_status.empty()) {
        ImGui::TextWrapped("%s", state.subject_mask_edit_status.c_str());
    }
}

void drawSubjectShapeQcSection(const FrameDebugWindowContext& context,
                               FrameDebugWindowState& state,
                               FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasSubjectShapeData()) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Subject Shape QC:");
    ImGui::Text("Rows: %zu | Run: %s",
                context.zarr_loader.getSubjectShapeRowCount(),
                context.zarr_loader.getSubjectShapeRunName().c_str());

    auto filters = state.subject_shape_qc_filters;
    bool changed = false;
    changed |= ImGui::Checkbox("Any invalid", &filters.any_invalid);
    changed |= ImGui::Checkbox("Source mask QC failure",
                               &filters.source_mask_qc_failure);
    changed |= ImGui::Checkbox("Body frame invalid",
                               &filters.body_frame_invalid);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Snout invalid", &filters.snout_invalid);
    changed |= ImGui::Checkbox("Centerline invalid",
                               &filters.centerline_invalid);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Centerline misses snout",
                               &filters.centerline_misses_snout);
    changed |= ImGui::Checkbox("B-spline invalid",
                               &filters.bspline_invalid);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Tail base invalid",
                               &filters.tail_base_invalid);
    changed |= ImGui::Checkbox("Tail samples invalid",
                               &filters.tail_sample_invalid);
    if (ImGui::InputText("Reason contains",
                         state.subject_shape_reason_filter.data(),
                         state.subject_shape_reason_filter.size())) {
        changed = true;
    }
    if (changed) {
        filters.reason_substring =
            state.subject_shape_reason_filter.data();
        state.subject_shape_qc_filters = filters;
        state.subject_shape_qc_status.clear();
    } else {
        state.subject_shape_qc_filters.reason_substring =
            state.subject_shape_reason_filter.data();
    }

    if (ImGui::Button("Prev Shape QC Frame")) {
        result.request_prev_subject_shape_qc_frame = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Shape QC Frame")) {
        result.request_next_subject_shape_qc_frame = true;
    }
    if (!state.subject_shape_qc_status.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                           "%s",
                           state.subject_shape_qc_status.c_str());
    }
}

void drawEyeMaskTab(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state,
    FrameDebugWindowResult& result,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    if (!context.zarr_loader.hasEyeMasks() &&
        !context.zarr_loader.hasSubjectShapeData() &&
        artifact == nullptr) {
        ImGui::TextDisabled("Subject mask data unavailable");
        return;
    }

    if (context.zarr_loader.hasEyeMasks()) {
        if (!context.zarr_loader.getEyeMaskSourceLabel().empty()) {
            ImGui::Text("Source: %s",
                        context.zarr_loader.getEyeMaskSourceLabel().c_str());
        }
        if (!context.zarr_loader.getEyeMaskSourcePath().empty()) {
            ImGui::TextWrapped("Dataset: %s",
                               context.zarr_loader.getEyeMaskSourcePath().c_str());
        } else {
            ImGui::Text("Run: %s",
                        context.zarr_loader.getEyeMaskRunName().c_str());
        }
        if (context.zarr_loader.eyeMasksUseRefinedSubjectMasks()) {
            const auto& labels = context.zarr_loader.getEyeMaskChannelLabels();
            const auto& channels = context.zarr_loader.getEyeMaskChannelIndices();
            const std::string left_channel =
                channels[0] == std::numeric_limits<size_t>::max()
                    ? "unavailable"
                    : std::to_string(channels[0]);
            const std::string right_channel =
                channels[1] == std::numeric_limits<size_t>::max()
                    ? "unavailable"
                    : std::to_string(channels[1]);
            ImGui::Text("Channels: %s=%s, %s=%s",
                        labels[0].c_str(),
                        left_channel.c_str(),
                        labels[1].c_str(),
                        right_channel.c_str());
        }
        if (!context.zarr_loader.getEyeMaskWarning().empty()) {
            ImGui::TextWrapped("Warning: %s",
                               context.zarr_loader.getEyeMaskWarning().c_str());
        }
        if (context.detection_details != nullptr &&
            context.detection_details->includes_eye_masks) {
            size_t valid_masks = 0;
            size_t masks_with_axes = 0;
            size_t masks_with_angle_labels = 0;
            size_t masks_with_subject_body = 0;
            size_t masks_with_swim_bladder = 0;
            size_t masks_with_component_contours = 0;
            for (const auto& mask : context.detection_details->eye_masks) {
                if (mask.valid) {
                    ++valid_masks;
                }
                if (mask.has_feret_axes) {
                    ++masks_with_axes;
                }
                if (mask.has_eye_angles &&
                    ((mask.feret_angle_valid[0] != 0) ||
                     (mask.feret_angle_valid[1] != 0))) {
                    ++masks_with_angle_labels;
                }
                for (const auto& component : mask.subject_mask_components) {
                    if (!component.valid) {
                        continue;
                    }
                    if (component.label == "subject_body") {
                        ++masks_with_subject_body;
                    } else if (component.label == "swim_bladder") {
                        ++masks_with_swim_bladder;
                    }
                    if (component.has_contour) {
                        ++masks_with_component_contours;
                    }
                }
            }
            ImGui::Text("Current frame valid masks: %zu", valid_masks);
            ImGui::Text("Current frame masks with axes: %zu", masks_with_axes);
            ImGui::Text("Current frame masks with angle labels: %zu",
                        masks_with_angle_labels);
            if (context.zarr_loader.eyeMasksUseRefinedSubjectMasks()) {
                ImGui::Text("Current frame body/swim bladder masks: %zu / %zu",
                            masks_with_subject_body,
                            masks_with_swim_bladder);
                ImGui::Text("Current frame component contours: %zu",
                            masks_with_component_contours);
            }
        } else {
            ImGui::TextDisabled("%s", unavailableMaskDetailsText(context));
        }
        if (context.zarr_loader.hasEyeAngleData()) {
            ImGui::Text("Angle run: %s",
                        context.zarr_loader.getEyeAngleRunName().c_str());
        }
        drawSubjectMaskEditPreviewSection(context, state);
    } else {
        ImGui::TextDisabled("Eye mask arrays unavailable for current dataset");
    }
    drawSubjectShapeQcSection(context, state, result);

    ImGui::Separator();
    ImGui::Text("%s", maskReviewTitle(context));
    drawReviewArtifactBlock(artifact);
}

std::string tailReasonAt(
    const ZarrDetectionData::TailKinematicsData& tail,
    size_t row) {
    if (row < tail.failure_reason.size()) {
        return tail.failure_reason[row];
    }
    return {};
}

void drawTailSeriesPlot(const char* title,
                        const std::vector<int32_t>& frame_index,
                        const std::vector<float>& values,
                        int current_frame_num) {
    if (values.empty()) {
        ImGui::TextDisabled("%s unavailable", title);
        return;
    }
    std::vector<double> xs;
    std::vector<double> ys;
    const size_t count = values.size();
    xs.reserve(count);
    ys.reserve(count);
    for (size_t row = 0; row < count; ++row) {
        const float value = values[row];
        if (!std::isfinite(value)) {
            continue;
        }
        const double x =
            (row < frame_index.size() && frame_index[row] >= 0)
                ? static_cast<double>(frame_index[row])
                : static_cast<double>(row);
        xs.push_back(x);
        ys.push_back(static_cast<double>(value));
    }
    if (xs.size() < 2) {
        ImGui::TextDisabled("%s has no finite values", title);
        return;
    }

    if (ImPlot::BeginPlot(title, ImVec2(-1.0f, 190.0f))) {
        ImPlot::SetupAxes("Frame", title, ImPlotAxisFlags_AutoFit,
                          ImPlotAxisFlags_AutoFit);
        ImPlot::PlotLine(title,
                         xs.data(),
                         ys.data(),
                         static_cast<int>(xs.size()));
        const double current_x[2] = {
            static_cast<double>(current_frame_num),
            static_cast<double>(current_frame_num)};
        const double current_y[2] = {
            *std::min_element(ys.begin(), ys.end()),
            *std::max_element(ys.begin(), ys.end())};
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 0.8f), 1.5f);
        ImPlot::PlotLine("Current Frame", current_x, current_y, 2);
        ImPlot::EndPlot();
    }
}

void drawTailKinematicsTab(const FrameDebugWindowContext& context,
                           FrameDebugWindowState& state,
                           FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasTailKinematicsData()) {
        ImGui::TextDisabled("Tail kinematics data unavailable");
        return;
    }

    const auto& tail = context.zarr_loader.getTailKinematicsData();
    ImGui::Text("Run: %s", tail.run_name.c_str());
    ImGui::Text("Source subject shape: %s",
                tail.source_subject_shape_run.empty()
                    ? "<unknown>"
                    : tail.source_subject_shape_run.c_str());
    if (!tail.source_refined_subject_masks_run.empty()) {
        ImGui::Text("Source refined masks: %s",
                    tail.source_refined_subject_masks_run.c_str());
    }
    ImGui::Text("Rows: %zu | Samples: %zu", tail.row_count,
                tail.sample_count);
    if (!tail.warning.empty()) {
        ImGui::TextWrapped("Warning: %s", tail.warning.c_str());
    }

    auto overlay_options = context.tail_kinematics_overlay_options;
    ImGui::Separator();
    ImGui::Text("Tail Kinematics Overlay:");
    ImGui::Checkbox("Show k=10 tail-angle samples",
                    &overlay_options.show_overlay);
    ImGui::BeginDisabled(!overlay_options.show_overlay);
    ImGui::Checkbox("Tail-angle samples k=10 (analysis output)",
                    &overlay_options.show_samples);
    ImGui::Checkbox("Connect k=10 samples", &overlay_options.show_segments);
    ImGui::Checkbox("Tail angle vectors",
                    &overlay_options.show_angle_vectors);
    ImGui::Checkbox("Lateral deflection",
                    &overlay_options.show_lateral_deflection);
    ImGui::Checkbox("Color invalid frames",
                    &overlay_options.color_invalid_frames);
    ImGui::EndDisabled();
    result.tail_kinematics_overlay_options = overlay_options;

    if (auto current_row =
            context.zarr_loader.findTailKinematicsRowForFrame(
                context.current_frame_num)) {
        state.tail_kinematics_selected_row =
            static_cast<int>(*current_row);
        const bool current_valid =
            *current_row < tail.valid.size() && tail.valid[*current_row] != 0;
        ImGui::Text("Current frame tail row: %zu (%s)",
                    *current_row,
                    current_valid ? "valid" : "invalid");
        const std::string reason = tailReasonAt(tail, *current_row);
        if (!reason.empty()) {
            ImGui::TextWrapped("Current reason: %s", reason.c_str());
        }
    } else {
        ImGui::TextDisabled("No tail row mapped to current frame");
    }

    int selected_row = state.tail_kinematics_selected_row;
    if (ImGui::InputInt("Selected tail row", &selected_row)) {
        selected_row = std::clamp(
            selected_row,
            -1,
            tail.row_count == 0
                ? -1
                : static_cast<int>(tail.row_count - 1));
        state.tail_kinematics_selected_row = selected_row;
    }
    if (state.tail_kinematics_selected_row >= 0) {
        const size_t row =
            static_cast<size_t>(state.tail_kinematics_selected_row);
        if (row < tail.row_to_frame.size()) {
            ImGui::Text("Selected row frame: %d", tail.row_to_frame[row]);
        }
        const std::string reason = tailReasonAt(tail, row);
        if (!reason.empty()) {
            ImGui::TextWrapped("Selected reason: %s", reason.c_str());
        }
        if (ImGui::Button("Seek Selected Tail Row")) {
            result.request_seek_tail_kinematics_row = true;
            result.requested_tail_kinematics_row = row;
        }
        ImGui::SameLine();
        if (ImGui::Button("Show In Subject Shape")) {
            result.request_seek_tail_kinematics_row = true;
            result.requested_tail_kinematics_row = row;
            state.active_tab = FrameInspectTab::EyeMasks;
        }
    }

    ImGui::Separator();
    ImGui::Text("Tail Kinematics QC:");
    auto filters = state.tail_kinematics_qc_filters;
    bool changed = false;
    changed |= ImGui::Checkbox("Invalid rows", &filters.invalid_rows);
    changed |= ImGui::Checkbox("Non-finite tail tip angle",
                               &filters.nonfinite_tail_tip_angle);
    changed |= ImGui::Checkbox("Non-finite tail tip deflection",
                               &filters.nonfinite_tail_tip_lateral_deflection);
    if (ImGui::InputText("Tail reason contains",
                         state.tail_kinematics_reason_filter.data(),
                         state.tail_kinematics_reason_filter.size())) {
        changed = true;
    }
    if (changed) {
        filters.reason_substring =
            state.tail_kinematics_reason_filter.data();
        state.tail_kinematics_qc_filters = filters;
        state.tail_kinematics_qc_status.clear();
    } else {
        state.tail_kinematics_qc_filters.reason_substring =
            state.tail_kinematics_reason_filter.data();
    }
    if (ImGui::Button("Prev Tail QC Frame")) {
        result.request_prev_tail_kinematics_qc_frame = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Tail QC Frame")) {
        result.request_next_tail_kinematics_qc_frame = true;
    }
    if (!state.tail_kinematics_qc_status.empty()) {
        ImGui::TextWrapped("%s", state.tail_kinematics_qc_status.c_str());
    }

    ImGui::Separator();
    drawTailSeriesPlot("Tail Tip Angle (deg)",
                       tail.frame_index,
                       tail.tail_tip_angle_deg,
                       context.current_frame_num);
    drawTailSeriesPlot("Tail Tip Lateral Deflection (px)",
                       tail.frame_index,
                       tail.tail_tip_lateral_deflection_px,
                       context.current_frame_num);
    if (!tail.max_abs_tail_curvature_px_inv.empty()) {
        drawTailSeriesPlot("Max Abs Tail Curvature (px^-1)",
                           tail.frame_index,
                           tail.max_abs_tail_curvature_px_inv,
                           context.current_frame_num);
    }
}

const ZarrDetectionData::EyeAngleRepresentationInfo*
findEyeAngleRepresentation(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    int index) {
    if (index < 0 ||
        static_cast<size_t>(index) >= eye.representations.size()) {
        return nullptr;
    }
    return &eye.representations[static_cast<size_t>(index)];
}

std::string eyeReasonAt(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    size_t row) {
    if (row < eye.roi_reason_labels.size()) {
        return eye.roi_reason_labels[row];
    }
    return {};
}

const ZarrDetectionData::EyeAngleScalarField* resolveEyeAnglePlotField(
    const ZarrDetectionLoader& loader,
    const std::string& requested_field,
    std::string& resolved_name) {
    resolved_name = requested_field;
    const auto* field = loader.findEyeAngleScalarField(requested_field);
    if (field != nullptr && (field->has_frame || field->has_roi)) {
        return field;
    }
    const std::string base = eyeUnsmoothedBaseField(requested_field);
    if (!base.empty()) {
        field = loader.findEyeAngleScalarField(base);
        if (field != nullptr && (field->has_frame || field->has_roi)) {
            resolved_name = base;
            return field;
        }
    }
    return nullptr;
}

void drawEyeAngleSeriesPlot(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleScalarField& field,
    const std::string& requested_name,
    const std::string& resolved_name,
    int current_frame_num) {
    const bool use_frame = field.has_frame && !field.frame_values.empty();
    const auto& values = use_frame ? field.frame_values : field.roi_values;
    if (values.empty()) {
        ImGui::TextDisabled("%s unavailable", requested_name.c_str());
        return;
    }

    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(values.size());
    ys.reserve(values.size());
    for (size_t row = 0; row < values.size(); ++row) {
        const float value = values[row];
        if (!std::isfinite(value)) {
            continue;
        }
        double x = static_cast<double>(row);
        if (row < eye.roi_frame_indices.size() &&
            eye.roi_frame_indices[row] >= 0) {
            x = static_cast<double>(eye.roi_frame_indices[row]);
        }
        xs.push_back(x);
        ys.push_back(static_cast<double>(value));
    }
    if (xs.size() < 2) {
        ImGui::TextDisabled("%s has no finite values", requested_name.c_str());
        return;
    }

    std::string title = requested_name;
    if (resolved_name != requested_name) {
        title += " (fallback: " + resolved_name + ")";
    }
    if (ImPlot::BeginPlot(title.c_str(), ImVec2(-1.0f, 170.0f))) {
        ImPlot::SetupAxes("Frame", field.units.empty() ? "deg" : field.units.c_str(),
                          ImPlotAxisFlags_AutoFit,
                          ImPlotAxisFlags_AutoFit);
        ImPlot::PlotLine(resolved_name.c_str(),
                         xs.data(),
                         ys.data(),
                         static_cast<int>(xs.size()));
        const double current_x[2] = {
            static_cast<double>(current_frame_num),
            static_cast<double>(current_frame_num)};
        const double current_y[2] = {
            *std::min_element(ys.begin(), ys.end()),
            *std::max_element(ys.begin(), ys.end())};
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 0.8f), 1.5f);
        ImPlot::PlotLine("Current Frame", current_x, current_y, 2);
        ImPlot::EndPlot();
    }
}

void drawEyeAngleCurrentValues(
    const FrameDebugWindowContext& context,
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep,
    size_t row) {
    ImGui::Text("Current eye-angle row: %zu", row);
    if (row < eye.row_to_frame.size()) {
        ImGui::Text("Row frame: %d", eye.row_to_frame[row]);
    }
    const bool left_valid =
        eye.roi_valid_left.empty() ||
        (row < eye.roi_valid_left.size() && eye.roi_valid_left[row] != 0);
    const bool right_valid =
        eye.roi_valid_right.empty() ||
        (row < eye.roi_valid_right.size() && eye.roi_valid_right[row] != 0);
    const bool frame_valid =
        eye.roi_valid_frame.empty() ||
        (row < eye.roi_valid_frame.size() && eye.roi_valid_frame[row] != 0);
    ImGui::Text("QA: left=%s right=%s frame=%s",
                left_valid ? "valid" : "invalid",
                right_valid ? "valid" : "invalid",
                frame_valid ? "valid" : "invalid");
    const bool marginal =
        (row < eye.roi_major_axis_marginal.size() &&
         eye.roi_major_axis_marginal[row] != 0) ||
        (row < eye.roi_left_major_axis_marginal.size() &&
         eye.roi_left_major_axis_marginal[row] != 0) ||
        (row < eye.roi_right_major_axis_marginal.size() &&
         eye.roi_right_major_axis_marginal[row] != 0);
    if (marginal) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "Major-axis marginal");
    }
    const std::string reason = eyeReasonAt(eye, row);
    if (!reason.empty()) {
        ImGui::TextWrapped("Reason: %s", reason.c_str());
    }

    auto draw_scalar_value = [&](const std::string& field_name) {
        const auto* field =
            context.zarr_loader.findEyeAngleScalarField(field_name);
        if (field == nullptr || !field->has_roi ||
            row >= field->roi_values.size()) {
            return;
        }
        const float value = field->roi_values[row];
        if (!std::isfinite(value)) {
            ImGui::Text("%s: NaN", field_name.c_str());
        } else {
            ImGui::Text("%s: %.2f %s",
                        field_name.c_str(),
                        value,
                        field->units.empty() ? "deg" : field->units.c_str());
        }
    };
    for (const auto& field : rep.primary_roi_fields) {
        draw_scalar_value(field);
    }
    for (const auto& field : rep.aggregate_roi_fields) {
        draw_scalar_value(field);
    }
    for (const auto& field_name : rep.vector_roi_fields) {
        const auto* field =
            context.zarr_loader.findEyeAngleVectorField(field_name);
        if (field == nullptr || !field->has_roi ||
            row >= field->roi_values.size()) {
            continue;
        }
        const auto value = field->roi_values[row];
        ImGui::Text("%s: [%.3f, %.3f]",
                    field_name.c_str(),
                    value[0],
                    value[1]);
    }
}

void drawEyeAngleTab(const FrameDebugWindowContext& context,
                     FrameDebugWindowState& state,
                     FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasEyeAngleAnalysisData()) {
        ImGui::TextDisabled("Eye-angle analysis data unavailable");
        return;
    }

    const auto& eye = context.zarr_loader.getEyeAngleAnalysisData();
    ImGui::Text("Run: %s", eye.run_name.c_str());
    ImGui::Text("Schema: %s v%d",
                eye.schema_id.empty() ? "<unknown>" : eye.schema_id.c_str(),
                eye.schema_version);
    ImGui::Text("Method: %s %s",
                eye.method.empty() ? "<unknown>" : eye.method.c_str(),
                eye.method_version.c_str());
    ImGui::Text("Source geometry: %s",
                eye.source_geometry_kind.empty()
                    ? "<unknown>"
                    : eye.source_geometry_kind.c_str());
    if (!eye.source_eye_geometry_run.empty()) {
        ImGui::Text("Source eye geometry run: %s",
                    eye.source_eye_geometry_run.c_str());
    }
    if (!eye.source_subject_shape_run.empty()) {
        ImGui::Text("Source subject shape: %s",
                    eye.source_subject_shape_run.c_str());
    }
    if (eye.variant_schema_inferred) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "Representation metadata inferred");
    }
    if (!eye.warning.empty()) {
        ImGui::TextWrapped("Warning: %s", eye.warning.c_str());
    }
    ImGui::Text("Rows: %zu | Frame rows: %zu | Scalars: %zu | Vectors: %zu",
                eye.row_count,
                eye.frame_count,
                eye.scalar_fields.size(),
                eye.vector_fields.size());

    if (eye.representations.empty()) {
        ImGui::TextDisabled("No eye-angle representations available");
        return;
    }
    if (state.eye_angle_representation_index < 0 ||
        static_cast<size_t>(state.eye_angle_representation_index) >=
            eye.representations.size()) {
        state.eye_angle_representation_index = 0;
        for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
            if (eye.representations[idx].key == eye.default_representation) {
                state.eye_angle_representation_index =
                    static_cast<int>(idx);
                break;
            }
        }
    }
    const auto* selected_rep = findEyeAngleRepresentation(
        eye, state.eye_angle_representation_index);
    const char* preview =
        selected_rep == nullptr
            ? "<none>"
            : (selected_rep->display_name.empty()
                   ? selected_rep->key.c_str()
                   : selected_rep->display_name.c_str());
    if (ImGui::BeginCombo("Representation", preview)) {
        for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
            const auto& rep = eye.representations[idx];
            const bool selected =
                static_cast<int>(idx) == state.eye_angle_representation_index;
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
    selected_rep = findEyeAngleRepresentation(
        eye, state.eye_angle_representation_index);
    if (selected_rep == nullptr) {
        return;
    }
    if (!selected_rep->role.empty() || !selected_rep->axis.empty()) {
        ImGui::Text("Role: %s | Axis: %s",
                    selected_rep->role.c_str(),
                    selected_rep->axis.c_str());
    }

    if (auto current_row =
            context.zarr_loader.findEyeAngleRowForFrame(
                context.current_frame_num)) {
        state.eye_angle_selected_row = static_cast<int>(*current_row);
        drawEyeAngleCurrentValues(context, eye, *selected_rep, *current_row);
    } else {
        ImGui::TextDisabled("No eye-angle row mapped to current frame");
    }

    int selected_row = state.eye_angle_selected_row;
    if (ImGui::InputInt("Selected eye-angle row", &selected_row)) {
        selected_row = std::clamp(
            selected_row,
            -1,
            eye.row_count == 0
                ? -1
                : static_cast<int>(eye.row_count - 1));
        state.eye_angle_selected_row = selected_row;
    }
    if (state.eye_angle_selected_row >= 0) {
        const size_t row = static_cast<size_t>(state.eye_angle_selected_row);
        if (row < eye.row_to_frame.size()) {
            ImGui::Text("Selected row frame: %d", eye.row_to_frame[row]);
        }
        const std::string reason = eyeReasonAt(eye, row);
        if (!reason.empty()) {
            ImGui::TextWrapped("Selected reason: %s", reason.c_str());
        }
        if (ImGui::Button("Seek Selected Eye Row")) {
            result.request_seek_eye_angle_row = true;
            result.requested_eye_angle_row = row;
        }
    }

    ImGui::Separator();
    ImGui::Text("Eye-Angle QC:");
    auto filters = state.eye_angle_qc_filters;
    bool changed = false;
    changed |= ImGui::Checkbox("Invalid rows", &filters.invalid_rows);
    changed |= ImGui::Checkbox("Major-axis marginal",
                               &filters.major_axis_marginal);
    if (ImGui::InputText("Eye reason contains",
                         state.eye_angle_reason_filter.data(),
                         state.eye_angle_reason_filter.size())) {
        changed = true;
    }
    if (changed) {
        filters.reason_substring = state.eye_angle_reason_filter.data();
        state.eye_angle_qc_filters = filters;
        state.eye_angle_qc_status.clear();
    } else {
        state.eye_angle_qc_filters.reason_substring =
            state.eye_angle_reason_filter.data();
    }
    if (ImGui::Button("Prev Eye QC Frame")) {
        result.request_prev_eye_angle_qc_frame = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Eye QC Frame")) {
        result.request_next_eye_angle_qc_frame = true;
    }
    if (!state.eye_angle_qc_status.empty()) {
        ImGui::TextWrapped("%s", state.eye_angle_qc_status.c_str());
    }

    ImGui::Separator();
    const auto& plot_fields = selected_rep->default_plot_fields.empty()
                                  ? selected_rep->primary_roi_fields
                                  : selected_rep->default_plot_fields;
    if (plot_fields.empty()) {
        ImGui::TextDisabled("No default plot fields for this representation");
    }
    for (const auto& requested_field : plot_fields) {
        std::string resolved_name;
        const auto* field = resolveEyeAnglePlotField(
            context.zarr_loader, requested_field, resolved_name);
        if (field == nullptr) {
            ImGui::TextDisabled("%s unavailable", requested_field.c_str());
            continue;
        }
        drawEyeAngleSeriesPlot(eye,
                               *field,
                               requested_field,
                               resolved_name,
                               context.current_frame_num);
    }
}

}  // namespace

void drawFrameDebugStatusPanel(const FrameDebugWindowContext& context,
                               FrameDebugWindowState& state,
                               FrameDebugWindowResult& result) {
    const auto review_artifacts = context.zarr_loader.getAvailableReviewArtifacts();
    const auto* detection_artifact = findReviewArtifact(
        review_artifacts, ZarrDetectionLoader::ReviewArtifactKind::Detection);
    const auto* keypoint_artifact = findReviewArtifact(
        review_artifacts, ZarrDetectionLoader::ReviewArtifactKind::Keypoint);
    const auto* eye_mask_artifact = findReviewArtifact(
        review_artifacts, ZarrDetectionLoader::ReviewArtifactKind::EyeMask);

    drawFrameOverviewSection(context);
    if (!ImGui::BeginTabBar("##frame_inspect_data_tabs")) {
        return;
    }

    if (context.zarr_loader.hasDetectionData() || detection_artifact != nullptr) {
        if (ImGui::BeginTabItem("Detect")) {
            state.active_tab = FrameInspectTab::Detect;
            drawDetectionTab(context, result, detection_artifact);
            ImGui::EndTabItem();
        }
    }

    if (context.zarr_loader.hasKeypointData() || keypoint_artifact != nullptr) {
        if (ImGui::BeginTabItem("Keypoints")) {
            state.active_tab = FrameInspectTab::Keypoints;
            drawKeypointTab(context, keypoint_artifact);
            ImGui::EndTabItem();
        }
    }

    if (context.zarr_loader.hasEyeMasks() ||
        context.zarr_loader.hasSubjectShapeData() ||
        eye_mask_artifact != nullptr) {
        if (ImGui::BeginTabItem(maskPanelTitle(context))) {
            state.active_tab = FrameInspectTab::EyeMasks;
            drawEyeMaskTab(context, state, result, eye_mask_artifact);
            ImGui::EndTabItem();
        }
    }

    if (context.zarr_loader.hasTailKinematicsData()) {
        if (ImGui::BeginTabItem("Tail Kinematics")) {
            state.active_tab = FrameInspectTab::TailKinematics;
            drawTailKinematicsTab(context, state, result);
            ImGui::EndTabItem();
        }
    }

    if (context.zarr_loader.hasEyeAngleAnalysisData()) {
        if (ImGui::BeginTabItem("Eye Angles")) {
            state.active_tab = FrameInspectTab::EyeAngles;
            drawEyeAngleTab(context, state, result);
            ImGui::EndTabItem();
        }
    }

    ImGui::EndTabBar();
}
