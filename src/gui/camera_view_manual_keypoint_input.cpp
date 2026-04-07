#include "gui/camera_view_manual_keypoint_input.h"

#include "gui/manual_keypoint_overlay.h"
#include "imgui.h"
#include "implot.h"

#include <cstdlib>

namespace {

KeyPoints* findFrameKeypoints(std::map<u32, KeyPoints*>* keypoints_map,
                              int current_frame_num) {
    if (keypoints_map == nullptr || current_frame_num < 0) {
        return nullptr;
    }
    auto it = keypoints_map->find(static_cast<u32>(current_frame_num));
    if (it == keypoints_map->end()) {
        return nullptr;
    }
    return it->second;
}

}  // namespace

CameraViewManualKeypointInputResult processCameraViewManualKeypointInput(
    const CameraViewManualKeypointInputContext& context) {
    CameraViewManualKeypointInputResult result;
    result.legacy_manual_keypoints_find =
        context.legacy_manual_keypoints_find;

    if (context.scene == nullptr || context.skeleton == nullptr ||
        context.keypoints_map == nullptr || context.view_idx < 0 ||
        context.view_idx >= static_cast<int>(context.scene->num_cams)) {
        return result;
    }

    KeyPoints* frame_keypoints =
        findFrameKeypoints(context.keypoints_map, context.current_frame_num);
    result.legacy_manual_keypoints_find = (frame_keypoints != nullptr);

    if (context.plot_hovered) {
        result.view_focused = true;

        if (ImGui::IsKeyPressed(ImGuiKey_C, false) &&
            !result.legacy_manual_keypoints_find) {
            KeyPoints* keypoints =
                static_cast<KeyPoints*>(malloc(sizeof(KeyPoints)));
            allocate_keypoints(keypoints, context.scene, context.skeleton);
            (*context.keypoints_map)[static_cast<u32>(context.current_frame_num)] =
                keypoints;
            frame_keypoints = keypoints;
            result.legacy_manual_keypoints_find = true;
        }

        if (frame_keypoints != nullptr) {
            u32* active_keypoint = &frame_keypoints->active_id[context.view_idx];

            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
                ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                frame_keypoints->keypoints2d[context.view_idx][*active_keypoint]
                    .position = {mouse.x, mouse.y};
                frame_keypoints->keypoints2d[context.view_idx][*active_keypoint]
                    .is_labeled = true;
                frame_keypoints->keypoints2d[context.view_idx][*active_keypoint]
                    .is_triangulated = false;
                if (*active_keypoint <
                    static_cast<u32>(context.skeleton->num_nodes - 1)) {
                    (*active_keypoint)++;
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_A, true)) {
                if (*active_keypoint == 0) {
                    *active_keypoint = 0;
                } else {
                    (*active_keypoint)--;
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_D, true)) {
                if (*active_keypoint >=
                    static_cast<u32>(context.skeleton->num_nodes - 1)) {
                    *active_keypoint =
                        static_cast<u32>(context.skeleton->num_nodes - 1);
                } else {
                    (*active_keypoint)++;
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
                *active_keypoint =
                    static_cast<u32>(context.skeleton->num_nodes - 1);
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
                *active_keypoint = 0;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
                free_keypoints(frame_keypoints, context.scene);
                context.keypoints_map->erase(
                    static_cast<u32>(context.current_frame_num));
                frame_keypoints = nullptr;
                result.legacy_manual_keypoints_find = false;
            }
        }
    }

    if (frame_keypoints != nullptr) {
        gui_plot_keypoints(frame_keypoints, context.skeleton, context.view_idx,
                           context.scene->num_cams);
        if (context.skeleton->name == "Rat4Box" ||
            context.skeleton->name == "Rat4Box3Ball") {
            gui_plot_bbox_from_keypoints(frame_keypoints, context.skeleton,
                                         context.view_idx, 4, 5);
        }
    }

    return result;
}
