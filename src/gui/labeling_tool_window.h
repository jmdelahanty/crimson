#pragma once

#include <fstream>

#include "render.h"
#include "skeleton.h"

#include <ctime>
#include <map>
#include <optional>
#include <string>

struct LabelingToolWindowState {
    bool load_old_format = false;
};

struct LabelingToolWindowContext {
    const std::string& root_dir;
    std::string& keypoints_root_folder;
    int num_cams = 0;
    const SkeletonContext* skeleton = nullptr;
    const std::map<u32, KeyPoints*>& keypoints_map;
    int current_frame_num = 0;
    bool legacy_manual_keypoints_find = false;
    bool triangulation_supported = false;
    std::time_t last_saved = static_cast<std::time_t>(-1);
    bool has_labeled_frames = false;
    int next_labeled_frame = -1;
    size_t total_labeled_frames = 0;
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
