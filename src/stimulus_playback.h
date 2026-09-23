#pragma once

#include "global.h"
#include "render.h"
#include "zarr/stimulus_repository.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>

enum class SeekState { Idle, WaitingMedia, WaitingCameras, WaitingStimulus, Ready, TimedOut };
enum class ResumePath {
    None,
    SmoothPause,
    BufferedSoft,
    CameraReanchor,
    HardSeekFallback,
};

inline const char *seekStateName(SeekState s) {
    switch (s) {
    case SeekState::Idle:             return "Idle";
    case SeekState::WaitingMedia:     return "WaitingMedia";
    case SeekState::WaitingCameras:   return "WaitingCameras";
    case SeekState::WaitingStimulus:  return "WaitingStimulus";
    case SeekState::Ready:            return "Ready";
    case SeekState::TimedOut:         return "TimedOut";
    }
    return "Unknown";
}

inline const char* resumePathName(ResumePath p) {
    switch (p) {
    case ResumePath::None: return "none";
    case ResumePath::SmoothPause: return "smooth resume from pause frame";
    case ResumePath::BufferedSoft: return "soft resume from active buffered span";
    case ResumePath::CameraReanchor: return "camera re-anchor from browsed frame";
    case ResumePath::HardSeekFallback: return "hard seek fallback";
    }
    return "Unknown";
}

struct SeekProgress {
    SeekState state = SeekState::Idle;
    uint64_t  seek_id = 0;           // monotonic generation counter
    int       requested_camera_frame = -1;
    int       target_camera_frame = 0;
    int       target_stimulus_frame = -1;
    bool      accurate = false;
    bool      skip_stimulus_hard_seek = false;
    int       cameras_settled = 0;
    int       cameras_total = 0;
    std::chrono::steady_clock::time_point deadline;
};

struct PlaybackState {
    int pause_selected = 0;
    int paused_frame_on_toggle = -1;
    bool buffer_browsed_since_pause = false;
    ResumePath last_resume_path = ResumePath::None;
    int last_resume_target_frame = -1;
    bool slider_just_changed = false;
    bool play_video = false;
    int to_display_frame_number = 0;
    int read_head = 0;
    bool just_seeked = false;
    bool pause_seeked = false;
    // Frames published by a rejected exact seek cannot be trusted by numeric
    // label alone. Keep the ring quarantined until a fresh backend generation
    // settles successfully.
    bool rejected_seek_ring_quarantined = false;
    int slider_frame_number = 0;
    double accumulated_play_time = 0.0;
    std::chrono::steady_clock::time_point last_play_time_start =
        std::chrono::steady_clock::now();
    int last_frame_num_playspeed = 0;
    std::chrono::steady_clock::time_point last_wall_time_playspeed =
        std::chrono::steady_clock::now();
    int current_stimulus_frame = -1;
};

struct StimulusPlayback {
    bool loaded = false;
    std::string window_name = "Stimulus";
    std::string video_path;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    int buffer_size = 100;
    bool use_cpu_buffer = false;
    bool use_software_decode = false;
    PictureBuffer *display_buffer = nullptr;
    PBO_CUDA pbo = {};
    GLuint texture = 0;
    std::unique_ptr<FFmpegDemuxer> demuxer;
    std::unique_ptr<DecoderContext> decoder_context;
    SeekInfo seek = {};
    std::thread decoder_thread;
    bool resources_initialized = false;
    int last_displayed_frame = -1;
    bool throttled = false;
    int throttle_resume_frame = -1;
    bool playback_catchup_seek_in_flight = false;
    uint64_t playback_catchup_seek_id = 0;
    int playback_catchup_target_frame = -1;
    std::chrono::steady_clock::time_point playback_catchup_last_request =
        std::chrono::steady_clock::time_point{};
};

// CPU-only probe result. The demuxer is not shared with a live decoder until
// the owner thread adopts it after the old decoder has stopped.
struct PreparedStimulusPlayback {
    std::string video_path;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    int buffer_size = 1;
    bool use_cpu_buffer = false;
    bool use_software_decode = false;
    std::unique_ptr<FFmpegDemuxer> demuxer;
};

void destroyStimulusPlayback(StimulusPlayback &stim);
void joinStimulusDecoder(StimulusPlayback &stim);
bool prepareStimulusPlayback(const std::string &video_path, int buffer_size,
                             bool use_cpu_buffer, bool use_software_decode,
                             PreparedStimulusPlayback &prepared,
                             std::string &error);
bool initializePreparedStimulusPlayback(StimulusPlayback &stim,
                                       PreparedStimulusPlayback prepared,
                                       int cuda_device_index,
                                       const std::function<void()> &poll_owner_events = {},
                                       const std::function<bool()> &opening_cancelled = {});
bool allocateStimulusBuffers(StimulusPlayback &stim,
                            const std::function<void()> &poll_owner_events = {},
                            const std::function<bool()> &opening_cancelled = {});
bool initializeStimulusPlayback(StimulusPlayback &stim,
                                const std::string &video_path,
                                int buffer_size,
                                bool use_cpu_buffer,
                                bool use_software_decode,
                                int cuda_device_index);
int findStimulusBuffer(const StimulusPlayback &stim, int target_frame);
void releaseStimulusBufferSlot(StimulusPlayback &stim, int index);
void uploadStimulusFrameToTexture(StimulusPlayback &stim, int buffer_index);
void discardStimulusFramesOlderThan(StimulusPlayback &stim, int keep_threshold);
int getOldestStimulusFrame(const StimulusPlayback &stim);
int getNewestStimulusFrame(const StimulusPlayback &stim);
void scheduleStimulusSeek(StimulusPlayback &stim,
                          const crimson::zarr::StimulusRepository *repository,
                          int camera_frame,
                          bool seek_accurate,
                          uint64_t seek_id = 0);
void seek_all_cameras(render_scene *scene, int frame_number, double video_fps,
                      PlaybackState &state, bool seek_accurate,
                      const crimson::zarr::StimulusRepository *repository,
                      StimulusPlayback *stimulus);

// Non-blocking seek API (Steps 3-4 of seek refactor)
void initiate_camera_seeks(render_scene *scene, int frame_number,
                           uint64_t seek_id, bool seek_accurate);
int poll_camera_seeks(render_scene *scene, uint64_t seek_id);
