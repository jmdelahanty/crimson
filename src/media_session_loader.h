#pragma once

#include "camera.h"
#include "stimulus_playback.h"
#include "ui_path_config.h"
#include "zarr_loader.h"

#include <atomic>
#include <functional>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct PaletteClippedMediaState {
    std::string current_video_path;
    std::string clip_id;
    std::string camera_serial;
    std::shared_ptr<const std::vector<int64_t>> parent_frame_by_clip_local;
    size_t selected_run_index = std::numeric_limits<size_t>::max();
    int64_t first_parent_frame = -1;
    int64_t last_parent_frame = -1;
    int64_t pending_switch_parent_frame = -1;
    bool switch_in_progress = false;
    int64_t last_presented_parent_frame = -1;
};

struct MediaSessionLoaderContext {
    render_scene* scene = nullptr;
    DecoderContext* decoder_context = nullptr;
    ZarrDetectionLoader* zarr_loader = nullptr;
    StimulusPlayback* stimulus_player = nullptr;
    PlaybackState* playback_state = nullptr;

    std::string* root_dir = nullptr;
    std::string* skeleton_dir = nullptr;
    std::vector<std::string>* camera_names = nullptr;
    std::vector<CameraParams>* camera_params = nullptr;
    std::vector<std::thread>* decoder_threads = nullptr;
    std::vector<std::unique_ptr<FFmpegDemuxer>>* demuxers = nullptr;
    std::vector<bool>* is_view_focused = nullptr;

    std::unordered_map<std::string, std::atomic<bool>>* window_need_decoding =
        nullptr;
    std::unordered_map<std::string, bool>* window_was_decoding = nullptr;
    PaletteClippedMediaState* clipped_media_state = nullptr;

    bool* video_loaded = nullptr;
    bool* zarr_loaded = nullptr;
    bool* input_is_imgs = nullptr;
    bool* show_error = nullptr;
    std::string* error_message = nullptr;

    int* label_buffer_size = nullptr;
    int* stimulus_buffer_size = nullptr;
    bool* stimulus_use_cpu_buffer = nullptr;
    bool* stimulus_use_software_decode = nullptr;
    double* video_fps = nullptr;
    int cuda_device_index = 0;
};

class MediaSessionLoader {
  public:
    explicit MediaSessionLoader(const MediaSessionLoaderContext& context);

    void loadCameraCalibrationsForCurrentMedia() const;
    void tryAutoLoadAffiliatedVideoFromZarr(const char* trigger_label) const;
    void tryAutoLoadStimulusVideo(const char* trigger_label) const;
    bool loadClippedVideoForParentFrame(int parent_frame) const;
    void bootstrapFromCli(
        const std::string& cli_zarr_override_path,
        const std::string& cli_recording_path,
        const std::function<void()>& refresh_detection_dataset_options,
        const std::function<void()>& clear_bbox_edits) const;

  private:
    void stopCameraDecodersForReload() const;
    bool loadSingleVideoMedia(const std::filesystem::path& video_path,
                              bool infer_recording_root,
                              const char* success_label) const;

    const MediaSessionLoaderContext context_;
};
