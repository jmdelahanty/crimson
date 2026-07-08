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
    result.show_eye_direction_beams = context.show_eye_direction_beams;
    result.show_eye_gaze_rays = context.show_eye_gaze_rays;
    result.show_eye_angle_arcs = context.show_eye_angle_arcs;
    result.show_eye_angle_labels = context.show_eye_angle_labels;
    result.mask_overlay_mode = context.mask_overlay_mode;
    result.subject_shape_overlay_options =
        context.subject_shape_overlay_options;
    result.tail_kinematics_overlay_options =
        context.tail_kinematics_overlay_options;
    result.show_movement_trail = context.show_movement_trail;
    result.movement_trail_seconds = context.movement_trail_seconds;
    result.movement_trail_valid_samples_only =
        context.movement_trail_valid_samples_only;
    result.stimulus_inset_options = context.stimulus_inset_options;
    result.chaser_distance_polar_inset_options =
        context.chaser_distance_polar_inset_options;
    result.show_stimulus_debug_windows =
        context.show_stimulus_debug_windows;

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
                drawTrackKinematicsOverlayPanel(context, result);
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
            case FrameInspectTab::TailKinematics:
                break;
            case FrameInspectTab::EyeAngles:
                break;
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "[Zarr] Detections:    Not loaded");
    }

    ImGui::End();
    return result;
}
