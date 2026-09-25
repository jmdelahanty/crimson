#include "analysis_series_timeline.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_analysis_series_timeline_repository.h"

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
              ("crimson-analysis-series-timeline-" + std::to_string(seed) +
               "-" + std::to_string(attempt));
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

bool WriteFixture(const std::filesystem::path &root) {
  CHECK(WriteJson(root / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));

  const std::string motion_group = "analysis/track_kinematics_runs/offline";
  const std::string motion_run = motion_group + "/motion_fixture";
  const std::string track = motion_run + "/tracks/id_0";
  CHECK(WriteJson(root / motion_group / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest", "motion_fixture"}}}}));
  CHECK(WriteJson(root / motion_run / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"fps", 100.0}}}}));
  CHECK(WriteJson(root / track / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));
  CHECK((WriteArray<int64_t, 1>(root, motion_run + "/track_ids", "int64", {1},
                                {0})));
  const std::vector<int64_t> frames = {0,  1,  3,  4,  8,  9,
                                       10, 15, 18, 19, 25, 26};
  std::vector<float> times(frames.size());
  std::vector<float> filtered(frames.size());
  std::vector<float> smoothed(frames.size());
  std::vector<float> raw(frames.size());
  std::vector<float> averaged(frames.size());
  std::vector<float> heading(frames.size());
  std::vector<float> heading_smoothed(frames.size());
  std::vector<float> positions(frames.size() * 2);
  std::vector<uint8_t> heading_valid(frames.size(), 1);
  heading_valid[4] = 0;
  for (size_t row = 0; row < frames.size(); ++row) {
    times[row] = static_cast<float>(frames[row]) * 0.01f;
    filtered[row] = 10.0f + static_cast<float>(row);
    smoothed[row] = 20.0f + static_cast<float>(row);
    raw[row] = 30.0f + static_cast<float>(row);
    averaged[row] = 40.0f + static_cast<float>(row);
    heading[row] = 50.0f + static_cast<float>(row);
    heading_smoothed[row] = 60.0f + static_cast<float>(row);
    positions[row * 2] = 70.0f + static_cast<float>(row);
    positions[row * 2 + 1] = 80.0f + static_cast<float>(row);
  }
  CHECK((WriteArray<int64_t, 1>(root, track + "/frame_indices", "int64",
                                {static_cast<ts::Index>(frames.size())},
                                frames)));
  CHECK((WriteArray<float, 1>(root, track + "/time_seconds", "float32",
                              {static_cast<ts::Index>(frames.size())}, times)));
  CHECK((WriteArray<float, 1>(root, track + "/speed_filtered_mm", "float32",
                              {static_cast<ts::Index>(frames.size())},
                              filtered)));
  CHECK((WriteArray<float, 1>(root, track + "/speed_smoothed_mm", "float32",
                              {static_cast<ts::Index>(frames.size())},
                              smoothed)));
  CHECK((WriteArray<float, 1>(root, track + "/speed_raw_mm", "float32",
                              {static_cast<ts::Index>(frames.size())}, raw)));
  CHECK((WriteArray<float, 1>(root, track + "/speed_averaged_mm", "float32",
                              {static_cast<ts::Index>(frames.size())},
                              averaged)));
  CHECK(
      (WriteArray<float, 1>(root, track + "/heading_degrees", "float32",
                            {static_cast<ts::Index>(frames.size())}, heading)));
  CHECK((WriteArray<float, 1>(
      root, track + "/smoothed_heading_degrees", "float32",
      {static_cast<ts::Index>(frames.size())}, heading_smoothed)));
  CHECK((WriteArray<uint8_t, 1>(root, track + "/keypoint_success", "uint8",
                                {static_cast<ts::Index>(frames.size())},
                                heading_valid)));
  CHECK((WriteArray<float, 2>(root, track + "/positions_mm", "float32",
                              {static_cast<ts::Index>(frames.size()), 2},
                              positions)));

  const std::string tail_group = "analysis/tail_kinematics_runs";
  const std::string tail_run = tail_group + "/tail_fixture";
  CHECK(WriteJson(root / tail_group / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", "tail_fixture"}}}}));
  CHECK(WriteJson(root / tail_run / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"schema_id", "analysis.tail_kinematics_runs"},
                     {"schema_version", 1},
                     {"tail_angle_sample_count", 16}}}}));
  std::vector<int32_t> tail_frames(frames.begin(), frames.end());
  CHECK((WriteArray<int32_t, 1>(root, tail_run + "/frame_index", "int32",
                                {static_cast<ts::Index>(tail_frames.size())},
                                tail_frames)));
  const std::array<std::string, 4> tail_fields = {
      "tail_tip_angle_deg", "max_abs_tail_angle_deg",
      "tail_tip_lateral_deflection_px", "max_abs_tail_curvature_px_inv"};
  for (size_t field = 0; field < tail_fields.size(); ++field) {
    std::vector<float> values(frames.size());
    for (size_t row = 0; row < frames.size(); ++row) {
      values[row] = static_cast<float>(field * 100 + row);
    }
    CHECK((WriteArray<float, 1>(
        root, tail_run + "/" + tail_fields[field], "float32",
        {static_cast<ts::Index>(values.size())}, values)));
  }
  return true;
}

bool TestMotion(const std::filesystem::path &root) {
  using namespace crimson::timeline;
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenMotionSeriesTimelineRepository(archive, 30, &error);
  CHECK(repository != nullptr);
  const auto &descriptor = repository->descriptor();
  CHECK(descriptor.kind == AnalysisSeriesKind::Motion);
  CHECK(descriptor.frame_count == 30);
  CHECK(descriptor.sources.size() == 4);
  CHECK(descriptor.sources.front().variant == "filtered");
  CHECK(descriptor.sources.front().traces.size() == 6);
  const size_t row_count = descriptor.sources.front().sample_count;
  CHECK(defaultAnalysisSeriesSource(descriptor) ==
        descriptor.sources.front().key);
  CHECK(repository->metrics().frame_index_block_reads == 0);

  AnalysisSeriesTimelineRequest request;
  request.source_key = descriptor.default_source;
  request.first_frame = 3;
  request.last_frame = 19;
  request.anchor_frame = 10;
  request.max_points_per_trace = 20;
  request.fallback_frames_per_second = 100.0;
  const auto window = repository->resolveWindow(request);
  CHECK(window.ready());
  CHECK(window.source_row_count == 8);
  CHECK(window.traces.size() == 6);
  CHECK(window.traces.front().frames.front() == 3);
  CHECK(window.traces.front().frames.back() == 19);
  CHECK(window.traces.front().values.front() == 12.0);
  const auto heading = std::find_if(
      window.traces.begin(), window.traces.end(), [](const auto &trace) {
        return trace.descriptor.role == AnalysisSeriesTraceRole::HeadingRaw;
      });
  CHECK(heading != window.traces.end());
  CHECK(heading->frames.size() == 7);
  const auto first_metrics = repository->metrics();
  CHECK(first_metrics.frame_index_block_reads == 1);
  CHECK(first_metrics.frame_index_cache_hits > 0);
  CHECK(first_metrics.frame_index_source_bytes == row_count * sizeof(int64_t));
  CHECK(first_metrics.cached_frame_index_bytes >=
        row_count * sizeof(int64_t));
  CHECK(first_metrics.cached_frame_index_bytes <= 2ULL * 1024ULL * 1024ULL);
  CHECK(first_metrics.peak_cached_frame_index_bytes ==
        first_metrics.cached_frame_index_bytes);
  CHECK(first_metrics.maximum_frame_index_read_ms >= 0.0);
  CHECK(repository->resolveWindow(request).ready());
  const auto warm_metrics = repository->metrics();
  CHECK(warm_metrics.frame_index_block_reads ==
        first_metrics.frame_index_block_reads);
  CHECK(warm_metrics.frame_index_cache_hits >
        first_metrics.frame_index_cache_hits);

  auto preloaded = crimson::zarr::OpenMotionSeriesTimelineRepository(
      archive, 30, &error, {1024 * 1024});
  CHECK(preloaded != nullptr);
  const auto preload_open_metrics = preloaded->metrics();
  CHECK(preload_open_metrics.preload_candidate_bytes > 0);
  CHECK(preload_open_metrics.default_source_preloaded);
  CHECK(preload_open_metrics.preloaded_retained_bytes > 0);
  CHECK(preload_open_metrics.frame_index_block_reads == 0);
  CHECK(preloaded->resolveWindow(request).ready());
  const auto preload_resolve_metrics = preloaded->metrics();
  CHECK(preload_resolve_metrics.preloaded_window_resolves == 1);
  CHECK(preload_resolve_metrics.paged_window_resolves == 0);
  CHECK(preload_resolve_metrics.frame_index_block_reads == 0);
  return true;
}

bool TestTail(const std::filesystem::path &root) {
  using namespace crimson::timeline;
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository = crimson::zarr::OpenTailKinematicsTimelineRepository(
      archive, 30, {}, &error);
  CHECK(repository != nullptr);
  const auto &descriptor = repository->descriptor();
  CHECK(descriptor.kind == AnalysisSeriesKind::TailKinematics);
  CHECK(descriptor.default_source == "tail_fixture");
  CHECK(descriptor.sources.size() == 1);
  CHECK(descriptor.sources.front().traces.size() == 4);
  const size_t row_count = descriptor.sources.front().sample_count;
  CHECK(descriptor.sources.front().traces[0].default_visible);
  CHECK(!descriptor.sources.front().traces[3].default_visible);

  AnalysisSeriesTimelineRequest request{
      descriptor.default_source, 8, 25, 15, 6, 100.0};
  const auto window = repository->resolveWindow(request);
  CHECK(window.ready());
  CHECK(window.source_row_count == 7);
  CHECK(window.traces.size() == 4);
  for (const auto &trace : window.traces) {
    CHECK(trace.values.size() <= 6);
    CHECK(trace.frames.front() >= 8);
    CHECK(trace.frames.back() <= 25);
  }
  const auto metrics = repository->metrics();
  CHECK(metrics.frame_index_block_reads == 1);
  CHECK(metrics.frame_index_source_bytes == row_count * sizeof(int32_t));
  CHECK(metrics.cached_frame_index_bytes >= row_count * sizeof(int64_t));
  CHECK(metrics.cached_frame_index_bytes <= 2ULL * 1024ULL * 1024ULL);

  auto preloaded = crimson::zarr::OpenTailKinematicsTimelineRepository(
      archive, 30, {}, &error, {1024 * 1024});
  CHECK(preloaded != nullptr);
  CHECK(preloaded->metrics().default_source_preloaded);
  CHECK(preloaded->resolveWindow(request).ready());
  CHECK(preloaded->metrics().preloaded_window_resolves == 1);
  CHECK(preloaded->metrics().frame_index_block_reads == 0);
  return true;
}

} // namespace

int main() {
  TemporaryDirectory fixture;
  if (fixture.path().empty() || !WriteFixture(fixture.path()) ||
      !TestMotion(fixture.path()) || !TestTail(fixture.path())) {
    return 1;
  }
  std::cout << "analysis_series_timeline_repository_tests: PASS\n";
  return 0;
}
