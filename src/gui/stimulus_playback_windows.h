#pragma once

#include "stimulus_playback.h"

class ZarrDetectionLoader;

struct StimulusPlaybackWindowsState {
    bool last_decoder_logged = false;
};

struct StimulusPlaybackWindowsContext {
    StimulusPlayback& stimulus_player;
    ZarrDetectionLoader* zarr_loader = nullptr;
    PlaybackState& playback_state;
    SeekProgress& seek_progress;
    int current_frame_num = 0;
    double video_fps = 0.0;
    int latest_decoded_frame = -1;
    bool decoder_active_now = false;
    uint64_t* catchup_seek_generation = nullptr;
};

struct StimulusPlaybackWindowsResult {
    bool decoder_requested = false;
    double stimulus_window_ui_ms = 0.0;
    double stimulus_buffer_window_ui_ms = 0.0;
};

StimulusPlaybackWindowsResult drawStimulusPlaybackWindows(
    const StimulusPlaybackWindowsContext& context,
    StimulusPlaybackWindowsState& state);
