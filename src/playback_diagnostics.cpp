#include "playback_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <system_error>

namespace crimson::playback::diagnostics {
namespace {

Json playbackJson(const PlaybackSnapshot &snapshot, bool nullable_paused,
                  bool include_resume_fields, bool include_stimulus_frame,
                  bool include_buffer_browsed_since_pause,
                  bool include_paused_frame) {
  Json result = {
      {"play_video", snapshot.play_video},
      {"to_display_frame_number", snapshot.to_display_frame_number},
      {"slider_frame_number", snapshot.slider_frame_number},
      {"read_head", snapshot.read_head},
      {"pause_seeked", snapshot.pause_seeked},
      {"just_seeked", snapshot.just_seeked},
      {"slider_just_changed", snapshot.slider_just_changed},
  };
  if (include_paused_frame) {
    result["paused_frame_on_toggle"] =
        nullable_paused ? nullableInt(snapshot.paused_frame_on_toggle)
                        : Json(snapshot.paused_frame_on_toggle);
  }
  if (include_buffer_browsed_since_pause) {
    result["buffer_browsed_since_pause"] = snapshot.buffer_browsed_since_pause;
  }
  if (include_resume_fields) {
    result["pause_selected"] = snapshot.pause_selected;
    result["last_resume_path"] = snapshot.last_resume_path;
    result["last_resume_target_frame"] =
        nullableInt(snapshot.last_resume_target_frame);
    result["accumulated_play_time"] = snapshot.accumulated_play_time;
  }
  if (include_stimulus_frame) {
    result["current_stimulus_frame"] = snapshot.current_stimulus_frame;
  }
  return result;
}

Json seekJson(const SeekProgressSnapshot &snapshot, bool nullable_frames,
              bool include_camera_counts) {
  const auto frame = [nullable_frames](int value) {
    return nullable_frames ? nullableInt(value) : Json(value);
  };
  Json result = {
      {"state", snapshot.state},
      {"seek_id", snapshot.seek_id},
      {"requested_camera_frame", frame(snapshot.requested_camera_frame)},
      {"target_camera_frame", frame(snapshot.target_camera_frame)},
      {"target_stimulus_frame", frame(snapshot.target_stimulus_frame)},
      {"accurate", snapshot.accurate},
      {"skip_stimulus_hard_seek", snapshot.skip_stimulus_hard_seek},
  };
  if (include_camera_counts) {
    result["cameras_settled"] = snapshot.cameras_settled;
    result["cameras_total"] = snapshot.cameras_total;
  }
  return result;
}

int normalizedReadHead(const CameraBufferSnapshot &snapshot) {
  if (snapshot.read_head < 0 || snapshot.buffer_size <= 0) {
    return -1;
  }
  return snapshot.read_head % snapshot.buffer_size;
}

} // namespace

Json nullableInt(int value) { return value >= 0 ? Json(value) : Json(nullptr); }

Json cameraBufferJson(const CameraBufferSnapshot &snapshot) {
  const int normalized_read_head = normalizedReadHead(snapshot);
  int valid_slots = 0;
  int oldest_frame = std::numeric_limits<int>::max();
  int newest_frame = -1;
  int exact_target_slot = -1;
  int selected_slot = -1;
  int front_slot = -1;
  int read_head_frame = -1;
  std::vector<int> valid_frames;
  std::vector<int> sample_frames;
  valid_frames.reserve(snapshot.slots.size());
  sample_frames.reserve(std::min<size_t>(snapshot.slots.size(), 12));
  for (size_t index = 0; index < snapshot.slots.size(); ++index) {
    const auto &slot = snapshot.slots[index];
    if (!slot.readable || slot.frame_number < 0) {
      continue;
    }
    const int slot_index = static_cast<int>(index);
    ++valid_slots;
    valid_frames.push_back(slot.frame_number);
    oldest_frame = std::min(oldest_frame, slot.frame_number);
    newest_frame = std::max(newest_frame, slot.frame_number);
    if (slot.frame_number == snapshot.target_frame) {
      exact_target_slot = slot_index;
    }
    if (slot.frame_number == snapshot.selected_frame) {
      selected_slot = slot_index;
    }
    if (slot.frame_number == snapshot.last_uploaded_frame) {
      front_slot = slot_index;
    }
    if (slot_index == normalized_read_head) {
      read_head_frame = slot.frame_number;
    }
    if (sample_frames.size() < 12) {
      sample_frames.push_back(slot.frame_number);
    }
  }
  std::sort(valid_frames.begin(), valid_frames.end());
  std::sort(sample_frames.begin(), sample_frames.end());
  int contiguous_span_start = -1;
  int contiguous_span_end = -1;
  if (!valid_frames.empty()) {
    contiguous_span_end = valid_frames.back();
    contiguous_span_start = contiguous_span_end;
    for (int index = static_cast<int>(valid_frames.size()) - 2; index >= 0;
         --index) {
      if (valid_frames[static_cast<size_t>(index)] + 1 ==
          contiguous_span_start) {
        --contiguous_span_start;
        continue;
      }
      break;
    }
  }
  return {
      {"visible_idx", snapshot.visible_idx},
      {"camera_name",
       snapshot.camera_name ? Json(*snapshot.camera_name) : Json(nullptr)},
      {"read_head", snapshot.read_head},
      {"normalized_read_head", normalized_read_head},
      {"read_head_frame", nullableInt(read_head_frame)},
      {"selected_slot", selected_slot},
      {"front_slot", front_slot},
      {"valid_slots", valid_slots},
      {"buffer_size", snapshot.buffer_size},
      {"oldest_frame", valid_slots > 0 ? Json(oldest_frame) : Json(nullptr)},
      {"newest_frame", valid_slots > 0 ? Json(newest_frame) : Json(nullptr)},
      {"newest_contiguous_span_start", nullableInt(contiguous_span_start)},
      {"newest_contiguous_span_end", nullableInt(contiguous_span_end)},
      {"sample_frames", sample_frames},
      {"exact_target_slot", exact_target_slot},
      {"contains_target", exact_target_slot >= 0},
      {"latest_decoded_frame", snapshot.latest_decoded_frame},
      {"texture_has_valid_frame", snapshot.texture_has_valid_frame},
      {"last_uploaded_frame", snapshot.last_uploaded_frame},
      {"front_parent_frame", snapshot.texture_has_valid_frame
                                 ? Json(snapshot.last_uploaded_frame)
                                 : Json(nullptr)},
      {"front_local_frame", snapshot.texture_has_valid_frame
                                ? Json(snapshot.last_uploaded_local_frame)
                                : Json(nullptr)},
      {"staging_valid", snapshot.staging_valid},
      {"staging_frame", snapshot.staging_frame},
      {"staging_parent_frame",
       snapshot.staging_valid ? Json(snapshot.staging_frame) : Json(nullptr)},
      {"staging_local_frame", snapshot.staging_valid
                                  ? Json(snapshot.staging_local_frame)
                                  : Json(nullptr)},
  };
}

Json playbackCameraBufferJson(const CameraBufferSnapshot &snapshot) {
  Json result = cameraBufferJson(snapshot);
  // The generic summary deliberately includes the union of the two historical
  // trace views. Keep the playback trace's v1 surface exact.
  result.erase("read_head");
  result.erase("normalized_read_head");
  result.erase("selected_slot");
  result.erase("front_slot");
  result.erase("newest_contiguous_span_start");
  result.erase("newest_contiguous_span_end");
  result.erase("front_parent_frame");
  result.erase("front_local_frame");
  result.erase("staging_parent_frame");
  result.erase("staging_local_frame");
  if (result["read_head_frame"].is_null()) {
    result["read_head_frame"] = -1;
  }
  return result;
}

Json clippedCameraBufferJson(const CameraBufferSnapshot &snapshot) {
  Json result = cameraBufferJson(snapshot);
  // `clipped_playback_state` has its own intentionally narrower v1 buffer
  // schema; do not grow it merely because the playback trace needs more data.
  result.erase("camera_name");
  result.erase("sample_frames");
  result.erase("exact_target_slot");
  result.erase("contains_target");
  result.erase("latest_decoded_frame");
  result.erase("texture_has_valid_frame");
  result.erase("last_uploaded_frame");
  result.erase("staging_frame");
  return result;
}

Json clippedPlaybackStateJson(const ClippedPlaybackSnapshot &snapshot) {
  return {
      {"current_frame_num", snapshot.current_frame_num},
      {"video_fps", snapshot.video_fps},
      {"playback",
       playbackJson(snapshot.playback, true, true, false, true, true)},
      {"seek_progress", seekJson(snapshot.seek, true, false)},
      {"buffer", snapshot.camera_buffer
                     ? clippedCameraBufferJson(*snapshot.camera_buffer)
                     : Json(nullptr)},
  };
}

Json clippedMediaStateJson(const ClippedMediaStateSnapshot &snapshot) {
  return {
      {"current_video_path", snapshot.current_video_path},
      {"clip_id", snapshot.clip_id},
      {"camera_serial", snapshot.camera_serial},
      {"selected_run_index", snapshot.selected_run_index},
      {"first_parent_frame", snapshot.first_parent_frame},
      {"last_parent_frame", snapshot.last_parent_frame},
      {"pending_switch_parent_frame", snapshot.pending_switch_parent_frame},
      {"switch_in_progress", snapshot.switch_in_progress},
      {"last_presented_parent_frame", snapshot.last_presented_parent_frame},
  };
}

Json playbackTraceEventJson(const std::string &event_name, const Json &details,
                            const PlaybackTraceSnapshot &snapshot) {
  int stimulus_buffered_frames = 0;
  for (const auto &slot : snapshot.stimulus.slots) {
    if (slot.readable && slot.frame_number >= 0) {
      ++stimulus_buffered_frames;
    }
  }
  return {
      {"event", event_name},
      {"details", details},
      {"video_loaded", snapshot.video_loaded},
      {"video_fps", snapshot.video_fps},
      {"current_frame_num", snapshot.current_frame_num},
      {"window_need_decoding_count", snapshot.window_need_decoding_count},
      {"playback",
       playbackJson(snapshot.playback, false, false, true, true, true)},
      {"seek", seekJson(snapshot.seek, false, true)},
      {"presenter",
       {{"view_idx", snapshot.presenter.view_idx},
        {"target_frame", snapshot.presenter.target_frame},
        {"preferred_paused_slot", snapshot.presenter.preferred_paused_slot},
        {"presented_slot", snapshot.presenter.presented_slot},
        {"presented_frame", snapshot.presenter.presented_frame},
        {"resolved_frame", snapshot.presenter.resolved_frame},
        {"prewarm_active", snapshot.presenter.prewarm_active}}},
      {"camera_buffer", snapshot.camera_buffer
                            ? playbackCameraBufferJson(*snapshot.camera_buffer)
                            : Json::object()},
      {"stimulus",
       {{"loaded", snapshot.stimulus.loaded},
        {"window_name", snapshot.stimulus.window_name},
        {"current_stimulus_frame", snapshot.stimulus.current_stimulus_frame},
        {"latest_decoded_frame", snapshot.stimulus.latest_decoded_frame},
        {"last_displayed_frame", snapshot.stimulus.last_displayed_frame},
        {"buffered_frames", stimulus_buffered_frames},
        {"buffer_size", snapshot.stimulus.buffer_size},
        {"seek_use", snapshot.stimulus.seek_use},
        {"seek_done", snapshot.stimulus.seek_done},
        {"seek_id", snapshot.stimulus.seek_id},
        {"seek_frame", snapshot.stimulus.seek_frame},
        {"settled_seek_id", snapshot.stimulus.settled_seek_id}}},
  };
}

Json frameSyncTraceEventJson(const Json &details,
                             const PlaybackTraceSnapshot &snapshot) {
  return {
      {"event", "camera_frame_sync"},
      {"details", details},
      {"video_loaded", snapshot.video_loaded},
      {"video_fps", snapshot.video_fps},
      {"current_frame_num", snapshot.current_frame_num},
      {"playback",
       playbackJson(snapshot.playback, false, false, false, false, false)},
      {"presenter",
       {{"view_idx", snapshot.presenter.view_idx},
        {"target_frame", snapshot.presenter.target_frame},
        {"preferred_paused_slot", snapshot.presenter.preferred_paused_slot},
        {"presented_slot", snapshot.presenter.presented_slot},
        {"presented_frame", snapshot.presenter.presented_frame},
        {"resolved_frame", snapshot.presenter.resolved_frame},
        {"prewarm_active", snapshot.presenter.prewarm_active}}},
  };
}

Json clippedPlaybackStateEventJson(const std::string &playback_event,
                                   const Json &details,
                                   const ClippedPlaybackSnapshot &snapshot) {
  return {
      {"event", "clipped_playback_state"},
      {"playback_event", playback_event},
      {"details", details},
      {"state", clippedPlaybackStateJson(snapshot)},
  };
}

Json clippedHandoffTraceEventJson(
    const std::string &handoff_event, const Json &details, bool video_loaded,
    int current_frame_num, const PlaybackSnapshot &playback,
    const std::optional<ClippedMediaStateSnapshot> &clipped_state) {
  return {
      {"event", "clipped_handoff"},
      {"handoff_event", handoff_event},
      {"details", details},
      {"video_loaded", video_loaded},
      {"current_frame_num", current_frame_num},
      {"playback",
       {{"play_video", playback.play_video},
        {"to_display_frame_number", playback.to_display_frame_number},
        {"read_head", playback.read_head}}},
      {"clipped_state",
       clipped_state ? clippedMediaStateJson(*clipped_state) : Json(nullptr)},
  };
}

bool PlaybackTraceJsonlWriter::open(const std::filesystem::path &output_path,
                                    std::string label) {
  close();
  last_error_.clear();
  if (output_path.empty()) {
    last_error_ = "output path is empty";
    return false;
  }
  label_ = label.empty() ? "PlaybackTrace" : std::move(label);
  path_ = output_path;
  std::error_code error;
  if (path_.has_parent_path()) {
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
      last_error_ = "failed to create parent directory: " + error.message();
      return false;
    }
  }
  stream_.open(path_, std::ios::out | std::ios::trunc);
  if (!stream_.is_open()) {
    last_error_ = "failed to open JSONL output";
    return false;
  }
  started_ = std::chrono::steady_clock::now();
  sequence_ = 0;
  samples_since_flush_ = 0;
  return true;
}

bool PlaybackTraceJsonlWriter::enabled() const { return stream_.is_open(); }

const std::filesystem::path &PlaybackTraceJsonlWriter::path() const {
  return path_;
}

const std::string &PlaybackTraceJsonlWriter::lastError() const {
  return last_error_;
}

void PlaybackTraceJsonlWriter::write(Json sample, bool force_flush) {
  if (!enabled()) {
    return;
  }
  const auto now_steady = std::chrono::steady_clock::now();
  const auto now_system = std::chrono::system_clock::now();
  sample["format"] = "crimson_playback_trace_v1";
  sample["sequence"] = sequence_++;
  sample["elapsed_s"] =
      std::chrono::duration<double>(now_steady - started_).count();
  sample["wall_epoch_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now_system.time_since_epoch())
          .count();
  stream_ << sample.dump() << '\n';
  ++samples_since_flush_;
  if (force_flush || samples_since_flush_ >= 30) {
    stream_.flush();
    samples_since_flush_ = 0;
  }
}

void PlaybackTraceJsonlWriter::close() {
  if (stream_.is_open()) {
    stream_.flush();
    stream_.close();
  }
}

} // namespace crimson::playback::diagnostics
