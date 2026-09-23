#pragma once

#include <cstdint>
#include <string>

struct CropPreviewPerfMetrics {
    bool attempted = false;
    bool refreshed = false;
    bool cache_hit = false;
    bool frame_changed = false;
    bool roi_changed = false;
    bool crop_rect_changed = false;
    bool rotated_requested = false;
    bool rotated_refresh_needed = false;
    bool rotated_valid_after = false;
    int32_t crop_roi_index = -1;
    int current_frame_num = -1;
    int frame_mod32 = -1;
    int frame_mod64 = -1;
    std::string crop_source;
    double provider_texture_lookup_ms = 0.0;
    double provider_image_lookup_ms = 0.0;
    double render_crop_texture_ms = 0.0;
    double image_upload_ms = 0.0;
    double get_raw_detections_ms = 0.0;
    double render_rotated_texture_ms = 0.0;
    double keypoint_transform_ms = 0.0;
    double refresh_ms = 0.0;
    double window_setup_ms = 0.0;
    double imgui_begin_ms = 0.0;
    double resolve_selection_ms = 0.0;
    double context_build_ms = 0.0;
    double editor_panel_ms = 0.0;
    double post_panel_ms = 0.0;
    double imgui_end_ms = 0.0;
    double finish_ms = 0.0;
    double total_window_ms = 0.0;
};
