#pragma once

#include "render.h"
#include "skeleton.h"

#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

struct LegacyLabelingState {
    std::time_t last_saved = static_cast<std::time_t>(-1);
    bool manual_label_mode = false;
    bool keypoints_find = false;
    bool skeleton_chosen = false;
    std::unique_ptr<SkeletonContext> skeleton;
    std::map<u32, KeyPoints*> keypoints_map;
    std::map<std::string, SkeletonPrimitive> skeleton_map;
    std::string keypoints_root_folder;

    void ensureSkeletonResources() {
        if (!skeleton) {
            skeleton = std::make_unique<SkeletonContext>();
        }
        if (skeleton_map.empty()) {
            skeleton_map = skeleton_get_all();
        }
    }

    std::string activeSkeletonName() const {
        return skeleton ? skeleton->name : std::string();
    }

    void activateManualMode(const std::string& root_dir) {
        manual_label_mode = true;
        keypoints_find = false;
        keypoints_root_folder = root_dir + "/labeled_data/";
        std::filesystem::create_directory(keypoints_root_folder);
        skeleton_chosen = true;
    }

    bool toolsEnabled(bool has_active_zarr_keypoint_review) const {
        return manual_label_mode && !has_active_zarr_keypoint_review;
    }

    bool hasFrameKeypoints(int current_frame_num) const {
        if (current_frame_num < 0) {
            return false;
        }
        return keypoints_map.find(static_cast<u32>(current_frame_num)) !=
               keypoints_map.end();
    }

    bool hasLabeledFrames() const {
        return !keypoints_map.empty();
    }

    int nextLabeledFrameAfter(int current_frame_num) const {
        if (keypoints_map.empty()) {
            return -1;
        }
        auto upper_it = keypoints_map.upper_bound(current_frame_num);
        if (upper_it == keypoints_map.end()) {
            upper_it = keypoints_map.begin();
        }
        return static_cast<int>(upper_it->first);
    }
};
