#include "gui/refined_keypoint_review_window.h"

#include "imgui.h"

namespace {

void drawActiveReviewSummary(const ZarrDetectionLoader& zarr_loader) {
    if (!zarr_loader.hasKeypointData()) {
        ImGui::TextDisabled("No keypoint data loaded.");
        return;
    }

    const std::string& run_name = zarr_loader.getKeypointsRunName();
    ImGui::Text("Active run: %s",
                run_name.empty() ? "(unresolved)" : run_name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s",
                        zarr_loader.isRefinedKeypoints() ? "[refined]"
                                                         : "[raw]");

    if (!zarr_loader.hasKeypointReviewStatus()) {
        ImGui::TextDisabled(
            "KP Review: no review metadata written for the active run.");
        return;
    }

    const auto& review_state = zarr_loader.getKeypointReviewState();
    ImVec4 status_color =
        (review_state == "approved")
            ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
            : (review_state == "rejected")
                  ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                  : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
    ImGui::TextColored(status_color, "KP Review: %s", review_state.c_str());
    ImGui::SameLine();
    ImGui::Text("| Use: %s | Method: %s",
                zarr_loader.getKeypointReviewIntendedUse().c_str(),
                zarr_loader.getKeypointReviewMethod().c_str());
    if (!zarr_loader.getKeypointReviewTimestamp().empty()) {
        ImGui::Text("Reviewed: %s",
                    zarr_loader.getKeypointReviewTimestamp().c_str());
    }
    if (!zarr_loader.getKeypointReviewReviewer().empty()) {
        ImGui::Text("Reviewer: %s",
                    zarr_loader.getKeypointReviewReviewer().c_str());
    }
    if (!zarr_loader.getKeypointReviewNotes().empty()) {
        ImGui::TextWrapped("Notes: %s",
                           zarr_loader.getKeypointReviewNotes().c_str());
    }
}

}  // namespace

RefinedKeypointReviewWindowResult drawRefinedKeypointReviewWindow(
    const RefinedKeypointReviewWindowContext& context,
    RefinedKeypointReviewWindowState& state) {
    RefinedKeypointReviewWindowResult result;
    if (!context.zarr_loader.hasKeypointData()) {
        return result;
    }

    if (!ImGui::Begin("Refined Keypoint Review")) {
        ImGui::End();
        return result;
    }

    drawActiveReviewSummary(context.zarr_loader);

    const RefinedKeypointReviewPanelContext panel_context{
        context.zarr_loader,
        nullptr,
        context.current_frame_num,
        context.selected_frame,
        context.selected_box,
        false,
    };
    const auto panel_result =
        drawRefinedKeypointReviewPanel(panel_context, state.panel_state);
    result.selected_selection = panel_result.selected_selection;
    result.request_review_write = panel_result.request_review_write;
    result.review_options = panel_result.review_options;

    ImGui::End();
    return result;
}
