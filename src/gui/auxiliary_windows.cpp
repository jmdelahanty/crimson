#include "gui/auxiliary_windows.h"

#include "imgui.h"

void processHelpMenuShortcut(bool& show_help_window) {
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
        show_help_window = !show_help_window;
    }
}

void drawHelpMenuWindow(bool show_help_window) {
    if (!show_help_window) {
        return;
    }

    if (ImGui::Begin("Help Menu")) {
        ImGui::Text("<Space>: toggle play and pause");
        ImGui::Text("<Left Arrow>    : Seek backward");
        ImGui::Text("<Shift+Left>    : Seek backward (x10)");
        ImGui::Text("<Right Arrow>   : Seek forward");
        ImGui::Text("<Shift+Right>   : Seek forward (x10)");

        ImGui::SeparatorText("When paused");
        ImGui::Text("<,>: previous image in buffer");
        ImGui::Text("<.>: next image in buffer");

        ImGui::SeparatorText("While hovering image");
        ImGui::Text("<c>: create keypoints on frame");
        ImGui::Text("<w>: drop active keypoint");
        ImGui::Text("<a>: active keypoint++ ");
        ImGui::Text("<d>: active keypoint--");
        ImGui::Text("<q>: active keypoint set to first node");
        ImGui::Text("<e>: active keypoint set to last node");
        ImGui::Text("<t> -> triangulate");
        ImGui::Text("<Backspace>: delete all keypoints");
        ImGui::Text("<Ctrl+s>         : Save labels");
        ImGui::Text("<Click+Drag>: move selected Zarr bbox (paused)");
        ImGui::Text("<N>: toggle draw-new-bbox mode");
        ImGui::Text("<Del>: delete selected Zarr bbox");
        ImGui::Text("<Esc>: cancel draw mode or clear selected Zarr bbox");
        ImGui::Text("<Shift+R>: reset current-frame Zarr bbox edits");
        ImGui::Text("Zarr Raw Detect is read-only");

        ImGui::SeparatorText("While hovering keypoints");
        ImGui::Text("<r>: delete active keypoint");
        ImGui::Text("<f>: delete active keypoint on all cameras");
        ImGui::Text("Click keypoint to active it");
    }
    ImGui::End();
}

void drawErrorPopup(bool& show_error, const std::string& error_message) {
    if (show_error) {
        ImGui::OpenPopup("Error");
        show_error = false;
    }

    if (!ImGui::BeginPopupModal("Error", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("%s", error_message.c_str());
    ImGui::Separator();

    if (ImGui::Button("OK")) {
        ImGui::CloseCurrentPopup();
        show_error = false;
    }

    ImGui::EndPopup();
}
