#include "platform/nvidia/nvidia_playback_trace_model.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using crimson::platform::nvidia::trace::BoundingBoxSnapshot;
using crimson::platform::nvidia::trace::ClippedFrameComparison;
using crimson::platform::nvidia::trace::ClippedFrameTraceSnapshot;
using crimson::platform::nvidia::trace::ClippedFrameTraceStats;
using crimson::platform::nvidia::trace::FrameSyncTraceState;
using crimson::platform::nvidia::trace::TextureDrawSnapshot;

[[noreturn]] void fail(const std::string &message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void require(bool value, const std::string &message) {
  if (!value) {
    fail(message);
  }
}

void testFrameSyncChangeGate() {
  FrameSyncTraceState previous;
  FrameSyncTraceState current;
  current.has_presented_frame = true;
  current.target_frame = 10;
  current.presented_frame = 9;
  current.zarr_box_count = 1;

  require(crimson::platform::nvidia::trace::frameSyncTraceChanged(previous,
                                                                  current),
          "an uninitialized trace state emits its first sample");
  crimson::platform::nvidia::trace::updateFrameSyncTraceState(previous,
                                                              current);
  require(!crimson::platform::nvidia::trace::frameSyncTraceChanged(previous,
                                                                   current),
          "an identical frame-sync state is suppressed");
  current.front_frame_after_draw = 10;
  require(crimson::platform::nvidia::trace::frameSyncTraceChanged(previous,
                                                                  current),
          "front texture advancement emits another sample");
}

void testMismatchAggregation() {
  ClippedFrameComparison first;
  first.current_parent_frame = 100;
  first.bbox_query_parent_frame = 101;
  first.resolved_parent_frame = 100;
  first.current_clip_local_frame = 5;
  first.bbox_clip_local_frame = 8;
  first.decoder_presented_local_frame = 6;
  first.front_before_valid = true;
  first.front_before_local_frame = 4;
  first.front_after_valid = true;
  first.front_after_local_frame = 5;
  const auto first_sanity =
      crimson::platform::nvidia::trace::evaluateClippedFrame(first);

  require(first_sanity.parent_matches_resolver,
          "matching parent resolver is recorded");
  require(!first_sanity.bbox_parent_matches_display,
          "parent bbox mismatch is observed");
  require(first_sanity.decoder_local_matches_resolver &&
              !*first_sanity.decoder_local_matches_resolver,
          "known decoder mismatch is non-null and false");
  require(first_sanity.front_texture_matches_resolver_before_draw &&
              !*first_sanity.front_texture_matches_resolver_before_draw,
          "known front mismatch is non-null and false");

  ClippedFrameTraceStats stats;
  crimson::platform::nvidia::trace::recordClippedFrame(stats, first_sanity);

  auto second = first;
  second.bbox_query_parent_frame = 100;
  second.bbox_clip_local_frame = 5;
  second.decoder_presented_local_frame = 5;
  second.front_before_local_frame = 5;
  const auto second_sanity =
      crimson::platform::nvidia::trace::evaluateClippedFrame(second);
  crimson::platform::nvidia::trace::recordClippedFrame(stats, second_sanity);

  const auto summary = stats.summaryJson();
  require(summary.at("frames_traced") == 2, "two observations are counted");
  require(summary.at("mismatches").at("bbox_parent_matches_display") == 1,
          "one parent bbox mismatch is counted");
  require(summary.at("mismatches").at("decoder_local_matches_resolver") == 1,
          "one decoder mismatch is counted");
  require(summary.at("mismatches")
                  .at("front_texture_matches_resolver_before_draw") == 1,
          "one front-before mismatch is counted");
  const auto &decoder_delta =
      summary.at("deltas").at("decoder_presented_local_minus_clip_local");
  require(
      decoder_delta.at("min") == 0 && decoder_delta.at("max") == 1 &&
          decoder_delta.at("most_common") == 0,
      "delta range and deterministic smallest-value tie break match schema");

  TextureDrawSnapshot draw;
  draw.enabled = true;
  draw.draw_sequence = 1;
  crimson::platform::nvidia::trace::recordTextureDrawOutcome(stats, draw);
  draw.callback_observed = true;
  crimson::platform::nvidia::trace::recordTextureDrawOutcome(stats, draw);
  const auto draw_summary = stats.summaryJson().at("mismatches");
  require(draw_summary.at("texture_draw_callback_missing") == 1 &&
              draw_summary.at("texture_draw_bound_mismatches") == 1,
          "draw callback outcomes are aggregated separately from frame rows");
}

void testUnknownComparisonStaysNull() {
  ClippedFrameComparison comparison;
  comparison.current_parent_frame = 40;
  comparison.bbox_query_parent_frame = 40;
  const auto sanity =
      crimson::platform::nvidia::trace::evaluateClippedFrame(comparison);
  require(!sanity.decoder_local_matches_resolver &&
              !sanity.front_texture_matches_resolver_before_draw &&
              !sanity.front_texture_matches_resolver_after_draw,
          "unavailable local frames remain unknown rather than false");
  require(!sanity.decoder_presented_local_minus_clip_local &&
              !sanity.bbox_query_local_minus_clip_local,
          "unavailable deltas remain unknown");
}

void testExactClippedFrameSchema() {
  ClippedFrameTraceSnapshot trace;
  trace.current_parent_frame_index = 50;
  trace.requested_parent_frame_index = 51;
  trace.playback_running = true;
  trace.playback_speed = 1.0;
  trace.playback_state = {{"play_video", true}};
  trace.active_video_path = "/clips/a.mp4";
  trace.active_clip_id = "clip-a";
  trace.decoder_presented_local_frame = 7;
  trace.bbox_query_parent_frame_index = 50;
  trace.bbox_row_count = 1;
  trace.detection_details_frame_id = 50;
  trace.sanity = crimson::platform::nvidia::trace::evaluateClippedFrame(
      ClippedFrameComparison{50, 50, 50, 7, 7, 7, true, 7, true, 7});

  BoundingBoxSnapshot box;
  box.payload_frame_id = 7;
  box.payload_camera_id = 42;
  box.box_index_in_payload = 0;
  box.x = 3.0F;
  box.y = 4.0F;
  box.w = 10.0F;
  box.h = 6.0F;
  box.class_id = 2;
  box.confidence = 0.75F;
  trace.first_bbox_display = box;

  const auto event = crimson::platform::nvidia::trace::clippedFrameJson(trace);
  require(event.at("event") == "clipped_frame",
          "clipped event name remains stable");
  require(event.at("resolver").is_null() &&
              event.at("requested_resolver").is_null(),
          "missing resolver rows remain JSON null");
  require(event.at("video").at("requested_decoder_local_frame").is_null(),
          "unknown requested decoder frame remains JSON null");
  require(event.at("video").at("draw_texture").is_null(),
          "disabled texture draw trace remains JSON null");
  require(event.at("bbox").at("first_bbox_source_image").is_null() &&
              event.at("bbox").at("first_bbox_display").at("cx") == 8.0,
          "bbox nullability and derived centre fields are preserved");
  require(event.at("sanity").at("decoder_local_matches_resolver") == true &&
              event.at("deltas").at(
                  "decoder_presented_local_minus_clip_local") == 0,
          "known matching comparisons preserve boolean and zero delta");
}

void testTextureDrawSchema() {
  TextureDrawSnapshot trace;
  trace.enabled = true;
  trace.draw_sequence = 4;
  trace.queued_texture_id = 10;
  trace.front_texture_id = 10;
  trace.staging_texture_id = 12;
  const auto json = crimson::platform::nvidia::trace::textureDrawJson(trace);
  require(json.at("queued_texture_matches_front") == true &&
              json.at("queued_texture_matches_staging") == false,
          "texture identity comparison fields are stable");
  require(json.at("callback_active_texture").is_null() &&
              json.at("front").at("parent_frame").is_null(),
          "unobserved callback and unknown front frame remain null");
}

void testFrameSyncHelpers() {
  BoundingBoxSnapshot box;
  box.payload_frame_id = 1;
  const auto bbox =
      crimson::platform::nvidia::trace::boundingBoxSummaryJson(3, box);
  require(bbox.at("count") == 3 && bbox.at("first").at("payload_frame_id") == 1,
          "bbox summary preserves count plus optional first box");
  const auto texture =
      crimson::platform::nvidia::trace::textureStateJson({true, 21, false, -1});
  require(
      texture ==
          crimson::platform::nvidia::trace::Json{{"front_valid", true},
                                                 {"front_frame", 21},
                                                 {"staging_valid", false},
                                                 {"staging_frame", -1}},
      "frame-sync texture state retains historical non-null sentinel frames");
}

} // namespace

int main() {
  testFrameSyncChangeGate();
  testMismatchAggregation();
  testUnknownComparisonStaysNull();
  testExactClippedFrameSchema();
  testTextureDrawSchema();
  testFrameSyncHelpers();
  std::cout << "nvidia_playback_trace_model_tests: PASS\n";
  return 0;
}
