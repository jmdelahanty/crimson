#include "gui/frame_debug_status_panel.h"

#include "gui/frame_debug_eye_angle_tab.h"
#include "gui/frame_debug_subject_mask_tab.h"
#include "gui/frame_debug_tail_kinematics_tab.h"

#include "imgui.h"

#include <algorithm>
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

void drawActiveRoiInsetControls(FrameDebugWindowState& state) {
    ImGui::Text("ROI Inset:");
    ImGui::Checkbox("Show ROI inset",
                    &state.active_roi_inset_options.show_inset);
    ImGui::BeginDisabled(!state.active_roi_inset_options.show_inset);
    ImGui::Checkbox(
        "Mirror enabled overlays",
        &state.active_roi_inset_options.mirror_enabled_overlays);
    ImGui::Checkbox(
        "Heading-normalized view",
        &state.active_roi_inset_options.heading_normalized_view);
    ImGui::SliderFloat("ROI inset width",
                       &state.active_roi_inset_options.width_px,
                       120.0f,
                       420.0f,
                       "%.0f px");
    state.active_roi_inset_options.width_px =
        std::clamp(state.active_roi_inset_options.width_px,
                   120.0f,
                   420.0f);
    ImGui::Checkbox("ROI inset label",
                    &state.active_roi_inset_options.show_label);
    ImGui::EndDisabled();
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
    if (context.zarr_loaded) {
        drawActiveRoiInsetControls(state);
    }
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
        if (ImGui::BeginTabItem(frameDebugSubjectMaskTabTitle(context))) {
            state.active_tab = FrameInspectTab::EyeMasks;
            drawSubjectMaskTab(context, state, result, eye_mask_artifact);
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
