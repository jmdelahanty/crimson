#pragma once

#include "render.h"
#include "skeleton.h"

#include <map>
#include <string>
#include <vector>

void save_keypoints_depreciated(std::map<u32, KeyPoints *> keypoints_map,
                                SkeletonContext *skeleton,
                                std::string root_dir,
                                int num_cameras,
                                std::vector<std::string> &camera_names,
                                bool *input_is_imgs,
                                const std::vector<std::string> &input_files);

void save_keypoints(std::map<u32, KeyPoints *> keypoints_map,
                    SkeletonContext *skeleton,
                    std::string root_dir,
                    int num_cameras,
                    std::vector<std::string> &camera_names,
                    bool *input_is_imgs,
                    const std::vector<std::string> &input_files);

int load_keypoints_depreciated(std::map<u32, KeyPoints *> &keypoints_map,
                               SkeletonContext *skeleton,
                               std::string root_dir,
                               render_scene *scene,
                               std::vector<std::string> &camera_names,
                               std::string &error_message);

int find_most_recent_labels(std::string root_dir,
                            std::string &most_recent_file,
                            std::string &error_message);

int load_keypoints(std::string keypoints_folder,
                   std::map<u32, KeyPoints *> &keypoints_map,
                   SkeletonContext *skeleton,
                   render_scene *scene,
                   std::vector<std::string> &camera_names,
                   std::string &error_message);
