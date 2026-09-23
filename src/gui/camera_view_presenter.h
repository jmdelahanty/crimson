#pragma once

#include "render.h"

struct CameraViewPresenterContext {
    render_scene* scene = nullptr;
    int view_idx = -1;
    int current_frame_num = 0;
    int target_display_frame = 0;
    int read_head = 0;
    int preferred_paused_slot = -1;

    bool play_video = false;
    bool pause_seeked = false;
    bool rejected_seek_ring_quarantined = false;
    bool yolo_detection = false;
    bool lightweight_playback_renderer_active = false;
    bool preview_active = false;
    bool prewarm_playback_textures = false;
    double preview_scale = 1.0;
    int preview_scale_mode = 0;
};

struct CameraViewPresenterPerfMetrics {
    double upload_ms = 0.0;
    int upload_count = 0;
    double texture_resize_ms = 0.0;
    double preview_resize_ms = 0.0;
    double display_convert_ms = 0.0;
    double pbo_copy_ms = 0.0;
    double texture_upload_ms = 0.0;
    double playback_front_path_ms = 0.0;
    double playback_stage_total_ms = 0.0;
    double playback_stage_upload_ms = 0.0;
    double playback_prewarm_total_ms = 0.0;
    double playback_prewarm_upload_ms = 0.0;
    int playback_prewarm_count = 0;
};

struct CameraViewPresenterResult {
    unsigned char* presented_rgba_cuda_buffer = nullptr;
    const PresentationTexture* presentation_texture = nullptr;
    bool swap_playback_surface_after_draw = false;
    int presented_slot = -1;
    int presented_frame = -1;
    int resolved_current_frame_num = 0;
    CameraViewPresenterPerfMetrics perf;
};

int findCameraDisplaySlotForFrame(const render_scene& scene,
                                  int cam_idx,
                                  int target_frame,
                                  int preferred_slot);

CameraViewPresenterResult presentCameraViewFrame(
    const CameraViewPresenterContext& context);
