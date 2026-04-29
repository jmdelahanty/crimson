#include "gui/frame_debug_status_panel.h"

#include "imgui.h"

#include <algorithm>
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
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks()
               ? "Subject Masks"
               : "Eye Masks";
}

const char* maskReviewTitle(const FrameDebugWindowContext& context) {
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks()
               ? "Subject Mask Review:"
               : "Eye Mask Review:";
}

const char* unavailableMaskDetailsText(const FrameDebugWindowContext& context) {
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks()
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

void drawEyeMaskTab(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    if (!context.zarr_loader.hasEyeMasks() && artifact == nullptr) {
        ImGui::TextDisabled("Eye mask data unavailable");
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

    ImGui::Separator();
    ImGui::Text("%s", maskReviewTitle(context));
    drawReviewArtifactBlock(artifact);
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

    if (context.zarr_loader.hasEyeMasks() || eye_mask_artifact != nullptr) {
        if (ImGui::BeginTabItem(maskPanelTitle(context))) {
            state.active_tab = FrameInspectTab::EyeMasks;
            drawEyeMaskTab(context, state, eye_mask_artifact);
            ImGui::EndTabItem();
        }
    }

    ImGui::EndTabBar();
}
