#include "read_only_overlay_scene.h"
#include "subject_mask_overlay_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/subject_mask_overlay_scene_adapter.h"
#include "zarr/tensorstore_subject_mask_overlay_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
              ("crimson-subject-mask-overlay-" + std::to_string(seed) + "-" +
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

struct BlockingRepositoryState {
  std::mutex mutex;
  std::condition_variable condition;
  bool first_read_started = false;
  bool release_first_read = false;
};

class BlockingSubjectMaskRepository final
    : public crimson::zarr::SubjectMaskOverlayRepository {
public:
  explicit BlockingSubjectMaskRepository(
      std::shared_ptr<BlockingRepositoryState> state)
      : state_(std::move(state)) {
    descriptor_.camera_frame_count = 1000;
  }

  const crimson::zarr::SubjectMaskOverlayDescriptor &descriptor()
      const override {
    return descriptor_;
  }

  crimson::zarr::SubjectMaskOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int, int) const override {
    if (camera_frame == 0) {
      std::unique_lock<std::mutex> lock(state_->mutex);
      state_->first_read_started = true;
      state_->condition.notify_all();
      state_->condition.wait(
          lock, [&] { return state_->release_first_read; });
    }
    crimson::zarr::SubjectMaskOverlayResolution result;
    result.camera_frame = camera_frame;
    result.status = crimson::zarr::SubjectMaskOverlayStatus::Missing;
    return result;
  }

private:
  std::shared_ptr<BlockingRepositoryState> state_;
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor_;
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

bool WriteCropFixture(const std::filesystem::path &root) {
  constexpr const char *run = "crop_fixture";
  const std::string base = std::string("crop_runs/") + run;
  CHECK(WriteJson(root / "crop_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest", run}}}}));
  CHECK(WriteJson(root / base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"roi_size", {4, 4}}}}}));
  CHECK((WriteArray<int32_t, 1>(root, base + "/frame_indices", "int32", {3},
                                {1, 2, 2})));
  CHECK((WriteArray<int32_t, 1>(root, base + "/detection_indices", "int32", {3},
                                {0, 0, 1})));
  CHECK((WriteArray<int32_t, 2>(root, base + "/roi_coordinates_full", "int32",
                                {3, 2}, {10, 20, 30, 40, 50, 60})));
  return true;
}

bool WriteCommonRunRows(const std::filesystem::path &root,
                        const std::string &base,
                        const std::vector<int32_t> &frames,
                        const std::vector<int32_t> &detections,
                        const std::vector<int64_t> &crop_rows) {
  CHECK((WriteArray<int32_t, 1>(root, base + "/frame_indices", "int32",
                                {static_cast<ts::Index>(frames.size())},
                                frames)));
  CHECK((WriteArray<int32_t, 1>(root, base + "/detection_indices", "int32",
                                {static_cast<ts::Index>(detections.size())},
                                detections)));
  CHECK((WriteArray<int64_t, 1>(root, base + "/source_crop_row_ids", "int64",
                                {static_cast<ts::Index>(crop_rows.size())},
                                crop_rows)));
  return true;
}

bool WriteDenseFixture(const std::filesystem::path &root) {
  constexpr const char *run = "dense_fixture";
  const std::string base = std::string("refined_subject_masks_runs/") + run;
  CHECK(WriteJson(root / "refined_subject_masks_runs/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", run}}}}));
  const std::vector<std::string> labels = {"eye_right", "subject_body",
                                           "eye_left", "swim_bladder"};
  CHECK(WriteJson(root / base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"source_crop_run", "crop_fixture"},
                     {"label_schema_id", "subject_v1_lr"},
                     {"mask_labels", labels}}}}));
  CHECK(WriteCommonRunRows(root, base, {2, 1, 2}, {1, 0, 0}, {2, 0, 1}));
  CHECK((WriteArray<uint8_t, 1>(root, base + "/available_channels", "uint8",
                                {4}, {1, 1, 1, 1})));
  std::vector<uint8_t> masks(3 * 4 * 4 * 4, 0);
  for (size_t row = 0; row < 3; ++row) {
    for (size_t channel = 0; channel < 4; ++channel) {
      const size_t pixel = (row + channel) % 16;
      masks[((row * 4 + channel) * 4 * 4) + pixel] = 1;
    }
  }
  CHECK((WriteArray<uint8_t, 4>(root, base + "/masks_roi", "uint8",
                                {3, 4, 4, 4}, masks)));
  for (const auto &label : labels) {
    const std::string contour =
        base + "/components/" + label + "/sampled_contours";
    CHECK(WriteJson(root / contour / "zarr.json",
                    {{"zarr_format", 3},
                     {"node_type", "group"},
                     {"attributes",
                      {{"schema_id", "sampled_component_contours_v1"},
                       {"coordinate_space", "roi_pixels"},
                       {"point_order", "xy"}}}}));
    CHECK((WriteArray<uint8_t, 1>(root, contour + "/valid", "uint8", {3},
                                  {1, 1, 1})));
    std::vector<float> points;
    for (size_t row = 0; row < 3; ++row) {
      points.insert(points.end(), {0, 0, 3, 0, 3, 3, 0, 3});
    }
    CHECK((WriteArray<float, 3>(root, contour + "/points_xy", "float32",
                                {3, 4, 2}, points)));
  }
  return true;
}

bool WriteBitpackedFixture(const std::filesystem::path &root) {
  constexpr const char *run = "bitpacked_fixture";
  const std::string base = std::string("refined_subject_masks_runs/") + run;
  CHECK(WriteJson(root / base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"source_crop_run", "crop_fixture"},
                     {"mask_labels", {"subject_body"}}}}}));
  CHECK(WriteCommonRunRows(root, base, {1}, {0}, {0}));
  CHECK(WriteJson(root / base / "mask_bitpacked/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"schema_id", "palette_mask_bitpacked_binary_v1"},
                     {"mask_encoding", "bitpacked_binary_v1"},
                     {"mask_value_semantics", "binary_0_1"},
                     {"layout", "packed_width_array"},
                     {"packed_axis", "width"},
                     {"packed_bitorder", "little"},
                     {"logical_shape", {1, 1, 2, 4}},
                     {"encoded_shape", {1, 1, 2, 1}}}}}));
  CHECK((WriteArray<uint8_t, 4>(root, base + "/mask_bitpacked/masks_packed",
                                "uint8", {1, 1, 2, 1},
                                {0b00000101, 0b00001010})));
  return true;
}

bool WriteRleFixture(const std::filesystem::path &root) {
  constexpr const char *run = "rle_fixture";
  const std::string base = std::string("refined_subject_masks_runs/") + run;
  CHECK(WriteJson(root / base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"source_crop_run", "crop_fixture"},
                     {"mask_labels", {"subject_body"}}}}}));
  CHECK(WriteCommonRunRows(root, base, {1}, {0}, {0}));
  CHECK(WriteJson(root / base / "mask_rle/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"schema_id", "palette_mask_rle_binary_v1"},
                     {"mask_encoding", "coco_rle_fortran_v1"},
                     {"mask_value_semantics", "binary_0_1"},
                     {"layout", "component_groups"},
                     {"encoded_shape_hw", {2, 4}},
                     {"n_rows", 1},
                     {"component_count", 1}}}}));
  const std::string component = base + "/mask_rle/components/00_subject_body";
  CHECK(WriteJson(
      root / component / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"component_name", "subject_body"}, {"component_index", 0}}}}));
  CHECK((WriteArray<uint32_t, 1>(root, component + "/counts", "uint32", {3},
                                 {1, 2, 5})));
  CHECK((WriteArray<int64_t, 1>(root, component + "/indptr", "int64", {2},
                                {0, 3})));
  CHECK((
      WriteArray<uint8_t, 1>(root, component + "/present", "uint8", {1}, {1})));
  return true;
}

bool TestDenseRepositoryAndScene(
    const std::shared_ptr<crimson::zarr::ArchiveContext> &archive) {
  std::string error;
  auto repository =
      crimson::zarr::OpenSubjectMaskOverlayRepository(archive, {}, &error);
  CHECK(repository != nullptr);
  const auto descriptor = repository->descriptor();
  CHECK(descriptor.run_name == "dense_fixture");
  CHECK(descriptor.storage == crimson::zarr::SubjectMaskStorage::Dense);
  CHECK(descriptor.row_count == 3);
  CHECK(descriptor.component_labels.size() == 4);
  const auto frame = repository->resolveCameraFrame(2, 100, 80);
  CHECK(frame.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  CHECK(frame.detections.size() == 2);
  CHECK(frame.detections[0].source_crop_row_id == 2);
  CHECK(frame.detections[0].roi_x == 50.0);
  CHECK(frame.detections[0].components.size() == 4);
  CHECK(frame.detections[0].components[0].present);
  CHECK(frame.detections[0].components[0].mask->size() == 16);
  CHECK(!frame.detections[0].components[0].contour.empty());
  CHECK(frame.detections[0].components[0].contour[0].x == 50.0);
  CHECK(frame.detections[0].components[0].contour[0].y == 60.0);
  for (const auto &detection : frame.detections) {
    CHECK(std::all_of(detection.components.begin(), detection.components.end(),
                      [](const auto &component) {
                        return component.present && component.mask != nullptr;
                      }));
    CHECK(std::all_of(
        detection.components.begin(), detection.components.end(),
        [](const auto &component) { return !component.contour.empty(); }));
  }

  auto input = crimson::zarr::makeSubjectMaskOverlaySceneInput(
      descriptor, frame, 0, 2, 0, 100, 80);
  CHECK(input.subject_masks.size() == 8);
  CHECK(input.subject_masks[0].cache_namespace ==
        "refined_subject_masks_runs/dense_fixture");
  CHECK(std::all_of(
      input.subject_masks.begin(), input.subject_masks.end(),
      [](const auto &component) { return component.mask != nullptr; }));
  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(scene.ready());
  CHECK(scene.rasterCount(crimson::overlay::CameraOverlayLayer::SubjectMasks) ==
        8);
  CHECK(scene.count(crimson::overlay::CameraOverlayLayer::SubjectMasks) == 8);
  CHECK(scene.raster_masks[0].label == "subject_body");
  CHECK(scene.raster_masks[2].label == "swim_bladder");
  CHECK(scene.raster_masks[4].label == "eye_left");
  CHECK(scene.raster_masks[6].label == "eye_right");
  input.identity.overlay_frame = 1;
  CHECK(!crimson::overlay::buildReadOnlyOverlayScene(input).ready());
  CHECK(repository->resolveCameraFrame(0, 100, 80).status ==
        crimson::zarr::SubjectMaskOverlayStatus::Missing);
  CHECK(repository->resolveCameraFrame(3, 100, 80).status ==
        crimson::zarr::SubjectMaskOverlayStatus::OutOfRange);

  SubjectMaskOverlayBuffer buffer;
  CHECK(buffer.open(std::move(repository), 2, 4, &error));
  CHECK(buffer.requestFrame(1, 100, 80, true, &error));
  CHECK(buffer.waitForFrame(1, std::chrono::seconds(2)));
  const auto ready = buffer.frame(1);
  CHECK(ready != nullptr);
  CHECK(ready->status == crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  CHECK(buffer.metrics().resolved_frames >= 1);
  buffer.close();
  return true;
}

bool TestCompactRepositories(
    const std::shared_ptr<crimson::zarr::ArchiveContext> &archive) {
  std::string error;
  auto bitpacked = crimson::zarr::OpenSubjectMaskOverlayRepository(
      archive, "bitpacked_fixture", &error);
  CHECK(bitpacked != nullptr);
  CHECK(bitpacked->descriptor().storage ==
        crimson::zarr::SubjectMaskStorage::Bitpacked);
  const auto bit_frame = bitpacked->resolveCameraFrame(1, 100, 80);
  CHECK(bit_frame.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  const auto &bit_mask = *bit_frame.detections[0].components[0].mask;
  CHECK(bit_mask == std::vector<uint8_t>({255, 0, 255, 0, 0, 255, 0, 255}));

  auto rle = crimson::zarr::OpenSubjectMaskOverlayRepository(
      archive, "rle_fixture", &error);
  CHECK(rle != nullptr);
  CHECK(rle->descriptor().storage == crimson::zarr::SubjectMaskStorage::Rle);
  const auto rle_frame = rle->resolveCameraFrame(1, 100, 80);
  CHECK(rle_frame.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  const auto &rle_mask = *rle_frame.detections[0].components[0].mask;
  CHECK(rle_mask == std::vector<uint8_t>({0, 255, 0, 0, 255, 0, 0, 0}));
  return true;
}

bool TestNeutralRepositoryContract() {
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  descriptor.component_labels = {"subject_body"};
  descriptor.camera_frame_count = 5;

  auto mask = std::make_shared<std::vector<uint8_t>>(
      std::initializer_list<uint8_t>{255, 0, 0, 255});
  crimson::zarr::SubjectMaskOverlayComponent body;
  body.label = "subject_body";
  body.present = true;
  body.mask_width = 2;
  body.mask_height = 2;
  body.mask = mask;
  body.contour = {{1.0, 2.0},
                  {std::numeric_limits<double>::quiet_NaN(), 3.0}};
  crimson::zarr::SubjectMaskOverlayComponent invalid = body;
  invalid.label = "not_in_descriptor";
  invalid.channel_index = 1;

  crimson::zarr::SubjectMaskOverlayRow row;
  row.camera_frame = 4;
  row.detection_index = 7;
  row.source_crop_row_id = 9;
  row.roi_x = 10.0;
  row.roi_y = 20.0;
  row.roi_width = 2.0;
  row.roi_height = 2.0;
  row.components = {body, invalid};

  auto repository = crimson::zarr::MakeSubjectMaskOverlayRepository(
      descriptor, {std::move(row)});
  const auto resolution = repository->resolveCameraFrame(4, 100, 80);
  CHECK(resolution.status ==
        crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  CHECK(resolution.detections.size() == 1);
  CHECK(resolution.detections[0].components[0].contour.size() == 1);
  CHECK(resolution.detections[0].components[0].contour[0].x == 11.0);
  CHECK(resolution.detections[0].components[0].contour[0].y == 22.0);
  const auto input = crimson::zarr::makeSubjectMaskOverlaySceneInput(
      repository->descriptor(), resolution, 0, 4, 0, 100, 80);
  CHECK(input.subject_masks.size() == 1);
  CHECK(repository->resolveCameraFrame(4, 0, 80).status ==
        crimson::zarr::SubjectMaskOverlayStatus::InvalidDimensions);
  return true;
}

bool TestBoundedBufferAndDiscontinuity() {
  auto state = std::make_shared<BlockingRepositoryState>();
  SubjectMaskOverlayBuffer buffer;
  std::string error;
  CHECK(buffer.open(std::make_unique<BlockingSubjectMaskRepository>(state), 3,
                    5, &error));
  CHECK(buffer.requestFrame(0, 100, 80, true, &error));
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    CHECK(state->condition.wait_for(
        lock, std::chrono::seconds(2),
        [&] { return state->first_read_started; }));
  }
  for (int64_t frame = 1; frame <= 100; ++frame) {
    CHECK(buffer.requestFrame(frame, 100, 80, false, &error));
  }
  CHECK(buffer.requestFrame(500, 100, 80, true, &error));
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->release_first_read = true;
    state->condition.notify_all();
  }
  CHECK(buffer.waitForFrame(500, std::chrono::seconds(2)));
  const auto metrics = buffer.metrics();
  CHECK(metrics.peak_pending_frames <= 4);
  CHECK(metrics.discarded_results == 1);
  CHECK(buffer.frame(0) == nullptr);
  CHECK(buffer.frame(500) != nullptr);
  buffer.close();
  return true;
}

} // namespace

int main() {
  TemporaryDirectory temporary;
  if (temporary.path().empty()) {
    return 1;
  }
  const auto root = temporary.path() / "analysis.zarr";
  std::filesystem::create_directories(root);
  if (!WriteCropFixture(root) || !WriteDenseFixture(root) ||
      !WriteBitpackedFixture(root) || !WriteRleFixture(root)) {
    return 1;
  }
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  if (!archive || !TestDenseRepositoryAndScene(archive) ||
      !TestCompactRepositories(archive) ||
      !TestNeutralRepositoryContract() ||
      !TestBoundedBufferAndDiscontinuity()) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "subject_mask_overlay_repository_tests: PASS\n";
  return 0;
}
