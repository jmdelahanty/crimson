#pragma once

#include <fstream>

#include "render.h"
#include "skeleton.h"

#include <map>
#include <string>
#include <vector>

struct KeypointsWindowContext {
    int num_cams = 0;
    const SkeletonContext* skeleton = nullptr;
    const std::map<u32, KeyPoints*>& keypoints_map;
    int current_frame_num = 0;
    const std::vector<std::string>& camera_names;
    const std::vector<bool>& is_view_focused;
    bool keypoints_find = false;
};

void drawKeypointsWindow(const KeypointsWindowContext& context);
