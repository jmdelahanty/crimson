#pragma once

#include "stimulus_playback.h"

struct StimulusPlaybackPresentationState {
    bool last_decoder_logged = false;
    int last_logged_target_stimulus_frame = -2;
    int last_logged_displayed_stimulus_frame = -2;
    bool last_logged_decoder_requested = false;
    bool last_logged_throttled = false;
    int presentation_debug_logs = 0;
};

struct StimulusPlaybackPresentationContext {
    StimulusPlayback& stimulus_player;
    const crimson::zarr::StimulusRepository* stimulus_repository = nullptr;
    PlaybackState& playback_state;
    SeekProgress& seek_progress;
    int current_frame_num = 0;
    double video_fps = 0.0;
    int latest_decoded_frame = -1;
    bool decoder_active_now = false;
    uint64_t* catchup_seek_generation = nullptr;
};

struct StimulusPlaybackPresentationResult {
    bool decoder_requested = false;
    bool uploaded_frame = false;
    int target_stimulus_frame = -1;
    int displayed_stimulus_frame = -1;
    double update_ms = 0.0;
};

struct StimulusPlaybackDebugWindowsContext {
    StimulusPlayback& stimulus_player;
    const crimson::zarr::StimulusRepository* stimulus_repository = nullptr;
    PlaybackState& playback_state;
    SeekProgress& seek_progress;
    int current_frame_num = 0;
    int latest_decoded_frame = -1;
};

struct StimulusPlaybackDebugWindowsResult {
    double stimulus_window_ui_ms = 0.0;
    double stimulus_buffer_window_ui_ms = 0.0;
};

StimulusPlaybackPresentationResult updateStimulusPlaybackPresentation(
    const StimulusPlaybackPresentationContext& context,
    StimulusPlaybackPresentationState& state);

StimulusPlaybackDebugWindowsResult drawStimulusPlaybackDebugWindows(
    const StimulusPlaybackDebugWindowsContext& context);
