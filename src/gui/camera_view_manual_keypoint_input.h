#pragma once

#include "legacy_labeling_state.h"
#include "render.h"

struct CameraViewManualKeypointInputContext {
    render_scene* scene = nullptr;
    LegacyLabelingState* legacy_state = nullptr;
    int current_frame_num = 0;
    int view_idx = 0;
    bool plot_hovered = false;
};

struct CameraViewManualKeypointInputResult {
    bool legacy_manual_keypoints_find = false;
    bool view_focused = false;
};

CameraViewManualKeypointInputResult processCameraViewManualKeypointInput(
    const CameraViewManualKeypointInputContext& context);
