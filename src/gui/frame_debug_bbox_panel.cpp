#include "gui/frame_debug_bbox_panel.h"
#include "gui/review_metadata_editor.h"

#include "imgui.h"

void drawFrameDebugBBoxPanel(const FrameDebugWindowContext& context,
                             FrameDebugWindowState& state,
                             FrameDebugWindowResult& result) {
    ImGui::Separator();
    ImGui::Text("BBox Edit (in-memory):");
    if (!context.dataset_allows_bbox_edit) {
        ImGui::TextColored(
            ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
            "Read-only: Zarr Raw Detect (read-only) cannot be edited.");
    }
    ImGui::BeginDisabled(!context.dataset_allows_bbox_edit);
    ImGui::Checkbox("Enable bbox drag editing", &context.bbox_edit_state.enabled);
    bool draw_mode_enabled = context.bbox_edit_state.draw_mode;
    if (ImGui::Checkbox("Draw new boxes (N)", &draw_mode_enabled)) {
        context.bbox_edit_state.draw_mode = draw_mode_enabled;
        if (!draw_mode_enabled) {
            context.bbox_edit_state.cancelDraw();
        } else {
            context.bbox_edit_state.clearSelection();
        }
    }
    if (context.play_video && context.bbox_edit_state.enabled &&
        !context.bbox_edit_state.allow_edit_while_playing) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Pause playback to drag boxes.");
    }
    if (context.bbox_edit_state.draw_mode) {
        ImGui::Text(
            "  Draw mode active: Ctrl + left-drag to place; Esc cancels.");
    }
    ImGui::Text("  Cycle/select bbox: B (Shift+B reverse)");
    ImGui::Text("  Move selected bbox: Ctrl + left-drag");
    ImGui::Text("  Pan while editing: Shift + drag");
    ImGui::Text("  Delete selected bbox: Del");
    ImGui::Text("  Current frame: %s",
                context.frame_has_bbox_edits ? "edited (unsaved)" : "unchanged");
    ImGui::Text("  Pending edited frames: %zu",
                context.bbox_edit_state.dirtyFrameCount());
    if (ImGui::Button("Reset Frame BBox Edits (Shift+R)")) {
        result.request_reset_frame_bbox_edits = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear BBox Selection (Esc)")) {
        result.request_clear_bbox_selection = true;
    }
    if (ImGui::Button("Build Manual Payload Preview")) {
        result.request_build_manual_payload_preview = true;
    }
    ImGui::Separator();
    ImGui::Text("Detection Review Write:");
    drawReviewMetadataEditor("detect_review_write", state.manual_write_review);
    if (ImGui::Button("Write Manual Payload to Zarr")) {
        result.request_write_manual_payload = true;
    }
    ImGui::TextWrapped(
        "  Writes refined_detect_runs/<latest>/manual and updates manual pointers/status.");
    ImGui::EndDisabled();

    if (!context.bbox_payload_status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                           "%s",
                           context.bbox_payload_status.c_str());
    }
}
