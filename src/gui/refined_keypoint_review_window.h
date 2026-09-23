#pragma once

#include "gui/refined_keypoint_review_panel.h"

struct RefinedKeypointReviewWindowState {
    RefinedKeypointReviewPanelState panel_state;
};

struct RefinedKeypointReviewWindowContext {
    ZarrDetectionLoader& zarr_loader;
    int current_frame_num = 0;
    int selected_frame = -1;
    int selected_box = -1;
};

struct RefinedKeypointReviewWindowResult {
    std::optional<RefinedKeypointSelection> selected_selection;
    bool request_review_write = false;
    RefinedKeypointReviewStatusWriteOptions review_options;
};

RefinedKeypointReviewWindowResult drawRefinedKeypointReviewWindow(
    const RefinedKeypointReviewWindowContext& context,
    RefinedKeypointReviewWindowState& state);
