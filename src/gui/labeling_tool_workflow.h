#pragma once

#include "camera.h"
#include "gui/labeling_tool_window.h"
#include "legacy_labeling_state.h"
#include "render.h"

#include <optional>
#include <string>
#include <vector>

struct LabelingToolWorkflowContext {
    LegacyLabelingState& legacy_state;
    int current_frame_num = 0;
    const std::vector<CameraParams>& camera_params;
    render_scene* scene = nullptr;
    std::vector<std::string>& camera_names;
    bool* input_is_imgs = nullptr;
    const std::vector<std::string>& imgs_names;
    std::string& error_message;
    bool& show_error;
};

struct LabelingToolWorkflowResult {
    std::optional<int> jump_target_frame;
};

LabelingToolWorkflowResult applyLabelingToolWindowActions(
    const LabelingToolWindowResult& window_result,
    const LabelingToolWorkflowContext& context);
