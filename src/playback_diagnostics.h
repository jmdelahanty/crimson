#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crimson::playback::diagnostics {

using Json = nlohmann::json;

// A backend supplies only the observable state of a decoded display slot.
struct BufferSlotSnapshot {
  bool readable = false;
  int frame_number = -1;
};

struct CameraBufferSnapshot {
  int visible_idx = -1;
  std::optional<std::string> camera_name;
  int buffer_size = 0;
  int read_head = -1;
  int target_frame = -1;
  int selected_frame = -1;
  int last_uploaded_frame = -1;
  int last_uploaded_local_frame = -1;
  bool texture_has_valid_frame = false;
  bool staging_valid = false;
  int staging_frame = -1;
  int staging_local_frame = -1;
  int latest_decoded_frame = -1;
  std::vector<BufferSlotSnapshot> slots;
};

struct PlaybackSnapshot {
  bool play_video = false;
  int to_display_frame_number = -1;
  int slider_frame_number = -1;
  int read_head = -1;
  bool pause_selected = false;
  bool pause_seeked = false;
  bool just_seeked = false;
  bool slider_just_changed = false;
  bool buffer_browsed_since_pause = false;
  int paused_frame_on_toggle = -1;
  std::string last_resume_path;
  int last_resume_target_frame = -1;
  double accumulated_play_time = 0.0;
  int current_stimulus_frame = -1;
};

struct SeekProgressSnapshot {
  std::string state;
  uint64_t seek_id = 0;
  int requested_camera_frame = -1;
  int target_camera_frame = -1;
  int target_stimulus_frame = -1;
  bool accurate = false;
  bool skip_stimulus_hard_seek = false;
  int cameras_settled = 0;
  int cameras_total = 0;
};

struct PresenterSnapshot {
  int view_idx = -1;
  int target_frame = -1;
  int preferred_paused_slot = -1;
  int presented_slot = -1;
  int presented_frame = -1;
  int resolved_frame = -1;
  bool prewarm_active = false;
};

struct StimulusBufferSnapshot {
  bool loaded = false;
  std::string window_name;
  int current_stimulus_frame = -1;
  int latest_decoded_frame = -1;
  int last_displayed_frame = -1;
  int buffer_size = 0;
  std::vector<BufferSlotSnapshot> slots;
  bool seek_use = false;
  bool seek_done = false;
  uint64_t seek_id = 0;
  uint64_t seek_frame = 0;
  uint64_t settled_seek_id = 0;
};

struct PlaybackTraceSnapshot {
  bool video_loaded = false;
  double video_fps = 0.0;
  int current_frame_num = -1;
  int window_need_decoding_count = 0;
  PlaybackSnapshot playback;
  SeekProgressSnapshot seek;
  PresenterSnapshot presenter;
  std::optional<CameraBufferSnapshot> camera_buffer;
  StimulusBufferSnapshot stimulus;
};

struct ClippedPlaybackSnapshot {
  int current_frame_num = -1;
  double video_fps = 0.0;
  PlaybackSnapshot playback;
  SeekProgressSnapshot seek;
  std::optional<CameraBufferSnapshot> camera_buffer;
};

struct ClippedMediaStateSnapshot {
  std::string current_video_path;
  std::string clip_id;
  std::string camera_serial;
  uint64_t selected_run_index = 0;
  int64_t first_parent_frame = -1;
  int64_t last_parent_frame = -1;
  int64_t pending_switch_parent_frame = -1;
  bool switch_in_progress = false;
  int64_t last_presented_parent_frame = -1;
};

Json nullableInt(int value);
Json cameraBufferJson(const CameraBufferSnapshot &snapshot);
Json playbackCameraBufferJson(const CameraBufferSnapshot &snapshot);
Json clippedCameraBufferJson(const CameraBufferSnapshot &snapshot);
Json clippedPlaybackStateJson(const ClippedPlaybackSnapshot &snapshot);
Json clippedMediaStateJson(const ClippedMediaStateSnapshot &snapshot);
Json playbackTraceEventJson(const std::string &event_name, const Json &details,
                            const PlaybackTraceSnapshot &snapshot);
Json frameSyncTraceEventJson(const Json &details,
                             const PlaybackTraceSnapshot &snapshot);
Json clippedPlaybackStateEventJson(const std::string &playback_event,
                                   const Json &details,
                                   const ClippedPlaybackSnapshot &snapshot);
Json clippedHandoffTraceEventJson(
    const std::string &handoff_event, const Json &details, bool video_loaded,
    int current_frame_num, const PlaybackSnapshot &playback,
    const std::optional<ClippedMediaStateSnapshot> &clipped_state);

// JSONL sink with the historical Crimson playback-trace envelope and flush
// cadence. Callers retain responsibility for deciding which events to write.
class PlaybackTraceJsonlWriter {
public:
  bool open(const std::filesystem::path &output_path,
            std::string label = "PlaybackTrace");
  bool enabled() const;
  const std::filesystem::path &path() const;
  const std::string &lastError() const;
  void write(Json sample, bool force_flush = false);
  void close();

private:
  std::ofstream stream_;
  std::filesystem::path path_;
  std::string label_ = "PlaybackTrace";
  std::string last_error_;
  std::chrono::steady_clock::time_point started_{};
  uint64_t sequence_ = 0;
  int samples_since_flush_ = 0;
};

} // namespace crimson::playback::diagnostics
