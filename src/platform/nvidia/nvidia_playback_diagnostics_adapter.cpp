#include "platform/nvidia/nvidia_playback_diagnostics_adapter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace crimson::platform::nvidia::diagnostics {

trace::BoundingBoxSnapshot toTraceBoundingBox(const LoggedBoundingBox &box) {
  return {static_cast<int64_t>(box.payload_frame_id),
          static_cast<int64_t>(box.payload_camera_id),
          static_cast<int64_t>(box.box_index_in_payload),
          box.x_min,
          box.y_min,
          box.width,
          box.height,
          static_cast<int64_t>(box.class_id),
          box.confidence};
}

std::optional<trace::BoundingBoxSnapshot>
firstTraceBoundingBox(const std::vector<LoggedBoundingBox> &boxes) {
  if (boxes.empty()) {
    return std::nullopt;
  }
  return toTraceBoundingBox(boxes.front());
}

trace::TextureDrawSnapshot
toTraceTextureDraw(const CameraTextureDrawTrace &trace) {
  return {trace.enabled,
          trace.queue_sequence,
          trace.view_idx,
          static_cast<uint64_t>(trace.queued_texture_id),
          static_cast<uint64_t>(trace.front_texture_id),
          static_cast<uint64_t>(trace.staging_texture_id),
          static_cast<uint64_t>(trace.front_pbo_id),
          static_cast<uint64_t>(trace.staging_pbo_id),
          trace.front_valid,
          trace.front_parent_frame,
          trace.front_local_frame,
          trace.front_pts,
          trace.staging_valid,
          trace.staging_parent_frame,
          trace.staging_local_frame,
          trace.staging_pts,
          trace.callback_observed,
          static_cast<uint64_t>(trace.callback_count),
          static_cast<uint64_t>(trace.callback_active_texture),
          static_cast<uint64_t>(trace.callback_bound_texture_id),
          trace.callback_bound_matches_queued};
}

std::optional<trace::SelectedRunSnapshot>
toTraceSelectedRun(const std::optional<SelectedRunSourceSnapshot> &selected) {
  if (!selected) {
    return std::nullopt;
  }
  return trace::SelectedRunSnapshot{
      selected->work_unit_id,       selected->detect_run,
      selected->refined_detect_run, selected->detect_group_path,
      selected->refined_group_path, selected->video_path};
}

std::optional<trace::ResolverSnapshot>
toTraceResolver(const std::optional<ResolverSourceSnapshot> &source) {
  if (!source) {
    return std::nullopt;
  }
  return trace::ResolverSnapshot{source->resolved_parent_frame_index,
                                 source->recording_frame_id,
                                 source->clip_id,
                                 source->run_index,
                                 source->camera_serial,
                                 source->clip_local_frame_index,
                                 source->selected_run_index,
                                 toTraceSelectedRun(source->selected_run)};
}

std::optional<trace::DetectionSourceSnapshot>
toTraceDetectionSource(const std::vector<uint8_t> &source_codes,
                       const std::vector<std::string> &source_kinds) {
  if (source_codes.empty() && source_kinds.empty()) {
    return std::nullopt;
  }
  const std::optional<int64_t> source_code =
      source_codes.empty() ? std::nullopt
                           : std::optional<int64_t>(source_codes.front());
  const std::optional<std::string> source_kind =
      source_kinds.empty() || source_kinds.front().empty()
          ? std::nullopt
          : std::optional<std::string>(source_kinds.front());
  std::string reason_lower = source_kind.value_or("");
  std::transform(
      reason_lower.begin(), reason_lower.end(), reason_lower.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return trace::DetectionSourceSnapshot{source_code, source_kind,
                                        reason_lower.find("manual") !=
                                            std::string::npos};
}

std::optional<playback::diagnostics::CameraBufferSnapshot>
makeCameraBufferSnapshot(const CameraBufferSnapshotRequest &request) {
  if ((request.require_video_loaded && !request.video_loaded) ||
      request.scene == nullptr || request.visible_idx < 0 ||
      request.visible_idx >= static_cast<int>(request.scene->num_cams) ||
      request.scene->size_of_buffer == 0) {
    return std::nullopt;
  }

  const auto &camera = request.scene->cameras[request.visible_idx];
  playback::diagnostics::CameraBufferSnapshot snapshot;
  snapshot.visible_idx = request.visible_idx;
  if (request.camera_names != nullptr &&
      request.visible_idx < static_cast<int>(request.camera_names->size())) {
    snapshot.camera_name = (*request.camera_names)[request.visible_idx];
  }
  snapshot.buffer_size = static_cast<int>(request.scene->size_of_buffer);
  snapshot.read_head = request.read_head;
  snapshot.target_frame = request.target_frame;
  snapshot.selected_frame = request.selected_frame;
  snapshot.last_uploaded_frame = camera.last_uploaded_frame;
  snapshot.last_uploaded_local_frame = camera.last_uploaded_local_frame;
  snapshot.texture_has_valid_frame = camera.texture_has_valid_frame;
  snapshot.staging_valid = camera.playback_staging_valid;
  snapshot.staging_frame = camera.playback_staging_frame;
  snapshot.staging_local_frame = camera.playback_staging_local_frame;
  if (request.latest_decoded_frame != nullptr) {
    const auto latest = request.latest_decoded_frame->find(
        snapshot.camera_name.value_or(std::string{}));
    snapshot.latest_decoded_frame =
        latest != request.latest_decoded_frame->end() ? latest->second.load()
                                                      : -1;
  }
  snapshot.slots.reserve(request.scene->size_of_buffer);
  for (u32 slot_idx = 0; slot_idx < request.scene->size_of_buffer; ++slot_idx) {
    const auto metadata =
        frameSlotSnapshotReadable(camera.display_buffer[slot_idx]);
    snapshot.slots.push_back({metadata.has_value(), metadata.has_value()
                                                        ? metadata->frame_number
                                                        : -1});
  }
  return snapshot;
}

playback::diagnostics::ClippedPlaybackSnapshot makeClippedPlaybackSnapshot(
    int current_frame_num, double video_fps,
    const playback::diagnostics::PlaybackSnapshot &playback,
    const playback::diagnostics::SeekProgressSnapshot &seek,
    std::optional<playback::diagnostics::CameraBufferSnapshot> camera_buffer) {
  playback::diagnostics::ClippedPlaybackSnapshot snapshot;
  snapshot.current_frame_num = current_frame_num;
  snapshot.video_fps = video_fps;
  snapshot.playback = playback;
  snapshot.seek = seek;
  snapshot.camera_buffer = std::move(camera_buffer);
  return snapshot;
}

trace::Json makeClippedFramePlaybackStateJson(
    const ClippedFramePlaybackStateRequest &request) {
  trace::Json buffer = nullptr;
  if (request.camera_buffer) {
    const auto &camera = *request.camera_buffer;
    const int normalized_read_head =
        camera.read_head >= 0 && camera.buffer_size > 0
            ? camera.read_head % camera.buffer_size
            : -1;
    int valid_slots = 0;
    int oldest_frame = std::numeric_limits<int>::max();
    int newest_frame = -1;
    int read_head_frame = -1;
    int target_slot = -1;
    int presented_slot_by_frame = -1;
    int front_slot = -1;
    int contiguous_span_start = -1;
    int contiguous_span_end = -1;
    std::vector<int> valid_frames;
    valid_frames.reserve(camera.slots.size());
    for (size_t slot_index = 0; slot_index < camera.slots.size();
         ++slot_index) {
      const auto &slot = camera.slots[slot_index];
      if (!slot.readable || slot.frame_number < 0) {
        continue;
      }
      ++valid_slots;
      valid_frames.push_back(slot.frame_number);
      oldest_frame = std::min(oldest_frame, slot.frame_number);
      newest_frame = std::max(newest_frame, slot.frame_number);
      if (static_cast<int>(slot_index) == normalized_read_head) {
        read_head_frame = slot.frame_number;
      }
      if (slot.frame_number == camera.target_frame) {
        target_slot = static_cast<int>(slot_index);
      }
      if (slot.frame_number == camera.selected_frame) {
        presented_slot_by_frame = static_cast<int>(slot_index);
      }
      if (slot.frame_number == camera.last_uploaded_frame) {
        front_slot = static_cast<int>(slot_index);
      }
    }
    if (!valid_frames.empty()) {
      std::sort(valid_frames.begin(), valid_frames.end());
      contiguous_span_end = valid_frames.back();
      contiguous_span_start = contiguous_span_end;
      for (int index = static_cast<int>(valid_frames.size()) - 2; index >= 0;
           --index) {
        if (valid_frames[index] + 1 != contiguous_span_start) {
          break;
        }
        contiguous_span_start = valid_frames[index];
      }
    }

    std::string current_frame_source = "presenter_presented_frame";
    if (!request.has_presented_camera_frame) {
      current_frame_source = request.playback.play_video && read_head_frame >= 0
                                 ? "read_head_slot"
                                 : "to_display_frame_number";
    } else if (request.presented_slot >= 0 &&
               request.presented_slot == normalized_read_head) {
      current_frame_source = "presenter_read_head_slot";
    } else if (presented_slot_by_frame >= 0 &&
               presented_slot_by_frame != normalized_read_head) {
      current_frame_source = "presenter_exact_frame_search";
    }

    buffer = {
        {"current_frame_source", current_frame_source},
        {"read_head", camera.read_head},
        {"normalized_read_head", normalized_read_head},
        {"read_head_frame", trace::nullableInt64(read_head_frame)},
        {"presented_slot", request.presented_slot},
        {"presented_slot_by_frame", presented_slot_by_frame},
        {"target_slot", target_slot},
        {"front_slot", front_slot},
        {"valid_slots", valid_slots},
        {"buffer_size", camera.buffer_size},
        {"oldest_frame",
         valid_slots > 0 ? trace::Json(oldest_frame) : trace::Json(nullptr)},
        {"newest_frame",
         valid_slots > 0 ? trace::Json(newest_frame) : trace::Json(nullptr)},
        {"newest_contiguous_span_start",
         trace::nullableInt64(contiguous_span_start)},
        {"newest_contiguous_span_end",
         trace::nullableInt64(contiguous_span_end)},
        {"front_parent_frame",
         trace::nullableInt64(camera.last_uploaded_frame)},
        {"front_local_frame",
         trace::nullableInt64(camera.last_uploaded_local_frame)},
        {"staging_valid", camera.staging_valid},
        {"staging_parent_frame", trace::nullableInt64(camera.staging_frame)},
        {"staging_local_frame",
         trace::nullableInt64(camera.staging_local_frame)},
    };
  }

  const auto &playback = request.playback;
  return {
      {"play_video", playback.play_video},
      {"to_display_frame_number", playback.to_display_frame_number},
      {"slider_frame_number", playback.slider_frame_number},
      {"pause_selected", playback.pause_selected},
      {"pause_seeked", playback.pause_seeked},
      {"just_seeked", playback.just_seeked},
      {"slider_just_changed", playback.slider_just_changed},
      {"buffer_browsed_since_pause", playback.buffer_browsed_since_pause},
      {"paused_frame_on_toggle",
       trace::nullableInt64(playback.paused_frame_on_toggle)},
      {"last_resume_path", playback.last_resume_path},
      {"last_resume_target_frame",
       trace::nullableInt64(playback.last_resume_target_frame)},
      {"accumulated_play_time", playback.accumulated_play_time},
      {"clock_frame", static_cast<int>(std::ceil(
                          playback.accumulated_play_time * request.video_fps))},
      {"clipped_rebase_before_play", request.clipped_rebase_before_play},
      {"buffer", std::move(buffer)},
  };
}

} // namespace crimson::platform::nvidia::diagnostics
