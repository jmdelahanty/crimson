#include "gui/frame_debug_window.h"
#include "gui/frame_debug_bbox_panel.h"
#include "gui/frame_debug_review_panel.h"
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

    if (!ImGui::Begin("Frame Debug")) {
        ImGui::End();
        return result;
    }

    drawFrameDebugStatusPanel(context, result);
    if (context.zarr_loaded) {
        drawFrameDebugReviewPanel(context, result);
        drawFrameDebugBBoxPanel(context, state, result);
        drawOverlayDebugPanel(context, result);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "[Zarr] Detections:    Not loaded");
    }

    ImGui::End();
    return result;
}
