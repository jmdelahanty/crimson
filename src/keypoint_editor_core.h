#pragma once

#include "render.h"
#include "skeleton.h"

#include <cstdlib>
#include <map>

struct KeypointEditorPlotHotkeys {
    bool create_frame = false;
    bool drop_active_keypoint = false;
    bool active_prev = false;
    bool active_next = false;
    bool active_first = false;
    bool active_last = false;
    bool delete_frame = false;
};

struct KeypointEditorPlotInput {
    u32 frame_num = 0;
    u32 view_idx = 0;
    double mouse_x = 0.0;
    double mouse_y = 0.0;
    bool frame_has_keypoints = false;
};

struct KeypointEditorPlotResult {
    bool frame_created = false;
    bool frame_deleted = false;
};

inline KeypointEditorPlotResult HandleKeypointEditorPlotHotkeys(
    const KeypointEditorPlotHotkeys& hotkeys,
    const KeypointEditorPlotInput& input,
    std::map<u32, KeyPoints*>& keypoints_map,
    SkeletonContext* skeleton,
    render_scene* scene) {
    KeypointEditorPlotResult result{};

    if (!skeleton || !scene) {
        return result;
    }

    if (hotkeys.create_frame && !input.frame_has_keypoints) {
        KeyPoints* keypoints = static_cast<KeyPoints*>(malloc(sizeof(KeyPoints)));
        allocate_keypoints(keypoints, scene, skeleton);
        keypoints_map[input.frame_num] = keypoints;
        result.frame_created = true;
    }

    if (!input.frame_has_keypoints || input.view_idx >= scene->num_cams) {
        return result;
    }

    auto frame_it = keypoints_map.find(input.frame_num);
    if (frame_it == keypoints_map.end()) {
        return result;
    }

    KeyPoints* frame_keypoints = frame_it->second;
    u32* active_node = &frame_keypoints->active_id[input.view_idx];

    if (hotkeys.drop_active_keypoint) {
        frame_keypoints->keypoints2d[input.view_idx][*active_node].position =
            {input.mouse_x, input.mouse_y};
        frame_keypoints->keypoints2d[input.view_idx][*active_node].is_labeled =
            true;
        frame_keypoints->keypoints2d[input.view_idx][*active_node]
            .is_triangulated = false;
        if (*active_node < static_cast<u32>(skeleton->num_nodes - 1)) {
            (*active_node)++;
        }
    }

    if (hotkeys.active_prev) {
        if (*active_node <= 0) {
            *active_node = 0;
        } else {
            (*active_node)--;
        }
    }

    if (hotkeys.active_next) {
        if (*active_node >= static_cast<u32>(skeleton->num_nodes - 1)) {
            *active_node = static_cast<u32>(skeleton->num_nodes - 1);
        } else {
            (*active_node)++;
        }
    }

    if (hotkeys.active_last) {
        *active_node = static_cast<u32>(skeleton->num_nodes - 1);
    }

    if (hotkeys.active_first) {
        *active_node = 0;
    }

    if (hotkeys.delete_frame) {
        free_keypoints(frame_keypoints, scene);
        keypoints_map.erase(input.frame_num);
        result.frame_deleted = true;
    }

    return result;
}
