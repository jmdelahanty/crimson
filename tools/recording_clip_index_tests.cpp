#include "recording_clip_index.h"
#include "recording_clip_media_provider.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-recording-clips-" + std::to_string(seed) + "-" +
               std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    throw std::runtime_error("Could not create temporary directory");
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw std::runtime_error(std::string("CHECK failed: ") + #condition +    \
                               " at " + __FILE__ + ":" +                       \
                               std::to_string(__LINE__));                      \
    }                                                                          \
  } while (false)

void WriteText(const std::filesystem::path &path, const std::string &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
  CHECK(output.good());
}

json Clip(size_t index, int64_t start, int64_t stop) {
  const std::string id = "clip_" + std::to_string(index);
  return {{"clip_index", index},
          {"clip_id", id},
          {"recording_id", "recording_fixture"},
          {"camera_serial", "2010095"},
          {"video_path", "clips/" + id + "/camera.mov"},
          {"status", "materialized"},
          {"start_is_keyframe", true},
          {"final_clip", index == 1},
          {"actual_start_frame", start},
          {"end_frame_exclusive", stop},
          {"first_clip_local_frame_index", 0},
          {"last_clip_local_frame_index", stop - start - 1},
          {"frame_count", stop - start}};
}

json ValidIndex() {
  return {{"status", "ok"},
          {"mode", "materialized_stream_copy"},
          {"recording_id", "recording_fixture"},
          {"camera_serial", "2010095"},
          {"clip_count", 2},
          {"source", {{"total_frames", 5}, {"fps", 30.0}}},
          {"clips", {Clip(0, 0, 3), Clip(1, 3, 5)}},
          {"checks",
           {{{"code", "metadata_rows_match_keyframe_total_frames"},
             {"status", "ok"}},
            {{"code", "recording_frame_id_continuity"}, {"status", "ok"}},
            {{"code", "clip_start_is_keyframe"},
             {"clip_id", "clip_0"},
             {"status", "ok"}},
            {{"code", "clip_recording_frame_id_continuity"},
             {"clip_id", "clip_0"},
             {"status", "ok"}},
            {{"code", "clip_start_is_keyframe"},
             {"clip_id", "clip_1"},
             {"status", "ok"}},
            {{"code", "clip_recording_frame_id_continuity"},
             {"clip_id", "clip_1"},
             {"status", "ok"}}}}};
}

std::filesystem::path WriteFixture(const std::filesystem::path &root,
                                   const json &document,
                                   bool write_second_video = true) {
  WriteText(root / "clips/clip_0/camera.mov", "clip zero");
  if (write_second_video) {
    WriteText(root / "clips/clip_1/camera.mov", "clip one");
  }
  const auto index = root / "recording_clip_index.json";
  WriteText(index, document.dump(2) + '\n');
  return index;
}

bool Rejected(const std::filesystem::path &root, const json &document,
              bool write_second_video = true) {
  std::string error;
  return !crimson::media::RecordingClipIndex::Open(
              WriteFixture(root, document, write_second_video), &error)
              .has_value() &&
         !error.empty();
}

void TestValidMapping() {
  TemporaryDirectory temporary;
  std::string error;
  auto index = crimson::media::RecordingClipIndex::Open(
      WriteFixture(temporary.path(), ValidIndex()), &error);
  CHECK(index.has_value());
  CHECK(error.empty());
  CHECK(index->recordingId() == "recording_fixture");
  CHECK(index->cameraSerial() == "2010095");
  CHECK(index->totalFrameCount() == 5);
  CHECK(index->framesPerSecond() == 30.0);
  CHECK(index->clips().size() == 2);
  CHECK(index->clip(2) == nullptr);

  const auto zero = index->resolveParentFrame(0);
  const auto two = index->resolveParentFrame(2);
  const auto three = index->resolveParentFrame(3);
  const auto four = index->resolveParentFrame(4);
  CHECK(zero && zero->clip->index == 0 && zero->clip_local_frame == 0);
  CHECK(two && two->clip->index == 0 && two->clip_local_frame == 2);
  CHECK(three && three->clip->index == 1 && three->clip_local_frame == 0);
  CHECK(four && four->clip->index == 1 && four->clip_local_frame == 1);
  CHECK(!index->resolveParentFrame(-1));
  CHECK(!index->resolveParentFrame(5));
}

void TestMediaProviderTransitions() {
  TemporaryDirectory temporary;
  std::string error;
  auto provider = crimson::media::RecordingClipMediaProvider::Open(
      WriteFixture(temporary.path(), ValidIndex()), &error);
  CHECK(provider.has_value());
  CHECK(error.empty());

  const auto first = provider->resolveParentFrame(0);
  const auto before_boundary = provider->resolveParentFrame(2);
  const auto boundary = provider->resolveParentFrame(3);
  const auto final = provider->resolveParentFrame(4);
  CHECK(first && first->clip_index == 0 && first->clip_local_frame == 0);
  CHECK(before_boundary && before_boundary->clip_index == 0 &&
        before_boundary->clip_local_frame == 2);
  CHECK(boundary && boundary->clip_index == 1 &&
        boundary->clip_local_frame == 0);
  CHECK(final && final->clip_index == 1 && final->clip_local_frame == 1);
  CHECK(!provider->resolveParentFrame(-1));
  CHECK(!provider->resolveParentFrame(5));

  CHECK(first->first_parent_frame == 0 && first->last_parent_frame == 2);
  CHECK(boundary->first_parent_frame == 3 && boundary->last_parent_frame == 4);
  CHECK(first->parent_frame_by_clip_local->size() == 3);
  CHECK((*first->parent_frame_by_clip_local)[0] == 0);
  CHECK((*first->parent_frame_by_clip_local)[2] == 2);
  CHECK(boundary->parent_frame_by_clip_local->size() == 2);
  CHECK((*boundary->parent_frame_by_clip_local)[0] == 3);
  CHECK((*boundary->parent_frame_by_clip_local)[1] == 4);
  CHECK(first->parent_frame_by_clip_local ==
        before_boundary->parent_frame_by_clip_local);
  CHECK(first->parent_frame_by_clip_local !=
        boundary->parent_frame_by_clip_local);
}

void TestMalformedIndexesFailClosed() {
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][1]["actual_start_frame"] = 4;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][1]["actual_start_frame"] = 2;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][1]["clip_id"] = "clip_0";
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][0]["start_is_keyframe"] = false;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][0]["video_path"] = "../escape.mov";
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["source"]["total_frames"] = 6;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["checks"][2]["status"] = "failed";
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    CHECK(Rejected(temporary.path(), ValidIndex(), false));
  }
}

json RollingFixture(int64_t first = 1, int64_t offset = 1) {
  json rows = json::array();
  json inputs = json::array();
  json checks = json::array();
  int64_t next = first;
  for (int index = 0; index < 3; ++index) {
    const int64_t count = index == 1 ? 5 : (index == 0 ? 3 : 2);
    const int64_t last = next + count - 1;
    const std::string id = "clip_00000" + std::to_string(index);
    const std::string dir = "clips/" + id;
    const std::string video = dir + "/Cam2010095_fixture.mp4";
    const std::string metadata = dir + "/Cam2010095_fixture_meta.csv";
    const std::string keyframes = dir + "/Cam2010095_fixture_keyframe.json";
    const std::string manifest = dir + "/clip_manifest.json";
    rows.push_back({{"clip_index", index}, {"clip_id", id},
      {"recording_id", "rolling_fixture"}, {"session_id", "rolling_session"},
      {"camera_serial", "2010095"},
      {"producer", "orange_gui_external_ipc"},
      {"recording_backend_mode", "external_ipc"}, {"source_layout", "rolling_clips"},
      {"status", "completed"}, {"drain_completed", true},
      {"final_clip", index == 2}, {"first_recording_frame_id", next},
      {"last_recording_frame_id", last}, {"frame_count", count},
      {"recording_frame_id_gaps", 0}, {"packet_count", count},
      {"video_path", video}, {"metadata_path", metadata},
      {"keyframe_path", keyframes}, {"clip_manifest_path", manifest}});
    inputs.push_back({{"camera_serial", "2010095"}, {"clip_id", id},
      {"first_recording_frame_id", next}, {"last_recording_frame_id", last},
      {"rows", count}, {"recording_frame_id_gaps", 0},
      {"parent_frame_index_offset", offset},
      {"metadata_path", "/original/rolling_fixture/" + metadata}});
    checks.push_back({{"clip_id", id}, {"code", "metadata_rows_match_clip_index_frame_count"},
      {"expected", count}, {"observed", count}, {"status", "ok"}});
    checks.push_back({{"clip_id", id}, {"code", "first_recording_frame_id_matches_clip_index"},
      {"expected", next}, {"observed", next}, {"status", "ok"}});
    checks.push_back({{"clip_id", id}, {"code", "last_recording_frame_id_matches_clip_index"},
      {"expected", last}, {"observed", last}, {"status", "ok"}});
    checks.push_back({{"clip_id", id}, {"code", "clip_recording_frame_id_continuity"},
      {"recording_frame_id_gaps", 0}, {"status", "ok"}});
    if (index)
      checks.push_back({{"camera_serial", "2010095"}, {"clip_id", id},
        {"code", "inter_clip_recording_frame_id_continuity"},
        {"previous_last_recording_frame_id", next - 1},
        {"current_first_recording_frame_id", next}, {"status", "ok"}});
    next = last + 1;
  }
  checks.push_back({{"code", "recording_frame_index_nonempty"},
    {"row_count", 10}, {"status", "ok"}});
  return {{"index", {{"schema_id", "palette.orange_external_ipc_recording_clip_index.v1"},
    {"schema_version", 1}, {"mode", "rolling_clips"},
    {"source_layout", "rolling_clips"}, {"recording_backend_mode", "external_ipc"},
    {"row_granularity", "clip_camera"}, {"recording_id", "rolling_fixture"},
    {"session_id", "rolling_session"},
    {"producer", "orange_gui_external_ipc"}, {"recording_folder", "."},
    {"cameras", {"2010095"}}, {"row_count", 3}, {"clip_count", 3},
    {"camera_ranges", {{"2010095", {{"clip_count", 3},
      {"first_recording_frame_id", first}, {"last_recording_frame_id", next - 1},
      {"total_frame_count", 10}, {"recording_frame_id_gaps", 0}}},
      {"stale_other_camera", {{"clip_count", 99}}}}}, {"rows", rows}}},
    {"frame_manifest", {{"schema_version", "palette.recording_frame_index_manifest.v1"},
      {"frame_index_schema_version", "palette.recording_frame_index.v1"},
      {"status", "ok"}, {"failure_count", 0}, {"dry_run", false},
      {"source_authority", "recording_clip_index + per_clip_metadata_csv"},
      {"source_layout", "rolling_clips"}, {"recording_id", "rolling_fixture"},
      {"session_id", "rolling_session"}, {"camera_serials", {"2010095"}},
      {"recording_folder", "/original/rolling_fixture"}, {"row_count", 10},
      {"recording_frame_id_min", first}, {"recording_frame_id_max", next - 1},
      {"inputs", inputs}, {"checks", checks}}}};
}

std::filesystem::path WriteRollingFixture(const std::filesystem::path &root,
                                          const json &fixture) {
  const auto &rows = fixture.at("index").at("rows");
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto &row = rows.at(i);
    const auto video = row.at("video_path").get<std::string>();
    const auto metadata = row.at("metadata_path").get<std::string>();
    const auto keyframes = row.at("keyframe_path").get<std::string>();
    const auto manifest = row.at("clip_manifest_path").get<std::string>();
    WriteText(root / video, "video");
    WriteText(root / metadata, "frame_id,recording_frame_id\n");
    WriteText(root / keyframes, json({{"codec", "hevc"}, {"fps", 30.0},
      {"total_frames", row.at("frame_count")}, {"keyframe_frames", {0}}}).dump());
    const json artifact = {{"camera_serial", "2010095"}, {"video_path", video},
      {"metadata_path", metadata}, {"keyframe_path", keyframes},
      {"frame_count", row.at("frame_count")},
      {"first_recording_frame_id", row.at("first_recording_frame_id")},
      {"last_recording_frame_id", row.at("last_recording_frame_id")},
      {"recording_frame_id_gaps", 0}};
    json full = artifact; full.erase("camera_serial");
    full.erase("video_path"); full.erase("metadata_path"); full.erase("keyframe_path");
    full["role"] = "ingest_authoritative";
    full["output_kind"] = "full"; full["video"] = video;
    full["metadata"] = metadata; full["keyframes"] = keyframes;
    full["frame_rate"] = 30.0; full["codec"] = "hevc";
    WriteText(root / manifest, json({{"schema_id", "palette.orange_external_ipc_rolling_clip.v1"},
      {"schema_version", 1}, {"recording_id", "rolling_fixture"},
      {"session_id", "rolling_session"},
      {"clip_id", row.at("clip_id")}, {"clip_index", i}, {"status", "completed"},
      {"final_clip", i + 1 == rows.size()}, {"camera_artifacts", {artifact}},
      {"recording_outputs", {{"2010095", {{"full", full}}}}}}).dump());
  }
  WriteText(root / "recording_frame_index_manifest.json",
            fixture.at("frame_manifest").dump());
  const auto path = root / "recording_clip_index.json";
  WriteText(path, fixture.at("index").dump());
  return path;
}

void TestRollingClipIndexContract() {
  for (const auto pair : {std::pair<int64_t, int64_t>{1, 1}, {0, 0}, {100, 100}}) {
    TemporaryDirectory temporary; std::string error;
    auto index = crimson::media::RecordingClipIndex::Open(
        WriteRollingFixture(temporary.path(), RollingFixture(pair.first, pair.second)), &error);
    CHECK(index && error.empty() && index->totalFrameCount() == 10);
    CHECK(index->resolveParentFrame(0)->clip_local_frame == 0);
    CHECK(index->resolveParentFrame(3)->clip->index == 1);
    CHECK(index->resolveParentFrame(3)->clip_local_frame == 0);
    CHECK(index->resolveParentFrame(8)->clip->index == 2);
    CHECK(index->resolveParentFrame(9)->clip_local_frame == 1);
    CHECK(!index->resolveParentFrame(-1));
    CHECK(!index->resolveParentFrame(10));
  }
  TemporaryDirectory provider_root; std::string provider_error;
  auto provider = crimson::media::RecordingClipMediaProvider::Open(
      WriteRollingFixture(provider_root.path(), RollingFixture()), &provider_error);
  CHECK(provider && provider_error.empty());
  const auto first = provider->resolveParentFrame(0);
  const auto boundary = provider->resolveParentFrame(3);
  CHECK(first && first->first_parent_frame == 0 && first->last_parent_frame == 2);
  CHECK(boundary && boundary->clip_index == 1 && boundary->clip_local_frame == 0);
  CHECK(boundary->first_parent_frame == 3 && boundary->last_parent_frame == 7);
  CHECK(boundary->parent_frame_by_clip_local->size() == 5);
  CHECK((*boundary->parent_frame_by_clip_local)[4] == 7);
  TemporaryDirectory telemetry_root; std::string telemetry_error;
  auto telemetry_fixture = RollingFixture();
  telemetry_fixture["index"]["rows"][1]["packet_count"] = 4;
  auto telemetry_index = crimson::media::RecordingClipIndex::Open(
      WriteRollingFixture(telemetry_root.path(), telemetry_fixture),
      &telemetry_error);
  CHECK(telemetry_index && telemetry_error.empty());
  CHECK(telemetry_index->resolveParentFrame(7)->clip_local_frame == 4);
  const auto reject = [](const auto &mutation) {
    TemporaryDirectory temporary; auto fixture = RollingFixture();
    const auto path = WriteRollingFixture(temporary.path(), fixture);
    mutation(fixture);
    WriteText(path, fixture["index"].dump());
    WriteText(temporary.path() / "recording_frame_index_manifest.json",
              fixture["frame_manifest"].dump());
    std::string error; CHECK(!crimson::media::RecordingClipIndex::Open(path, &error));
    CHECK(!error.empty());
  };
  reject([](json &f) { f["index"]["rows"][1]["first_recording_frame_id"] = 5; });
  reject([](json &f) { f["index"]["schema_id"] = "orange.recording_clip_index"; });
  reject([](json &f) { f["index"]["rows"][2]["final_clip"] = false; });
  reject([](json &f) { f["index"]["rows"][1]["clip_id"] = "clip_000000"; });
  reject([](json &f) { f["index"]["rows"][1]["first_recording_frame_id"] = 3; });
  reject([](json &f) { f["frame_manifest"]["inputs"][0]["parent_frame_index_offset"] = 0; });
  reject([](json &f) { f["frame_manifest"]["inputs"][0].erase("parent_frame_index_offset"); });
  reject([](json &f) { f["frame_manifest"]["inputs"][1]["rows"] = 99; });
  reject([](json &f) { f["index"]["rows"][0]["drain_completed"] = false; });
  reject([](json &f) { f["index"]["cameras"].push_back("2010096"); });
  reject([](json &f) { f["frame_manifest"]["camera_serials"] = json{"2010096"}; });
  reject([](json &f) { f["frame_manifest"]["session_id"] = "wrong_session"; });
  reject([](json &f) { f["index"]["rows"][0]["session_id"] = "wrong_session"; });
  reject([](json &f) { f["index"]["rows"][0]["video_path"] = "../escape.mp4"; });
  reject([](json &f) { f["frame_manifest"]["inputs"][0]["metadata_path"] = "/other/wrong.csv"; });
  reject([](json &f) { f["index"]["rows"][1]["packet_count"] = -1; });
  reject([](json &f) { f["index"]["camera_ranges"]["2010095"]["recording_frame_id_gaps"] = 1; });
  reject([](json &f) { f["frame_manifest"]["checks"][0]["observed"] = 2; });
  reject([](json &f) {
    f["frame_manifest"]["checks"][8]["previous_last_recording_frame_id"] = 2;
  });
  reject([](json &f) { f["frame_manifest"]["checks"][8]["camera_serial"] = "2010096"; });

  TemporaryDirectory missing; auto fixture = RollingFixture();
  const auto path = WriteRollingFixture(missing.path(), fixture);
  std::filesystem::remove(missing.path() /
      fixture["index"]["rows"][1]["video_path"].get<std::string>());
  std::string error;
  CHECK(!crimson::media::RecordingClipIndex::Open(path, &error));
  CHECK(!error.empty());

  reject([](json &f) {
    f["index"]["schema_version"] = std::numeric_limits<uint64_t>::max();
  });

  const auto reject_artifact = [](const char *label, const auto &mutation) {
    TemporaryDirectory temporary;
    auto fixture = RollingFixture();
    const auto index_path = WriteRollingFixture(temporary.path(), fixture);
    mutation(temporary.path(), fixture);
    std::string artifact_error;
    if (crimson::media::RecordingClipIndex::Open(index_path, &artifact_error)) {
      throw std::runtime_error(std::string("Expected artifact rejection: ") +
                               label);
    }
    CHECK(!artifact_error.empty());
  };
  const auto replace_keyframe = [](const std::filesystem::path &root,
                                   const json &fixture, json value) {
    WriteText(root / fixture["index"]["rows"][0]["keyframe_path"]
                         .get<std::string>(),
              value.dump());
  };
  reject_artifact("missing keyframe", [&](const auto &root, const json &fixture) {
    std::filesystem::remove(root / fixture["index"]["rows"][0]
                                       ["keyframe_path"].get<std::string>());
  });
  reject_artifact("missing frame manifest", [&](const auto &root, const json &) {
    std::filesystem::remove(root / "recording_frame_index_manifest.json");
  });
  reject_artifact("nonzero first keyframe", [&](const auto &root, const json &fixture) {
    replace_keyframe(root, fixture, {{"codec", "hevc"}, {"fps", 30.0},
      {"total_frames", 3}, {"keyframe_frames", {1}}});
  });
  reject_artifact("unsorted keyframes", [&](const auto &root, const json &fixture) {
    replace_keyframe(root, fixture, {{"codec", "hevc"}, {"fps", 30.0},
      {"total_frames", 3}, {"keyframe_frames", {0, 2, 1}}});
  });
  reject_artifact("duplicate keyframes", [&](const auto &root, const json &fixture) {
    replace_keyframe(root, fixture, {{"codec", "hevc"}, {"fps", 30.0},
      {"total_frames", 3}, {"keyframe_frames", {0, 0}}});
  });
  reject_artifact("out-of-range keyframe", [&](const auto &root, const json &fixture) {
    replace_keyframe(root, fixture, {{"codec", "hevc"}, {"fps", 30.0},
      {"total_frames", 3}, {"keyframe_frames", {0, 3}}});
  });
  reject_artifact("inconsistent fps", [&](const auto &root, const json &fixture) {
    replace_keyframe(root, fixture, {{"codec", "hevc"}, {"fps", 29.0},
      {"total_frames", 3}, {"keyframe_frames", {0}}});
  });
  reject_artifact("keyframe count mismatch", [&](const auto &root, const json &fixture) {
    replace_keyframe(root, fixture, {{"codec", "hevc"}, {"fps", 30.0},
      {"total_frames", 4}, {"keyframe_frames", {0}}});
  });
  const auto mutate_clip_manifest = [](const std::filesystem::path &root,
                                       const json &fixture,
                                       const auto &mutation) {
    const auto relative = fixture["index"]["rows"][0]["clip_manifest_path"]
                              .get<std::string>();
    std::ifstream input(root / relative);
    json value; input >> value; CHECK(input.good() || input.eof());
    mutation(value);
    WriteText(root / relative, value.dump());
  };
  reject_artifact("clip identity mismatch", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture,
                         [](json &m) { m["clip_id"] = "wrong"; });
  });
  reject_artifact("missing artifact camera", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture,
      [](json &m) { m["camera_artifacts"][0].erase("camera_serial"); });
  });
  reject_artifact("missing full authority", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture,
      [](json &m) { m["recording_outputs"]["2010095"].erase("full"); });
  });
  reject_artifact("canonical full video mismatch", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture, [](json &m) {
      m["recording_outputs"]["2010095"]["full"]["video"] = "wrong.mp4";
    });
  });
  reject_artifact("alias cannot mask canonical mismatch", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture, [](json &m) {
      auto &full = m["recording_outputs"]["2010095"]["full"];
      full["video_path"] = full["video"];
      full["video"] = "wrong.mp4";
    });
  });
  reject_artifact("full count mismatch", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture, [](json &m) {
      m["recording_outputs"]["2010095"]["full"]["frame_count"] = 2;
    });
  });
  reject_artifact("incomplete clip manifest", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture,
                         [](json &m) { m["status"] = "recording"; });
  });
  reject_artifact("wrong final clip", [&](const auto &root, const json &fixture) {
    mutate_clip_manifest(root, fixture,
                         [](json &m) { m["final_clip"] = true; });
  });
  reject_artifact("missing clip manifest", [&](const auto &root, const json &fixture) {
    std::filesystem::remove(root / fixture["index"]["rows"][0]
                                       ["clip_manifest_path"].get<std::string>());
  });
}

} // namespace

int main() {
  try {
    TestValidMapping();
    TestMediaProviderTransitions();
    TestMalformedIndexesFailClosed();
    TestRollingClipIndexContract();
    std::cout << "recording_clip_index_tests: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "recording_clip_index_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
