#pragma once

#include "camera.h"
#include "gui/labeling_tool_window.h"
#include "render.h"
#include "skeleton.h"

#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct LabelingToolWorkflowContext {
    std::map<u32, KeyPoints*>& keypoints_map;
    SkeletonContext* skeleton = nullptr;
    int current_frame_num = 0;
    const std::vector<CameraParams>& camera_params;
    render_scene* scene = nullptr;
    const std::string& keypoints_root_folder;
    std::vector<std::string>& camera_names;
    bool* input_is_imgs = nullptr;
    const std::vector<std::string>& imgs_names;
    std::time_t& last_saved;
    std::string& error_message;
    bool& show_error;
};

struct LabelingToolWorkflowResult {
    std::optional<int> jump_target_frame;
};

LabelingToolWorkflowResult applyLabelingToolWindowActions(
    const LabelingToolWindowResult& window_result,
    const LabelingToolWorkflowContext& context);
