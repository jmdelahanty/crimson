#pragma once

#include "gui/full_frame_keypoint_edit_overlay.h"
#include "gui/review_metadata_editor.h"
#include "refined_keypoint_repository.h"

#include <optional>
#include <string>

class ZarrDetectionLoader;

struct RefinedKeypointReviewPanelState {
    ReviewMetadataEditorState review_metadata{};
    FullFrameKeypointEditState full_frame_edit{};
    std::string review_write_status;
    std::string manual_write_status;

    RefinedKeypointReviewPanelState() { review_metadata.intended_use = 1; }
};

struct RefinedKeypointReviewPanelContext {
    ZarrDetectionLoader& zarr_loader;
    int current_frame_num = 0;
    int selected_frame = -1;
    int selected_box = -1;
    bool play_video = false;
};

struct RefinedKeypointReviewPanelResult {
    std::optional<RefinedKeypointSelection> selected_selection;
    CropKeypointEditorAction edit_action;
    bool request_review_write = false;
    RefinedKeypointReviewStatusWriteOptions review_options;
};

RefinedKeypointReviewPanelResult drawRefinedKeypointReviewPanel(
    const RefinedKeypointReviewPanelContext& context,
    RefinedKeypointReviewPanelState& state);
