#include "stimulus_camera_overlay_scene.h"
#include "zarr/archive_context.h"
#include "zarr/stimulus_context_timeline_legacy_repository.h"
#include "zarr/tensorstore_stimulus_context_timeline_repository.h"
#include "zarr_loader.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const char* label) {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              (std::string("crimson-stimulus-overlay-") + label + "-" +
               std::to_string(seed) + "-" + std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    path_.clear();
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool writeJson(const std::filesystem::path& path, const json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value.dump(2) << '\n';
  return output.good();
}

bool writeGroup(const std::filesystem::path& root, const std::string& path,
                const json& attributes = json::object()) {
  json metadata = {{"zarr_format", 3}, {"node_type", "group"}};
  if (!attributes.empty()) {
    metadata["attributes"] = attributes;
  }
  return writeJson(root / path / "zarr.json", metadata);
}

template <typename T, size_t Rank>
bool writeArray(const std::filesystem::path& root, const std::string& path,
                const std::string& data_type,
                const std::array<ts::Index, Rank>& shape,
                const std::vector<T>& values) {
  size_t count = 1;
  json shape_json = json::array();
  json chunk_json = json::array();
  for (const ts::Index extent : shape) {
    count *= static_cast<size_t>(extent);
    shape_json.push_back(extent);
    chunk_json.push_back(std::max<ts::Index>(1, extent));
  }
  if (count != values.size()) {
    return false;
  }
  json bytes = {{"name", "bytes"}};
  if (sizeof(T) > 1) {
    bytes["configuration"] = {{"endian", "little"}};
  }
  const json fill_value = std::is_same_v<T, bool> ? json(false) : json(0);
  const json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata",
       {{"shape", shape_json},
        {"data_type", data_type},
        {"chunk_grid",
         {{"name", "regular"},
          {"configuration", {{"chunk_shape", chunk_json}}}}},
        {"chunk_key_encoding",
         {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", fill_value},
        {"codecs", json::array({bytes})}}}};
  auto store =
      ts::Open<T, Rank>(spec, ts::OpenMode::open | ts::OpenMode::create,
                        ts::ReadWriteMode::read_write)
          .result();
  if (!store.ok()) {
    std::cerr << "Failed to create " << path << ": " << store.status()
              << '\n';
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  const auto write = ts::Write(source, *store).commit_future.result();
  return write.ok();
}

std::vector<uint8_t> fixedStrings(const std::vector<std::string>& values,
                                  size_t width) {
  std::vector<uint8_t> result(values.size() * width, 0);
  for (size_t row = 0; row < values.size(); ++row) {
    std::copy_n(values[row].begin(), std::min(width, values[row].size()),
                result.begin() + row * width);
  }
  return result;
}

bool writeFixture(const std::filesystem::path& root, bool corrected) {
  constexpr size_t kFrames = 40;
  const std::string group = "analysis/stimulus_runs";
  const std::string run_name = corrected ? "corrected_fixture"
                                         : "legacy_fixture";
  const std::string run = group + "/" + run_name;
  CHECK(writeGroup(root, "", {{"total_frames", kFrames},
                               {"video_width", 640},
                               {"video_height", 360},
                               {"fps", 100.0}}));
  CHECK(writeGroup(root, group, {{"latest_complete", run_name}}));
  CHECK(writeGroup(root, run));

  CHECK((writeArray<int64_t, 1>(
      root, run + "/events/stimulus_frame_num", "int64", {3},
      {10, 20, 30})));
  CHECK((writeArray<int64_t, 1>(root, run + "/events/camera_frame_id",
                                "int64", {3}, {5, -1, 15})));
  CHECK((writeArray<int32_t, 1>(root, run + "/events/event_type_id",
                                "int32", {3}, {3, 5, 3})));
  CHECK((writeArray<int64_t, 1>(root,
                                run + "/events/timestamp_ns_session",
                                "int64", {3}, {1000, 2000, 3000})));
  const auto names = fixedStrings({"warmup", "target", "Trial end"}, 24);
  const auto details = fixedStrings(
      {R"({"direction":"left"})", "manual", "{}"}, 64);
  CHECK((writeArray<uint8_t, 2>(root, run + "/events/name_or_context",
                                "uint8", {3, 24}, names)));
  CHECK((writeArray<uint8_t, 2>(root, run + "/events/details_json",
                                "uint8", {3, 64}, details)));

  CHECK((writeArray<int32_t, 1>(root, "analysis/enums/events/id", "int32",
                                {2}, {3, 5})));
  CHECK((writeArray<uint8_t, 2>(
      root, "analysis/enums/events/name", "uint8", {2, 32},
      fixedStrings({"Trial start", "Pulse"}, 32))));

  std::vector<bool> camera_mask(kFrames, false);
  std::vector<int64_t> legacy_camera_to_metadata(kFrames, -1);
  legacy_camera_to_metadata[5] = 0;
  legacy_camera_to_metadata[6] = 1;
  legacy_camera_to_metadata[15] = 2;
  CHECK((writeArray<bool, 1>(
      root, run + "/frame_alignment/camera_interpolation_mask", "bool",
      {static_cast<ts::Index>(kFrames)}, camera_mask)));
  CHECK((writeArray<int64_t, 1>(
      root, run + "/frame_alignment/camera_to_metadata_index", "int64",
      {static_cast<ts::Index>(kFrames)}, legacy_camera_to_metadata)));
  if (corrected) {
    std::vector<int64_t> corrected_camera_to_metadata(kFrames, -1);
    corrected_camera_to_metadata[5] = 0;
    corrected_camera_to_metadata[8] = 1;
    corrected_camera_to_metadata[15] = 2;
    std::vector<int64_t> direct(kFrames, -1);
    direct[5] = 10;
    direct[8] = 20;
    direct[15] = 30;
    CHECK((writeArray<int64_t, 1>(
        root, run + "/frame_alignment/camera_to_metadata_index_corrected",
        "int64", {static_cast<ts::Index>(kFrames)},
        corrected_camera_to_metadata)));
    CHECK((writeArray<int64_t, 1>(
        root,
        run + "/frame_alignment/camera_to_stimulus_frame_corrected",
        "int64", {static_cast<ts::Index>(kFrames)}, direct)));
  }
  CHECK((writeArray<int32_t, 1>(
      root, run + "/video_metadata/frame_metadata/stimulus_frame_num",
      "int32", {3}, {10, 20, 30})));
  CHECK((writeArray<int64_t, 1>(
      root,
      run + "/video_metadata/frame_metadata/triggering_camera_frame_id",
      "int64", {3}, {5, 6, 15})));
  if (corrected) {
    CHECK((writeArray<int32_t, 1>(
        root,
        run +
            "/video_metadata/frame_metadata/stimulus_frame_num_corrected",
        "int32", {3}, {10, 20, 30})));
  }

  const std::string step = run + "/steps/step_0";
  CHECK(writeGroup(root, step,
                   {{"step_index", 0},
                    {"step_name", "Approach"},
                    {"stimulus_mode_id", 7},
                    {"stimulus_mode", "MOVING_GRATING"},
                    {"start_camera_frame", 0},
                    {"end_camera_frame", 20},
                    {"duration_s", 0.2},
                    {"raw_protocol_params_json", R"({"direction":90})"}}));
  CHECK(writeGroup(root, step + "/moving_grating",
                   {{"grating_direction_camera_deg", 90.0},
                    {"orientation_degrees_authored", 45.0},
                    {"camera_to_projector_offset_deg", 45.0},
                    {"direction_mapping_status", "validated"},
                    {"direction_mapping_validated", true},
                    {"speed_mm_s", 12.5},
                    {"temporal_frequency_hz", 2.0}}));
  return true;
}

bool equalDouble(double left, double right) {
  return (std::isnan(left) && std::isnan(right)) || left == right;
}

bool snapshotsAgree(
    const crimson::timeline::StimulusContextTimelineSnapshot& left,
    const crimson::timeline::StimulusContextTimelineSnapshot& right) {
  if (left.descriptor.run_name != right.descriptor.run_name ||
      left.descriptor.frame_count != right.descriptor.frame_count ||
      left.descriptor.event_count != right.descriptor.event_count ||
      left.descriptor.step_count != right.descriptor.step_count ||
      left.descriptor.event_types.size() !=
          right.descriptor.event_types.size() ||
      left.events.size() != right.events.size() ||
      left.steps.size() != right.steps.size()) {
    return false;
  }
  for (size_t index = 0; index < left.descriptor.event_types.size(); ++index) {
    const auto& a = left.descriptor.event_types[index];
    const auto& b = right.descriptor.event_types[index];
    if (a.id != b.id || a.display_name != b.display_name ||
        a.event_count != b.event_count) {
      return false;
    }
  }
  for (size_t index = 0; index < left.events.size(); ++index) {
    const auto& a = left.events[index];
    const auto& b = right.events[index];
    if (a.source_event_index != b.source_event_index ||
        a.stimulus_frame != b.stimulus_frame ||
        a.camera_frame != b.camera_frame ||
        a.timestamp_ns_session != b.timestamp_ns_session ||
        a.event_type_id != b.event_type_id ||
        a.event_type_name != b.event_type_name ||
        a.name_or_context != b.name_or_context ||
        a.details_json != b.details_json || a.label != b.label) {
      return false;
    }
  }
  for (size_t index = 0; index < left.steps.size(); ++index) {
    const auto& a = left.steps[index];
    const auto& b = right.steps[index];
    if (a.step_index != b.step_index || a.step_name != b.step_name ||
        a.stimulus_mode_id != b.stimulus_mode_id ||
        a.stimulus_mode != b.stimulus_mode || a.kind != b.kind ||
        a.start_camera_frame != b.start_camera_frame ||
        a.end_camera_frame != b.end_camera_frame ||
        !equalDouble(a.duration_s, b.duration_s) ||
        a.raw_protocol_params_json != b.raw_protocol_params_json ||
        a.moving_grating.present != b.moving_grating.present ||
        !equalDouble(a.moving_grating.grating_direction_camera_deg,
                     b.moving_grating.grating_direction_camera_deg) ||
        !equalDouble(a.moving_grating.orientation_degrees_authored,
                     b.moving_grating.orientation_degrees_authored) ||
        !equalDouble(a.moving_grating.camera_to_projector_offset_deg,
                     b.moving_grating.camera_to_projector_offset_deg) ||
        a.moving_grating.direction_mapping_status !=
            b.moving_grating.direction_mapping_status ||
        a.moving_grating.direction_mapping_validated !=
            b.moving_grating.direction_mapping_validated ||
        a.moving_grating.has_direction_mapping_validated !=
            b.moving_grating.has_direction_mapping_validated ||
        !equalDouble(a.moving_grating.speed_mm_s,
                     b.moving_grating.speed_mm_s) ||
        !equalDouble(a.moving_grating.temporal_frequency_hz,
                     b.moving_grating.temporal_frequency_hz) ||
        a.concentric_grating.present != b.concentric_grating.present ||
        a.concentric_grating.stimulus_role !=
            b.concentric_grating.stimulus_role ||
        a.concentric_grating.radial_polarity_authored !=
            b.concentric_grating.radial_polarity_authored ||
        !equalDouble(a.concentric_grating.radial_sign_authored,
                     b.concentric_grating.radial_sign_authored) ||
        a.concentric_grating.radial_polarity_validated !=
            b.concentric_grating.radial_polarity_validated ||
        a.concentric_grating.has_radial_polarity_validated !=
            b.concentric_grating.has_radial_polarity_validated ||
        !equalDouble(a.concentric_grating.center_x_px,
                     b.concentric_grating.center_x_px) ||
        !equalDouble(a.concentric_grating.center_y_px,
                     b.concentric_grating.center_y_px) ||
        !equalDouble(a.concentric_grating.center_x_mm,
                     b.concentric_grating.center_x_mm) ||
        !equalDouble(a.concentric_grating.center_y_mm,
                     b.concentric_grating.center_y_mm) ||
        !equalDouble(a.concentric_grating.target_radius_min_mm,
                     b.concentric_grating.target_radius_min_mm) ||
        !equalDouble(a.concentric_grating.target_radius_max_mm,
                     b.concentric_grating.target_radius_max_mm) ||
        !equalDouble(a.concentric_grating.speed_mm_s,
                     b.concentric_grating.speed_mm_s) ||
        !equalDouble(a.concentric_grating.temporal_frequency_hz,
                     b.concentric_grating.temporal_frequency_hz)) {
      return false;
    }
  }
  return true;
}

bool compareAdapters(const std::filesystem::path& archive_path,
                     ZarrDetectionLoader& loader,
                     const std::vector<int64_t>& frames) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(archive_path, &error);
  if (!archive) {
    std::cerr << error << '\n';
    return false;
  }
  auto legacy =
      crimson::zarr::MakeLegacyStimulusContextTimelineRepository(loader);
  auto tensorstore =
      crimson::zarr::OpenStimulusContextTimelineRepository(
          archive, loader.getTotalFrames(), loader.getStimulusEventsRunName(),
          &error);
  if (!legacy || !tensorstore || !legacy->snapshot() ||
      !tensorstore->snapshot()) {
    std::cerr << "adapter open failed: " << error << '\n';
    return false;
  }
  if (!snapshotsAgree(*legacy->snapshot(), *tensorstore->snapshot())) {
    std::cerr << "stimulus context adapter snapshots differ\n";
    return false;
  }
  for (const int64_t frame : frames) {
    const auto legacy_frame =
        crimson::stimulus::resolveStimulusCameraOverlayFrame(
            legacy->snapshot().get(), frame);
    const auto tensorstore_frame =
        crimson::stimulus::resolveStimulusCameraOverlayFrame(
            tensorstore->snapshot().get(), frame);
    const std::string legacy_text =
        crimson::stimulus::stimulusCameraOverlayEventText(legacy_frame);
    const std::string tensorstore_text =
        crimson::stimulus::stimulusCameraOverlayEventText(tensorstore_frame);
    const auto legacy_scene =
        crimson::stimulus::buildStimulusCameraOverlayScene(
            legacy_frame, {430.0, 298.0},
            {legacy_text.size() * 8.0, legacy_text.empty() ? 0.0 : 14.0});
    const auto tensorstore_scene =
        crimson::stimulus::buildStimulusCameraOverlayScene(
            tensorstore_frame, {430.0, 298.0},
            {tensorstore_text.size() * 8.0,
             tensorstore_text.empty() ? 0.0 : 14.0});
    if (crimson::stimulus::stimulusCameraOverlaySceneSemanticSignature(
            legacy_scene) !=
        crimson::stimulus::stimulusCameraOverlaySceneSemanticSignature(
            tensorstore_scene)) {
      std::cerr << "stimulus overlay scenes differ at camera frame " << frame
                << '\n';
      return false;
    }
  }
  return true;
}

bool testFixture(bool corrected) {
  TemporaryDirectory temporary(corrected ? "corrected" : "legacy");
  CHECK(!temporary.path().empty());
  CHECK(writeFixture(temporary.path(), corrected));
  ZarrDetectionLoader loader;
  std::string error;
  CHECK(loader.loadZarrFile(temporary.path().string(), error));
  CHECK(loader.getStimulusEventsRunName() ==
        (corrected ? "corrected_fixture" : "legacy_fixture"));
  auto repository =
      crimson::zarr::MakeLegacyStimulusContextTimelineRepository(loader);
  CHECK(repository != nullptr);
  const auto snapshot = repository->snapshot();
  CHECK(snapshot != nullptr);
  CHECK(snapshot->events.size() == 3);
  CHECK(snapshot->events[0].camera_frame == 5);
  CHECK(snapshot->events[1].camera_frame == (corrected ? 8 : 6));
  CHECK(snapshot->events[1].label == "Pulse - target [manual]");
  CHECK(snapshot->steps.size() == 1);
  CHECK(snapshot->steps[0].kind ==
        crimson::timeline::StimulusStepKind::MovingGrating);
  CHECK(snapshot->steps[0].moving_grating.grating_direction_camera_deg ==
        90.0);
  CHECK(compareAdapters(temporary.path(), loader,
                        {0, 5, corrected ? 8 : 6, 15, 21, 39}));
  return true;
}

int runSelfTest() {
  if (!testFixture(true) || !testFixture(false)) {
    return 1;
  }
  std::cout << "stimulus_context_timeline_legacy_probe: PASS\n";
  return 0;
}

int runProductionComparison(const std::filesystem::path& archive,
                            const std::vector<int64_t>& frames) {
  ZarrDetectionLoader loader;
  std::string error;
  if (!loader.loadZarrFile(archive.string(), error)) {
    std::cerr << "load failed: " << error << '\n';
    return 2;
  }
  if (!compareAdapters(archive, loader, frames)) {
    return 3;
  }
  auto repository =
      crimson::zarr::MakeLegacyStimulusContextTimelineRepository(loader);
  std::cout << "stimulus_context_timeline_adapter_compare: PASS"
            << " run=" << repository->descriptor().run_name
            << " events=" << repository->descriptor().event_count
            << " steps=" << repository->descriptor().step_count
            << " frames=" << frames.size() << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--self-test") {
    return runSelfTest();
  }
  if (argc >= 3 && std::string(argv[1]) == "--compare") {
    std::vector<int64_t> frames;
    for (int index = 3; index < argc; ++index) {
      try {
        frames.push_back(std::stoll(argv[index]));
      } catch (const std::exception&) {
        std::cerr << "invalid camera frame: " << argv[index] << '\n';
        return 1;
      }
    }
    if (frames.empty()) {
      frames = {0, 56, 1024, 7024, 140034};
    }
    return runProductionComparison(argv[2], frames);
  }
  std::cerr << "usage: " << argv[0]
            << " --self-test | --compare ARCHIVE [CAMERA_FRAME ...]\n";
  return 1;
}
