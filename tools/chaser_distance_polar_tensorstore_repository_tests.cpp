#include "chaser_distance_polar.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_chaser_distance_polar_repository.h"

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
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;
using crimson::polar::ChaserDistancePolarAvailability;
using crimson::polar::ChaserDistancePolarColorProvenance;
using crimson::polar::ChaserDistancePolarRadialScaleProvenance;
using crimson::polar::ChaserDistancePolarSelectionProvenance;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__          \
                << ": " #condition << '\n';                                   \
      return false;                                                            \
    }                                                                          \
  } while (false)

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(const std::string& label) {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-polar-tensorstore-" + label + "-" +
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

bool writeGroup(const std::filesystem::path& root,
                const std::string& path,
                const json& attributes = json::object()) {
  json metadata = {{"zarr_format", 3}, {"node_type", "group"}};
  if (!attributes.empty()) {
    metadata["attributes"] = attributes;
  }
  return writeJson(root / path / "zarr.json", metadata);
}

template <typename T, size_t Rank>
bool writeArray(const std::filesystem::path& root,
                const std::string& path,
                const std::string& data_type,
                const std::array<ts::Index, Rank>& shape,
                const std::vector<T>& values) {
  size_t count = 1;
  json shape_json = json::array();
  json chunk_json = json::array();
  for (const auto extent : shape) {
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
  auto store = ts::Open<T, Rank>(
                   spec, ts::OpenMode::open | ts::OpenMode::create,
                   ts::ReadWriteMode::read_write)
                   .result();
  if (!store.ok()) {
    std::cerr << "create failed for " << path << ": " << store.status()
              << '\n';
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

bool writeRoot(const std::filesystem::path& root) {
  return writeGroup(root, "", {{"total_frames", 100},
                                {"video_width", 640},
                                {"video_height", 360},
                                {"fps", 100.0}});
}

bool writeProtocol(const std::filesystem::path& root) {
  const json protocol = {
      {"chasers",
       json::array({{{"chaser_index", 0},
                     {"color_r", 0.25},
                     {"color_g", 0.5},
                     {"color_b", 0.75},
                     {"color_a", 1.0}}})}};
  return writeGroup(root, "analysis/stimulus_runs",
                    {{"latest", "stimulus_fixture"}}) &&
         writeGroup(root, "analysis/stimulus_runs/stimulus_fixture",
                    {{"protocol_json", protocol.dump()}});
}

bool writeComponent(const std::filesystem::path& root,
                    const std::string& run_name,
                    const std::string& component_name,
                    const std::vector<int64_t>& frames,
                    const std::vector<int32_t>& chasers,
                    const std::vector<float>& bearings,
                    const std::vector<float>& distances,
                    const std::vector<bool>& valid,
                    const json& attributes) {
  const auto rows = frames.size();
  const auto columns = chasers.size();
  if (rows * columns != bearings.size() ||
      rows * columns != distances.size() ||
      rows * columns != valid.size()) {
    return false;
  }
  const std::string base = "analysis/chaser_distance_runs/" + run_name +
                           "/egocentric_bearing/" + component_name;
  std::vector<uint8_t> stored_chasers;
  stored_chasers.reserve(chasers.size());
  std::transform(chasers.begin(), chasers.end(),
                 std::back_inserter(stored_chasers),
                 [](int32_t value) { return static_cast<uint8_t>(value); });
  return writeGroup(root, base, attributes) &&
         writeArray<int64_t, 1>(root, base + "/frames/camera_frame_id",
                                "int64", {static_cast<ts::Index>(rows)},
                                frames) &&
         writeArray<uint8_t, 1>(root, base + "/per_chaser/chaser_index",
                                "uint8", {static_cast<ts::Index>(columns)},
                                stored_chasers) &&
         writeArray<float, 2>(root, base + "/per_chaser/bearing_deg",
                              "float32",
                              {static_cast<ts::Index>(rows),
                               static_cast<ts::Index>(columns)},
                              bearings) &&
         writeArray<float, 2>(root, base + "/per_chaser/distance_mm",
                              "float32",
                              {static_cast<ts::Index>(rows),
                               static_cast<ts::Index>(columns)},
                              distances) &&
         writeArray<bool, 2>(root, base + "/per_chaser/valid", "bool",
                             {static_cast<ts::Index>(rows),
                              static_cast<ts::Index>(columns)},
                             valid);
}

bool writeSelectedFixture(const std::filesystem::path& root,
                          std::string coordinate_frame =
                              std::string(crimson::polar::
                                              kArenaRelativeCanvasPixelFrame)) {
  CHECK(writeRoot(root));
  CHECK(writeProtocol(root));
  CHECK(writeGroup(root, "analysis/chaser_distance_runs",
                   {{"latest_complete", "run_selected"},
                    {"latest", "run_ignored"}}));
  CHECK(writeGroup(root, "analysis/chaser_distance_runs/run_selected",
                   {{"coordinate_frame", coordinate_frame}}));
  CHECK(writeGroup(
      root,
      "analysis/chaser_distance_runs/run_selected/egocentric_bearing",
      {{"latest_completed", "component_selected"},
       {"latest", "component_ignored"}}));

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<int64_t> frames = {10, 20, 20, 30, 40};
  const std::vector<int32_t> chasers = {0, 1, 2};
  const std::vector<float> bearings = {
      0.0f, 90.0f, 45.0f, 180.0f, -45.0f, 30.0f, 10.0f, 20.0f,
      30.0f, 0.0f, 0.0f, -90.0f, 0.0f, 0.0f, 0.0f};
  const std::vector<float> distances = {
      10.0f, 20.0f, 999.0f, 30.0f, 60.0f, nan, 50.0f, 55.0f,
      58.0f, 0.0f, 0.0f, 40.0f, 70.0f, 80.0f, 90.0f};
  const std::vector<bool> valid = {
      true, true, false, false, true, true, true, true,
      true, false, false, true, false, false, false};
  return writeComponent(
      root, "run_selected", "component_selected", frames, chasers, bearings,
      distances, valid,
      {{"parameters",
        {{"angle_convention",
          std::string(crimson::polar::
                          kPositiveAnatomicalLeftAngleConvention)}}},
       {"summary",
        {{"chaser_color_hex", {{"0", "#ff0000"}, {"1", "#00ff00"}}}}}});
}

bool writeDiscoveryMarker(const std::filesystem::path& root,
                          const std::string& run_name) {
  const std::string base =
      "analysis/chaser_distance_runs/" + run_name;
  return writeGroup(root, base) &&
         writeArray<int64_t, 1>(root, base + "/frames/camera_frame_id",
                                "int64", {1}, {5}) &&
         writeArray<float, 1>(root, base + "/distances/distance_mm",
                              "float32", {1}, {1.0f});
}

bool writeFallbackFixture(const std::filesystem::path& root) {
  CHECK(writeRoot(root));
  CHECK(writeGroup(root, "analysis/chaser_distance_runs"));
  CHECK(writeDiscoveryMarker(root, "run_a"));
  CHECK(writeDiscoveryMarker(root, "run_z"));
  CHECK(writeGroup(root, "analysis/chaser_distance_runs/run_z",
                   {{"coordinate_frame",
                     std::string(crimson::polar::
                                     kArenaRelativeCanvasPixelFrame)}}));
  CHECK(writeGroup(
      root, "analysis/chaser_distance_runs/run_z/egocentric_bearing"));
  const json attributes = {
      {"angle_convention",
       std::string(crimson::polar::
                       kPositiveAnatomicalLeftAngleConvention)}};
  CHECK(writeComponent(root, "run_z", "component_a", {5}, {7}, {12.0f},
                       {34.0f}, {true}, attributes));
  return writeComponent(root, "run_z", "component_z", {5}, {7}, {12.0f},
                        {34.0f}, {true}, attributes);
}

struct FileState {
  std::string contents;
  std::filesystem::file_time_type modified;

  bool operator==(const FileState& other) const {
    return contents == other.contents && modified == other.modified;
  }
};

using TreeSnapshot = std::map<std::string, FileState>;

TreeSnapshot snapshotTree(const std::filesystem::path& root) {
  TreeSnapshot result;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file()) {
      continue;
    }
    std::ifstream input(iterator->path(), std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    result[std::filesystem::relative(iterator->path(), root).generic_string()] =
        {contents.str(), iterator->last_write_time()};
  }
  return result;
}

bool near(double actual, double expected, double tolerance = 1e-6) {
  return std::abs(actual - expected) <= tolerance;
}

bool testSelectedFixture() {
  TemporaryDirectory fixture("selected");
  CHECK(!fixture.path().empty());
  CHECK(writeSelectedFixture(fixture.path()));
  const auto before = snapshotTree(fixture.path());

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(fixture.path(), &error);
  CHECK(archive != nullptr);
  crimson::zarr::TensorStoreChaserDistancePolarOptions options;
  options.radial_scan_block_rows = 2;
  auto repository =
      crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
          archive, options, &error);
  CHECK(repository != nullptr);
  CHECK(error.empty());

  const auto& descriptor = repository->descriptor();
  CHECK(descriptor.ready());
  CHECK(descriptor.provenance.run_name == "run_selected");
  CHECK(descriptor.provenance.component_name == "component_selected");
  CHECK(descriptor.provenance.run_selection ==
        ChaserDistancePolarSelectionProvenance::LatestComplete);
  CHECK(descriptor.provenance.component_selection ==
        ChaserDistancePolarSelectionProvenance::LatestCompleted);
  CHECK(descriptor.coordinate_frame ==
        crimson::polar::kArenaRelativeCanvasPixelFrame);
  CHECK(descriptor.angle_convention ==
        crimson::polar::kPositiveAnatomicalLeftAngleConvention);
  CHECK(descriptor.row_count == 5);
  CHECK(descriptor.chaser_count == 3);
  CHECK(near(descriptor.dataset_global_max_distance_mm, 60.0));
  CHECK(descriptor.radial_scale.provenance ==
        ChaserDistancePolarRadialScaleProvenance::DatasetGlobalValidMaximum);
  CHECK(near(descriptor.radial_scale.display_max_distance_mm, 63.0));

  auto metrics = repository->metrics();
  CHECK(metrics.frame_index_rows_read == 5);
  CHECK(metrics.chaser_index_rows_read == 3);
  CHECK(metrics.radial_scan_rows == 5);
  CHECK(metrics.exact_frame_rows_read == 0);
  CHECK(metrics.matrix_read_operations == 6);
  CHECK(metrics.maximum_rows_per_matrix_read == 2);
  CHECK(metrics.resident_frame_ids == 5);
  CHECK(metrics.resident_chaser_ids == 3);

  const auto frame10 = repository->resolveCameraFrame(10);
  CHECK(frame10.availability == ChaserDistancePolarAvailability::Ready);
  CHECK(frame10.exactFrame());
  CHECK(frame10.source_point_count == 3);
  CHECK(frame10.discarded_point_count == 1);
  CHECK(frame10.points.size() == 2);
  CHECK(frame10.points[0].chaser_index == 0);
  CHECK(frame10.points[0].color.provenance ==
        ChaserDistancePolarColorProvenance::StimulusProtocol);
  CHECK(near(frame10.points[0].color.rgba.red, 0.25));
  CHECK(frame10.points[1].chaser_index == 1);
  CHECK(frame10.points[1].color.provenance ==
        ChaserDistancePolarColorProvenance::ComponentSummary);

  const auto frame20 = repository->resolveCameraFrame(20);
  CHECK(frame20.points.size() == 1);
  CHECK(frame20.points[0].chaser_index == 1);
  CHECK(near(frame20.points[0].distance_mm, 60.0));
  CHECK(near(frame20.points[0].bearing_degrees, -45.0));
  const auto frame30 = repository->resolveCameraFrame(30);
  CHECK(frame30.points.size() == 1);
  CHECK(frame30.points[0].chaser_index == 2);
  CHECK(frame30.points[0].color.provenance ==
        ChaserDistancePolarColorProvenance::FixedFallbackPalette);
  const auto missing = repository->resolveCameraFrame(15);
  CHECK(missing.availability ==
        ChaserDistancePolarAvailability::ExactFrameMissing);
  CHECK(!missing.source_camera_frame.has_value());
  const auto empty = repository->resolveCameraFrame(40);
  CHECK(empty.availability == ChaserDistancePolarAvailability::ValidFrameEmpty);
  CHECK(empty.exactFrame());
  CHECK(empty.source_point_count == 3);
  CHECK(empty.discarded_point_count == 3);

  metrics = repository->metrics();
  CHECK(metrics.exact_frame_rows_read == 4);
  CHECK(metrics.matrix_read_operations == 18);
  CHECK(metrics.maximum_rows_per_matrix_read == 2);
  CHECK(snapshotTree(fixture.path()) == before);
  return true;
}

bool testFallbackAndRequestedSelection() {
  TemporaryDirectory fixture("fallback");
  CHECK(!fixture.path().empty());
  CHECK(writeFallbackFixture(fixture.path()));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(fixture.path(), &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
          archive, {}, &error);
  CHECK(repository != nullptr);
  CHECK(repository->descriptor().ready());
  CHECK(repository->descriptor().provenance.run_name == "run_z");
  CHECK(repository->descriptor().provenance.component_name == "component_z");
  CHECK(repository->descriptor().provenance.run_selection ==
        ChaserDistancePolarSelectionProvenance::
            LexicographicCompatibilityFallback);
  CHECK(repository->descriptor().provenance.component_selection ==
        ChaserDistancePolarSelectionProvenance::
            LexicographicCompatibilityFallback);
  const auto frame = repository->resolveCameraFrame(5);
  CHECK(frame.points.size() == 1);
  CHECK(frame.points[0].chaser_index == 7);

  crimson::zarr::TensorStoreChaserDistancePolarOptions requested;
  requested.requested_run = "run_z";
  requested.requested_component = "component_a";
  auto exact = crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
      archive, requested, &error);
  CHECK(exact != nullptr);
  CHECK(exact->descriptor().ready());
  CHECK(exact->descriptor().provenance.component_name == "component_a");
  CHECK(exact->descriptor().provenance.run_selection ==
        ChaserDistancePolarSelectionProvenance::Requested);
  CHECK(exact->descriptor().provenance.component_selection ==
        ChaserDistancePolarSelectionProvenance::Requested);

  requested.requested_run = "../unsafe";
  auto unsafe = crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
      archive, requested, &error);
  CHECK(unsafe != nullptr);
  CHECK(unsafe->descriptor().availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);
  return true;
}

bool testUnavailableUnsupportedAndOptions() {
  TemporaryDirectory unavailable("unavailable");
  CHECK(!unavailable.path().empty());
  CHECK(writeRoot(unavailable.path()));
  std::string error;
  auto archive =
      crimson::zarr::ArchiveContext::Open(unavailable.path(), &error);
  CHECK(archive != nullptr);
  auto missing = crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
      archive, {}, &error);
  CHECK(missing != nullptr);
  CHECK(missing->descriptor().availability ==
        ChaserDistancePolarAvailability::DatasetUnavailable);
  CHECK(missing->resolveCameraFrame(10).availability ==
        ChaserDistancePolarAvailability::DatasetUnavailable);

  crimson::zarr::TensorStoreChaserDistancePolarOptions invalid_options;
  invalid_options.radial_scan_block_rows = 0;
  CHECK(crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
            archive, invalid_options, &error) == nullptr);
  CHECK(!error.empty());
  CHECK(crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
            nullptr, {}, &error) == nullptr);
  CHECK(!error.empty());

  TemporaryDirectory unsupported("unsupported");
  CHECK(!unsupported.path().empty());
  CHECK(writeSelectedFixture(unsupported.path(), "unsupported_coordinates"));
  archive = crimson::zarr::ArchiveContext::Open(unsupported.path(), &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
          archive, {}, &error);
  CHECK(repository != nullptr);
  CHECK(repository->descriptor().availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);
  CHECK(!repository->descriptor().error.empty());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
    const std::filesystem::path output = argv[2];
    std::error_code error;
    std::filesystem::remove_all(output, error);
    if (!writeSelectedFixture(output)) {
      std::filesystem::remove_all(output, error);
      std::cerr << "failed to write polar fixture: " << output << '\n';
      return 1;
    }
    std::cout << "chaser_distance_polar_tensorstore_repository_tests: "
              << "fixture=" << output << '\n';
    return 0;
  }
  if (argc != 1) {
    std::cerr << "usage: chaser_distance_polar_tensorstore_repository_tests "
              << "[--write-fixture PATH]\n";
    return 2;
  }
  if (!testSelectedFixture() || !testFallbackAndRequestedSelection() ||
      !testUnavailableUnsupportedAndOptions()) {
    return 1;
  }
  std::cout << "chaser_distance_polar_tensorstore_repository_tests: PASS\n";
  return 0;
}
