#include "zarr/affiliated_video_repository.h"
#include "zarr/archive_context.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace {
using json = nlohmann::json;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':'    \
                << __LINE__ << '\n';                                           \
      return false;                                                            \
    }                                                                          \
  } while (false)

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-affiliated-video-" + std::to_string(seed) + "-" +
               std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

bool WriteText(const std::filesystem::path &path, const std::string &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
  return output.good();
}

bool WriteAttributes(const std::filesystem::path &group, const json &attributes,
                     bool zarr_v2 = false) {
  std::filesystem::create_directories(group);
  const auto metadata_path = group / (zarr_v2 ? ".zattrs" : "zarr.json");
  const json payload = zarr_v2 ? attributes
                               : json{{"zarr_format", 3},
                                      {"node_type", "group"},
                                      {"attributes", attributes}};
  return WriteText(metadata_path, payload.dump(2) + "\n");
}

json FullDescriptor(const std::string &video,
                    const std::string &availability = "ok") {
  return {
      {"stream_key", "full"},
      {"availability_status", availability},
      {"required_missing",
       availability == "ok" ? json::array() : json::array({"video"})},
      {"warnings", json::array()},
      {"files", {{"video", {{"path", video}, {"exists", true}}}}},
      {"contract",
       {{"role", "ingest_authoritative_full_frame"},
        {"output_kind", "full"},
        {"source", "orange_external_ipc"},
        {"camera_id", "2010093"},
        {"video", video},
        {"frame_clock", "recording_frame_id"},
        {"coordinate_space", "full_frame_pixels"},
        {"frame_count", 3},
        {"container", "mp4"}}},
  };
}

json Inventory(const json &full) {
  return {
      {"schema_id", "palette.acquisition_video_streams.v1"},
      {"schema_version", 1},
      {"source_schema_id", "orange_runtime_video_streams_v1"},
      {"source_frame_clock", "recording_frame_id"},
      {"inventory_status", "ok"},
      {"stream_count", 2},
      {"stream_keys", {"crop", "full"}},
      {"crop_stream_available", true},
      {"streams",
       {{"crop",
         {{"stream_key", "crop"},
          {"availability_status", "ok"},
          {"contract",
           {{"role", "runtime_derived_acquisition_input"},
            {"video", "derived/crop.mp4"}}}}},
        {"full", full}}},
  };
}

bool WriteClipIndex(const std::filesystem::path &recording_root,
                    const std::string &recording_id) {
  const auto make_clip = [&](int index, int64_t start) {
    const std::string clip_id = "clip_" + std::to_string(index);
    const std::string video_path = clip_id + "/camera.mp4";
    if (!WriteText(recording_root / video_path, "video")) {
      return json{};
    }
    return json{{"clip_index", index},
                {"clip_id", clip_id},
                {"recording_id", recording_id},
                {"camera_serial", "2010095"},
                {"video_path", video_path},
                {"status", "materialized"},
                {"start_is_keyframe", true},
                {"final_clip", index == 1},
                {"actual_start_frame", start},
                {"end_frame_exclusive", start + 3},
                {"first_clip_local_frame_index", 0},
                {"last_clip_local_frame_index", 2},
                {"frame_count", 3}};
  };
  const auto first = make_clip(0, 0);
  const auto second = make_clip(1, 3);
  if (first.empty() || second.empty()) {
    return false;
  }
  const json index = {
      {"status", "ok"},
      {"mode", "materialized_stream_copy"},
      {"recording_id", recording_id},
      {"camera_serial", "2010095"},
      {"clip_count", 2},
      {"source", {{"total_frames", 6}, {"fps", 30.0}}},
      {"clips", {first, second}},
      {"checks",
       {{{"code", "metadata_rows_match_keyframe_total_frames"},
         {"status", "ok"}},
        {{"code", "recording_frame_id_continuity"}, {"status", "ok"}},
        {{"code", "clip_start_is_keyframe"}, {"status", "ok"}},
        {{"code", "clip_recording_frame_id_continuity"}, {"status", "ok"}},
        {{"code", "clip_start_is_keyframe"}, {"status", "ok"}},
        {{"code", "clip_recording_frame_id_continuity"}, {"status", "ok"}}}}};
  return WriteText(recording_root / "recording_clip_index.json",
                   index.dump(2) + "\n");
}

struct Fixture {
  explicit Fixture(TemporaryDirectory *temporary,
                   std::string recording_name = "fixture_recording")
      : recording_root(temporary->path() / std::move(recording_name)),
        archive_root(recording_root / "zarr/analysis.zarr") {
    std::filesystem::create_directories(archive_root);
  }

  std::shared_ptr<crimson::zarr::ArchiveContext> open(std::string *error) {
    return crimson::zarr::ArchiveContext::Open(archive_root, error);
  }

  std::filesystem::path recording_root;
  std::filesystem::path archive_root;
};

bool TestAuthoritativeInventory(bool zarr_v2) {
  TemporaryDirectory temporary;
  Fixture fixture(&temporary, zarr_v2 ? "fixture_v2" : "fixture_v3");
  const std::string stored = "cams/main.mp4";
  const auto full = FullDescriptor(stored);
  CHECK(WriteAttributes(fixture.archive_root /
                            "analysis/acquisition_video_streams",
                        Inventory(full), zarr_v2));
  CHECK(WriteAttributes(fixture.archive_root /
                            "analysis/acquisition_video_streams/streams/full",
                        full, zarr_v2));
  const auto video = fixture.recording_root / stored;
  CHECK(WriteText(video, "video"));

  std::string error;
  auto archive = fixture.open(&error);
  CHECK(archive != nullptr);
  auto descriptor = crimson::zarr::DiscoverAffiliatedVideo(archive, &error);
  CHECK(descriptor.has_value());
  CHECK(error.empty());
  CHECK(descriptor->source ==
        crimson::zarr::AffiliatedVideoSource::AcquisitionFullStream);
  CHECK(descriptor->stored_path == stored);
  CHECK(descriptor->recording_root == fixture.recording_root);
  CHECK(descriptor->resolved_path == video);
  CHECK(descriptor->resolution ==
        crimson::zarr::AffiliatedVideoResolution::RecordingRelative);
  CHECK(descriptor->metadata_key == "contract.video");
  return true;
}

bool TestForeignAbsoluteInventoryPath() {
  TemporaryDirectory temporary;
  Fixture fixture(&temporary);
  const std::string stored = "/groups/archive/" +
                             fixture.recording_root.filename().string() +
                             "/cams/main.mp4";
  const auto full = FullDescriptor(stored);
  CHECK(WriteAttributes(fixture.archive_root /
                            "analysis/acquisition_video_streams",
                        Inventory(full)));
  CHECK(WriteAttributes(fixture.archive_root /
                            "analysis/acquisition_video_streams/streams/full",
                        full));
  const auto video = fixture.recording_root / "cams/main.mp4";
  CHECK(WriteText(video, "video"));

  std::string error;
  auto descriptor =
      crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
  CHECK(descriptor.has_value());
  CHECK(descriptor->stored_path == stored);
  CHECK(descriptor->recording_root == fixture.recording_root);
  CHECK(descriptor->resolved_path == video);
  CHECK(descriptor->resolution ==
        crimson::zarr::AffiliatedVideoResolution::RelocatedAbsolute);
  return true;
}

bool TestAuthoritativePrecedenceAndValidation() {
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    const auto full = FullDescriptor("cams/full.mp4");
    CHECK(
        WriteAttributes(fixture.archive_root, {{"source_path", "legacy.mp4"}}));
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams",
                          Inventory(full)));
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams/streams/full",
                          full));
    CHECK(WriteText(fixture.recording_root / "cams/full.mp4", "full"));
    CHECK(WriteText(fixture.recording_root / "cams/legacy.mp4", "legacy"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(descriptor->stored_path == "cams/full.mp4");
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    auto root_full = FullDescriptor("cams/full.mp4");
    auto child_full = root_full;
    child_full["contract"]["video"] = "cams/other.mp4";
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams",
                          Inventory(root_full)));
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams/streams/full",
                          child_full));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("descriptors disagree") != std::string::npos);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    auto full = FullDescriptor("cams/full.mp4");
    full["contract"]["role"] = "runtime_derived_acquisition_input";
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams",
                          Inventory(full)));
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams/streams/full",
                          full));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("Unsupported acquisition full-stream contract") !=
          std::string::npos);
  }
  return true;
}

bool TestLegacyPrecedence() {
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"source_path", "root.mp4"},
                           {"source_video_metadata",
                            {{"locator",
                              {{"kind", "recording_relative"},
                               {"relative_path", "cams/locator.mp4"}}}}}},
                          true));
    CHECK(WriteAttributes(fixture.archive_root / "raw_video",
                          {{"source_path", "cams/raw.mp4"},
                           {"source_video", "raw_fallback.mp4"}}));
    CHECK(WriteText(fixture.recording_root / "cams/raw.mp4", "raw"));
    CHECK(WriteText(fixture.recording_root / "cams/locator.mp4", "locator"));
    CHECK(WriteText(fixture.recording_root / "cams/root.mp4", "root"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(descriptor->source ==
          crimson::zarr::AffiliatedVideoSource::RawVideoAttributes);
    CHECK(descriptor->stored_path == "cams/raw.mp4");
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"source_path", "/missing/legacy.mp4"},
                           {"source_video_metadata",
                            {{"locator",
                              {{"kind", "recording_relative"},
                               {"relative_path", "cams/locator.mp4"}}}}}},
                          true));
    const auto video = fixture.recording_root / "cams/locator.mp4";
    CHECK(WriteText(video, "locator"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(descriptor->source ==
          crimson::zarr::AffiliatedVideoSource::SourceVideoLocator);
    CHECK(descriptor->resolved_path == video);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root / "raw_video",
                          {{"source_video", "raw_fallback.mp4"}}));
    const auto video = fixture.recording_root / "cams/raw_fallback.mp4";
    CHECK(WriteText(video, "raw fallback"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(descriptor->source ==
          crimson::zarr::AffiliatedVideoSource::RawVideoAttributes);
    CHECK(descriptor->resolved_path == video);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"source_path", "old/location/main.mp4"}}));
    const auto video = fixture.recording_root / "cams/main.mp4";
    CHECK(WriteText(video, "legacy basename"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(descriptor->resolved_path == video);
    CHECK(descriptor->resolution ==
          crimson::zarr::AffiliatedVideoResolution::LegacyCameraBasename);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    const auto video = fixture.recording_root / "cams/absolute.mp4";
    CHECK(WriteText(video, "absolute"));
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"source_path", video.string()}}));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(descriptor->resolved_path == video);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(
        fixture.archive_root,
        {{"source_video_path", "cams/direct.mp4"},
         {"source_video_metadata", {{"source_path", "cams/nested.mp4"}}}}));
    CHECK(WriteText(fixture.recording_root / "cams/direct.mp4", "direct"));
    CHECK(WriteText(fixture.recording_root / "cams/nested.mp4", "nested"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(descriptor->metadata_key == "source_video_path");
    CHECK(descriptor->stored_path == "cams/direct.mp4");
  }
  return true;
}

bool TestContractPathsDoNotUseLegacyFallbacks() {
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    const auto full = FullDescriptor("cams/main.mp4");
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams",
                          Inventory(full)));
    CHECK(WriteAttributes(fixture.archive_root /
                              "analysis/acquisition_video_streams/streams/full",
                          full));
    CHECK(WriteText(fixture.archive_root / "cams/main.mp4", "wrong root"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("does not exist") != std::string::npos);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"source_video_metadata",
                            {{"locator",
                              {{"kind", "recording_relative"},
                               {"relative_path", "cams/locator.mp4"}}}}}}));
    CHECK(WriteText(fixture.archive_root / "cams/locator.mp4", "wrong root"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("does not exist") != std::string::npos);
  }
  return true;
}

bool TestUnavailableInventoryFallsBack() {
  TemporaryDirectory temporary;
  Fixture fixture(&temporary);
  const auto full = FullDescriptor("cams/missing.mp4", "missing");
  CHECK(WriteAttributes(fixture.archive_root,
                        {{"source_path", "cams/legacy.mp4"}}));
  CHECK(WriteAttributes(fixture.archive_root /
                            "analysis/acquisition_video_streams",
                        Inventory(full)));
  CHECK(WriteAttributes(fixture.archive_root /
                            "analysis/acquisition_video_streams/streams/full",
                        full));
  const auto video = fixture.recording_root / "cams/legacy.mp4";
  CHECK(WriteText(video, "legacy"));
  std::string error;
  auto descriptor =
      crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
  CHECK(descriptor.has_value());
  CHECK(descriptor->source ==
        crimson::zarr::AffiliatedVideoSource::RootAttributes);
  CHECK(descriptor->resolved_path == video);
  return true;
}

bool TestPathFailuresAndCleanAbsence() {
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"source_path", "../outside.mp4"}}));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("escapes the recording root") != std::string::npos);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"source_path", "cams/missing.mp4"}}));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("does not exist") != std::string::npos);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(fixture.archive_root, {{"recording_id", "fixture"}}));
    std::string error = "stale";
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.empty());
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(
        WriteAttributes(fixture.archive_root, {{"source_video", "main.mp4"}}));
    CHECK(WriteText(fixture.recording_root / "main.mp4", "root"));
    CHECK(WriteText(fixture.recording_root / "cams/main.mp4", "cams"));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("ambiguous") != std::string::npos);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary);
    CHECK(WriteAttributes(
        fixture.archive_root,
        {{"source_video_metadata",
          {{"locator",
            {{"kind", "remote_uri"},
             {"uri", "https://example.invalid/video.mp4"}}}}}}));
    std::string error;
    auto descriptor =
        crimson::zarr::DiscoverAffiliatedVideo(fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("Unsupported source video locator kind") !=
          std::string::npos);
  }
  return true;
}

bool TestUnavailableArchiveRoot() {
  TemporaryDirectory temporary;
  std::string error;
  const auto archive = crimson::zarr::ArchiveContext::Open(
      temporary.path() / "unavailable.zarr", &error);
  CHECK(!archive);
  CHECK(!error.empty());
  CHECK(error.find("unavailable.zarr") != std::string::npos);
  return true;
}

bool TestRecordingClipIndexDiscovery() {
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary, "direct_recording");
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"recording_id", "direct_recording"}}));
    CHECK(WriteClipIndex(fixture.recording_root, "direct_recording"));
    std::string error;
    const auto descriptor = crimson::zarr::DiscoverAffiliatedRecordingClipIndex(
        fixture.open(&error), &error);
    CHECK(descriptor.has_value());
    CHECK(error.empty());
    CHECK(descriptor->recording_id == "direct_recording");
    CHECK(descriptor->frame_count == 6);
    CHECK(descriptor->index_path ==
          fixture.recording_root / "recording_clip_index.json");
  }
  {
    TemporaryDirectory temporary;
    const std::string recording_id = "benchmark_source_recording";
    const auto recordings = temporary.path() / "recordings";
    const auto recording_root = recordings / recording_id;
    const auto archive_root =
        recordings / ".palette_benchmarks" /
        "subject_masks/full_duration/canary/analysis.zarr";
    CHECK(WriteAttributes(archive_root, {{"recording_id", recording_id}}));
    CHECK(WriteClipIndex(recording_root, recording_id));
    std::string error;
    const auto archive =
        crimson::zarr::ArchiveContext::Open(archive_root, &error);
    CHECK(archive != nullptr);
    const auto descriptor =
        crimson::zarr::DiscoverAffiliatedRecordingClipIndex(archive, &error);
    CHECK(descriptor.has_value());
    CHECK(error.empty());
    CHECK(descriptor->recording_root == recording_root);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary, "stored_identity");
    CHECK(WriteAttributes(fixture.archive_root,
                          {{"recording_id", "different_identity"}}));
    CHECK(WriteClipIndex(fixture.recording_root, "stored_identity"));
    std::string error;
    const auto descriptor = crimson::zarr::DiscoverAffiliatedRecordingClipIndex(
        fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("identity disagrees") != std::string::npos);
  }
  {
    TemporaryDirectory temporary;
    Fixture fixture(&temporary, "directory_identity");
    CHECK(WriteAttributes(fixture.archive_root, json::object()));
    CHECK(WriteClipIndex(fixture.recording_root, "different_identity"));
    std::string error;
    const auto descriptor = crimson::zarr::DiscoverAffiliatedRecordingClipIndex(
        fixture.open(&error), &error);
    CHECK(!descriptor.has_value());
    CHECK(error.find("identity disagrees") != std::string::npos);
  }
  return true;
}

} // namespace

int main() {
  if (!TestAuthoritativeInventory(false) || !TestAuthoritativeInventory(true) ||
      !TestForeignAbsoluteInventoryPath() ||
      !TestAuthoritativePrecedenceAndValidation() || !TestLegacyPrecedence() ||
      !TestContractPathsDoNotUseLegacyFallbacks() ||
      !TestUnavailableInventoryFallsBack() ||
      !TestPathFailuresAndCleanAbsence() || !TestUnavailableArchiveRoot() ||
      !TestRecordingClipIndexDiscovery()) {
    return 1;
  }
  std::cout << "affiliated_video_repository_tests: PASS\n";
  return 0;
}
