#pragma once

#include "gui/review_metadata_editor.h"
#include "refined_keypoint_repository.h"

#include <optional>
#include <string>

class ZarrDetectionLoader;

struct RefinedKeypointReviewPanelState {
    ReviewMetadataEditorState review_metadata{};
    std::string review_write_status;
    std::string manual_write_status;

    RefinedKeypointReviewPanelState() { review_metadata.intended_use = 1; }
};

struct RefinedKeypointReviewPanelContext {
    ZarrDetectionLoader& zarr_loader;
    int current_frame_num = 0;
    int selected_frame = -1;
    int selected_box = -1;
};

struct RefinedKeypointReviewPanelResult {
    std::optional<RefinedKeypointSelection> selected_selection;
    bool request_review_write = false;
    RefinedKeypointReviewStatusWriteOptions review_options;
};

RefinedKeypointReviewPanelResult drawRefinedKeypointReviewPanel(
    const RefinedKeypointReviewPanelContext& context,
    RefinedKeypointReviewPanelState& state);
