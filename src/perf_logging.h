#pragma once

#include "gui/camera_view_overlay_renderer.h"
#include "gui/crop_preview_perf.h"
#include "stimulus_playback.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

struct PerfLogWriter {
    std::ofstream stream;
    std::filesystem::path csv_path;
    std::filesystem::path metadata_path;
    std::chrono::steady_clock::time_point start_steady{};
    std::chrono::steady_clock::time_point last_sample_steady{};

    bool open(const std::filesystem::path& output_path);
    bool enabled() const;
};

struct MaskPerfLogWriter {
    std::ofstream stream;
    std::filesystem::path jsonl_path;
    int samples_since_flush = 0;
    std::chrono::steady_clock::time_point last_flush_steady{};

    bool open(const std::filesystem::path& output_path);
    bool enabled() const;
};

struct PerfLogFrameContext {
    std::vector<std::string> camera_names;
    std::filesystem::path cwd;
    std::filesystem::path argv0_path;
    std::string cli_recording_path;
    std::string cli_zarr_override_path;

    bool play_video = false;
    double set_playback_speed = 0.0;
    double inst_speed = 0.0;
    double video_fps = 0.0;
    int perf_requested_camera_frame = -1;
    int displayed_camera_frame = -1;
    int current_frame_num = -1;
    int perf_min_decoded_camera_frame = -1;

    bool scene_use_cpu_buffer = false;
    int scene_buffer_size = 0;
    int label_buffer_size = 0;
    bool video_loaded = false;
    std::string playback_preview_scale_label;
    bool playback_preview_active = false;
    std::string playback_renderer_mode_label;

    int perf_camera_viewport_width_px = 0;
    int perf_camera_viewport_height_px = 0;
    double perf_camera_view_x_min = 0.0;
    double perf_camera_view_x_max = 0.0;
    double perf_camera_view_y_min = 0.0;
    double perf_camera_view_y_max = 0.0;
    double perf_camera_view_visible_fraction = 0.0;
    int perf_camera_view_zoomed_in = 0;

    int frame_camera_upload_count = 0;
    double frame_camera_upload_ms = 0.0;
    double frame_camera_texture_resize_ms = 0.0;
    double frame_camera_preview_resize_ms = 0.0;
    double frame_camera_display_convert_ms = 0.0;
    double frame_camera_pbo_copy_ms = 0.0;
    double frame_camera_texture_upload_ms = 0.0;
    double frame_camera_playback_front_path_ms = 0.0;
    double frame_camera_playback_stage_total_ms = 0.0;
    double frame_camera_playback_stage_upload_ms = 0.0;
    double frame_camera_playback_swap_ms = 0.0;
    double frame_camera_plot_image_ui_ms = 0.0;
    double frame_camera_overlay_ui_ms = 0.0;
    double frame_camera_scene_ui_ms = 0.0;
    double frame_file_browser_ui_ms = 0.0;
    double frame_frame_debug_ui_ms = 0.0;
    double frame_buffer_window_ui_ms = 0.0;
    double frame_crop_preview_ui_ms = 0.0;
    CropPreviewPerfMetrics crop_preview_perf;
    double frame_stimulus_buffer_window_ui_ms = 0.0;
    double frame_keypoints_window_ui_ms = 0.0;
    double frame_labeling_tool_ui_ms = 0.0;
    double frame_stimulus_window_ui_ms = 0.0;
    double frame_stimulus_timeline_ui_ms = 0.0;
    double frame_movement_timeline_ui_ms = 0.0;
    double frame_help_menu_ui_ms = 0.0;
    double frame_gl_draw_ms = 0.0;
    double frame_swap_ms = 0.0;
    double frame_cap_sleep_ms = 0.0;
    double frame_cap_fps = 0.0;
    double frame_ui_build_ms = 0.0;
    double frame_imgui_render_ms = 0.0;
    int frame_imgui_draw_cmd_count = 0;
    int frame_imgui_draw_list_count = 0;
    int frame_imgui_total_vtx_count = 0;
    int frame_imgui_total_idx_count = 0;

    const StimulusPlayback* stimulus_player = nullptr;
    bool stimulus_use_software_decode_fallback = false;
    bool stimulus_use_cpu_buffer_fallback = false;
    int stimulus_buffer_size_fallback = 0;
    int stimulus_target_frame = -1;
    int stimulus_latest_decoded = -1;

    int window_swap_interval = 0;
    int window_width = 0;
    int window_height = 0;

    std::chrono::steady_clock::time_point frame_loop_start{};
};

struct MaskPerfLogFrameContext {
    std::filesystem::path cwd;
    std::filesystem::path argv0_path;
    std::string cli_recording_path;
    std::string cli_zarr_override_path;
    std::string loaded_zarr_path;

    int current_frame_num = -1;
    int displayed_camera_frame = -1;
    bool play_video = false;
    bool overlay_enabled = false;
    bool zarr_loaded = false;
    std::string source_label;
    std::string source_path;
    std::string run_name;
    int32_t selected_roi_index = -1;
    std::string selected_component_name;
    double mask_data_load_ms = 0.0;
    CameraViewMaskPerfMetrics metrics;
    const PerfLogFrameContext* frame_perf = nullptr;

    std::chrono::steady_clock::time_point frame_loop_start{};
};

std::filesystem::path defaultMaskPerfLogPath(
    const std::filesystem::path& default_buffer_dump_root);

void maybeWritePerfLogSample(PerfLogWriter& writer,
                             const PerfLogFrameContext& context,
                             std::chrono::milliseconds sample_period);

void writeMaskPerfLogSample(MaskPerfLogWriter& writer,
                            const MaskPerfLogFrameContext& context);
