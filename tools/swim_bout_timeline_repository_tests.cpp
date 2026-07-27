#include "swim_bout_timeline.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_swim_bout_timeline_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-swim-bout-timeline-" + std::to_string(seed) + "-" +
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
        {"fill_value", 0},
        {"codecs", json::array({bytes})}}}};
  auto store =
      ts::Open<T, Rank>(spec, ts::OpenMode::open | ts::OpenMode::create,
                        ts::ReadWriteMode::read_write)
          .result();
  if (!store.ok()) {
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

std::vector<uint8_t> FixedStrings(const std::vector<std::string> &values,
                                  size_t width) {
  std::vector<uint8_t> result(values.size() * width, 0);
  for (size_t row = 0; row < values.size(); ++row) {
    std::copy_n(values[row].begin(), std::min(width, values[row].size()),
                result.begin() + static_cast<std::ptrdiff_t>(row * width));
  }
  return result;
}

bool WriteCompact(const std::filesystem::path &root, const std::string &group) {
  const std::string run = group + "/compact_fixture";
  CHECK(WriteJson(root / run / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"layout", "compact_tabular_v2"},
                     {"schema_id", "palette.swim_bout_runs"},
                     {"schema_version", 7},
                     {"default_candidate_id", 0},
                     {"default_signal_id", 4},
                     {"default_level", "speed_exponential"},
                     {"source_track_kinematics_run", "motion_fixture"},
                     {"track_id", 0},
                     {"detection_method", "peak_event"},
                     {"min_peak_prominence_mm_s", 4.0},
                     {"peak_width_rel_height", 0.98}}}}));
  const std::string candidates = run + "/indexes/candidates/";
  CHECK((WriteArray<int32_t, 1>(root, candidates + "candidate_id", "int32", {2},
                                {0, 1})));
  CHECK((WriteArray<uint8_t, 1>(root, candidates + "is_default", "uint8", {2},
                                {1, 0})));
  const auto methods = FixedStrings({"peak_event", "threshold"}, 16);
  CHECK((WriteArray<uint8_t, 2>(root, candidates + "detection_method", "uint8",
                                {2, 16}, methods)));
  CHECK((WriteArray<float, 1>(root, candidates + "min_bout_duration_s",
                              "float32", {2}, {0.05f, 0.1f})));
  CHECK((WriteArray<double, 1>(root, candidates + "min_gap_duration_s",
                               "float64", {2}, {0.1, 0.2})));

  const std::string signals = run + "/indexes/signal_variants/";
  CHECK((WriteArray<int32_t, 1>(root, signals + "signal_id", "int32", {3},
                                {1, 4, 9})));
  CHECK((WriteArray<int16_t, 1>(root, signals + "candidate_id", "int16", {3},
                                {0, 0, 1})));
  for (const auto &item :
       std::vector<std::pair<std::string, std::vector<std::string>>>{
           {"speed_level",
            {"speed_filtered", "speed_exponential", "speed_raw"}},
           {"role", {"physical", "detector", "unused"}},
           {"signal_name", {"filtered", "exponential", "raw"}},
           {"source_level", {"speed_filtered", "speed_filtered", "speed_raw"}},
           {"path_distance_source_level",
            {"speed_filtered", "speed_filtered", "speed_raw"}},
           {"transform_type", {"identity", "exponential", "identity"}},
           {"units", {"mm/s", "mm/s", "mm/s"}}}) {
    const auto bytes = FixedStrings(item.second, 24);
    CHECK((WriteArray<uint8_t, 2>(root, signals + item.first, "uint8", {3, 24},
                                  bytes)));
  }
  CHECK((WriteArray<float, 1>(root, signals + "tau_s", "float32", {3},
                              {0.0f, 0.025f, 0.0f})));

  const std::string bouts = run + "/tables/bouts/";
  CHECK((WriteArray<int32_t, 1>(root, bouts + "candidate_id", "int32", {4},
                                {0, 0, 0, 1})));
  CHECK((WriteArray<int32_t, 1>(root, bouts + "signal_id", "int32", {4},
                                {4, 1, 4, 9})));
  CHECK((WriteArray<int64_t, 1>(root, bouts + "start_frame", "int64", {4},
                                {30, 5, 60, 1})));
  CHECK((WriteArray<int32_t, 1>(root, bouts + "end_frame", "int32", {4},
                                {45, 15, 80, 3})));
  CHECK((WriteArray<int32_t, 1>(root, bouts + "core_start_frame", "int32", {4},
                                {35, 7, 65, 1})));
  CHECK((WriteArray<int64_t, 1>(root, bouts + "core_end_frame", "int64", {4},
                                {40, 12, 75, 2})));
  CHECK((WriteArray<uint8_t, 1>(root, bouts + "gap_censored", "uint8", {4},
                                {0, 1, 0, 0})));

  CHECK((WriteArray<int32_t, 1>(
      root, run + "/signals/detector_signal_signal_ids", "int32", {1}, {4})));
  CHECK((WriteArray<int64_t, 1>(root, run + "/signals/frame_indices", "int64",
                                {8}, {0, 2, 4, 6, 8, 10, 12, 14})));
  CHECK((WriteArray<float, 2>(
      root, run + "/signals/detector_signal_mm_s", "float32", {1, 8},
      {0.0f, 1.0f, 2.0f, 8.0f, 3.0f, 2.0f, 1.0f, 0.0f})));
  return true;
}

bool WriteLegacy(const std::filesystem::path &root, const std::string &group) {
  const std::string run = group + "/legacy_fixture";
  CHECK(WriteJson(root / run / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"default_level", "speed_exponential"},
                     {"source_track_kinematics_run", "motion_fixture"},
                     {"track_id", 0},
                     {"detection_method", "threshold"},
                     {"exponential_source_level", "speed_filtered"},
                     {"exponential_tau_s", 0.05}}}}));
  for (const auto &level : {"speed_filtered", "speed_exponential"}) {
    const std::string base = run + "/" + level + "/";
    CHECK(WriteJson(
        root / base / "zarr.json",
        {{"zarr_format", 3},
         {"node_type", "group"},
         {"attributes",
          {{"source_speed_level", "speed_filtered"},
           {"is_default_level", std::string(level) == "speed_exponential"}}}}));
    CHECK((WriteArray<int32_t, 1>(root, base + "bouts/start_frame", "int32",
                                  {2}, {10, 50})));
    CHECK((WriteArray<int32_t, 1>(root, base + "bouts/end_frame", "int32", {2},
                                  {20, 70})));
    CHECK((WriteArray<int32_t, 1>(root, base + "bouts/core_start_frame",
                                  "int32", {2}, {12, 55})));
    CHECK((WriteArray<int32_t, 1>(root, base + "bouts/core_end_frame", "int32",
                                  {2}, {18, 65})));
    if (std::string(level) == "speed_exponential") {
      CHECK((WriteArray<double, 1>(root, base + "detection_signal_mm_s",
                                   "float64", {6},
                                   {0.0, 1.0, 4.0, 2.0, 1.0, 0.0})));
    }
  }
  return true;
}

bool WriteFixture(const std::filesystem::path &root) {
  CHECK(WriteJson(root / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));
  const std::string group = "analysis/swim_bout_runs";
  CHECK(WriteJson(root / group / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"latest_success", "legacy_fixture"},
                     {"latest_complete", "compact_fixture"},
                     {"latest", "compact_fixture"}}}}));
  return WriteCompact(root, group) && WriteLegacy(root, group);
}

const crimson::timeline::SwimBoutCandidateDescriptor *
FindLevel(const crimson::timeline::SwimBoutTimelineDescriptor &descriptor,
          const std::string &run, const std::string &level) {
  const auto found = std::find_if(
      descriptor.candidates.begin(), descriptor.candidates.end(),
      [&](const auto &candidate) {
        return candidate.run_name == run && candidate.speed_level == level;
      });
  return found == descriptor.candidates.end() ? nullptr : &*found;
}

bool TestCompactAndLegacy(const std::filesystem::path &root) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenSwimBoutTimelineRepository(archive, 100, {}, &error);
  CHECK(repository != nullptr);
  const auto &descriptor = repository->descriptor();
  CHECK(descriptor.frame_count == 100);
  CHECK(descriptor.candidates.size() == 4);
  const auto *filtered =
      FindLevel(descriptor, "compact_fixture", "speed_filtered");
  const auto *exponential =
      FindLevel(descriptor, "compact_fixture", "speed_exponential");
  const auto *legacy =
      FindLevel(descriptor, "legacy_fixture", "speed_exponential");
  CHECK(filtered != nullptr);
  CHECK(filtered->bout_count == 1);
  CHECK(exponential != nullptr);
  CHECK(exponential->candidate_id == 0);
  CHECK(exponential->signal_id == 4);
  CHECK(exponential->bout_count == 2);
  CHECK(exponential->detector_sample_count == 8);
  CHECK(exponential->latest_run);
  CHECK(exponential->default_level);
  CHECK(legacy != nullptr);
  CHECK(!legacy->compact_layout);
  CHECK(legacy->has_detector_trace);
  CHECK(descriptor.default_candidate == exponential->key);

  crimson::timeline::SwimBoutTimelineRequest request;
  request.candidate_key = exponential->key;
  request.first_frame = 0;
  request.last_frame = 90;
  request.anchor_frame = 6;
  request.max_detector_points = 6;
  request.fallback_frames_per_second = 100.0;
  const auto compact_window = repository->resolveWindow(request);
  CHECK(compact_window.ready());
  CHECK(compact_window.intervals.size() == 2);
  CHECK(compact_window.intervals.front().start_frame == 30);
  CHECK(compact_window.detector_values.size() >= 3);
  CHECK(compact_window.detector_values.size() <= 6);
  CHECK(std::find(compact_window.detector_values.begin(),
                  compact_window.detector_values.end(),
                  8.0) != compact_window.detector_values.end());

  request.candidate_key = legacy->key;
  const auto legacy_window = repository->resolveWindow(request);
  CHECK(legacy_window.ready());
  CHECK(legacy_window.intervals.size() == 2);
  CHECK(legacy_window.detector_frames.size() == 6);
  CHECK(legacy_window.detector_frames.front() == 0);
  CHECK(legacy_window.detector_frames.back() == 5);
  return true;
}

bool TestRequestedRunAndFailure(const std::filesystem::path &root) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository = crimson::zarr::OpenSwimBoutTimelineRepository(
      archive, 100, "legacy_fixture", &error);
  CHECK(repository != nullptr);
  CHECK(repository->descriptor().candidates.size() == 2);
  CHECK(crimson::zarr::OpenSwimBoutTimelineRepository(archive, 100, "missing",
                                                      &error) == nullptr);
  CHECK(!error.empty());
  return true;
}

} // namespace

int main() {
  TemporaryDirectory fixture;
  if (fixture.path().empty() || !WriteFixture(fixture.path()) ||
      !TestCompactAndLegacy(fixture.path()) ||
      !TestRequestedRunAndFailure(fixture.path())) {
    return 1;
  }
  std::cout << "swim_bout_timeline_repository_tests: PASS\n";
  return 0;
}
