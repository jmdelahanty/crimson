#include "gui/refined_keypoint_review_panel.h"

#include "gui/full_frame_keypoint_edit_overlay.h"
#include "gui/review_metadata_editor.h"
#include "imgui.h"
#include "zarr_loader.h"

RefinedKeypointReviewPanelResult drawRefinedKeypointReviewPanel(
    const RefinedKeypointReviewPanelContext& context,
    RefinedKeypointReviewPanelState& state) {
    RefinedKeypointReviewPanelResult result;

    if (!context.zarr_loader.hasKeypointData()) {
        return result;
    }

    ImGui::Separator();
    ImGui::Text("Keypoint Review Write:");

    RefinedKeypointRepository refined_keypoint_repo(context.zarr_loader);
    std::string keypoint_edit_reason;
    const bool can_write_kp_review =
        refined_keypoint_repo.canEditActiveRun(&keypoint_edit_reason);

    if (context.selected_frame == context.current_frame_num &&
        context.selected_box >= 0) {
        result.selected_selection =
            refined_keypoint_repo.resolveFrameDetectionSelection(
                static_cast<size_t>(context.current_frame_num),
                static_cast<size_t>(context.selected_box),
                false);
    }

    if (result.selected_selection.has_value()) {
        const auto& selection = *result.selected_selection;
        if (selection.valid) {
            ImGui::Text("Selected ROI: %d | detection %zu | run: %s",
                        selection.roi_index,
                        selection.detection_index,
                        selection.run_name.c_str());
        } else if (!selection.message.empty()) {
            ImGui::TextWrapped("%s", selection.message.c_str());
        }
    } else {
        ImGui::TextDisabled("Select a detection to edit refined keypoints.");
    }

    ImGui::Separator();
    ImGui::Text("Keypoint Edit:");

    const bool has_editable_selection =
        result.selected_selection.has_value() && result.selected_selection->valid &&
        result.selected_selection->editable;
    if (!has_editable_selection) {
        ImGui::TextDisabled(
            "Select a refined keypoint detection to edit in the main video or Crop Preview.");
    } else {
        ImGui::TextDisabled(
            "Drag selected points in the main video or use Crop Preview for precision adjustments.");
    }

    ImGui::BeginDisabled(!has_editable_selection);
    ImGui::Checkbox("Enable full-frame keypoint edit",
                    &state.full_frame_edit.enabled);
    ImGui::EndDisabled();
    if (state.full_frame_edit.enabled && context.play_video) {
        ImGui::TextDisabled("Pause playback to drag or save keypoints.");
    }
    if (state.full_frame_edit.enabled && state.full_frame_edit.dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                           "Unsaved full-frame keypoint changes.");
    }

    ImGui::BeginDisabled(!has_editable_selection || context.play_video);
    if (ImGui::Button("Save Keypoint Edit")) {
        std::string edit_error;
        if (!buildFullFrameKeypointEditAction(*result.selected_selection,
                                              state.full_frame_edit,
                                              result.edit_action,
                                              &edit_error)) {
            state.manual_write_status = edit_error;
        } else {
            state.manual_write_status.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Mark No Keypoints")) {
        state.manual_write_status.clear();
        result.edit_action.type = CropKeypointEditorActionType::MarkNoKeypoints;
    }
    ImGui::SameLine();
    if (ImGui::Button("Mark Detection Issue")) {
        state.manual_write_status.clear();
        result.edit_action.type =
            CropKeypointEditorActionType::MarkDetectionIssue;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Keypoint Edit")) {
        resetFullFrameKeypointEditState(state.full_frame_edit);
        state.manual_write_status = "Reset unsaved full-frame keypoint edits.";
    }
    ImGui::EndDisabled();

    if (!state.manual_write_status.empty()) {
        ImGui::TextColored(ImVec4(0.8f, 0.95f, 0.6f, 1.0f),
                           "%s",
                           state.manual_write_status.c_str());
    }

    if (!can_write_kp_review) {
        ImGui::TextWrapped("%s", keypoint_edit_reason.c_str());
    }

    ImGui::BeginDisabled(!can_write_kp_review);
    drawReviewMetadataEditor("kp_review_write", state.review_metadata);
    if (ImGui::Button("Write Keypoint Review Status")) {
        const auto metadata =
            resolveReviewMetadataValues(state.review_metadata);
        result.request_review_write = true;
        result.review_options.intended_use = metadata.intended_use;
        result.review_options.state = metadata.review_state;
        result.review_options.method = metadata.method;
        result.review_options.reviewer = metadata.reviewer;
        result.review_options.notes = metadata.notes;
    }
    ImGui::EndDisabled();

    ImGui::TextWrapped(
        "  Writes keypoint_review_status on refined_keypoints_runs/<active> and updates the latest status pointer.");
    if (!state.review_write_status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                           "%s",
                           state.review_write_status.c_str());
    }

    return result;
}
