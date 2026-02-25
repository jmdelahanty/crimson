#pragma once

#include "global.h"
#include "render.h"
#include "zarr_loader.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>

enum class SeekState { Idle, WaitingCameras, WaitingStimulus, Ready, TimedOut };

inline const char *seekStateName(SeekState s) {
    switch (s) {
    case SeekState::Idle:             return "Idle";
    case SeekState::WaitingCameras:   return "WaitingCameras";
    case SeekState::WaitingStimulus:  return "WaitingStimulus";
    case SeekState::Ready:            return "Ready";
    case SeekState::TimedOut:         return "TimedOut";
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
    int       cameras_settled = 0;
    int       cameras_total = 0;
    std::chrono::steady_clock::time_point deadline;
};

struct PlaybackState {
    int pause_selected = 0;
    bool slider_just_changed = false;
    bool play_video = false;
    int to_display_frame_number = 0;
    int read_head = 0;
    bool just_seeked = false;
    bool pause_seeked = false;
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
    PictureBuffer *display_buffer = nullptr;
    PBO_CUDA pbo = {};
    GLuint texture = 0;
    std::unique_ptr<FFmpegDemuxer> demuxer;
    std::unique_ptr<DecoderContext> decoder_context;
    SeekInfo seek = {false, false, 0, false, 0, 0};
    std::thread decoder_thread;
    bool resources_initialized = false;
    int last_displayed_frame = -1;
    bool throttled = false;
    int throttle_resume_frame = -1;
};

void destroyStimulusPlayback(StimulusPlayback &stim);
bool allocateStimulusBuffers(StimulusPlayback &stim);
bool initializeStimulusPlayback(StimulusPlayback &stim,
                                const std::string &video_path,
                                int buffer_size,
                                int cuda_device_index);
int findStimulusBuffer(const StimulusPlayback &stim, int target_frame);
void releaseStimulusBufferSlot(StimulusPlayback &stim, int index);
void uploadStimulusFrameToTexture(StimulusPlayback &stim, int buffer_index);
void discardStimulusFramesOlderThan(StimulusPlayback &stim, int keep_threshold);
int getOldestStimulusFrame(const StimulusPlayback &stim);
int getNewestStimulusFrame(const StimulusPlayback &stim);
void scheduleStimulusSeek(StimulusPlayback &stim,
                          ZarrDetectionLoader *loader,
                          int camera_frame,
                          bool seek_accurate,
                          uint64_t seek_id = 0);
void seek_all_cameras(render_scene *scene, int frame_number, double video_fps,
                      PlaybackState &state, bool seek_accurate,
                      ZarrDetectionLoader *zarr_loader,
                      StimulusPlayback *stimulus);

// Non-blocking seek API (Steps 3-4 of seek refactor)
void initiate_camera_seeks(render_scene *scene, int frame_number,
                           uint64_t seek_id, bool seek_accurate);
int poll_camera_seeks(render_scene *scene, uint64_t seek_id);
