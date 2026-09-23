#include "zarr/archive_context.h"
#include "zarr/stimulus_repository.h"
#include "zarr/tensorstore_stimulus_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;
using crimson::zarr::StimulusMappingPreference;
using crimson::zarr::StimulusMappingSource;
using crimson::zarr::StimulusMappingStatus;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' \
                << __LINE__ << '\n';                                         \
      return false;                                                           \
    }                                                                         \
  } while (false)

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = base / ("crimson-stimulus-repository-" +
                      std::to_string(attempt));
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

json ReadJsonFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  return json::parse(input);
}

bool WriteJsonFile(const std::filesystem::path& path, const json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    return false;
  }
  output << value.dump(2) << '\n';
  return output.good();
}

template <typename T>
bool WriteArray(const std::filesystem::path& root,
                const std::string& path,
                const std::string& data_type,
                const std::vector<T>& values) {
  json bytes_codec = {{"name", "bytes"}};
  if (sizeof(T) > 1) {
    bytes_codec["configuration"] = {{"endian", "little"}};
  }
  const auto size = static_cast<ts::Index>(values.size());
  const json fill_value = std::is_same_v<T, bool> ? json(false) : json(0);
  json metadata = {
      {"shape", {size}},
      {"data_type", data_type},
      {"chunk_grid",
       {{"name", "regular"},
        {"configuration", {{"chunk_shape", {std::max<ts::Index>(1, size)}}}}}},
      {"chunk_key_encoding",
       {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
      {"fill_value", fill_value},
      {"codecs", json::array({bytes_codec})},
  };
  json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata", std::move(metadata)},
  };
  auto store = ts::Open<T, 1>(
                   spec, ts::OpenMode::open | ts::OpenMode::create,
                   ts::ReadWriteMode::read_write)
                   .result();
  if (!store.ok()) {
    std::cerr << "Failed to create fixture array " << path << ": "
              << store.status() << '\n';
    return false;
  }
  const ts::Index extents[1] = {size};
  auto source = ts::AllocateArray<T>(extents);
  std::copy(values.begin(), values.end(), source.data());
  auto write = ts::Write(source, *store).commit_future.result();
  if (!write.ok()) {
    std::cerr << "Failed to write fixture array " << path << ": "
              << write.status() << '\n';
    return false;
  }
  return true;
}

bool BuildFixtureArchive(const json& fixture,
                         const std::filesystem::path& root) {
  const std::string run_name = fixture.at("run_name").get<std::string>();
  const std::string base = "analysis/stimulus_runs/" + run_name + "/";
  const auto& arrays = fixture.at("arrays");

  CHECK(WriteJsonFile(
      root / "analysis/stimulus_runs/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes", {{"latest", run_name}}}}));
  CHECK(WriteJsonFile(
      root / base / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"source_stimulus_video_path",
          fixture.at("source_stimulus_video_path")}}}}));
  CHECK(WriteJsonFile(
      root / base / "frame_alignment/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"camera_frame_offset", fixture.at("camera_frame_offset")}}}}));

  CHECK(WriteArray<int64_t>(
      root, base + "frame_alignment/camera_to_metadata_index", "int64",
      arrays.at("camera_to_metadata_index").get<std::vector<int64_t>>()));
  CHECK(WriteArray<int64_t>(
      root, base + "frame_alignment/camera_to_metadata_index_corrected",
      "int64", arrays.at("camera_to_metadata_index_corrected")
                   .get<std::vector<int64_t>>()));
  CHECK(WriteArray<int64_t>(
      root, base + "frame_alignment/camera_to_stimulus_frame_corrected",
      "int64", arrays.at("camera_to_stimulus_frame_corrected")
                   .get<std::vector<int64_t>>()));
  CHECK(WriteArray<int64_t>(
      root, base + "video_metadata/frame_metadata/stimulus_frame_num",
      "int64", arrays.at("stimulus_frame_num").get<std::vector<int64_t>>()));
  CHECK(WriteArray<int64_t>(
      root,
      base +
          "video_metadata/frame_metadata/stimulus_frame_num_corrected",
      "int64",
      arrays.at("stimulus_frame_num_corrected").get<std::vector<int64_t>>()));
  CHECK(WriteArray<bool>(
      root, base + "frame_alignment/camera_interpolation_mask", "bool",
      arrays.at("camera_interpolation_mask").get<std::vector<bool>>()));
  CHECK(WriteArray<uint8_t>(
      root, base + "frame_alignment/camera_stimulus_frame_interpolated",
      "uint8", arrays.at("camera_stimulus_frame_interpolated")
                   .get<std::vector<uint8_t>>()));
  return true;
}

StimulusMappingStatus StatusFromString(const std::string& value) {
  if (value == "mapped") {
    return StimulusMappingStatus::Mapped;
  }
  if (value == "out_of_range") {
    return StimulusMappingStatus::OutOfRange;
  }
  return StimulusMappingStatus::Missing;
}

StimulusMappingSource SourceFromString(const std::string& value) {
  if (value == "corrected_direct") {
    return StimulusMappingSource::CorrectedDirect;
  }
  if (value == "corrected_metadata") {
    return StimulusMappingSource::CorrectedMetadata;
  }
  if (value == "legacy_metadata") {
    return StimulusMappingSource::LegacyMetadata;
  }
  return StimulusMappingSource::None;
}

bool CheckExpectations(const crimson::zarr::StimulusRepository& repository,
                       const json& expectations,
                       StimulusMappingPreference preference) {
  for (const auto& expected : expectations) {
    const auto result = repository.resolveCameraFrame(
        expected.at("camera").get<int32_t>(), preference);
    CHECK(result.status ==
          StatusFromString(expected.at("status").get<std::string>()));
    if (result.status != StimulusMappingStatus::Mapped) {
      CHECK(result.source == StimulusMappingSource::None);
      CHECK(!result.stimulus_frame.has_value());
      continue;
    }
    CHECK(result.source ==
          SourceFromString(expected.at("source").get<std::string>()));
    CHECK(result.stimulus_frame == expected.at("stimulus").get<int32_t>());
    CHECK(result.interpolated == expected.at("interpolated").get<bool>());
    if (expected.contains("metadata")) {
      CHECK(result.metadata_index == expected.at("metadata").get<int32_t>());
    } else {
      CHECK(!result.metadata_index.has_value());
    }
  }
  return true;
}

bool RunTest() {
  const auto fixture_path = std::filesystem::path(CRIMSON_SOURCE_DIR) /
                            "tests/fixtures/"
                            "stimulus_alignment_repository_fixture.json";
  const json fixture = ReadJsonFile(fixture_path);
  CHECK(!fixture.empty());

  TemporaryDirectory temporary_directory;
  CHECK(!temporary_directory.path().empty());
  const auto recording_root = temporary_directory.path() / "fixture_recording";
  const auto archive_root = recording_root / "zarr/fixture_analysis.zarr";
  const auto local_video = recording_root / "raw/stimulus.mp4";
  std::filesystem::create_directories(local_video.parent_path());
  {
    std::ofstream output(local_video);
    CHECK(output.good());
  }
  std::filesystem::create_directories(archive_root);
  CHECK(BuildFixtureArchive(fixture, archive_root));

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(
      archive_root, &error);
  CHECK(archive != nullptr);
  CHECK(archive->rootPath() == archive_root);
  auto repository =
      crimson::zarr::OpenStimulusRepository(archive, {}, &error);
  CHECK(repository != nullptr);
  CHECK(repository->runName() == fixture.at("run_name").get<std::string>());
  CHECK(repository->sourceVideoPath() ==
        fixture.at("source_stimulus_video_path").get<std::string>());
  CHECK(repository->resolvedSourceVideoPath() == local_video.string());
  CHECK(repository->cameraFrameCount() ==
        fixture.at("arrays").at("camera_to_metadata_index").size());
  CHECK(repository->cameraFrameOffset() ==
        fixture.at("camera_frame_offset").get<int64_t>());
  CHECK(repository->hasMapping());
  CHECK(repository->hasCorrectedMapping());

  CHECK(CheckExpectations(*repository, fixture.at("corrected_expectations"),
                          StimulusMappingPreference::PreferCorrected));
  CHECK(CheckExpectations(*repository, fixture.at("legacy_expectations"),
                          StimulusMappingPreference::LegacyOnly));

  CHECK(repository->cameraFrameForStimulus(
            fixture.at("reverse_corrected_stimulus").get<int32_t>()) ==
        fixture.at("reverse_corrected_camera").get<int32_t>());
  CHECK(repository->firstCameraFrameWithStimulus() ==
        fixture.at("first_corrected_camera").get<int32_t>());
  CHECK(repository->firstStimulusFrame() ==
        fixture.at("first_corrected_stimulus").get<int32_t>());
  CHECK(repository->firstCameraFrameWithStimulus(
            StimulusMappingPreference::LegacyOnly) ==
        fixture.at("first_legacy_camera").get<int32_t>());
  CHECK(repository->firstStimulusFrame(
            StimulusMappingPreference::LegacyOnly) ==
        fixture.at("first_legacy_stimulus").get<int32_t>());
  return true;
}

}  // namespace

int main() {
  if (!RunTest()) {
    return 1;
  }
  std::cout << "Stimulus repository fixture: PASS\n";
  return 0;
}
