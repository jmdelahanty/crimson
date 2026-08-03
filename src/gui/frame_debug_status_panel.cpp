#include "gui/frame_debug_status_panel.h"

#include "gui/frame_debug_detection_adapter.h"
#include "gui/frame_debug_eye_angle_tab.h"
#include "gui/frame_debug_keypoint_adapter.h"
#include "gui/frame_debug_subject_mask_tab.h"
#include "gui/frame_debug_tail_kinematics_tab.h"
#include "gui/frame_inspect_detection_module.h"
#include "gui/frame_inspect_keypoint_module.h"

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
                    &state.active_roi_inset_options.visible);
    ImGui::BeginDisabled(!state.active_roi_inset_options.visible);
    bool match_camera_overlays =
        state.active_roi_inset_options.overlay_policy ==
        crimson::crop::RoiInsetOverlayPolicy::MatchCamera;
    if (ImGui::Checkbox("Match camera overlays", &match_camera_overlays)) {
        state.active_roi_inset_options.overlay_policy =
            match_camera_overlays
                ? crimson::crop::RoiInsetOverlayPolicy::MatchCamera
                : crimson::crop::RoiInsetOverlayPolicy::SelectedComponent;
    }
    bool heading_normalized =
        state.active_roi_inset_options.orientation ==
        crimson::crop::RoiInsetOrientation::HeadingNormalized;
    if (ImGui::Checkbox("Heading-normalized view", &heading_normalized)) {
        state.active_roi_inset_options.orientation =
            heading_normalized
                ? crimson::crop::RoiInsetOrientation::HeadingNormalized
                : crimson::crop::RoiInsetOrientation::Acquisition;
    }
    ImGui::SliderFloat("ROI inset width",
                       &state.active_roi_inset_options.width_px,
                       crimson::crop::kMinimumRoiInsetWidthPx,
                       crimson::crop::kMaximumRoiInsetWidthPx,
                       "%.0f px");
    state.active_roi_inset_options.width_px =
        std::clamp(state.active_roi_inset_options.width_px,
                   crimson::crop::kMinimumRoiInsetWidthPx,
                   crimson::crop::kMaximumRoiInsetWidthPx);
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

void drawDetectionTab(
    const FrameDebugWindowContext& context,
    FrameDebugWindowResult& result,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    drawDatasetSelectionSection(context, result);
    crimson::gui::DetectionInspectModuleState presentation_state;
    const auto presentation =
        makeFrameDebugDetectionInspectPresentation(context);
    crimson::gui::drawFrameInspectDetectionModule(presentation,
                                                   presentation_state);
    if (artifact != nullptr || context.zarr_loader.hasDetectionData()) {
        ImGui::Separator();
        ImGui::Text("Detection Review:");
        drawReviewArtifactBlock(artifact);
    }
}

void drawKeypointTab(
    const FrameDebugWindowContext& context,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    if (!context.zarr_loader.hasKeypointData() && artifact == nullptr) {
        ImGui::TextDisabled("Keypoint data unavailable");
        return;
    }

    if (context.zarr_loader.hasKeypointData()) {
        crimson::gui::KeypointInspectModuleState presentation_state;
        const auto presentation =
            makeFrameDebugKeypointInspectPresentation(context);
        crimson::gui::drawFrameInspectKeypointModule(presentation,
                                                     presentation_state);
        ImGui::Separator();
        drawKeypointSkeletonSection(context);
        ImGui::Separator();
        drawHeadingContractSection(context);
    } else {
        ImGui::TextDisabled("Keypoint arrays unavailable for current dataset");
    }

    ImGui::Separator();
    ImGui::Text("Keypoint Review:");
    drawReviewArtifactBlock(artifact);
}

}  // namespace

FrameDebugModuleCatalog buildFrameDebugModuleCatalog(
    const FrameDebugWindowContext& context) {
    return {context.zarr_loader.getAvailableReviewArtifacts()};
}

void drawFrameDebugStatusHeader(const FrameDebugWindowContext& context,
                                FrameDebugWindowState& state) {
    drawFrameOverviewSection(context);
    if (context.zarr_loaded) {
        drawActiveRoiInsetControls(state);
    }
}

bool frameDebugModuleAvailable(
    const FrameDebugWindowContext& context,
    const FrameDebugModuleCatalog& catalog,
    crimson::workspace::FrameInspectView view) {
    const auto* detection_artifact = findReviewArtifact(
        catalog.review_artifacts,
        ZarrDetectionLoader::ReviewArtifactKind::Detection);
    const auto* keypoint_artifact = findReviewArtifact(
        catalog.review_artifacts,
        ZarrDetectionLoader::ReviewArtifactKind::Keypoint);
    const auto* eye_mask_artifact = findReviewArtifact(
        catalog.review_artifacts,
        ZarrDetectionLoader::ReviewArtifactKind::EyeMask);

    switch (view) {
        case crimson::workspace::FrameInspectView::Detect:
            return context.zarr_loader.hasDetectionData() ||
                   detection_artifact != nullptr;
        case crimson::workspace::FrameInspectView::Keypoints:
            return context.zarr_loader.hasKeypointData() ||
                   keypoint_artifact != nullptr;
        case crimson::workspace::FrameInspectView::EyeMasks:
            return context.zarr_loader.hasEyeMasks() ||
                   context.zarr_loader.hasSubjectShapeData() ||
                   eye_mask_artifact != nullptr;
        case crimson::workspace::FrameInspectView::TailKinematics:
            return context.zarr_loader.hasTailKinematicsData();
        case crimson::workspace::FrameInspectView::EyeAngles:
            return context.zarr_loader.hasEyeAngleAnalysisData();
    }
    return false;
}

const char* frameDebugModuleLabel(
    const FrameDebugWindowContext& context,
    crimson::workspace::FrameInspectView view) {
    switch (view) {
        case crimson::workspace::FrameInspectView::Detect:
            return "Detect";
        case crimson::workspace::FrameInspectView::Keypoints:
            return "Keypoints";
        case crimson::workspace::FrameInspectView::EyeMasks:
            return frameDebugSubjectMaskTabTitle(context);
        case crimson::workspace::FrameInspectView::TailKinematics:
            return "Tail Kinematics";
        case crimson::workspace::FrameInspectView::EyeAngles:
            return "Eye Angles";
    }
    return "Inspect";
}

void drawFrameDebugStatusModule(
    const FrameDebugWindowContext& context,
    const FrameDebugModuleCatalog& catalog,
    FrameDebugWindowState& state,
    FrameDebugWindowResult& result,
    crimson::workspace::FrameInspectView view) {
    const auto* detection_artifact = findReviewArtifact(
        catalog.review_artifacts,
        ZarrDetectionLoader::ReviewArtifactKind::Detection);
    const auto* keypoint_artifact = findReviewArtifact(
        catalog.review_artifacts,
        ZarrDetectionLoader::ReviewArtifactKind::Keypoint);
    const auto* eye_mask_artifact = findReviewArtifact(
        catalog.review_artifacts,
        ZarrDetectionLoader::ReviewArtifactKind::EyeMask);

    switch (view) {
        case crimson::workspace::FrameInspectView::Detect:
            drawDetectionTab(context, result, detection_artifact);
            break;
        case crimson::workspace::FrameInspectView::Keypoints:
            drawKeypointTab(context, keypoint_artifact);
            break;
        case crimson::workspace::FrameInspectView::EyeMasks:
            drawSubjectMaskTab(context, state, result, eye_mask_artifact);
            break;
        case crimson::workspace::FrameInspectView::TailKinematics:
            drawTailKinematicsTab(context, state, result);
            break;
        case crimson::workspace::FrameInspectView::EyeAngles:
            drawEyeAngleTab(context, state, result);
            break;
    }
}
