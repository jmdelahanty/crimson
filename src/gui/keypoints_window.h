#pragma once

#include <fstream>

#include "legacy_labeling_state.h"

#include <string>
#include <vector>

struct KeypointsWindowContext {
    int num_cams = 0;
    const LegacyLabelingState& legacy_state;
    int current_frame_num = 0;
    const std::vector<std::string>& camera_names;
    const std::vector<bool>& is_view_focused;
};

void drawKeypointsWindow(const KeypointsWindowContext& context);
