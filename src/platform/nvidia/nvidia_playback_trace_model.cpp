#include "platform/nvidia/nvidia_playback_trace_model.h"

#include <algorithm>

namespace crimson::platform::nvidia::trace {

bool frameSyncTraceChanged(const FrameSyncTraceState &previous,
                           const FrameSyncTraceState &current) {
  return !previous.initialized ||
         previous.has_presented_frame != current.has_presented_frame ||
         previous.target_frame != current.target_frame ||
         previous.presented_frame != current.presented_frame ||
         previous.bbox_query_frame != current.bbox_query_frame ||
         previous.latest_decoded_frame != current.latest_decoded_frame ||
         previous.front_frame_before_draw != current.front_frame_before_draw ||
         previous.front_frame_after_draw != current.front_frame_after_draw ||
         previous.staging_frame_before_draw !=
             current.staging_frame_before_draw ||
         previous.staging_frame_after_draw !=
             current.staging_frame_after_draw ||
         previous.zarr_box_count != current.zarr_box_count;
}

void updateFrameSyncTraceState(FrameSyncTraceState &previous,
                               const FrameSyncTraceState &current) {
  previous = current;
  previous.initialized = true;
}

void DeltaStats::add(int64_t delta) {
  if (!initialized) {
    min_delta = delta;
    max_delta = delta;
    initialized = true;
  } else {
    min_delta = std::min(min_delta, delta);
    max_delta = std::max(max_delta, delta);
  }
  ++histogram[delta];
}

Json DeltaStats::summaryJson() const {
  if (!initialized) {
    return nullptr;
  }
  int64_t common_delta = 0;
  uint64_t common_count = 0;
  for (const auto &[delta, count] : histogram) {
    if (count > common_count ||
        (count == common_count && delta < common_delta)) {
      common_delta = delta;
      common_count = count;
    }
  }
  return {{"min", min_delta},
          {"max", max_delta},
          {"most_common", common_delta},
          {"most_common_count", common_count}};
}

Json ClippedFrameTraceStats::summaryJson() const {
  return {
      {"frames_traced", frames_traced},
      {"mismatches",
       {{"parent_matches_resolver", parent_mismatches},
        {"bbox_parent_matches_display", bbox_parent_mismatches},
        {"bbox_local_matches_resolver", bbox_local_mismatches},
        {"decoder_local_matches_resolver", decoder_local_mismatches},
        {"front_texture_matches_resolver_before_draw",
         front_texture_before_mismatches},
        {"front_texture_matches_resolver_after_draw",
         front_texture_after_mismatches},
        {"texture_draw_callback_missing", texture_draw_callback_missing},
        {"texture_draw_bound_mismatches", texture_draw_bound_mismatches}}},
      {"deltas",
       {{"decoder_presented_local_minus_clip_local",
         decoder_local_delta.summaryJson()},
        {"bbox_query_parent_minus_current_parent",
         bbox_parent_delta.summaryJson()},
        {"bbox_query_local_minus_clip_local", bbox_local_delta.summaryJson()}}},
  };
}

ClippedFrameSanity evaluateClippedFrame(const ClippedFrameComparison &input) {
  ClippedFrameSanity result;
  result.parent_matches_resolver =
      input.resolved_parent_frame &&
      input.current_parent_frame == *input.resolved_parent_frame;
  result.bbox_parent_matches_display =
      input.bbox_query_parent_frame == input.current_parent_frame;
  result.bbox_local_matches_resolver =
      input.current_clip_local_frame && input.bbox_clip_local_frame &&
      *input.current_clip_local_frame == *input.bbox_clip_local_frame;
  result.bbox_query_parent_minus_current_parent =
      input.bbox_query_parent_frame - input.current_parent_frame;

  if (input.current_clip_local_frame && input.decoder_presented_local_frame &&
      *input.decoder_presented_local_frame >= 0) {
    const int64_t delta =
        *input.decoder_presented_local_frame - *input.current_clip_local_frame;
    result.decoder_presented_local_minus_clip_local = delta;
    result.decoder_local_matches_resolver = delta == 0;
  }
  if (input.current_clip_local_frame && input.bbox_clip_local_frame) {
    result.bbox_query_local_minus_clip_local =
        *input.bbox_clip_local_frame - *input.current_clip_local_frame;
  }
  if (input.current_clip_local_frame && input.front_before_valid &&
      input.front_before_local_frame && *input.front_before_local_frame >= 0) {
    result.front_texture_matches_resolver_before_draw =
        *input.front_before_local_frame == *input.current_clip_local_frame;
  }
  if (input.current_clip_local_frame && input.front_after_valid &&
      input.front_after_local_frame && *input.front_after_local_frame >= 0) {
    result.front_texture_matches_resolver_after_draw =
        *input.front_after_local_frame == *input.current_clip_local_frame;
  }
  return result;
}

void recordClippedFrame(ClippedFrameTraceStats &stats,
                        const ClippedFrameSanity &sanity) {
  ++stats.frames_traced;
  if (!sanity.parent_matches_resolver) {
    ++stats.parent_mismatches;
  }
  if (!sanity.bbox_parent_matches_display) {
    ++stats.bbox_parent_mismatches;
  }
  if (!sanity.bbox_local_matches_resolver) {
    ++stats.bbox_local_mismatches;
  }
  if (sanity.decoder_presented_local_minus_clip_local) {
    stats.decoder_local_delta.add(
        *sanity.decoder_presented_local_minus_clip_local);
  }
  if (sanity.decoder_local_matches_resolver &&
      !*sanity.decoder_local_matches_resolver) {
    ++stats.decoder_local_mismatches;
  }
  stats.bbox_parent_delta.add(sanity.bbox_query_parent_minus_current_parent);
  if (sanity.bbox_query_local_minus_clip_local) {
    stats.bbox_local_delta.add(*sanity.bbox_query_local_minus_clip_local);
  }
  if (sanity.front_texture_matches_resolver_before_draw &&
      !*sanity.front_texture_matches_resolver_before_draw) {
    ++stats.front_texture_before_mismatches;
  }
  if (sanity.front_texture_matches_resolver_after_draw &&
      !*sanity.front_texture_matches_resolver_after_draw) {
    ++stats.front_texture_after_mismatches;
  }
}

void recordTextureDrawOutcome(ClippedFrameTraceStats &stats,
                              const TextureDrawSnapshot &snapshot) {
  if (!snapshot.enabled || snapshot.draw_sequence == 0) {
    return;
  }
  if (!snapshot.callback_observed) {
    ++stats.texture_draw_callback_missing;
  } else if (!snapshot.callback_bound_matches_queued) {
    ++stats.texture_draw_bound_mismatches;
  }
}

Json nullableInt64(int64_t value) {
  return value >= 0 ? Json(value) : Json(nullptr);
}

Json selectedRunJson(const SelectedRunSnapshot &snapshot) {
  return {{"work_unit_id", snapshot.work_unit_id},
          {"detect_run", snapshot.detect_run},
          {"refined_detect_run", snapshot.refined_detect_run},
          {"detect_group_path", snapshot.detect_group_path},
          {"refined_group_path", snapshot.refined_group_path},
          {"video_path", snapshot.video_path}};
}

Json resolverJson(const std::optional<ResolverSnapshot> &snapshot) {
  if (!snapshot) {
    return nullptr;
  }
  return {
      {"resolved_parent_frame_index", snapshot->resolved_parent_frame_index},
      {"recording_frame_id", snapshot->recording_frame_id},
      {"clip_id", snapshot->clip_id},
      {"clip_index", snapshot->clip_index},
      {"camera_serial", snapshot->camera_serial},
      {"clip_local_frame_index", snapshot->clip_local_frame_index},
      {"selected_run_index", snapshot->selected_run_index},
      {"selected_run", snapshot->selected_run
                           ? selectedRunJson(*snapshot->selected_run)
                           : Json(nullptr)}};
}

Json boundingBoxJson(const std::optional<BoundingBoxSnapshot> &snapshot) {
  if (!snapshot) {
    return nullptr;
  }
  return {{"payload_frame_id", snapshot->payload_frame_id},
          {"payload_camera_id", snapshot->payload_camera_id},
          {"box_index_in_payload", snapshot->box_index_in_payload},
          {"x", snapshot->x},
          {"y", snapshot->y},
          {"w", snapshot->w},
          {"h", snapshot->h},
          {"cx", snapshot->x + snapshot->w * 0.5F},
          {"cy", snapshot->y + snapshot->h * 0.5F},
          {"class_id", snapshot->class_id},
          {"confidence", snapshot->confidence}};
}

Json boundingBoxSummaryJson(int64_t count,
                            const std::optional<BoundingBoxSnapshot> &first) {
  Json result = {{"count", count}};
  if (first) {
    result["first"] = boundingBoxJson(first);
  }
  return result;
}

Json textureStateJson(const TextureStateSnapshot &snapshot) {
  return {{"front_valid", snapshot.front_valid},
          {"front_frame", snapshot.front_frame},
          {"staging_valid", snapshot.staging_valid},
          {"staging_frame", snapshot.staging_frame}};
}

Json detectionSourceJson(
    const std::optional<DetectionSourceSnapshot> &snapshot) {
  if (!snapshot) {
    return nullptr;
  }
  return {{"source_code", snapshot->source_code ? Json(*snapshot->source_code)
                                                : Json(nullptr)},
          {"source_kind", snapshot->source_kind ? Json(*snapshot->source_kind)
                                                : Json(nullptr)},
          {"manual", snapshot->manual}};
}

Json textureDrawJson(const TextureDrawSnapshot &snapshot) {
  if (!snapshot.enabled || snapshot.draw_sequence == 0) {
    return nullptr;
  }
  return {
      {"draw_sequence", snapshot.draw_sequence},
      {"view_idx", snapshot.view_idx},
      {"queued_texture_id", snapshot.queued_texture_id},
      {"front_texture_id", snapshot.front_texture_id},
      {"staging_texture_id", snapshot.staging_texture_id},
      {"front_pbo_id", snapshot.front_pbo_id},
      {"staging_pbo_id", snapshot.staging_pbo_id},
      {"queued_texture_matches_front",
       snapshot.queued_texture_id == snapshot.front_texture_id},
      {"queued_texture_matches_staging",
       snapshot.queued_texture_id == snapshot.staging_texture_id},
      {"front",
       {{"valid", snapshot.front_valid},
        {"parent_frame", nullableInt64(snapshot.front_parent_frame)},
        {"local_frame", nullableInt64(snapshot.front_local_frame)},
        {"pts", nullableInt64(snapshot.front_pts)}}},
      {"staging",
       {{"valid", snapshot.staging_valid},
        {"parent_frame", nullableInt64(snapshot.staging_parent_frame)},
        {"local_frame", nullableInt64(snapshot.staging_local_frame)},
        {"pts", nullableInt64(snapshot.staging_pts)}}},
      {"callback_observed", snapshot.callback_observed},
      {"callback_count", snapshot.callback_count},
      {"callback_active_texture", snapshot.callback_observed
                                      ? Json(snapshot.callback_active_texture)
                                      : Json(nullptr)},
      {"callback_bound_texture_id",
       snapshot.callback_observed ? Json(snapshot.callback_bound_texture_id)
                                  : Json(nullptr)},
      {"callback_bound_matches_queued",
       snapshot.callback_observed ? Json(snapshot.callback_bound_matches_queued)
                                  : Json(nullptr)},
  };
}

Json clippedTextureDrawEventJson(const TextureDrawSnapshot &snapshot) {
  if (!snapshot.enabled || snapshot.draw_sequence == 0) {
    return nullptr;
  }
  return {
      {"event", "clipped_texture_draw"},
      {"draw_sequence", snapshot.draw_sequence},
      {"view_idx", snapshot.view_idx},
      {"queued_texture_id", snapshot.queued_texture_id},
      {"front_texture_id", snapshot.front_texture_id},
      {"staging_texture_id", snapshot.staging_texture_id},
      {"front_pbo_id", snapshot.front_pbo_id},
      {"staging_pbo_id", snapshot.staging_pbo_id},
      {"queued_texture_matches_front",
       snapshot.queued_texture_id == snapshot.front_texture_id},
      {"queued_texture_matches_staging",
       snapshot.queued_texture_id == snapshot.staging_texture_id},
      {"front",
       {{"valid", snapshot.front_valid},
        {"parent_frame", nullableInt64(snapshot.front_parent_frame)},
        {"local_frame", nullableInt64(snapshot.front_local_frame)},
        {"pts", nullableInt64(snapshot.front_pts)}}},
      {"staging",
       {{"valid", snapshot.staging_valid},
        {"parent_frame", nullableInt64(snapshot.staging_parent_frame)},
        {"local_frame", nullableInt64(snapshot.staging_local_frame)},
        {"pts", nullableInt64(snapshot.staging_pts)}}},
      {"callback",
       {{"observed", snapshot.callback_observed},
        {"count", snapshot.callback_count},
        {"active_texture", snapshot.callback_observed
                               ? Json(snapshot.callback_active_texture)
                               : Json(nullptr)},
        {"bound_texture_id", snapshot.callback_observed
                                 ? Json(snapshot.callback_bound_texture_id)
                                 : Json(nullptr)},
        {"bound_matches_queued",
         snapshot.callback_observed
             ? Json(snapshot.callback_bound_matches_queued)
             : Json(nullptr)}}},
  };
}

Json clippedTextureDumpEventJson(const ClippedTextureDumpSnapshot &snapshot) {
  const auto &trace = snapshot.texture_draw;
  Json resolver = nullptr;
  if (snapshot.resolver) {
    resolver = {
        {"resolved_parent_frame_index",
         snapshot.resolver->resolved_parent_frame_index},
        {"recording_frame_id", snapshot.resolver->recording_frame_id},
        {"clip_id", snapshot.resolver->clip_id},
        {"clip_local_frame_index", snapshot.resolver->clip_local_frame_index},
        {"camera_serial", snapshot.resolver->camera_serial},
        {"selected_run_index", snapshot.resolver->selected_run_index}};
  }
  Json result = {
      {"event", "clipped_texture_dump"},
      {"requested_parent_frame", snapshot.requested_parent_frame},
      {"ok", snapshot.ok},
      {"error", snapshot.error ? Json(*snapshot.error) : Json(nullptr)},
      {"raw_path", snapshot.raw_path},
      {"flip_y_path", snapshot.flip_y_path},
      {"metadata_path", snapshot.metadata_path},
      {"width", snapshot.width},
      {"height", snapshot.height},
      {"draw_sequence", trace.draw_sequence},
      {"view_idx", trace.view_idx},
      {"bound_texture_id", trace.callback_bound_texture_id},
      {"queued_texture_id", trace.queued_texture_id},
      {"front_texture_id", trace.front_texture_id},
      {"staging_texture_id", trace.staging_texture_id},
      {"bound_matches_queued", trace.callback_bound_matches_queued},
      {"front",
       {{"valid", trace.front_valid},
        {"parent_frame", nullableInt64(trace.front_parent_frame)},
        {"local_frame", nullableInt64(trace.front_local_frame)},
        {"pts", nullableInt64(trace.front_pts)}}},
      {"staging",
       {{"valid", trace.staging_valid},
        {"parent_frame", nullableInt64(trace.staging_parent_frame)},
        {"local_frame", nullableInt64(trace.staging_local_frame)},
        {"pts", nullableInt64(trace.staging_pts)}}},
      {"resolver", resolver},
  };
  if (snapshot.metadata_write_error) {
    result["metadata_write_error"] = *snapshot.metadata_write_error;
  }
  return result;
}

const char *decoderFrameSourceLabel(int source_code) {
  switch (source_code) {
  case 1:
    return "seek";
  case 2:
    return "sequential_decode";
  default:
    return "unknown";
  }
}

const char *presentationSourceLabel(bool surface_swapped_before_draw,
                                    uint64_t upload_count) {
  if (surface_swapped_before_draw) {
    return "prefetched_staged_texture";
  }
  return upload_count > 0 ? "uploaded_this_frame" : "cached_front_texture";
}

Json clippedFrameJson(const ClippedFrameTraceSnapshot &snapshot) {
  return {
      {"event", "clipped_frame"},
      {"ui_parent_timeline",
       {{"current_parent_frame_index", snapshot.current_parent_frame_index},
        {"requested_parent_frame_index", snapshot.requested_parent_frame_index},
        {"playback_running", snapshot.playback_running},
        {"playback_speed", snapshot.playback_speed}}},
      {"playback_state", snapshot.playback_state},
      {"resolver", resolverJson(snapshot.resolver)},
      {"requested_resolver", resolverJson(snapshot.requested_resolver)},
      {"video",
       {{"active_video_path", snapshot.active_video_path},
        {"active_clip_id", snapshot.active_clip_id},
        {"requested_decoder_local_frame",
         snapshot.requested_decoder_local_frame
             ? Json(*snapshot.requested_decoder_local_frame)
             : Json(nullptr)},
        {"decoder_presented_local_frame",
         snapshot.decoder_presented_local_frame
             ? Json(*snapshot.decoder_presented_local_frame)
             : Json(nullptr)},
        {"front_texture_local_frame_before_draw",
         snapshot.front_texture_local_frame_before_draw
             ? Json(*snapshot.front_texture_local_frame_before_draw)
             : Json(nullptr)},
        {"front_texture_local_frame_after_draw",
         snapshot.front_texture_local_frame_after_draw
             ? Json(*snapshot.front_texture_local_frame_after_draw)
             : Json(nullptr)},
        {"front_texture_parent_frame_before_draw",
         snapshot.front_texture_parent_frame_before_draw
             ? Json(*snapshot.front_texture_parent_frame_before_draw)
             : Json(nullptr)},
        {"front_texture_parent_frame_after_draw",
         snapshot.front_texture_parent_frame_after_draw
             ? Json(*snapshot.front_texture_parent_frame_after_draw)
             : Json(nullptr)},
        {"presented_pts", snapshot.presented_pts ? Json(*snapshot.presented_pts)
                                                 : Json(nullptr)},
        {"front_texture_pts_before_draw",
         snapshot.front_texture_pts_before_draw
             ? Json(*snapshot.front_texture_pts_before_draw)
             : Json(nullptr)},
        {"front_texture_pts_after_draw",
         snapshot.front_texture_pts_after_draw
             ? Json(*snapshot.front_texture_pts_after_draw)
             : Json(nullptr)},
        {"timebase", snapshot.timebase},
        {"decoder_frame_source",
         decoderFrameSourceLabel(snapshot.decoder_frame_source_code)},
        {"presentation_source",
         presentationSourceLabel(snapshot.surface_swapped_before_draw,
                                 snapshot.upload_count)},
        {"presented_slot", snapshot.presented_slot},
        {"latest_decoded_parent_frame", snapshot.latest_decoded_parent_frame},
        {"draw_texture", textureDrawJson(snapshot.texture_draw)}}},
      {"bbox",
       {{"bbox_query_parent_frame_index",
         snapshot.bbox_query_parent_frame_index},
        {"bbox_query_clip_local_frame_index",
         snapshot.bbox_query_clip_local_frame_index
             ? Json(*snapshot.bbox_query_clip_local_frame_index)
             : Json(nullptr)},
        {"bbox_payload_frame_index",
         snapshot.bbox_query_clip_local_frame_index
             ? Json(*snapshot.bbox_query_clip_local_frame_index)
             : Json(nullptr)},
        {"bbox_row_count", snapshot.bbox_row_count},
        {"first_bbox_source_image",
         boundingBoxJson(snapshot.first_bbox_source_image)},
        {"first_bbox_display", boundingBoxJson(snapshot.first_bbox_display)},
        {"first_bbox_source", detectionSourceJson(snapshot.first_bbox_source)},
        {"detection_details_frame_id", snapshot.detection_details_frame_id}}},
      {"sanity",
       {{"parent_matches_resolver", snapshot.sanity.parent_matches_resolver},
        {"bbox_parent_matches_display",
         snapshot.sanity.bbox_parent_matches_display},
        {"bbox_local_matches_resolver",
         snapshot.sanity.bbox_local_matches_resolver},
        {"decoder_local_matches_resolver",
         snapshot.sanity.decoder_local_matches_resolver
             ? Json(*snapshot.sanity.decoder_local_matches_resolver)
             : Json(nullptr)},
        {"front_texture_matches_resolver_before_draw",
         snapshot.sanity.front_texture_matches_resolver_before_draw
             ? Json(*snapshot.sanity.front_texture_matches_resolver_before_draw)
             : Json(nullptr)},
        {"front_texture_matches_resolver_after_draw",
         snapshot.sanity.front_texture_matches_resolver_after_draw
             ? Json(*snapshot.sanity.front_texture_matches_resolver_after_draw)
             : Json(nullptr)}}},
      {"deltas",
       {{"decoder_presented_local_minus_clip_local",
         snapshot.sanity.decoder_presented_local_minus_clip_local
             ? Json(*snapshot.sanity.decoder_presented_local_minus_clip_local)
             : Json(nullptr)},
        {"bbox_query_parent_minus_current_parent",
         snapshot.sanity.bbox_query_parent_minus_current_parent},
        {"bbox_query_local_minus_clip_local",
         snapshot.sanity.bbox_query_local_minus_clip_local
             ? Json(*snapshot.sanity.bbox_query_local_minus_clip_local)
             : Json(nullptr)}}},
  };
}

} // namespace crimson::platform::nvidia::trace
