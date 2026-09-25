#pragma once

#include "stimulus_playback.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

struct DecodeDebugDumpContext {
    bool video_loaded = false;
    render_scene* scene = nullptr;
    const std::vector<std::string>* camera_names = nullptr;
    std::unordered_map<std::string, std::atomic<bool>>* window_need_decoding =
        nullptr;
    PlaybackState* playback_state = nullptr;
    StimulusPlayback* stimulus_player = nullptr;
    std::filesystem::path default_buffer_dump_root;
    double video_fps = 0.0;
};

struct RandomSeekDumpContext {
    DecodeDebugDumpContext dump_context;
    DecoderContext* decoder_context = nullptr;
    std::mt19937* debug_rng = nullptr;
    std::function<void(int, bool)> seek_to_frame;
    std::function<void(bool)> set_camera_decode_requests;
};

std::optional<std::filesystem::path> dumpDecodeBuffersToVideos(
    const DecodeDebugDumpContext& context,
    const std::string& tag,
    std::string& status);

void randomSeekAndDumpBuffers(const RandomSeekDumpContext& context,
                              std::string& status);
