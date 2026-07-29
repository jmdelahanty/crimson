#include "zarr/archive_context.h"
#include "zarr/tensorstore_analysis_crop_geometry_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <chrono>
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
              ("crimson-analysis-crop-" + std::to_string(seed) + "-" +
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

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

bool WriteJson(const std::filesystem::path &path, const json &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value.dump(2) << '\n';
  return output.good();
}

template <typename T, size_t Rank>
bool WriteArray(const std::filesystem::path &root, const std::string &path,
                const std::string &data_type,
                const std::array<ts::Index, Rank> &shape,
                const std::vector<T> &values) {
  size_t element_count = 1;
  json shape_json = json::array();
  json chunk_shape = json::array();
  for (const auto extent : shape) {
    element_count *= static_cast<size_t>(extent);
    shape_json.push_back(extent);
    chunk_shape.push_back(std::max<ts::Index>(1, extent));
  }
  if (element_count != values.size()) {
    return false;
  }
  json bytes_codec = {{"name", "bytes"}};
  if (sizeof(T) > 1) {
    bytes_codec["configuration"] = {{"endian", "little"}};
  }
  json metadata = {
      {"shape", shape_json},
      {"data_type", data_type},
      {"chunk_grid",
       {{"name", "regular"},
        {"configuration", {{"chunk_shape", chunk_shape}}}}},
      {"chunk_key_encoding",
       {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
      {"fill_value", 0},
      {"codecs", json::array({bytes_codec})},
  };
  json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata", std::move(metadata)},
  };
  auto store =
      ts::Open<T, Rank>(spec, ts::OpenMode::open | ts::OpenMode::create,
                        ts::ReadWriteMode::read_write)
          .result();
  if (!store.ok()) {
    std::cerr << "Failed to create " << path << ": " << store.status() << '\n';
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

template <typename T, size_t Rank>
bool CreateArrayMetadata(const std::filesystem::path &root,
                         const std::string &path, const std::string &data_type,
                         const std::array<ts::Index, Rank> &shape) {
  json shape_json = json::array();
  json chunk_shape = json::array();
  for (const auto extent : shape) {
    shape_json.push_back(extent);
    chunk_shape.push_back(std::max<ts::Index>(1, extent));
  }
  json bytes_codec = {{"name", "bytes"}};
  if (sizeof(T) > 1) {
    bytes_codec["configuration"] = {{"endian", "little"}};
  }
  json metadata = {
      {"shape", shape_json},
      {"data_type", data_type},
      {"chunk_grid",
       {{"name", "regular"},
        {"configuration", {{"chunk_shape", chunk_shape}}}}},
      {"chunk_key_encoding",
       {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
      {"fill_value", 0},
      {"codecs", json::array({bytes_codec})},
  };
  json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata", std::move(metadata)},
  };
  return ts::Open<T, Rank>(spec, ts::OpenMode::open | ts::OpenMode::create,
                           ts::ReadWriteMode::read_write)
      .result()
      .ok();
}

bool BuildFixture(const std::filesystem::path &root) {
  constexpr const char *run = "crop_geometry_fixture";
  const std::string base = std::string("crop_runs/") + run;
  CHECK(WriteJson(root / "crop_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", run}}}}));
  CHECK(WriteJson(root / base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"roi_size", {48, 64}}}}}));
  CHECK((WriteArray<int32_t, 1>(root, base + "/frame_indices", "int32", {4},
                                {1, 2, 2, 4})));
  CHECK((WriteArray<int32_t, 2>(root, base + "/roi_coordinates_full", "int32",
                                {4, 2}, {8, 10, 16, 12, 24, 18, 80, 50})));
  CHECK((WriteArray<float, 2>(
      root, base + "/bbox_norm_coords", "float32", {4, 4},
      {0.25f, 0.25f, 0.20f, 0.20f, 0.35f, 0.35f, 0.20f, 0.20f, 0.45f, 0.45f,
       0.20f, 0.20f, 0.85f, 0.75f, 0.10f, 0.10f})));

  const std::string legacy_base = "crop_runs/legacy_metadata_fixture";
  CHECK(WriteJson(root / legacy_base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", json::object()}}));
  CHECK((WriteArray<int32_t, 1>(root, legacy_base + "/frame_indices", "int32",
                                {1}, {0})));
  CHECK((WriteArray<int32_t, 2>(root, legacy_base + "/roi_coordinates_full",
                                "int32", {1, 2}, {4, 6})));
  CHECK((CreateArrayMetadata<uint8_t, 3>(root, legacy_base + "/roi_images",
                                         "uint8", {1, 32, 40})));
  return true;
}

bool BuildLineagePointers(const std::filesystem::path &root,
                          const std::string &mask_crop_run,
                          const std::string &keypoint_crop_run) {
  CHECK(WriteJson(root / "crop_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", json::object()}}));
  CHECK(WriteJson(root / "refined_subject_masks_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", "mask_fixture"}}}}));
  CHECK(WriteJson(root / "refined_subject_masks_runs/mask_fixture/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"source_crop_run", mask_crop_run}}}}));
  CHECK(WriteJson(root / "refined_keypoints_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", "keypoint_fixture"}}}}));
  CHECK(WriteJson(root / "refined_keypoints_runs/keypoint_fixture/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"source_crop_run", keypoint_crop_run}}}}));
  return true;
}

bool RunTest() {
  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto archive_root = temporary.path() / "recording/zarr/analysis.zarr";
  std::filesystem::create_directories(archive_root);
  CHECK(BuildFixture(archive_root));

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(archive_root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenAnalysisCropGeometryRepository(archive, {}, &error);
  CHECK(repository != nullptr);
  CHECK(repository->descriptor().run_name == "crop_geometry_fixture");
  CHECK(repository->descriptor().output_width == 64);
  CHECK(repository->descriptor().output_height == 48);
  CHECK(repository->descriptor().row_count == 4);
  CHECK(repository->descriptor().camera_frame_count == 5);
  CHECK(repository->sourceCapabilities().live_geometry);
  CHECK(!repository->sourceCapabilities().acquisition_video);
  CHECK(!repository->sourceCapabilities().persisted_zarr);
  const auto memory = repository->memoryMetrics();
  CHECK(memory.retained_payload_bytes > 0);
  CHECK(memory.retained_index_bytes > 0);

  const auto missing = repository->resolveCameraFrame(0, 128, 96);
  CHECK(missing.status == crimson::zarr::AnalysisCropGeometryStatus::Missing);
  CHECK(!missing.geometry);

  const auto first = repository->resolveCameraFrame(1, 128, 96);
  CHECK(first.status == crimson::zarr::AnalysisCropGeometryStatus::Mapped);
  CHECK(first.roi_index == 0);
  CHECK(first.frame_row_count == 1);
  CHECK(first.geometry.has_value());
  CHECK(first.geometry->usableForLiveCrop());
  CHECK(first.geometry->full_frame_crop.x == 8.0);
  CHECK(first.geometry->full_frame_crop.y == 10.0);
  CHECK(first.geometry->output_width == 64);
  CHECK(first.geometry->output_height == 48);
  CHECK(first.geometry->full_frame_detection.has_value());

  const auto duplicate = repository->resolveCameraFrame(2, 128, 96);
  CHECK(duplicate.status == crimson::zarr::AnalysisCropGeometryStatus::Mapped);
  CHECK(duplicate.roi_index == 1);
  CHECK(duplicate.frame_row_count == 2);
  CHECK(duplicate.geometry->full_frame_crop.x == 16.0);

  CHECK(repository->resolveCameraFrame(3, 128, 96).status ==
        crimson::zarr::AnalysisCropGeometryStatus::Missing);
  const auto escaped = repository->resolveCameraFrame(4, 128, 96);
  CHECK(escaped.geometry.has_value());
  CHECK(!escaped.geometry->valid());
  CHECK(repository->resolveCameraFrame(5, 128, 96).status ==
        crimson::zarr::AnalysisCropGeometryStatus::OutOfRange);

  auto explicit_repository = crimson::zarr::OpenAnalysisCropGeometryRepository(
      archive, "crop_runs/crop_geometry_fixture", &error);
  CHECK(explicit_repository != nullptr);
  auto invalid_explicit_repository =
      crimson::zarr::OpenAnalysisCropGeometryRepository(
          archive, "crop_runs/nested/run", &error);
  CHECK(invalid_explicit_repository == nullptr);

  auto legacy_repository = crimson::zarr::OpenAnalysisCropGeometryRepository(
      archive, "legacy_metadata_fixture", &error);
  CHECK(legacy_repository != nullptr);
  CHECK(legacy_repository->descriptor().output_width == 40);
  CHECK(legacy_repository->descriptor().output_height == 32);
  CHECK(!std::filesystem::exists(
      archive_root / "crop_runs/legacy_metadata_fixture/roi_images/c"));

  const auto mask_lineage_root =
      temporary.path() / "mask-lineage/zarr/analysis.zarr";
  std::filesystem::create_directories(mask_lineage_root);
  CHECK(BuildFixture(mask_lineage_root));
  CHECK(BuildLineagePointers(mask_lineage_root, "crop_geometry_fixture",
                             "legacy_metadata_fixture"));
  auto mask_lineage_archive =
      crimson::zarr::ArchiveContext::Open(mask_lineage_root, &error);
  CHECK(mask_lineage_archive != nullptr);
  auto mask_lineage_repository =
      crimson::zarr::OpenAnalysisCropGeometryRepository(mask_lineage_archive,
                                                        {}, &error);
  CHECK(mask_lineage_repository != nullptr);
  CHECK(mask_lineage_repository->descriptor().run_name ==
        "crop_geometry_fixture");

  const auto keypoint_lineage_root =
      temporary.path() / "keypoint-lineage/zarr/analysis.zarr";
  std::filesystem::create_directories(keypoint_lineage_root);
  CHECK(BuildFixture(keypoint_lineage_root));
  CHECK(BuildLineagePointers(keypoint_lineage_root, "invalid/nested/run",
                             "crop_runs/legacy_metadata_fixture"));
  auto keypoint_lineage_archive =
      crimson::zarr::ArchiveContext::Open(keypoint_lineage_root, &error);
  CHECK(keypoint_lineage_archive != nullptr);
  auto keypoint_lineage_repository =
      crimson::zarr::OpenAnalysisCropGeometryRepository(
          keypoint_lineage_archive, {}, &error);
  CHECK(keypoint_lineage_repository != nullptr);
  CHECK(keypoint_lineage_repository->descriptor().run_name ==
        "legacy_metadata_fixture");

  std::cout << "analysis_crop_geometry_repository_tests: PASS\n";
  return true;
}

} // namespace

int main() { return RunTest() ? 0 : 1; }
