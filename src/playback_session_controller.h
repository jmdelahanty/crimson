#pragma once

#include "debug_flags.h"
#include "gui/camera_view_presenter.h"
#include "playback_clock.h"
#include "playback_presentation_lifecycle.h"
#include "stimulus_playback.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct PlaybackSessionControllerContext {
  render_scene *scene = nullptr;
  DecoderContext *decoder_context = nullptr;
  ZarrDetectionLoader *zarr_loader = nullptr;
  StimulusPlayback *stimulus_player = nullptr;
  PlaybackState *playback_state = nullptr;
  crimson::playback::PlaybackTransportController *playback_transport = nullptr;
  SeekProgress *seek_progress = nullptr;
  int *current_frame_num = nullptr;
  double *video_fps = nullptr;
  std::vector<std::string> *camera_names = nullptr;
  std::unordered_map<std::string, bool> *window_was_decoding = nullptr;
  std::unordered_map<std::string, std::atomic<bool>> *window_need_decoding =
      nullptr;
  // Resolves the parent recording frame to the active decoder's local frame,
  // loading/switching media when required. Single-file adapters return the
  // parent frame unchanged.
  std::function<std::optional<int>(int)> resolve_decoder_frame_for_parent_frame;
};

double playbackPreviewScaleFactor(int playback_preview_scale_mode);
const char *playbackPreviewScaleLabel(int playback_preview_scale_mode);
bool playbackPreviewIsActive(bool play_video, bool yolo_detection,
                             int playback_preview_scale_mode);
const char *playbackRendererModeLabel(int playback_renderer_mode);
bool playbackLightweightRendererIsActive(bool play_video,
                                         int playback_renderer_mode);

class PlaybackSessionController {
public:
  explicit PlaybackSessionController(
      const PlaybackSessionControllerContext &context);

  int getVisibleCameraIndex() const;
  void setCameraDecodeRequests(bool enabled) const;
  int countBufferedStimulusFrames() const;
  int findNearestPausedBufferSlot(int visible_idx, int target_frame) const;
  bool stepPausedFrameFromBuffer(int target_frame) const;
  crimson::playback::PlaybackSeekExecutionResult
  seekToFrame(int target_frame, bool prefer_buffer_when_paused,
              bool force_inaccurate = false,
              bool skip_stimulus_hard_seek = false) const;
  void syncPlaybackStartToCurrentFrame() const;
  void stepFrames(int delta_frames) const;
  void applyPlaybackToggle() const;
  std::optional<crimson::playback::PlaybackSeekExecutionResult>
  pollSeekState() const;
  crimson::playback::PlaybackPresentationTarget
  planPresentationTarget(int requested_frame,
                         std::optional<int> minimum_decoded_frame) const;
  crimson::playback::PlaybackPresentationCommit
  commitPresentedFrame(int presenter_target_frame, int presented_frame,
                       int presented_slot) const;

private:
  int findDisplaySlotForFrame(int cam_idx, int target_frame,
                              int preferred_slot) const;
  void releaseBufferedHistoryBeforeFrame(int cam_idx, int frame) const;
  bool resumeFromBufferedFrame(int resume_frame) const;
  bool isWithinNewestContiguousBufferedSpan(int frame) const;

  const PlaybackSessionControllerContext context_;
};
