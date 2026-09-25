#pragma once

#include "camera.h"
#include "clipped_media_transition_worker.h"
#include "clipped_media_handoff.h"
#include "media_selection_plan.h"
#include "playback_clock.h"
#include "recording_clip_media_provider.h"
#include "recording_open_workflow.h"
#include "stimulus_media_open.h"
#include "stimulus_playback.h"
#include "ui_path_config.h"
#include "zarr_loader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
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
  std::string camera_serial;
  std::shared_ptr<const std::vector<int64_t>> parent_frame_by_clip_local;
  crimson::playback::ClippedMediaHandoffState handoff;
};

struct PreparedCameraMedia {
  crimson::media::CameraMediaOpenPlan plan;
  std::vector<std::unique_ptr<FFmpegDemuxer>> demuxers;
  std::vector<std::pair<int, int>> camera_dimensions;
  int seek_interval = 1;
  double video_fps = 0.0;
  int buffer_size = 1;
};

enum class DecoderFrameResolutionStatus : uint8_t {
  Ready, ReadyBuffered, Pending, Failed
};
struct DecoderFrameResolution {
  DecoderFrameResolutionStatus status = DecoderFrameResolutionStatus::Failed;
  int decoder_frame = -1;
  std::string error;
};

struct AffiliatedMediaDiscovery {
  std::optional<std::filesystem::path> video_path;
  std::optional<std::filesystem::path> clip_index_path;
  std::string error;
};

AffiliatedMediaDiscovery discoverAffiliatedMedia(
    const std::string &archive_path, const std::string &legacy_source_hint);

bool prepareCameraMedia(const crimson::media::CameraMediaOpenPlan &plan,
                        const std::string &image_root, int buffer_size,
                        double video_fps, PreparedCameraMedia &prepared,
                        std::string &error);
std::string discoverRecordingFallbackVideo(const std::string &recording_root);
bool prepareCameraCalibrations(const std::vector<std::string> &camera_names,
                               const std::string &recording_root,
                               ZarrDetectionLoader *zarr_loader,
                               bool zarr_loaded,
                               std::vector<CameraParams> &camera_params,
                               std::string &error);

struct MediaSessionLoaderContext {
  render_scene *scene = nullptr;
  DecoderContext *decoder_context = nullptr;
  ZarrDetectionLoader *zarr_loader = nullptr;
  const crimson::zarr::StimulusRepository *stimulus_repository = nullptr;
  StimulusPlayback *stimulus_player = nullptr;
  PlaybackState *playback_state = nullptr;
  crimson::playback::PlaybackTransportController *playback_transport = nullptr;

  std::string *root_dir = nullptr;
  std::string *skeleton_dir = nullptr;
  std::vector<std::string> *camera_names = nullptr;
  std::vector<std::string> *image_names = nullptr;
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
  // Called on the UI owner thread. The adapter may load archive data on a
  // worker while it alone pumps a loading-only GUI frame.
  std::function<bool(const std::string &, bool, std::string &)> open_archive;
  std::function<bool()> opening_cancelled;
  std::function<bool(const crimson::media::CameraMediaOpenPlan &,
                     const std::string &, int, double, PreparedCameraMedia &,
                     std::string &)> prepare_camera_media;
  std::function<void(std::vector<std::thread> &)> join_camera_decoders;
  std::function<std::string(const std::string &)> discover_recording_video;
  std::function<std::shared_ptr<const crimson::media::RecordingClipMediaProvider>(
      const std::filesystem::path &, std::string &)> prepare_recording_clip_index;
  std::function<bool(const std::vector<std::string> &, const std::string &,
                     ZarrDetectionLoader *, bool, std::vector<CameraParams> &,
                     std::string &)> prepare_camera_calibrations;
  std::function<void()> poll_owner_events;
  std::function<bool(const crimson::media::StimulusMediaOpenRequest &,
                     PreparedStimulusPlayback &, std::string &)> prepare_stimulus;
  std::function<void(StimulusPlayback &)> join_stimulus_decoder;
  std::function<AffiliatedMediaDiscovery(const std::string &,
                                         const std::string &)> discover_affiliated_media;
  std::function<std::optional<std::filesystem::path>(
      const std::string &, const std::string &, const std::string &,
      const std::string &)> resolve_stimulus_video;
  // Worker-only seams for the nonblocking clipped-media transition.
  std::function<bool(const crimson::media::CameraMediaOpenPlan &,
                     const std::string &, int, double, PreparedCameraMedia &,
                     std::string &)> prepare_clip_media_async;
  std::function<void(std::vector<std::thread> &)> join_clip_decoders_async;
};

class MediaSessionLoader {
public:
  explicit MediaSessionLoader(const MediaSessionLoaderContext &context);
  ~MediaSessionLoader();

  bool loadSelectedCameraMedia(
      const std::vector<crimson::media::CameraMediaSelection> &selections,
      std::string &error_message) const;
  void loadCameraCalibrationsForCurrentMedia() const;
  void tryAutoLoadAffiliatedVideoFromZarr(const char *trigger_label) const;
  void tryAutoLoadStimulusVideo(const char *trigger_label) const;
  crimson::media::StimulusMediaOpenResult openStimulusMedia(
      const crimson::media::StimulusMediaOpenRequest &request) const;
  bool loadClippedVideoForParentFrame(int parent_frame) const;
  bool hasMappedMedia() const;
  int64_t mappedMediaFrameCount() const;
  crimson::playback::ClippedFrameBinding
  resolveMappedMediaFrame(int64_t parent_frame) const;
  std::optional<int> resolveDecoderFrameForParentFrame(int parent_frame) const;
  DecoderFrameResolution requestDecoderFrameForParentFrame(int parent_frame);
  // Owner-thread poll during sequential playback. Prepares only the immediate
  // next recording clip; ordinary seeks keep the existing switch path.
  void pollSequentialClipPrewarm(int presented_parent_frame,
                                bool playback_active);
  bool cancelPendingClipSwitch(bool restore_current = true);
  void restoreCurrentClipDecoderAfterCancellation();
  bool hasPendingClipSwitch() const;
  bool hasPendingPrewarmActivity() const;
  void stopAdoptedDecoderForShutdown();
  std::string activeRecordingClipIndexPath() const;
  void bootstrapFromCli(
      const std::string &cli_zarr_override_path,
      const std::string &cli_recording_path,
      const std::string &cli_recording_clip_index_path,
      const std::function<void()> &refresh_detection_dataset_options,
      const std::function<void()> &clear_bbox_edits) const;

private:
  struct PrewarmCandidate;
  struct ClipSwitchWork {
    crimson::playback::ClippedMediaTransitionWorker stage;
    bool ready = false;
    PreparedCameraMedia prepared;
    std::string error;
    std::chrono::steady_clock::time_point started;
  };
  std::unique_ptr<ClipSwitchWork> clip_switch_work_;
  std::shared_ptr<PrewarmCandidate> prewarm_candidate_;
  mutable std::shared_ptr<PrewarmCandidate> adopted_candidate_;
  crimson::playback::ClippedMediaTransitionWorker prewarm_cleanup_stage_;
  crimson::playback::ClippedMediaTransitionWorker prewarm_old_join_stage_;
  bool prewarm_old_retiring_ = false;
  bool prewarm_old_retired_ = false;
  bool prewarm_adoption_fallback_ = false;
  int prewarm_failed_boundary_ = -1;
  int prewarm_cleanup_launch_failures_ = 0;
  void discardPrewarmCandidate();
  bool adoptPrewarmCandidate(int parent_frame, int local_frame);
  int pending_clip_parent_frame_ = -1;
  std::string pending_clip_video_path_;
  bool clip_join_started_ = false;
  crimson::playback::ClippedMediaTransitionWorker clip_join_stage_;
  std::string clip_join_error_;
  std::chrono::steady_clock::time_point clip_join_started_at_;
  void finishClipSwitchWorkers();
  void restartCurrentClipDecoder();
  void stopCameraDecodersForReload() const;
  bool loadClippedVideoForParentFrame(int parent_frame,
                                      PreparedCameraMedia *preprepared,
                                      bool start_decoder = true,
                                      PrewarmCandidate *pending_adoption = nullptr) const;
  bool loadSingleVideoMedia(const std::filesystem::path &video_path,
                            bool infer_recording_root,
                            const char *success_label) const;
  bool executeCameraMediaPlan(const crimson::media::CameraMediaOpenPlan &plan,
                              bool infer_recording_root,
                              const char *success_label,
                              std::string *error_message) const;
  bool activateRecordingClipIndex(const std::filesystem::path &index_path,
                                  std::string *error_message = nullptr) const;

  const MediaSessionLoaderContext context_;
};
