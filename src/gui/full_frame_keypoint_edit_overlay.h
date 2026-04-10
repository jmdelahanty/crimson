#pragma once

#include "gui/crop_keypoint_editor.h"
#include "refined_keypoint_repository.h"
#include "zarr_loader.h"

#include <string>
#include <vector>

struct FullFrameKeypointEditState {
    bool enabled = false;
    std::vector<std::array<float, 2>> positions_img;
    int active_handle = -1;
    int roi_index = -1;
    int frame = -1;
    int detection_index = -1;
    std::string run_name;
    bool dirty = false;
};

struct FullFrameKeypointEditContext {
    const RefinedKeypointSelection* selection = nullptr;
    const ZarrDetectionLoader::FrameDetections* detection_details = nullptr;
    float image_width_px = 0.0f;
    float image_height_px = 0.0f;
    bool plot_hovered = false;
    bool play_video = false;
};

struct FullFrameKeypointEditOverlayResult {
    FullFrameKeypointEditState state;
    bool selection_editable = false;
};

void resetFullFrameKeypointEditState(FullFrameKeypointEditState& state);

FullFrameKeypointEditOverlayResult processFullFrameKeypointEditOverlay(
    const FullFrameKeypointEditContext& context,
    const FullFrameKeypointEditState& initial_state);

bool buildFullFrameKeypointEditAction(
    const RefinedKeypointSelection& selection,
    const FullFrameKeypointEditState& state,
    CropKeypointEditorAction& action,
    std::string* error_message = nullptr);
