#pragma once

#include "camera.h"
#include "playback_clock.h"
#include "recording_clip_media_provider.h"
#include "recording_open_workflow.h"
#include "stimulus_playback.h"
#include "ui_path_config.h"
#include "zarr_loader.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class ClippedMediaSource : uint8_t {
  None,
  RecordingClipIndex,
  LegacyZarrCollection,
};

struct PaletteClippedMediaState {
  ClippedMediaSource source = ClippedMediaSource::None;
  std::shared_ptr<const crimson::media::RecordingClipMediaProvider>
      recording_clip_provider;
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
  render_scene *scene = nullptr;
  DecoderContext *decoder_context = nullptr;
  ZarrDetectionLoader *zarr_loader = nullptr;
  StimulusPlayback *stimulus_player = nullptr;
  PlaybackState *playback_state = nullptr;
  crimson::playback::PlaybackTransportController *playback_transport = nullptr;

  std::string *root_dir = nullptr;
  std::string *skeleton_dir = nullptr;
  std::vector<std::string> *camera_names = nullptr;
  std::vector<CameraParams> *camera_params = nullptr;
  std::vector<std::thread> *decoder_threads = nullptr;
  std::vector<std::unique_ptr<FFmpegDemuxer>> *demuxers = nullptr;
  std::vector<bool> *is_view_focused = nullptr;

  std::unordered_map<std::string, std::atomic<bool>> *window_need_decoding =
      nullptr;
  std::unordered_map<std::string, bool> *window_was_decoding = nullptr;
  PaletteClippedMediaState *clipped_media_state = nullptr;

  bool *video_loaded = nullptr;
  bool *zarr_loaded = nullptr;
  bool *input_is_imgs = nullptr;
  bool *show_error = nullptr;
  std::string *error_message = nullptr;

  int *label_buffer_size = nullptr;
  int *stimulus_buffer_size = nullptr;
  bool *stimulus_use_cpu_buffer = nullptr;
  bool *stimulus_use_software_decode = nullptr;
  double *video_fps = nullptr;
  crimson::session::RecordingOpenWorkflowController *recording_opens = nullptr;
  int cuda_device_index = 0;
};

class MediaSessionLoader {
public:
  explicit MediaSessionLoader(const MediaSessionLoaderContext &context);

  void loadCameraCalibrationsForCurrentMedia() const;
  void tryAutoLoadAffiliatedVideoFromZarr(const char *trigger_label) const;
  void tryAutoLoadStimulusVideo(const char *trigger_label) const;
  bool loadClippedVideoForParentFrame(int parent_frame) const;
  std::optional<int> resolveDecoderFrameForParentFrame(int parent_frame) const;
  std::string activeRecordingClipIndexPath() const;
  void bootstrapFromCli(
      const std::string &cli_zarr_override_path,
      const std::string &cli_recording_path,
      const std::string &cli_recording_clip_index_path,
      const std::function<void()> &refresh_detection_dataset_options,
      const std::function<void()> &clear_bbox_edits) const;

private:
  void stopCameraDecodersForReload() const;
  bool loadSingleVideoMedia(const std::filesystem::path &video_path,
                            bool infer_recording_root,
                            const char *success_label) const;
  bool activateRecordingClipIndex(const std::filesystem::path &index_path,
                                  std::string *error_message = nullptr) const;

  const MediaSessionLoaderContext context_;
};
