#pragma once

#include <fstream>

#include "legacy_labeling_state.h"

#include <optional>
#include <string>

struct LabelingToolWindowState {
    bool load_old_format = false;
};

struct LabelingToolWindowContext {
    const std::string& root_dir;
    LegacyLabelingState& legacy_state;
    int num_cams = 0;
    int current_frame_num = 0;
    bool triangulation_supported = false;
    int next_labeled_frame = -1;
};

struct LabelingToolWindowResult {
    bool request_triangulate = false;
    bool request_save = false;
    bool request_load_most_recent = false;
    bool load_old_format = false;
    std::optional<std::string> selected_load_folder;
    std::optional<int> jump_target_frame;
};

LabelingToolWindowResult drawLabelingToolWindow(
    const LabelingToolWindowContext& context,
    LabelingToolWindowState& state);
