#include "gui/refined_keypoint_review_panel.h"

#include "imgui.h"
#include "zarr_loader.h"

namespace {

constexpr const char* kKeypointReviewUseItems[] = {"full_recording", "training"};
constexpr const char* kKeypointReviewStateItems[] = {
    "approved", "needs_review", "pending", "rejected"};
constexpr const char* kKeypointReviewMethodItems[] = {
    "manual", "algorithmic", "hybrid", "spotcheck"};

}  // namespace

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
            ImGui::TextDisabled(
                "Use Crop Preview to drag points, save corrections, or mark failures.");
        } else if (!selection.message.empty()) {
            ImGui::TextWrapped("%s", selection.message.c_str());
        }
    } else {
        ImGui::TextDisabled(
            "Select a detection to edit refined keypoints in Crop Preview.");
    }

    if (!can_write_kp_review) {
        ImGui::TextWrapped("%s", keypoint_edit_reason.c_str());
    }

    ImGui::BeginDisabled(!can_write_kp_review);
    ImGui::Combo("KP Intended Use##write",
                 &state.intended_use,
                 kKeypointReviewUseItems,
                 IM_ARRAYSIZE(kKeypointReviewUseItems));
    ImGui::Combo("KP Review State##write",
                 &state.review_state,
                 kKeypointReviewStateItems,
                 IM_ARRAYSIZE(kKeypointReviewStateItems));
    ImGui::Combo("KP Review Method##write",
                 &state.method,
                 kKeypointReviewMethodItems,
                 IM_ARRAYSIZE(kKeypointReviewMethodItems));
    ImGui::InputText("KP Reviewer##write",
                     state.reviewer.data(),
                     state.reviewer.size());
    ImGui::InputText("KP Notes##write",
                     state.notes.data(),
                     state.notes.size());
    if (ImGui::Button("Write Keypoint Review Status")) {
        result.request_review_write = true;
        result.review_options.intended_use =
            kKeypointReviewUseItems[state.intended_use];
        result.review_options.state =
            kKeypointReviewStateItems[state.review_state];
        result.review_options.method =
            kKeypointReviewMethodItems[state.method];
        result.review_options.reviewer = state.reviewer.data();
        result.review_options.notes = state.notes.data();
    }
    ImGui::EndDisabled();

    ImGui::TextWrapped(
        "  Writes keypoint_review_status on refined_keypoints_runs/<active> and updates the latest status pointer.");
    if (!state.review_write_status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                           "%s",
                           state.review_write_status.c_str());
    }
    if (!state.manual_write_status.empty()) {
        ImGui::TextColored(ImVec4(0.8f, 0.95f, 0.6f, 1.0f),
                           "%s",
                           state.manual_write_status.c_str());
    }

    return result;
}
