#include "playback_diagnostics.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace diagnostics = crimson::playback::diagnostics;

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

std::set<std::string> keys(const diagnostics::Json &value) {
  std::set<std::string> result;
  for (auto it = value.begin(); it != value.end(); ++it) {
    result.insert(it.key());
  }
  return result;
}

diagnostics::CameraBufferSnapshot populatedBuffer() {
  diagnostics::CameraBufferSnapshot buffer;
  buffer.visible_idx = 2;
  buffer.camera_name = "Cam2010095";
  buffer.buffer_size = 5;
  buffer.read_head = 7;
  buffer.target_frame = 14;
  buffer.selected_frame = 13;
  buffer.last_uploaded_frame = 13;
  buffer.last_uploaded_local_frame = 4;
  buffer.texture_has_valid_frame = true;
  buffer.staging_valid = true;
  buffer.staging_frame = 15;
  buffer.staging_local_frame = 6;
  buffer.latest_decoded_frame = 18;
  buffer.slots = {{true, 11}, {false, 12}, {true, 13}, {true, 14}, {true, 15}};
  return buffer;
}

void testCameraBufferSummaryAndProfiles() {
  const auto buffer = populatedBuffer();
  const auto json = diagnostics::cameraBufferJson(buffer);
  require(json.at("normalized_read_head") == 2, "normalized read head");
  require(json.at("valid_slots") == 4, "valid slots");
  require(json.at("oldest_frame") == 11, "oldest frame");
  require(json.at("newest_frame") == 15, "newest frame");
  require(json.at("newest_contiguous_span_start") == 13, "contiguous start");
  require(json.at("newest_contiguous_span_end") == 15, "contiguous end");
  require(json.at("selected_slot") == 2, "selected slot");
  require(json.at("front_slot") == 2, "front slot");
  require(json.at("exact_target_slot") == 3, "exact target slot");
  require(json.at("read_head_frame") == 13, "read head frame");
  require(json.at("contains_target") == true, "contains target");

  const auto playback = diagnostics::playbackCameraBufferJson(buffer);
  require(keys(playback) ==
              std::set<std::string>(
                  {"buffer_size", "camera_name", "contains_target",
                   "exact_target_slot", "last_uploaded_frame",
                   "latest_decoded_frame", "newest_frame", "oldest_frame",
                   "read_head_frame", "sample_frames", "staging_frame",
                   "staging_valid", "texture_has_valid_frame", "valid_slots",
                   "visible_idx"}),
          "exact playback camera buffer keys");
  require(playback.at("read_head_frame") == 13,
          "playback read head uses raw integer");

  const auto clipped = diagnostics::clippedCameraBufferJson(buffer);
  require(keys(clipped) ==
              std::set<std::string>(
                  {"buffer_size", "front_local_frame", "front_parent_frame",
                   "front_slot", "newest_contiguous_span_end",
                   "newest_contiguous_span_start", "newest_frame",
                   "normalized_read_head", "oldest_frame", "read_head",
                   "read_head_frame", "selected_slot", "staging_local_frame",
                   "staging_parent_frame", "staging_valid", "valid_slots",
                   "visible_idx"}),
          "exact clipped camera buffer keys");

  auto invalid_frame_with_valid_texture = buffer;
  invalid_frame_with_valid_texture.last_uploaded_frame = -1;
  invalid_frame_with_valid_texture.last_uploaded_local_frame = -1;
  invalid_frame_with_valid_texture.staging_frame = -1;
  invalid_frame_with_valid_texture.staging_local_frame = -1;
  const auto invalid_clipped =
      diagnostics::clippedCameraBufferJson(invalid_frame_with_valid_texture);
  require(invalid_clipped.at("front_parent_frame") == -1,
          "valid texture keeps raw invalid parent frame");
  require(invalid_clipped.at("front_local_frame") == -1,
          "valid texture keeps raw invalid local frame");
  require(invalid_clipped.at("staging_parent_frame") == -1,
          "valid staging keeps raw invalid parent frame");
  require(invalid_clipped.at("staging_local_frame") == -1,
          "valid staging keeps raw invalid local frame");
}

void testNullAndInvalidFields() {
  diagnostics::ClippedPlaybackSnapshot snapshot;
  snapshot.current_frame_num = 4;
  snapshot.video_fps = 100.0;
  snapshot.playback.paused_frame_on_toggle = -1;
  snapshot.playback.last_resume_target_frame = -1;
  snapshot.seek.state = "Idle";
  const auto state = diagnostics::clippedPlaybackStateJson(snapshot);
  require(state.at("buffer").is_null(), "null buffer");
  require(state.at("playback").at("paused_frame_on_toggle").is_null(),
          "null paused frame");
  require(state.at("playback").at("last_resume_target_frame").is_null(),
          "null resume target");
  require(state.at("seek_progress").at("requested_camera_frame").is_null(),
          "null requested seek");
  require(diagnostics::nullableInt(-1).is_null(), "nullable invalid int");
  require(keys(state.at("seek_progress")) ==
              std::set<std::string>({"accurate", "requested_camera_frame",
                                     "seek_id", "skip_stimulus_hard_seek",
                                     "state", "target_camera_frame",
                                     "target_stimulus_frame"}),
          "exact clipped seek keys");
}

void testPresentationAndSeekEvents() {
  diagnostics::PlaybackTraceSnapshot snapshot;
  snapshot.video_loaded = true;
  snapshot.video_fps = 30.0;
  snapshot.current_frame_num = 17;
  snapshot.window_need_decoding_count = 2;
  snapshot.playback.play_video = true;
  snapshot.playback.to_display_frame_number = 17;
  snapshot.playback.current_stimulus_frame = 4;
  snapshot.seek.state = "Seeking";
  snapshot.seek.seek_id = 12;
  snapshot.seek.target_camera_frame = 17;
  snapshot.presenter.view_idx = 0;
  snapshot.presenter.target_frame = 17;
  snapshot.presenter.presented_slot = 2;
  snapshot.presenter.presented_frame = 17;
  snapshot.presenter.resolved_frame = 17;
  snapshot.camera_buffer = populatedBuffer();
  snapshot.stimulus.loaded = true;
  snapshot.stimulus.buffer_size = 3;
  snapshot.stimulus.slots = {{true, 4}, {false, 5}, {true, 6}};
  const auto event = diagnostics::playbackTraceEventJson(
      "transport_seek", {{"stage", "outcome"}}, snapshot);
  require(event.at("event") == "transport_seek", "playback event name");
  require(event.at("details").at("stage") == "outcome",
          "playback event details");
  require(event.at("presenter").at("presented_frame") == 17, "presented frame");
  require(event.at("seek").at("seek_id") == 12, "seek id");
  require(event.at("stimulus").at("buffered_frames") == 2,
          "stimulus buffered frames");
  require(keys(event.at("seek")) ==
              std::set<std::string>({"accurate", "cameras_settled",
                                     "cameras_total", "requested_camera_frame",
                                     "seek_id", "skip_stimulus_hard_seek",
                                     "state", "target_camera_frame",
                                     "target_stimulus_frame"}),
          "exact playback seek keys");

  const auto sync =
      diagnostics::frameSyncTraceEventJson({{"cause", "present"}}, snapshot);
  require(sync.at("event") == "camera_frame_sync", "frame sync event");
  require(sync.at("presenter").at("target_frame") == 17, "frame sync target");
  require(keys(sync.at("playback")) ==
              std::set<std::string>(
                  {"just_seeked", "pause_seeked", "play_video", "read_head",
                   "slider_frame_number", "slider_just_changed",
                   "to_display_frame_number"}),
          "exact frame-sync playback keys");
}

void testWriterEnvelopeAndFlush() {
  const auto path = std::filesystem::temp_directory_path() /
                    "crimson-playback-diagnostics-test.jsonl";
  std::error_code error;
  std::filesystem::remove(path, error);
  diagnostics::PlaybackTraceJsonlWriter writer;
  require(writer.open(path, "PlaybackDiagnosticsTest"), "open writer");
  writer.write({{"event", "first"}}, true);
  writer.write({{"event", "second"}}, true);
  writer.close();
  std::ifstream input(path);
  std::string first_line;
  std::string second_line;
  require(static_cast<bool>(std::getline(input, first_line)),
          "read first JSONL line");
  require(static_cast<bool>(std::getline(input, second_line)),
          "read second JSONL line");
  const auto first = diagnostics::Json::parse(first_line);
  const auto second = diagnostics::Json::parse(second_line);
  require(first.at("format") == "crimson_playback_trace_v1",
          "writer trace format");
  require(first.at("sequence") == 0, "first sequence");
  require(second.at("sequence") == 1, "second sequence");
  require(first.at("wall_epoch_ms").is_number_integer(), "writer wall time");
  require(first.at("elapsed_s").is_number(), "writer elapsed time");
  std::filesystem::remove(path, error);
}

} // namespace

int main() {
  testCameraBufferSummaryAndProfiles();
  testNullAndInvalidFields();
  testPresentationAndSeekEvents();
  testWriterEnvelopeAndFlush();
  return 0;
}
