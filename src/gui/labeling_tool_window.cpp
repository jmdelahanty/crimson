#include "gui/labeling_tool_window.h"

#include <ImGuiFileDialog.h>
#include "imgui.h"

#include <algorithm>

namespace {

bool allCurrentKeypointsTriangulated(const LabelingToolWindowContext& context) {
    if (!context.legacy_state.keypoints_find ||
        context.legacy_state.skeleton == nullptr) {
        return false;
    }
    auto frame_it =
        context.legacy_state.keypoints_map.find(context.current_frame_num);
    if (frame_it == context.legacy_state.keypoints_map.end() ||
        frame_it->second == nullptr) {
        return false;
    }

    KeyPoints* frame_keypoints = frame_it->second;
    for (int cam_idx = 0; cam_idx < context.num_cams; ++cam_idx) {
        for (int node_idx = 0; node_idx < context.legacy_state.skeleton->num_nodes;
             ++node_idx) {
            if (!frame_keypoints->keypoints2d[cam_idx][node_idx]
                     .is_triangulated) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

LabelingToolWindowResult drawLabelingToolWindow(
    const LabelingToolWindowContext& context,
    LabelingToolWindowState& state) {
    LabelingToolWindowResult result;

    if (!ImGui::Begin("Legacy Labeling Tool")) {
        ImGui::End();
        return result;
    }

    ImGui::TextDisabled("Legacy CSV/manual labeling workflow");
    ImGui::Separator();

        const bool keypoint_triangulated_all =
        allCurrentKeypointsTriangulated(context);
    if (context.num_cams > 1) {
        const bool enabled =
            context.legacy_state.keypoints_find &&
            context.triangulation_supported;
        const bool apply_color = context.triangulation_supported &&
                                 !keypoint_triangulated_all && enabled;
        if (apply_color) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  (ImVec4)ImColor::HSV(0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  (ImVec4)ImColor::HSV(0.8f, 0.9f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  (ImVec4)ImColor::HSV(0.8f, 0.9f, 0.5f));
        }

        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button("Triangulate")) {
            result.request_triangulate = true;
        }
        ImGui::EndDisabled();

        if (apply_color) {
            ImGui::PopStyleColor(3);
        }

        if (!context.triangulation_supported) {
            ImGui::SameLine();
            ImGui::TextDisabled("SFM disabled in this build");
        }

        if (enabled && ImGui::IsKeyPressed(ImGuiKey_T, false)) {
            result.request_triangulate = true;
        }
    }

    if (ImGui::Button("Update keypoints working directory")) {
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 1;
        config.path = context.root_dir;
        config.flags = ImGuiFileDialogFlags_Modal;
        ImGuiFileDialog::Instance()->OpenDialog("ChooseKeypointsFolder",
                                                "Choose keypoints working directory",
                                                nullptr,
                                                config);
    }
    ImGui::SameLine();
    ImGui::Text("%s", context.legacy_state.keypoints_root_folder.c_str());

    if (ImGui::Button("Save Labeled Data") ||
        (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
        result.request_save = true;
    }
    if (context.legacy_state.last_saved != static_cast<std::time_t>(-1)) {
        ImGui::SameLine();
        ImGui::Text("Last saved: %s", ctime(&context.legacy_state.last_saved));
    }

    if (ImGui::Button("Load Most Recent Labels")) {
        result.request_load_most_recent = true;
        result.load_old_format = state.load_old_format;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Old format", &state.load_old_format);

    if (ImGui::Button("Load From Selected")) {
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 1;
        config.path = context.legacy_state.keypoints_root_folder;
        config.flags = ImGuiFileDialogFlags_Modal;
        ImGuiFileDialog::Instance()->OpenDialog("LoadFromSelected",
                                                "Load from selected",
                                                nullptr,
                                                config);
    }

    ImGui::Separator();
    if (context.legacy_state.hasLabeledFrames()) {
        ImGui::Text("Next labeled frame : %d", context.next_labeled_frame);
        if (ImGui::Button("Jump to Next Labeled Frame")) {
            result.jump_target_frame = context.next_labeled_frame;
        }
    } else {
        ImGui::TextDisabled("Next labeled frame : none");
        ImGui::BeginDisabled();
        ImGui::Button("Jump to Next Labeled Frame");
        ImGui::EndDisabled();
    }
    ImGui::Text("Total labeled frames : %zu",
                context.legacy_state.keypoints_map.size());

    ImGui::End();

    if (ImGuiFileDialog::Instance()->Display("ChooseKeypointsFolder")) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            context.legacy_state.keypoints_root_folder =
                ImGuiFileDialog::Instance()->GetCurrentPath();
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("LoadFromSelected")) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            result.selected_load_folder =
                ImGuiFileDialog::Instance()->GetCurrentPath();
        }
        ImGuiFileDialog::Instance()->Close();
    }

    return result;
}
