#include "keypoint_overlay_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/keypoint_overlay_repository.h"
#include "zarr/keypoint_overlay_scene_adapter.h"
#include "zarr/tensorstore_keypoint_overlay_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
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

bool Near(double actual, double expected) {
  return std::abs(actual - expected) < 1e-6;
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-keypoint-overlay-" + std::to_string(seed) + "-" +
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

struct BlockingKeypointRepositoryState {
  std::mutex mutex;
  std::condition_variable condition;
  bool speculative_read_started = false;
  bool release_speculative_read = false;
};

class BlockingKeypointRepository final
    : public crimson::zarr::KeypointOverlayRepository {
public:
  explicit BlockingKeypointRepository(
      std::shared_ptr<BlockingKeypointRepositoryState> state)
      : state_(std::move(state)) {
    descriptor_.source_group = "refined_keypoints_runs";
    descriptor_.run_name = "reverse_buffer_fixture";
    descriptor_.coordinate_space =
        crimson::zarr::KeypointCoordinateSpace::Image;
    descriptor_.keypoint_labels = {"point"};
    descriptor_.row_count = 7;
    descriptor_.camera_frame_count = 7;
  }

  const crimson::zarr::KeypointOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  crimson::zarr::KeypointOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int, int) const override {
    if (camera_frame == 6) {
      std::unique_lock<std::mutex> lock(state_->mutex);
      state_->speculative_read_started = true;
      state_->condition.notify_all();
      state_->condition.wait(lock,
                             [&] { return state_->release_speculative_read; });
    }

    crimson::zarr::KeypointOverlayResolution resolution;
    resolution.status = crimson::zarr::KeypointOverlayStatus::Mapped;
    resolution.camera_frame = camera_frame;
    crimson::zarr::KeypointOverlayDetection detection;
    detection.detection_index = camera_frame;
    detection.keypoints = {{static_cast<double>(camera_frame), 2.0}};
    resolution.detections.push_back(std::move(detection));
    return resolution;
  }

private:
  std::shared_ptr<BlockingKeypointRepositoryState> state_;
  crimson::zarr::KeypointOverlayDescriptor descriptor_;
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

bool BuildFixture(const std::filesystem::path &root) {
  constexpr const char *crop_run = "crop_fixture";
  const std::string crop_base = std::string("crop_runs/") + crop_run;
  CHECK(WriteJson(root / "crop_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", crop_run}}}}));
  CHECK(WriteJson(root / crop_base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"roi_size", {20, 40}}}}}));
  CHECK((WriteArray<int32_t, 1>(root, crop_base + "/frame_indices", "int32",
                                {3}, {1, 2, 2})));
  CHECK((WriteArray<int32_t, 1>(root, crop_base + "/detection_indices", "int32",
                                {3}, {0, 0, 1})));
  CHECK((WriteArray<int32_t, 2>(root, crop_base + "/roi_coordinates_full",
                                "int32", {3, 2}, {10, 20, 30, 40, 50, 60})));
  CHECK((WriteArray<float, 2>(root, crop_base + "/bbox_norm_coords", "float32",
                              {3, 4},
                              {0.2f, 0.2f, 0.2f, 0.2f, 0.5f, 0.5f, 0.2f, 0.2f,
                               0.75f, 0.75f, 0.2f, 0.2f})));
  CHECK((WriteArray<uint8_t, 1>(root, crop_base + "/detection_source", "uint8",
                                {3}, {0, 0, 1})));

  constexpr const char *refined_run = "refined_fixture";
  const std::string refined_base =
      std::string("refined_keypoints_runs/") + refined_run;
  CHECK(WriteJson(root / "refined_keypoints_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest", refined_run}}}}));
  CHECK(WriteJson(
      root / refined_base / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"source_crop_run", crop_run},
         {"keypoint_labels", {"swim_bladder", "eye_left", "eye_right"}},
         {"pose_schema", {{"edges", {{0, 1}, {0, 2}, {1, 2}}}}}}}}));
  CHECK((WriteArray<int32_t, 1>(root, refined_base + "/frame_indices", "int32",
                                {3}, {1, 2, 2})));
  CHECK((WriteArray<int32_t, 1>(root, refined_base + "/frame_counts", "int32",
                                {3}, {0, 1, 2})));
  CHECK((WriteArray<int32_t, 1>(root, refined_base + "/detection_indices",
                                "int32", {3}, {0, 1, 0})));
  CHECK((WriteArray<int64_t, 1>(root, refined_base + "/source_crop_row_ids",
                                "int64", {3}, {0, 2, 1})));
  CHECK((WriteArray<double, 3>(root, refined_base + "/keypoints_img", "float64",
                               {3, 3, 2},
                               {20, 20, 22, 18, 24, 18, 70, 60, 72, 58, 74, 58,
                                45, 40, 47, 38, 49, 38})));
  CHECK((WriteArray<double, 1>(root, refined_base + "/heading", "float64", {3},
                               {45.0, 90.0, 0.0})));
  CHECK((WriteArray<uint8_t, 1>(root, refined_base + "/detection_success",
                                "uint8", {3}, {0, 1, 1})));
  CHECK((WriteArray<uint8_t, 1>(root, refined_base + "/detection_source",
                                "uint8", {3}, {0, 1, 0})));
  CHECK((WriteArray<uint8_t, 1>(root, refined_base + "/usable_keypoints",
                                "uint8", {3}, {1, 0, 1})));
  CHECK((WriteArray<uint8_t, 1>(root, refined_base + "/flip_corrected", "uint8",
                                {3}, {0, 1, 0})));

  constexpr const char *raw_run = "raw_fixture";
  const std::string raw_base = std::string("keypoints_runs/") + raw_run;
  CHECK(WriteJson(root / "keypoints_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", raw_run}}}}));
  CHECK(WriteJson(
      root / raw_base / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"source_crop_run", crop_run}, {"keypoint_labels", {"only"}}}}}));
  CHECK((WriteArray<int32_t, 1>(root, raw_base + "/frame_indices", "int32", {3},
                                {1, 2, 2})));
  CHECK((WriteArray<int32_t, 1>(root, raw_base + "/detection_indices", "int32",
                                {3}, {0, 0, 1})));
  CHECK((WriteArray<float, 3>(root, raw_base + "/keypoints_roi", "float32",
                              {3, 1, 2}, {1, 2, 3, 4, 5, 6})));
  CHECK((WriteArray<float, 1>(root, raw_base + "/heading", "float32", {3},
                              {10, 20, 30})));
  return true;
}

bool TestTensorStoreRepository() {
  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto archive_root = temporary.path() / "analysis.zarr";
  std::filesystem::create_directories(archive_root);
  CHECK(BuildFixture(archive_root));

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(archive_root, &error);
  CHECK(archive != nullptr);
  crimson::zarr::KeypointRepositoryOpenMetrics open_metrics;
  auto repository = crimson::zarr::OpenKeypointOverlayRepository(
      archive, {}, &error, &open_metrics);
  CHECK(repository != nullptr);
  CHECK(open_metrics.lazy_attempted);
  CHECK(open_metrics.lazy_path);
  CHECK(!open_metrics.fallback_path);
  CHECK(open_metrics.attribute_reads >= 3);
  CHECK(open_metrics.array_open_attempts > 0);
  CHECK(open_metrics.array_open_successes > 0);
  CHECK(open_metrics.array_open_failures > 0);
  CHECK(open_metrics.array_reads == 1);
  CHECK(!open_metrics.events.empty());
  CHECK(std::any_of(open_metrics.events.begin(), open_metrics.events.end(),
                    [](const auto &event) {
                      return event.phase == "frame_counts_read" &&
                             event.operation == "array_read" && event.success;
                    }));
  const auto &descriptor = repository->descriptor();
  CHECK(descriptor.source_group == "refined_keypoints_runs");
  CHECK(descriptor.run_name == "refined_fixture");
  CHECK(descriptor.source_crop_run == "crop_fixture");
  CHECK(descriptor.refined);
  CHECK(descriptor.coordinate_space ==
        crimson::zarr::KeypointCoordinateSpace::Image);
  CHECK(descriptor.keypoint_labels.size() == 3);
  CHECK(descriptor.skeleton_edges.size() == 3);
  CHECK(descriptor.row_count == 3);
  CHECK(descriptor.camera_frame_count == 3);
  const auto memory = repository->memoryMetrics();
  CHECK(memory.retained_index_bytes >= 4 * sizeof(uint64_t));
  CHECK(memory.reportedRetainedBytes() == memory.retained_index_bytes);

  const auto frame_two = repository->resolveCameraFrame(2, 100, 80);
  CHECK(frame_two.status == crimson::zarr::KeypointOverlayStatus::Mapped);
  CHECK(frame_two.detections.size() == 2);
  const auto &first = frame_two.detections[0];
  CHECK(first.detection_index == 1);
  CHECK(first.source_crop_row_id == 2);
  CHECK(first.detection_interpolated);
  CHECK(first.keypoint_detection_interpolated);
  CHECK(first.refined_keypoints);
  CHECK(!first.keypoint_usable);
  CHECK(first.keypoint_flip_corrected);
  CHECK(first.heading_valid);
  CHECK(first.heading_degrees && Near(*first.heading_degrees, 90.0));
  CHECK(first.keypoints.size() == 3);
  CHECK(Near(first.keypoints[0].x, 70.0));
  CHECK(Near(first.keypoints[0].y, 60.0));
  CHECK(first.full_frame_box_xywh.has_value());
  CHECK(Near((*first.full_frame_box_xywh)[0], 65.0));
  CHECK(Near((*first.full_frame_box_xywh)[1], 52.0));
  CHECK(Near((*first.full_frame_box_xywh)[2], 20.0));
  CHECK(Near((*first.full_frame_box_xywh)[3], 16.0));
  CHECK(frame_two.detections[1].source_crop_row_id == 1);
  CHECK(!frame_two.detections[1].detection_interpolated);
  const auto scene_input = crimson::zarr::makeKeypointOverlaySceneInput(
      descriptor, frame_two, 0, 2, 0, 100, 80);
  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(scene_input);
  CHECK(scene.ready());
  CHECK(scene.count(crimson::overlay::PrimitiveType::Marker) == 6);
  CHECK(scene.count(crimson::overlay::PrimitiveType::Arrow) == 1);
  CHECK(scene.count(crimson::overlay::CameraOverlayLayer::Keypoints) == 12);
  const auto stale_input = crimson::zarr::makeKeypointOverlaySceneInput(
      descriptor, frame_two, 0, 1, 0, 100, 80);
  CHECK(!crimson::overlay::buildReadOnlyOverlayScene(stale_input).ready());

  const auto frame_one = repository->resolveCameraFrame(1, 100, 80);
  CHECK(frame_one.detections.size() == 1);
  CHECK(!frame_one.detections[0].heading_valid);
  CHECK(repository->resolveCameraFrame(0, 100, 80).status ==
        crimson::zarr::KeypointOverlayStatus::Missing);
  CHECK(repository->resolveCameraFrame(3, 100, 80).status ==
        crimson::zarr::KeypointOverlayStatus::OutOfRange);
  CHECK(repository->resolveCameraFrame(1, 0, 80).status ==
        crimson::zarr::KeypointOverlayStatus::InvalidDimensions);

  auto raw = crimson::zarr::OpenKeypointOverlayRepository(
      archive, "keypoints_runs/raw_fixture", &error);
  CHECK(raw != nullptr);
  CHECK(!raw->descriptor().refined);
  CHECK(raw->descriptor().coordinate_space ==
        crimson::zarr::KeypointCoordinateSpace::Roi);
  const auto raw_frame = raw->resolveCameraFrame(1, 100, 80);
  CHECK(raw_frame.detections.size() == 1);
  CHECK(Near(raw_frame.detections[0].keypoints[0].x, 11.0));
  CHECK(Near(raw_frame.detections[0].keypoints[0].y, 22.0));
  return true;
}

bool TestNormalizedContract() {
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  descriptor.coordinate_space =
      crimson::zarr::KeypointCoordinateSpace::NormalizedRoi;
  descriptor.keypoint_labels = {"point"};
  crimson::zarr::KeypointOverlayRow row;
  row.camera_frame = 0;
  row.detection_index = 4;
  row.keypoints = {{0.5, 0.25}};
  row.roi_offset = crimson::zarr::KeypointOverlayPoint{5.0, 7.0};
  row.roi_width = 40.0;
  row.roi_height = 20.0;
  auto repository = crimson::zarr::MakeKeypointOverlayRepository(
      std::move(descriptor), {std::move(row)});
  const auto resolved = repository->resolveCameraFrame(0, 100, 80);
  CHECK(resolved.status == crimson::zarr::KeypointOverlayStatus::Mapped);
  CHECK(resolved.detections.size() == 1);
  CHECK(Near(resolved.detections[0].keypoints[0].x, 25.0));
  CHECK(Near(resolved.detections[0].keypoints[0].y, 12.0));
  return true;
}

bool TestScheduledBuffer() {
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  descriptor.source_group = "refined_keypoints_runs";
  descriptor.run_name = "buffer_fixture";
  descriptor.coordinate_space = crimson::zarr::KeypointCoordinateSpace::Image;
  descriptor.keypoint_labels = {"point"};
  std::vector<crimson::zarr::KeypointOverlayRow> rows;
  for (int64_t frame = 1; frame <= 4; ++frame) {
    crimson::zarr::KeypointOverlayRow row;
    row.camera_frame = frame;
    row.detection_index = frame;
    row.keypoints = {{static_cast<double>(frame), 2.0}};
    rows.push_back(std::move(row));
  }
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(16, 2, 1);
  KeypointOverlayBuffer buffer(scheduler, "fixture");
  std::string error;
  CHECK(buffer.open(crimson::zarr::MakeKeypointOverlayRepository(
                        std::move(descriptor), std::move(rows)),
                    2, 3, &error));
  CHECK(buffer.requestFrame(1, 100, 80, false, &error));
  CHECK(buffer.waitForFrame(1, std::chrono::seconds(2)));
  CHECK(buffer.frame(1) != nullptr);
  CHECK(buffer.frame(1)->status ==
        crimson::zarr::KeypointOverlayStatus::Mapped);
  CHECK(buffer.requestFrame(1, 100, 80, false, &error));
  CHECK(buffer.metrics().cache_hits >= 1);
  CHECK(buffer.requestFrame(4, 100, 80, true, &error));
  CHECK(buffer.waitForFrame(4, std::chrono::seconds(2)));
  CHECK(buffer.frame(1) == nullptr);
  CHECK(buffer.frame(4)->detections.size() == 1);
  CHECK(buffer.metrics().resolved_frames >= 2);
  buffer.close();
  scheduler->shutdown();
  return true;
}

bool TestScheduledBufferReversePrefetch() {
  auto state = std::make_shared<BlockingKeypointRepositoryState>();
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(16, 2, 1);
  KeypointOverlayBuffer buffer(scheduler, "reverse_fixture");
  std::string error;
  CHECK(buffer.open(std::make_unique<BlockingKeypointRepository>(state), 2, 5,
                    &error));

  CHECK(buffer.requestFrame(5, 100, 80, true, &error));
  CHECK(buffer.waitForFrame(5, std::chrono::seconds(2)));
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    CHECK(state->condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return state->speculative_read_started;
    }));
  }
  CHECK(buffer.requestFrame(4, 100, 80, false, &error));
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->release_speculative_read = true;
    state->condition.notify_all();
  }
  CHECK(buffer.waitForFrame(4, std::chrono::seconds(2)));
  CHECK(buffer.waitForFrame(3, std::chrono::seconds(2)));
  CHECK(buffer.waitForFrame(2, std::chrono::seconds(2)));
  scheduler->waitUntilIdle();
  CHECK(buffer.frame(5) != nullptr);
  CHECK(buffer.frame(4) != nullptr);
  CHECK(buffer.frame(3) != nullptr);
  CHECK(buffer.frame(2) != nullptr);
  CHECK(buffer.frame(6) == nullptr);
  CHECK(buffer.metrics().discarded_results == 1);

  buffer.close();
  scheduler->shutdown();
  return true;
}

} // namespace

int main() {
  if (!TestTensorStoreRepository() || !TestNormalizedContract() ||
      !TestScheduledBuffer() || !TestScheduledBufferReversePrefetch()) {
    return 1;
  }
  std::cout << "keypoint_overlay_repository_tests: PASS\n";
  return 0;
}
