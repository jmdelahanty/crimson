#pragma once

#include "render.h"
#include "skeleton.h"

#include <map>

struct CameraViewManualKeypointInputContext {
    render_scene* scene = nullptr;
    SkeletonContext* skeleton = nullptr;
    std::map<u32, KeyPoints*>* keypoints_map = nullptr;
    int current_frame_num = 0;
    int view_idx = 0;
    bool legacy_manual_keypoints_find = false;
    bool plot_hovered = false;
};

struct CameraViewManualKeypointInputResult {
    bool legacy_manual_keypoints_find = false;
    bool view_focused = false;
};

CameraViewManualKeypointInputResult processCameraViewManualKeypointInput(
    const CameraViewManualKeypointInputContext& context);
