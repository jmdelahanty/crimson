#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

// This model intentionally contains only values observed from the NVIDIA
// backend.  The OpenGL/CUDA/Zarr adapters live at the call site.
namespace crimson::platform::nvidia::trace {

using Json = nlohmann::json;

struct FrameSyncTraceState {
  bool initialized = false;
  bool has_presented_frame = false;
  int64_t target_frame = -1;
  int64_t presented_frame = -1;
  int64_t bbox_query_frame = -1;
  int64_t latest_decoded_frame = -1;
  int64_t front_frame_before_draw = -1;
  int64_t front_frame_after_draw = -1;
  int64_t staging_frame_before_draw = -1;
  int64_t staging_frame_after_draw = -1;
  int64_t zarr_box_count = -1;
};

// Returns true when the historic frame-sync trace should emit another sample.
bool frameSyncTraceChanged(const FrameSyncTraceState &previous,
                           const FrameSyncTraceState &current);
void updateFrameSyncTraceState(FrameSyncTraceState &previous,
                               const FrameSyncTraceState &current);

struct DeltaStats {
  bool initialized = false;
  int64_t min_delta = 0;
  int64_t max_delta = 0;
  std::unordered_map<int64_t, uint64_t> histogram;

  void add(int64_t delta);
  Json summaryJson() const;
};

struct ClippedFrameTraceStats {
  uint64_t frames_traced = 0;
  uint64_t parent_mismatches = 0;
  uint64_t bbox_parent_mismatches = 0;
  uint64_t bbox_local_mismatches = 0;
  uint64_t decoder_local_mismatches = 0;
  uint64_t front_texture_before_mismatches = 0;
  uint64_t front_texture_after_mismatches = 0;
  uint64_t texture_draw_callback_missing = 0;
  uint64_t texture_draw_bound_mismatches = 0;
  DeltaStats decoder_local_delta;
  DeltaStats bbox_parent_delta;
  DeltaStats bbox_local_delta;

  Json summaryJson() const;
};

// The resolver and backend adapter populate this without exposing their own
// implementation types to the trace model.
struct ClippedFrameComparison {
  int64_t current_parent_frame = -1;
  int64_t bbox_query_parent_frame = -1;
  std::optional<int64_t> resolved_parent_frame;
  std::optional<int64_t> current_clip_local_frame;
  std::optional<int64_t> bbox_clip_local_frame;
  std::optional<int64_t> decoder_presented_local_frame;
  bool front_before_valid = false;
  std::optional<int64_t> front_before_local_frame;
  bool front_after_valid = false;
  std::optional<int64_t> front_after_local_frame;
};

struct ClippedFrameSanity {
  bool parent_matches_resolver = false;
  bool bbox_parent_matches_display = false;
  bool bbox_local_matches_resolver = false;
  std::optional<bool> decoder_local_matches_resolver;
  std::optional<bool> front_texture_matches_resolver_before_draw;
  std::optional<bool> front_texture_matches_resolver_after_draw;
  std::optional<int64_t> decoder_presented_local_minus_clip_local;
  int64_t bbox_query_parent_minus_current_parent = 0;
  std::optional<int64_t> bbox_query_local_minus_clip_local;
};

ClippedFrameSanity evaluateClippedFrame(const ClippedFrameComparison &input);
void recordClippedFrame(ClippedFrameTraceStats &stats,
                        const ClippedFrameSanity &sanity);

Json nullableInt64(int64_t value);

struct SelectedRunSnapshot {
  std::string work_unit_id;
  std::string detect_run;
  std::string refined_detect_run;
  std::string detect_group_path;
  std::string refined_group_path;
  std::string video_path;
};

struct ResolverSnapshot {
  int64_t resolved_parent_frame_index = -1;
  int64_t recording_frame_id = -1;
  std::string clip_id;
  uint64_t clip_index = 0;
  std::string camera_serial;
  int64_t clip_local_frame_index = -1;
  uint64_t selected_run_index = 0;
  std::optional<SelectedRunSnapshot> selected_run;
};

struct BoundingBoxSnapshot {
  int64_t payload_frame_id = -1;
  int64_t payload_camera_id = -1;
  int64_t box_index_in_payload = -1;
  float x = 0.0F;
  float y = 0.0F;
  float w = 0.0F;
  float h = 0.0F;
  int64_t class_id = -1;
  float confidence = 0.0F;
};

struct TextureStateSnapshot {
  bool front_valid = false;
  int64_t front_frame = -1;
  bool staging_valid = false;
  int64_t staging_frame = -1;
};

struct DetectionSourceSnapshot {
  std::optional<int64_t> source_code;
  std::optional<std::string> source_kind;
  bool manual = false;
};

struct TextureDrawSnapshot {
  bool enabled = false;
  uint64_t draw_sequence = 0;
  int64_t view_idx = -1;
  uint64_t queued_texture_id = 0;
  uint64_t front_texture_id = 0;
  uint64_t staging_texture_id = 0;
  uint64_t front_pbo_id = 0;
  uint64_t staging_pbo_id = 0;
  bool front_valid = false;
  int64_t front_parent_frame = -1;
  int64_t front_local_frame = -1;
  int64_t front_pts = -1;
  bool staging_valid = false;
  int64_t staging_parent_frame = -1;
  int64_t staging_local_frame = -1;
  int64_t staging_pts = -1;
  bool callback_observed = false;
  uint64_t callback_count = 0;
  uint64_t callback_active_texture = 0;
  uint64_t callback_bound_texture_id = 0;
  bool callback_bound_matches_queued = false;
};

void recordTextureDrawOutcome(ClippedFrameTraceStats &stats,
                              const TextureDrawSnapshot &snapshot);

Json selectedRunJson(const SelectedRunSnapshot &snapshot);
Json resolverJson(const std::optional<ResolverSnapshot> &snapshot);
Json boundingBoxJson(const std::optional<BoundingBoxSnapshot> &snapshot);
Json boundingBoxSummaryJson(int64_t count,
                            const std::optional<BoundingBoxSnapshot> &first);
Json textureStateJson(const TextureStateSnapshot &snapshot);
Json detectionSourceJson(
    const std::optional<DetectionSourceSnapshot> &snapshot);
Json textureDrawJson(const TextureDrawSnapshot &snapshot);

// The standalone post-draw event deliberately differs from textureDrawJson:
// its callback fields are nested below `callback` for historical consumers.
Json clippedTextureDrawEventJson(const TextureDrawSnapshot &snapshot);

struct ClippedTextureDumpResolverSnapshot {
  int64_t resolved_parent_frame_index = -1;
  int64_t recording_frame_id = -1;
  std::string clip_id;
  int64_t clip_local_frame_index = -1;
  std::string camera_serial;
  uint64_t selected_run_index = 0;
};

struct ClippedTextureDumpSnapshot {
  int64_t requested_parent_frame = -1;
  bool ok = false;
  std::optional<std::string> error;
  std::string raw_path;
  std::string flip_y_path;
  std::string metadata_path;
  int64_t width = 0;
  int64_t height = 0;
  TextureDrawSnapshot texture_draw;
  std::optional<ClippedTextureDumpResolverSnapshot> resolver;
  // This is absent on the initial on-disk metadata payload and only added
  // when the call-site metadata write itself fails.
  std::optional<std::string> metadata_write_error;
};

Json clippedTextureDumpEventJson(const ClippedTextureDumpSnapshot &snapshot);

// Presentation-source naming is part of the on-disk trace schema.
const char *decoderFrameSourceLabel(int source_code);
const char *presentationSourceLabel(bool surface_swapped_before_draw,
                                    uint64_t upload_count);

struct ClippedFrameTraceSnapshot {
  int64_t current_parent_frame_index = -1;
  int64_t requested_parent_frame_index = -1;
  bool playback_running = false;
  double playback_speed = 0.0;
  Json playback_state = nullptr;
  std::optional<ResolverSnapshot> resolver;
  std::optional<ResolverSnapshot> requested_resolver;
  std::string active_video_path;
  std::string active_clip_id;
  std::optional<int64_t> requested_decoder_local_frame;
  std::optional<int64_t> decoder_presented_local_frame;
  std::optional<int64_t> front_texture_local_frame_before_draw;
  std::optional<int64_t> front_texture_local_frame_after_draw;
  std::optional<int64_t> front_texture_parent_frame_before_draw;
  std::optional<int64_t> front_texture_parent_frame_after_draw;
  std::optional<int64_t> presented_pts;
  std::optional<int64_t> front_texture_pts_before_draw;
  std::optional<int64_t> front_texture_pts_after_draw;
  double timebase = 0.0;
  int decoder_frame_source_code = 0;
  bool surface_swapped_before_draw = false;
  uint64_t upload_count = 0;
  int64_t presented_slot = -1;
  int64_t latest_decoded_parent_frame = -1;
  TextureDrawSnapshot texture_draw;
  int64_t bbox_query_parent_frame_index = -1;
  std::optional<int64_t> bbox_query_clip_local_frame_index;
  int64_t bbox_row_count = 0;
  std::optional<BoundingBoxSnapshot> first_bbox_source_image;
  std::optional<BoundingBoxSnapshot> first_bbox_display;
  std::optional<DetectionSourceSnapshot> first_bbox_source;
  int64_t detection_details_frame_id = -1;
  ClippedFrameSanity sanity;
};

// Produces the historic event payload; callers add the JSONL envelope.
Json clippedFrameJson(const ClippedFrameTraceSnapshot &snapshot);

} // namespace crimson::platform::nvidia::trace
