#pragma once

#include "frame_slot.h"
#include "h5_loader.h"
#include "platform/nvidia/nvidia_playback_trace_model.h"
#include "playback_diagnostics.h"
#include "render.h"

#include <atomic>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace crimson::platform::nvidia::diagnostics {

trace::BoundingBoxSnapshot toTraceBoundingBox(const LoggedBoundingBox &box);

std::optional<trace::BoundingBoxSnapshot>
firstTraceBoundingBox(const std::vector<LoggedBoundingBox> &boxes);

trace::TextureDrawSnapshot
toTraceTextureDraw(const CameraTextureDrawTrace &trace);

struct SelectedRunSourceSnapshot {
  std::string work_unit_id;
  std::string detect_run;
  std::string refined_detect_run;
  std::string detect_group_path;
  std::string refined_group_path;
  std::string video_path;
};

std::optional<trace::SelectedRunSnapshot>
toTraceSelectedRun(const std::optional<SelectedRunSourceSnapshot> &selected);

struct ResolverSourceSnapshot {
  int64_t resolved_parent_frame_index = -1;
  int64_t recording_frame_id = -1;
  std::string clip_id;
  uint64_t run_index = 0;
  std::string camera_serial;
  int64_t clip_local_frame_index = -1;
  uint64_t selected_run_index = 0;
  std::optional<SelectedRunSourceSnapshot> selected_run;
};

std::optional<trace::ResolverSnapshot>
toTraceResolver(const std::optional<ResolverSourceSnapshot> &source);

std::optional<trace::DetectionSourceSnapshot>
toTraceDetectionSource(const std::vector<uint8_t> &source_codes,
                       const std::vector<std::string> &source_kinds);

// The caller owns the live scene and decoding map. This adapter reads only
// observable buffer metadata; it does not acquire a frame or mutate playback.
struct CameraBufferSnapshotRequest {
  bool video_loaded = false;
  bool require_video_loaded = true;
  const render_scene *scene = nullptr;
  int visible_idx = -1;
  int read_head = -1;
  int target_frame = -1;
  int selected_frame = -1;
  const std::vector<std::string> *camera_names = nullptr;
  const std::unordered_map<std::string, std::atomic<int>>
      *latest_decoded_frame = nullptr;
};

std::optional<playback::diagnostics::CameraBufferSnapshot>
makeCameraBufferSnapshot(const CameraBufferSnapshotRequest &request);

playback::diagnostics::ClippedPlaybackSnapshot makeClippedPlaybackSnapshot(
    int current_frame_num, double video_fps,
    const playback::diagnostics::PlaybackSnapshot &playback,
    const playback::diagnostics::SeekProgressSnapshot &seek,
    std::optional<playback::diagnostics::CameraBufferSnapshot> camera_buffer);

// The detailed clipped-frame trace predates the general playback trace and has
// a narrower, stable playback/buffer schema. Keep that compatibility surface
// out of the render loop while taking only already-captured diagnostic state.
struct ClippedFramePlaybackStateRequest {
  bool has_presented_camera_frame = false;
  int presented_slot = -1;
  double video_fps = 0.0;
  bool clipped_rebase_before_play = false;
  playback::diagnostics::PlaybackSnapshot playback;
  std::optional<playback::diagnostics::CameraBufferSnapshot> camera_buffer;
};

trace::Json makeClippedFramePlaybackStateJson(
    const ClippedFramePlaybackStateRequest &request);

} // namespace crimson::platform::nvidia::diagnostics
