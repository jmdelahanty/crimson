#include "read_only_overlay_scene.h"
#include "subject_shape_overlay_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/subject_shape_overlay_scene_adapter.h"
#include "zarr/tensorstore_subject_shape_overlay_repository.h"

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
#include <memory>
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

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-subject-shape-overlay-" + std::to_string(seed) + "-" +
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
  json spec = {
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
    std::cerr << "Failed to create " << path << ": " << store.status() << '\n';
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

std::vector<float> PointRows(float offset) {
  return {offset + 1, offset + 1, offset + 2,
          offset + 1, offset + 3, offset + 1};
}

std::vector<float> SequenceRows(float offset) {
  std::vector<float> values;
  for (size_t row = 0; row < 3; ++row) {
    for (size_t point = 0; point < 3; ++point) {
      values.push_back(offset + static_cast<float>(row + point));
      values.push_back(offset + static_cast<float>(row + point + 1));
    }
  }
  return values;
}

bool WriteSubjectShapeRunMetadata(const std::filesystem::path &root,
                                  const std::string &row_axis) {
  return WriteJson(root / "analysis/subject_shape_runs/shape_fixture/zarr.json",
                   {{"zarr_format", 3},
                    {"node_type", "group"},
                    {"attributes",
                     {{"source_refined_subject_masks_run", "refined_fixture"},
                      {"schema_id", "analysis.subject_shape_runs"},
                      {"schema_version", 3},
                      {"method", "fixture_method"},
                      {"method_version", 8},
                      {"row_axis", row_axis},
                      {"head_endpoint_semantics", "validated_snout_tip"}}}});
}

bool WriteFixture(const std::filesystem::path &root) {
  CHECK(WriteJson(root / "crop_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest", "crop_fixture"}}}}));
  CHECK(WriteJson(root / "crop_runs/crop_fixture/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"roi_size", {40, 80}}}}}));
  CHECK((WriteArray<int32_t, 1>(root, "crop_runs/crop_fixture/frame_indices",
                                "int32", {3}, {1, 2, 2})));
  CHECK(
      (WriteArray<int32_t, 1>(root, "crop_runs/crop_fixture/detection_indices",
                              "int32", {3}, {0, 0, 1})));
  CHECK((WriteArray<int32_t, 2>(root,
                                "crop_runs/crop_fixture/roi_coordinates_full",
                                "int32", {3, 2}, {10, 20, 30, 40, 50, 60})));

  const std::string refined = "refined_subject_masks_runs/refined_fixture";
  CHECK(WriteJson(root / refined / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"source_crop_run", "crop_fixture"}}}}));
  CHECK((WriteArray<int32_t, 1>(root, refined + "/frame_indices", "int32", {3},
                                {2, 1, 2})));
  CHECK((WriteArray<int32_t, 1>(root, refined + "/detection_indices", "int32",
                                {3}, {1, 0, 0})));
  CHECK((WriteArray<int64_t, 1>(root, refined + "/source_crop_row_ids", "int64",
                                {3}, {2, 0, 1})));
  CHECK((WriteArray<int64_t, 1>(root, refined + "/source_refined_row_ids",
                                "int64", {3}, {102, 100, 101})));
  CHECK((WriteArray<uint8_t, 4>(root, refined + "/masks_roi", "uint8",
                                {3, 1, 4, 8},
                                std::vector<uint8_t>(3 * 1 * 4 * 8, 0))));

  constexpr const char *run = "shape_fixture";
  CHECK(WriteJson(root / "analysis/subject_shape_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", run}}}}));
  const std::string base = std::string("analysis/subject_shape_runs/") + run;
  CHECK(WriteSubjectShapeRunMetadata(root, "refined_subject_mask_rows"));
  const std::string rows = base + "/row_index";
  CHECK((WriteArray<int32_t, 1>(root, rows + "/frame_indices", "int32", {3},
                                {2, 1, 2})));
  CHECK((WriteArray<int32_t, 1>(root, rows + "/detection_indices", "int32", {3},
                                {1, 0, 0})));
  CHECK((WriteArray<int64_t, 1>(root, rows + "/source_refined_row_ids", "int64",
                                {3}, {102, 100, 101})));
  CHECK((WriteArray<int64_t, 1>(root, rows + "/source_crop_row_ids", "int64",
                                {3}, {2, 0, 1})));

  const std::vector<uint8_t> valid = {1, 1, 1};
  const std::string body = base + "/body_frame";
  CHECK((WriteArray<uint8_t, 1>(root, body + "/valid", "uint8", {3}, valid)));
  CHECK((WriteArray<float, 2>(root, body + "/origin_xy", "float32", {3, 2},
                              PointRows(0))));
  CHECK((WriteArray<float, 2>(root, body + "/forward_axis_xy", "float32",
                              {3, 2}, {1, 0, 1, 0, 1, 0})));
  CHECK((WriteArray<float, 2>(root, body + "/left_axis_xy", "float32", {3, 2},
                              {0, 1, 0, 1, 0, 1})));

  const std::string subject = base + "/components/subject_body";
  for (const auto *name :
       {"snout_tip_valid", "tail_base_valid", "centerline_valid",
        "centerline_reaches_snout", "bspline_valid", "tail_sample_valid"}) {
    CHECK((WriteArray<uint8_t, 1>(root, subject + "/" + name, "uint8", {3},
                                  valid)));
  }
  CHECK((WriteArray<float, 2>(root, subject + "/snout_tip_xy", "float32",
                              {3, 2}, PointRows(0))));
  CHECK((WriteArray<float, 2>(root, subject + "/tail_base_xy", "float32",
                              {3, 2}, PointRows(1))));
  CHECK((WriteArray<float, 2>(root, subject + "/tail_tip_xy", "float32", {3, 2},
                              PointRows(2))));
  for (const auto *name :
       {"centerline_xy", "bspline_sample_xy", "bspline_control_points_xy",
        "tail_sample_xy", "tail_normal_xy"}) {
    CHECK((WriteArray<float, 3>(root, subject + "/" + name, "float32",
                                {3, 3, 2}, SequenceRows(0))));
  }

  const std::string bladder = base + "/components/swim_bladder";
  CHECK((WriteArray<uint8_t, 1>(root, bladder + "/caudal_contour_valid",
                                "uint8", {3}, valid)));
  CHECK((WriteArray<float, 2>(root, bladder + "/caudal_contour_point_xy",
                              "float32", {3, 2}, PointRows(3))));
  return true;
}

bool TestRepositoryAndScene(
    const std::shared_ptr<crimson::zarr::ArchiveContext> &archive) {
  std::string error;
  auto repository =
      crimson::zarr::OpenSubjectShapeOverlayRepository(archive, {}, &error);
  CHECK(repository != nullptr);
  const auto descriptor = repository->descriptor();
  CHECK(descriptor.run_name == "shape_fixture");
  CHECK(descriptor.source_refined_subject_masks_run == "refined_fixture");
  CHECK(descriptor.source_crop_run == "crop_fixture");
  CHECK(descriptor.row_count == 3);
  CHECK(descriptor.coordinate_width == 8);
  CHECK(descriptor.coordinate_height == 4);
  CHECK(descriptor.centerline_point_count == 3);

  const auto frame = repository->resolveCameraFrame(2, 200, 100);
  CHECK(frame.status == crimson::zarr::SubjectShapeOverlayStatus::Mapped);
  CHECK(frame.detections.size() == 2);
  CHECK(frame.detections[0].shape_row == 0);
  CHECK(frame.detections[0].source_refined_row_id == 102);
  CHECK(frame.detections[0].source_crop_row_id == 2);
  CHECK(frame.detections[0].roi_x == 50.0);
  CHECK(frame.detections[0].roi_y == 60.0);
  CHECK(frame.detections[0].geometry.centerline.size() == 3);
  CHECK(frame.detections[1].shape_row == 2);
  const auto memory = repository->memoryMetrics();
  CHECK(memory.retained_metadata_bytes >= 3 * sizeof(size_t));
  CHECK(memory.retained_index_bytes > 0);
  CHECK(memory.decoded_cache_bytes > 0);
  CHECK(repository->resolveCameraFrame(0, 200, 100).status ==
        crimson::zarr::SubjectShapeOverlayStatus::Missing);
  CHECK(repository->resolveCameraFrame(3, 200, 100).status ==
        crimson::zarr::SubjectShapeOverlayStatus::OutOfRange);
  CHECK(repository->resolveCameraFrame(2, 0, 100).status ==
        crimson::zarr::SubjectShapeOverlayStatus::InvalidDimensions);

  auto input = crimson::zarr::makeSubjectShapeOverlaySceneInput(
      descriptor, frame, 0, 2, 0, 200, 100);
  CHECK(input.subject_shapes.size() == 2);
  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(scene.ready());
  CHECK(scene.count(crimson::overlay::CameraOverlayLayer::SubjectShape) == 12);
  CHECK(scene.primitives[0].label == "##shape_centerline_2");
  CHECK(std::abs(scene.primitives[0].points[0].x - 50.0) < 1e-6);
  CHECK(std::abs(scene.primitives[0].points[0].y - 70.0) < 1e-6);
  CHECK(scene.primitives[2].label == "##shape_snout_2");
  CHECK(std::abs(scene.primitives[2].points[0].x - 60.0) < 1e-6);
  CHECK(std::abs(scene.primitives[2].points[0].y - 50.0) < 1e-6);

  input.show_subject_shape_body_axes = true;
  input.show_subject_shape_bspline_control_points = true;
  input.show_subject_shape_tail_samples = true;
  input.show_subject_shape_tail_normals = true;
  const auto debug_scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(debug_scene.count(crimson::overlay::CameraOverlayLayer::SubjectShape) >
        scene.count(crimson::overlay::CameraOverlayLayer::SubjectShape));

  --input.identity.overlay_frame;
  CHECK(!crimson::overlay::buildReadOnlyOverlayScene(input).ready());
  CHECK(!crimson::zarr::appendSubjectShapeOverlaySceneInput(descriptor, frame,
                                                            2, &input));
  return true;
}

bool TestLineageMismatchRejected(const std::filesystem::path &root) {
  const std::string path =
      "analysis/subject_shape_runs/shape_fixture/row_index/detection_indices";
  CHECK((WriteArray<int32_t, 1>(root, path, "int32", {3}, {9, 0, 0})));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenSubjectShapeOverlayRepository(archive, {}, &error);
  CHECK(repository == nullptr);
  CHECK(error.find("does not match") != std::string::npos);
  return true;
}

bool TestRowAxisMismatchRejected(const std::filesystem::path &root) {
  CHECK(WriteSubjectShapeRunMetadata(root, "camera_frames"));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenSubjectShapeOverlayRepository(archive, {}, &error);
  CHECK(repository == nullptr);
  CHECK(error.find("row_axis") != std::string::npos);
  CHECK(WriteSubjectShapeRunMetadata(root, "refined_subject_mask_rows"));
  return true;
}

struct BlockingState {
  std::mutex mutex;
  std::condition_variable condition;
  bool started = false;
  bool release = false;
};

class BlockingRepository final
    : public crimson::zarr::SubjectShapeOverlayRepository {
public:
  explicit BlockingRepository(std::shared_ptr<BlockingState> state)
      : state_(std::move(state)) {
    descriptor_.camera_frame_count = 100;
  }
  const crimson::zarr::SubjectShapeOverlayDescriptor &
  descriptor() const override {
    return descriptor_;
  }
  crimson::zarr::SubjectShapeOverlayResolution
  resolveCameraFrame(int64_t frame, int, int) const override {
    if (frame == 0) {
      std::unique_lock<std::mutex> lock(state_->mutex);
      state_->started = true;
      state_->condition.notify_all();
      state_->condition.wait(lock, [&] { return state_->release; });
    }
    crimson::zarr::SubjectShapeOverlayResolution result;
    result.camera_frame = frame;
    result.status = crimson::zarr::SubjectShapeOverlayStatus::Missing;
    return result;
  }

private:
  std::shared_ptr<BlockingState> state_;
  crimson::zarr::SubjectShapeOverlayDescriptor descriptor_;
};

bool TestBoundedAsyncBuffer() {
  auto state = std::make_shared<BlockingState>();
  SubjectShapeOverlayBuffer buffer;
  std::string error;
  CHECK(buffer.open(std::make_unique<BlockingRepository>(state), 3, 5, &error));
  CHECK(buffer.requestFrame(0, 200, 100, false, &error));
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    CHECK(state->condition.wait_for(lock, std::chrono::seconds(2),
                                    [&] { return state->started; }));
  }
  CHECK(buffer.requestFrame(50, 200, 100, true, &error));
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->release = true;
    state->condition.notify_all();
  }
  CHECK(buffer.waitForFrame(50, std::chrono::seconds(2)));
  CHECK(buffer.frame(50) != nullptr);
  const auto metrics = buffer.metrics();
  CHECK(metrics.peak_pending_frames <= 4);
  CHECK(metrics.peak_cached_frames <= 5);
  CHECK(metrics.discarded_results == 1);
  buffer.close();
  return true;
}

} // namespace

int main() {
  TemporaryDirectory temporary;
  if (temporary.path().empty() || !WriteFixture(temporary.path())) {
    return 1;
  }
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(temporary.path(), &error);
  if (!archive) {
    std::cerr << error << '\n';
    return 1;
  }
  if (!TestRepositoryAndScene(archive) ||
      !TestRowAxisMismatchRejected(temporary.path()) ||
      !TestLineageMismatchRejected(temporary.path()) ||
      !TestBoundedAsyncBuffer()) {
    return 1;
  }
  std::cout << "subject_shape_overlay_repository_tests: PASS\n";
  return 0;
}
