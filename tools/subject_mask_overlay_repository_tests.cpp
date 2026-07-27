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
#include <thread>
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
bool WriteArrayWithChunks(
    const std::filesystem::path &root, const std::string &path,
    const std::string &data_type,
    const std::array<ts::Index, Rank> &shape,
    const std::array<ts::Index, Rank> &chunk_shape,
    const std::vector<T> &values) {
  size_t count = 1;
  json shape_json = json::array();
  json chunk_json = json::array();
  for (size_t dimension = 0; dimension < Rank; ++dimension) {
    const auto extent = shape[dimension];
    count *= static_cast<size_t>(extent);
    shape_json.push_back(extent);
    chunk_json.push_back(std::max<ts::Index>(1, chunk_shape[dimension]));
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

template <typename T, size_t Rank>
bool WriteArray(const std::filesystem::path &root, const std::string &path,
                const std::string &data_type,
                const std::array<ts::Index, Rank> &shape,
                const std::vector<T> &values) {
  return WriteArrayWithChunks(root, path, data_type, shape, shape, values);
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
  CHECK((WriteArray<int32_t, 1>(root, base + "/frame_indices", "int32", {4},
                                {1, 2, 2, 4})));
  CHECK((WriteArray<int32_t, 1>(root, base + "/detection_indices", "int32", {4},
                                {0, 0, 1, 0})));
  CHECK((WriteArray<int32_t, 2>(root, base + "/roi_coordinates_full", "int32",
                                {4, 2},
                                {10, 20, 30, 40, 50, 60, 70, 80})));
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
  CHECK((WriteArray<int32_t, 1>(root, base + "/frame_counts", "int32", {3},
                                {0, 1, 2})));
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

bool WriteInvalidFrameCountsFixture(const std::filesystem::path &root) {
  constexpr const char *run = "invalid_frame_counts_fixture";
  const std::string base = std::string("refined_subject_masks_runs/") + run;
  CHECK(WriteJson(root / base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"source_crop_run", "crop_fixture"},
                     {"mask_labels", {"subject_body"}}}}}));
  CHECK(WriteCommonRunRows(root, base, {1, 2}, {0, 0}, {0, 1}));
  CHECK((WriteArray<int32_t, 1>(root, base + "/frame_counts", "int32", {3},
                                {0, 1, 0})));
  CHECK((WriteArray<uint8_t, 4>(root, base + "/masks_roi", "uint8",
                                {2, 1, 2, 2},
                                {1, 0, 0, 0, 0, 1, 0, 0})));
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

bool WriteChunkedDenseFixture(const std::filesystem::path &root) {
  constexpr const char *run = "chunked_dense_fixture";
  constexpr const char *crop_run = "chunk_crop_fixture";
  const std::string base = std::string("refined_subject_masks_runs/") + run;
  const std::string crop_base = std::string("crop_runs/") + crop_run;
  CHECK(WriteJson(root / crop_base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"roi_size", {2, 2}}}}}));
  CHECK((WriteArray<int32_t, 1>(root, crop_base + "/frame_indices", "int32",
                                {8}, {1, 2, 3, 4, 5, 6, 7, 8})));
  CHECK((WriteArray<int32_t, 1>(root, crop_base + "/detection_indices",
                                "int32", {8}, {0, 0, 0, 0, 0, 0, 0, 0})));
  CHECK((WriteArray<int32_t, 2>(
      root, crop_base + "/roi_coordinates_full", "int32", {8, 2},
      {10, 20, 20, 30, 30, 40, 40, 50, 50, 60, 60, 70, 70, 80, 80,
       90})));
  CHECK(WriteJson(root / base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"source_crop_run", crop_run},
                     {"mask_labels", {"subject_body"}}}}}));
  CHECK(WriteCommonRunRows(root, base, {1, 2, 3, 4, 5, 6, 7, 8},
                           {0, 0, 0, 0, 0, 0, 0, 0},
                           {0, 1, 2, 3, 4, 5, 6, 7}));
  CHECK((WriteArrayWithChunks<int32_t, 1>(
      root, base + "/frame_counts", "int32", {10}, {5},
      {0, 1, 1, 1, 1, 1, 1, 1, 1, 0})));
  std::vector<uint8_t> masks(8 * 1 * 2 * 2, 0);
  for (size_t row = 0; row < 8; ++row) {
    masks[row * 4 + row % 4] = 1;
  }
  CHECK((WriteArrayWithChunks<uint8_t, 4>(
      root, base + "/masks_roi", "uint8", {8, 1, 2, 2}, {2, 1, 2, 2},
      masks)));
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
  const auto open_metrics = repository->metrics();
  CHECK(open_metrics.open_total_ms >= 0.0);
  CHECK(open_metrics.catalog_ms >= 0.0);
  CHECK(open_metrics.mapping_read_ms >= 0.0);
  CHECK(open_metrics.frame_indices_ms >= 0.0);
  CHECK(open_metrics.detection_indices_ms >= 0.0);
  CHECK(open_metrics.source_crop_row_ids_ms >= 0.0);
  CHECK(open_metrics.crop_frame_indices_ms >= 0.0);
  CHECK(open_metrics.crop_coordinates_ms >= 0.0);
  CHECK(open_metrics.crop_detection_indices_ms >= 0.0);
  CHECK(open_metrics.storage_open_ms >= 0.0);
  CHECK(open_metrics.contour_open_ms >= 0.0);
  CHECK(open_metrics.metadata_index_ms >= 0.0);
  CHECK(open_metrics.metadata_decoded_bytes > 0);
  CHECK(open_metrics.lazy_mapping);
  CHECK(open_metrics.subject_mapping_bytes == 0);
  CHECK(open_metrics.crop_mapping_bytes == 0);
  CHECK(open_metrics.frame_index_rows_read == 0);
  CHECK(open_metrics.metadata_decoded_bytes >=
        open_metrics.subject_mapping_bytes + open_metrics.crop_mapping_bytes);
  CHECK(open_metrics.metadata_retained_bytes > 0);
  const auto frame = repository->resolveCameraFrame(2, 100, 80);
  CHECK(frame.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  CHECK(frame.detections.size() == 2);
  const auto resolved_metrics = repository->metrics();
  CHECK(resolved_metrics.fallback_frame_index_builds == 1);
  CHECK(resolved_metrics.fallback_frame_index_rows == 3);
  CHECK(resolved_metrics.frame_index_rows_read == 6);
  CHECK(resolved_metrics.mapping_page_reads == 2);
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
  CHECK(buffer.repositoryMetrics().demand_chunk_loads == 1);
  CHECK(buffer.repositoryMetrics().chunk_source_bytes_read > 0);
  CHECK(buffer.repositoryMetrics().peak_cached_payload_bytes > 0);
  CHECK(buffer.metrics().cached_payload_bytes == 0);
  CHECK(buffer.metrics().peak_cached_payload_bytes > 0);
  CHECK(buffer.metrics().released_payload_bytes > 0);
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

bool TestChunkCacheAndPrefetch(
    const std::shared_ptr<crimson::zarr::ArchiveContext> &archive) {
  std::string error;
  auto repository = crimson::zarr::OpenSubjectMaskOverlayRepository(
      archive, "chunked_dense_fixture", &error);
  CHECK(repository != nullptr);
  CHECK(repository->descriptor().storage_chunk_rows == 2);
  CHECK(repository->descriptor().camera_frame_count == 10);
  const auto open_metrics = repository->metrics();
  CHECK(open_metrics.lazy_mapping);
  CHECK(open_metrics.frame_index_rows_read == 0);
  CHECK(open_metrics.mapping_page_reads == 0);
  CHECK(open_metrics.subject_mapping_bytes == 0);
  CHECK(open_metrics.crop_mapping_bytes == 0);

  const auto first = repository->resolveCameraFrame(1, 100, 80);
  CHECK(first.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  CHECK(*first.detections[0].components[0].mask ==
        std::vector<uint8_t>({255, 0, 0, 0}));
  CHECK(repository->metrics().demand_chunk_loads == 1);
  const auto first_metrics = repository->metrics();
  CHECK(first_metrics.frame_index_rows_read == 10);
  CHECK(first_metrics.frame_index_source_bytes > 0);
  CHECK(first_metrics.frame_index_retained_bytes > 0);
  CHECK(first_metrics.mapping_page_reads == 2);
  CHECK(first_metrics.mapping_page_cache_hits == 0);
  CHECK(first_metrics.mapping_page_source_bytes > 0);
  CHECK(first_metrics.cached_mapping_bytes > 0);
  CHECK(first_metrics.peak_cached_mapping_bytes >=
        first_metrics.cached_mapping_bytes);
  CHECK(first_metrics.mapping_initialize_failures == 0);
  CHECK(repository->resolveCameraFrame(0, 100, 80).status ==
        crimson::zarr::SubjectMaskOverlayStatus::Missing);
  CHECK(repository->resolveCameraFrame(10, 100, 80).status ==
        crimson::zarr::SubjectMaskOverlayStatus::OutOfRange);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (repository->metrics().prefetched_chunk_loads < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(repository->metrics().prefetched_chunk_loads == 2);

  const auto prefetched = repository->resolveCameraFrame(3, 100, 80);
  CHECK(prefetched.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  CHECK(*prefetched.detections[0].components[0].mask ==
        std::vector<uint8_t>({0, 0, 255, 0}));

  const auto final_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (repository->metrics().prefetched_chunk_loads < 3 &&
         std::chrono::steady_clock::now() < final_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(repository->metrics().prefetched_chunk_loads == 3);
  for (int repeat = 0; repeat < 4; ++repeat) {
    CHECK(repository->resolveCameraFrame(3, 100, 80).status ==
          crimson::zarr::SubjectMaskOverlayStatus::Mapped);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto metrics = repository->metrics();
  CHECK(metrics.demand_chunk_loads == 1);
  CHECK(metrics.prefetch_requests == 3);
  CHECK(metrics.chunk_cache_hits >= 5);
  CHECK(metrics.cached_chunks == 3);
  CHECK(metrics.peak_cached_chunks == 3);
  CHECK(metrics.chunk_evictions == 1);
  CHECK(metrics.chunk_load_failures == 0);
  CHECK(metrics.chunk_source_bytes_read == 32);
  CHECK(metrics.chunk_retained_bytes_produced > 0);
  CHECK(metrics.cached_payload_bytes > 0);
  CHECK(metrics.peak_cached_payload_bytes >= metrics.cached_payload_bytes);
  CHECK(metrics.evicted_payload_bytes > 0);
  CHECK(metrics.chunk_read_ms >= 0.0);
  CHECK(metrics.chunk_convert_ms >= 0.0);
  CHECK(metrics.contour_load_ms >= 0.0);
  CHECK(metrics.maximum_chunk_read_ms >= 0.0);
  CHECK(metrics.maximum_chunk_convert_ms >= 0.0);
  CHECK(metrics.maximum_contour_load_ms >= 0.0);
  CHECK(metrics.mapping_page_reads == 2);
  CHECK(metrics.mapping_page_cache_hits >= 5);
  CHECK(metrics.mapping_page_evictions == 0);
  CHECK(metrics.maximum_mapping_page_read_ms >= 0.0);
  return true;
}

bool TestInvalidFrameCountsFailOnce(
    const std::shared_ptr<crimson::zarr::ArchiveContext> &archive) {
  std::string error;
  auto repository = crimson::zarr::OpenSubjectMaskOverlayRepository(
      archive, "invalid_frame_counts_fixture", &error);
  CHECK(repository != nullptr);
  CHECK(repository->metrics().lazy_mapping);
  CHECK(repository->metrics().frame_index_rows_read == 0);

  const auto first = repository->resolveCameraFrame(1, 100, 80);
  CHECK(first.status == crimson::zarr::SubjectMaskOverlayStatus::ReadFailed);
  CHECK(first.error.find("do not sum") != std::string::npos);
  const auto first_metrics = repository->metrics();
  CHECK(first_metrics.frame_index_rows_read == 3);
  CHECK(first_metrics.mapping_initialize_failures == 1);
  CHECK(first_metrics.frame_index_retained_bytes == 0);
  CHECK(first_metrics.mapping_page_reads == 0);

  const auto second = repository->resolveCameraFrame(1, 100, 80);
  CHECK(second.status == crimson::zarr::SubjectMaskOverlayStatus::ReadFailed);
  const auto second_metrics = repository->metrics();
  CHECK(second_metrics.frame_index_rows_read ==
        first_metrics.frame_index_rows_read);
  CHECK(second_metrics.mapping_initialize_failures ==
        first_metrics.mapping_initialize_failures);
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
      !WriteInvalidFrameCountsFixture(root) ||
      !WriteBitpackedFixture(root) || !WriteRleFixture(root) ||
      !WriteChunkedDenseFixture(root)) {
    return 1;
  }
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  if (!archive || !TestDenseRepositoryAndScene(archive) ||
      !TestCompactRepositories(archive) ||
      !TestChunkCacheAndPrefetch(archive) ||
      !TestInvalidFrameCountsFailOnce(archive) ||
      !TestNeutralRepositoryContract() ||
      !TestBoundedBufferAndDiscontinuity()) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "subject_mask_overlay_repository_tests: PASS\n";
  return 0;
}
