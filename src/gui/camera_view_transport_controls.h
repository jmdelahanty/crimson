#pragma once

#include <optional>

struct CameraViewTransportControlsContext {
    int current_display_frame = 0;
    int total_num_frames = 0;
    int estimated_num_frames = 0;
    float video_fps = 0.0f;
    bool play_video = false;
    int slider_frame_number = 0;
};

struct CameraViewTransportControlsResult {
    bool toggle_playback = false;
    int step_delta = 0;
    int slider_frame_number = 0;
    bool slider_just_changed = false;
    std::optional<int> seek_target_frame;
    bool force_inaccurate_seek = false;
};

struct CameraViewPlaybackShortcutsResult {
    bool toggle_playback = false;
    int step_delta = 0;
};

CameraViewTransportControlsResult drawCameraViewTransportControls(
    const CameraViewTransportControlsContext& context);

CameraViewPlaybackShortcutsResult handleCameraViewPlaybackShortcuts();
