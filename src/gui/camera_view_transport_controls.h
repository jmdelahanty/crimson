#pragma once

#include "workspace_state.h"

#include <cstdint>
#include <optional>

enum class CameraViewTransportAction : uint8_t {
    None,
    Restart,
    TogglePlayback,
    StepBackward,
    StepForward,
    SeekPreview,
    SeekCommit,
};

struct CameraViewTransportControlsContext {
    int64_t current_display_frame = 0;
    int64_t total_num_frames = 0;
    int64_t maximum_frame_number = 0;
    double video_fps = 0.0;
    bool play_video = false;
    int64_t slider_frame_number = 0;
    bool enabled = true;
};

struct CameraViewTransportControlsResult {
    CameraViewTransportAction action = CameraViewTransportAction::None;
    std::optional<crimson::workspace::PlaybackIntent> intent;
    int64_t step_delta = 0;
    int64_t slider_frame_number = 0;
    bool slider_just_changed = false;
    bool slider_active = false;
    bool slider_released = false;
    bool force_inaccurate_seek = false;
};

using CameraViewPlaybackShortcutsResult =
    crimson::workspace::PlaybackShortcutResult;

CameraViewTransportControlsResult drawCameraViewTransportControls(
    const CameraViewTransportControlsContext& context);

CameraViewPlaybackShortcutsResult
handleCameraViewPlaybackShortcuts(bool enabled = true);
