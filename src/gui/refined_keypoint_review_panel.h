#pragma once

#include "refined_keypoint_repository.h"

#include <array>
#include <optional>
#include <string>

class ZarrDetectionLoader;

struct RefinedKeypointReviewPanelState {
    int intended_use = 1;
    int review_state = 0;
    int method = 0;
    std::array<char, 64> reviewer{};
    std::array<char, 256> notes{};
    std::string review_write_status;
    std::string manual_write_status;
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
