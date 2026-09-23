#pragma once

#include <array>
#include <fstream>

#include "render.h"
#include "skeleton.h"
#include "ui_path_config.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>

enum class FileBrowserDetectionAction {
    None = 0,
    YOLOv5,
    YOLOv8,
    YOLOv8Pose,
};

struct FileBrowserSkeletonSelection {
    std::string name;
    SkeletonPrimitive primitive = SP_LOAD;
};

struct FileBrowserWindowState {
    static constexpr size_t kPathBufferSize = 4096;

    int seek_accurate_frame_num = 0;
    bool enable_legacy_manual_labeling = false;
    bool request_open_path_editor = false;
    std::array<char, kPathBufferSize> default_start_path_buffer{};
    std::vector<std::array<char, kPathBufferSize>> preferred_root_buffers;
    int path_browse_target = -2;
    std::string path_editor_message;
    bool path_editor_message_is_error = false;
};

struct FileBrowserWindowContext {
    const UiPathConfig& ui_path_config;
    std::string& start_folder_name;
    const std::string& root_dir;
    const std::string& skeleton_dir;
    bool video_loaded = false;
    bool can_offer_legacy_manual_labeling = true;
    bool legacy_manual_mode_active = false;
    bool skeleton_chosen = false;
    const std::string& active_skeleton_name;
    const std::map<std::string, SkeletonPrimitive>& skeleton_map;
    bool cpu_buffer_toggle = true;
    bool& scene_use_cpu_buffer;
    int& label_buffer_size;
    int& playback_preview_scale_mode;
    int& playback_renderer_mode;
    bool yolo_detection = false;
    bool play_video = false;
    float& set_playback_speed;
    double& inst_speed;
    double video_fps = 0.0;
    int current_frame_num = 0;
    int& last_frame_num_playspeed;
    std::chrono::steady_clock::time_point& last_wall_time_playspeed;
    bool stimulus_loaded = false;
    int& stimulus_buffer_size;
    bool& stimulus_use_cpu_buffer;
    bool& stimulus_use_software_decode;
    int loaded_stimulus_buffer_size = 0;
    bool loaded_stimulus_use_cpu_buffer = false;
    bool loaded_stimulus_use_software_decode = false;
    int& seek_interval;
    bool detection_quality_available = false;
    bool detection_quality_requested = false;
    bool keypoint_quality_available = false;
    bool keypoint_quality_requested = false;
};

struct FileBrowserWindowResult {
    std::optional<FileBrowserSkeletonSelection> skeleton_selection;
    FileBrowserDetectionAction detection_action =
        FileBrowserDetectionAction::None;
    std::optional<int> accurate_seek_target_frame;
    std::optional<UiPathConfig> updated_path_config;
    std::optional<bool> detection_quality_requested;
    std::optional<bool> keypoint_quality_requested;
};

FileBrowserWindowResult drawFileBrowserWindow(const FileBrowserWindowContext& context,
                                              FileBrowserWindowState& state);
