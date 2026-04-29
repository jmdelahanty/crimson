#include "gui/frame_debug_window.h"
#include "gui/frame_debug_bbox_panel.h"
#include "gui/frame_debug_review_panel.h"
#include "gui/refined_keypoint_review_panel.h"
#include "gui/frame_debug_status_panel.h"
#include "gui/overlay_debug_panel.h"

#include "imgui.h"

FrameDebugWindowResult drawFrameDebugWindow(const FrameDebugWindowContext& context,
                                            FrameDebugWindowState& state) {
    FrameDebugWindowResult result;
    result.review_frame_filters = context.review_frame_filters;
    result.show_keypoint_markers = context.show_keypoint_markers;
    result.show_heading_arrows = context.show_heading_arrows;
    result.show_eye_masks = context.show_eye_masks;
    result.show_subject_body_mask = context.show_subject_body_mask;
    result.show_eye_left_mask = context.show_eye_left_mask;
    result.show_eye_right_mask = context.show_eye_right_mask;
    result.show_swim_bladder_mask = context.show_swim_bladder_mask;

    ImGui::SetNextWindowSize(ImVec2(760.0f, 840.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Frame Inspect")) {
        ImGui::End();
        return result;
    }

    drawFrameDebugStatusPanel(context, state, result);
    if (context.zarr_loaded) {
        switch (state.active_tab) {
            case FrameInspectTab::Detect:
                drawFrameDebugReviewPanel(context, result);
                drawFrameDebugBBoxPanel(context, state, result);
                break;
            case FrameInspectTab::Keypoints:
                drawKeypointHeadingOverlayPanel(context, result);
                {
                    const RefinedKeypointReviewPanelContext
                        keypoint_review_panel_context{
                            context.zarr_loader,
                            context.detection_details,
                            context.current_frame_num,
                            context.bbox_edit_state.selected_frame,
                            context.bbox_edit_state.selected_box,
                            context.play_video,
                        };
                    const auto keypoint_review_panel_result =
                        drawRefinedKeypointReviewPanel(
                            keypoint_review_panel_context,
                            state.keypoint_review_panel);
                    result.selected_keypoint_selection =
                        keypoint_review_panel_result.selected_selection;
                    result.keypoint_edit_action =
                        keypoint_review_panel_result.edit_action;
                    result.request_keypoint_review_write =
                        keypoint_review_panel_result.request_review_write;
                    result.keypoint_review_options =
                        keypoint_review_panel_result.review_options;
                }
                break;
            case FrameInspectTab::EyeMasks:
                drawEyeMaskOverlayPanel(context, result);
                break;
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "[Zarr] Detections:    Not loaded");
    }

    ImGui::End();
    return result;
}
