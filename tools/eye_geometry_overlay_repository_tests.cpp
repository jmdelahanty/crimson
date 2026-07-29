#include "eye_geometry_overlay_buffer.h"
#include "read_only_overlay_scene.h"
#include "zarr/archive_context.h"
#include "zarr/eye_geometry_overlay_scene_adapter.h"
#include "zarr/tensorstore_eye_geometry_overlay_repository.h"

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
              ("crimson-eye-geometry-overlay-" + std::to_string(seed) + "-" +
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
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

std::vector<uint8_t> Names(const std::vector<std::string> &names) {
  std::vector<uint8_t> bytes(names.size() * 256, 0);
  for (size_t row = 0; row < names.size(); ++row) {
    const size_t count = std::min<size_t>(255, names[row].size());
    std::copy_n(names[row].begin(), count, bytes.begin() + row * 256);
  }
  return bytes;
}

bool WriteFixture(const std::filesystem::path &root, bool lineage_mismatch,
                  bool channel_shape_mismatch = false) {
  CHECK(WriteJson(root / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));
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
  CHECK((WriteArray<int64_t, 1>(root, refined + "/source_crop_row_ids", "int64",
                                {3}, {2, 0, 1})));
  const std::vector<float> left_ellipse = {20, 20, 12, 6,  0,  21, 20, 12,
                                           6,  10, 22, 20, 12, 6,  20};
  const std::vector<float> right_ellipse = {50, 20,  14, 7,  180, 51, 20, 14,
                                            7,  170, 52, 20, 14,  7,  160};
  CHECK((WriteArray<float, 2>(
      root, refined + "/components/eye_left/geometry/ellipse_params", "float32",
      {3, 5}, left_ellipse)));
  CHECK((WriteArray<float, 2>(
      root, refined + "/components/eye_right/geometry/ellipse_params",
      "float32", {3, 5}, right_ellipse)));
  CHECK((WriteArray<uint8_t, 1>(
      root, refined + "/components/eye_left/geometry/ellipse_success", "uint8",
      {3}, {1, 1, 1})));
  CHECK((WriteArray<uint8_t, 1>(
      root, refined + "/components/eye_right/geometry/ellipse_success", "uint8",
      {3}, {1, 1, 1})));

  const std::string group = "analysis/eye_angle_runs";
  const std::string run = group + "/eye_fixture";
  CHECK(WriteJson(root / group / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", "eye_fixture"}}}}));
  CHECK(
      WriteJson(root / run / "zarr.json",
                {{"zarr_format", 3},
                 {"node_type", "group"},
                 {"attributes",
                  {{"schema_id", "analysis.eye_angle_runs"},
                   {"schema_version", 5},
                   {"method", "ellipse_and_centroid_eye_angles"},
                   {"method_version", "eye_angle_analysis.v5"},
                   {"row_axis", "keypoint_detection_rows"},
                   {"layout", "compact_dense_v2"},
                   {"source_refined_subject_masks_run", "refined_fixture"}}}}));
  CHECK((WriteArray<int64_t, 1>(
      root, run + "/support/frame_indices", "int64", {3},
      lineage_mismatch ? std::vector<int64_t>{2, 9, 2}
                       : std::vector<int64_t>{2, 1, 2})));
  CHECK((WriteArray<float, 2>(root, run + "/support/body_frame/origin_xy",
                              "float32", {3, 2}, {35, 20, 35, 20, 35, 20})));
  CHECK((WriteArray<float, 2>(root, run + "/support/body_frame/forward_axis_xy",
                              "float32", {3, 2}, {1, 0, 1, 0, 1, 0})));
  CHECK((WriteArray<float, 2>(root, run + "/support/body_frame/left_axis_xy",
                              "float32", {3, 2}, {0, 1, 0, 1, 0, 1})));
  CHECK((WriteArray<uint8_t, 1>(root, run + "/support/body_frame/valid",
                                "uint8", {3}, {1, 1, 1})));

  const std::vector<std::string> angle_names = {
      "left_eye_angle_deg", "right_eye_angle_deg", "vergence_eye_angle_deg",
      "left_gaze_signed_deg", "right_gaze_signed_deg"};
  CHECK((WriteArray<uint8_t, 2>(root, run + "/angle_channel_index/name",
                                "uint8", {5, 256}, Names(angle_names))));
  CHECK(
      (WriteArray<uint8_t, 1>(root, run + "/angle_channel_index/roi_available",
                              "uint8", {5}, {1, 1, 1, 1, 1})));
  CHECK((WriteArray<float, 2>(
      root, run + "/roi_angles", "float32", {3, 5},
      {10, -12, -2, 15, -17, 11, -13, -2, 16, -18, 12, -14, -2, 17, -19})));

  const std::vector<std::string> vector_names = {"left_gaze_xy",
                                                 "right_gaze_xy"};
  CHECK((WriteArray<uint8_t, 2>(root, run + "/vector_channel_index/name",
                                "uint8", {2, 256}, Names(vector_names))));
  CHECK(
      (WriteArray<uint8_t, 1>(root, run + "/vector_channel_index/roi_available",
                              "uint8", {2}, {1, 1})));
  CHECK((WriteArray<float, 3>(
      root, run + "/roi_vectors", "float32", {3, 2, 2},
      {1, 0, -1, 0, 1, 0.1f, -1, 0.1f, 1, 0.2f, -1, 0.2f})));

  const std::vector<std::string> qa_names = {"valid_left", "valid_right",
                                             "valid_frame"};
  CHECK((WriteArray<uint8_t, 2>(root, run + "/qa_channel_index/name", "uint8",
                                {3, 256}, Names(qa_names))));
  CHECK((WriteArray<uint8_t, 1>(root, run + "/qa_channel_index/roi_available",
                                "uint8", {3}, {1, 1, 1})));
  CHECK((WriteArray<uint16_t, 2>(
      root, run + "/roi_qa", "uint16",
      channel_shape_mismatch ? std::array<ts::Index, 2>{3, 2}
                             : std::array<ts::Index, 2>{3, 3},
      channel_shape_mismatch
          ? std::vector<uint16_t>{1, 1, 1, 1, 1, 1}
          : std::vector<uint16_t>{1, 1, 1, 1, 1, 1, 1, 1, 1})));
  return true;
}

bool TestFixture(const std::filesystem::path &root) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenEyeGeometryOverlayRepository(archive, {}, &error);
  CHECK(repository != nullptr);
  const auto descriptor = repository->descriptor();
  CHECK(descriptor.run_name == "eye_fixture");
  CHECK(descriptor.source_refined_subject_masks_run == "refined_fixture");
  CHECK(descriptor.source_crop_run == "crop_fixture");
  CHECK(descriptor.row_count == 3);
  CHECK(descriptor.coordinate_width == 80);
  CHECK(descriptor.coordinate_height == 40);

  const auto frame = repository->resolveCameraFrame(2, 200, 100);
  CHECK(frame.status == crimson::zarr::EyeGeometryOverlayStatus::Mapped);
  CHECK(frame.detections.size() == 2);
  CHECK(frame.detections[0].eye_row == 0);
  CHECK(frame.detections[0].source_crop_row_id == 2);
  CHECK(frame.detections[0].roi_x == 50.0);
  CHECK(frame.detections[0].eyes[0].valid);
  CHECK(frame.detections[0].eyes[0].major_axis.valid);
  CHECK(frame.detections[0].eyes[0].gaze_valid);
  CHECK(std::abs(frame.detections[0].eyes[0].eye_frame_angle_degrees - 10.0) <
        1e-6);
  CHECK(frame.detections[1].eye_row == 2);
  const auto memory = repository->memoryMetrics();
  CHECK(memory.retained_metadata_bytes > 0);
  CHECK(memory.retained_index_bytes > 0);
  CHECK(memory.decoded_cache_bytes > 0);
  CHECK(repository->resolveCameraFrame(0, 200, 100).status ==
        crimson::zarr::EyeGeometryOverlayStatus::Missing);
  CHECK(repository->resolveCameraFrame(3, 200, 100).status ==
        crimson::zarr::EyeGeometryOverlayStatus::OutOfRange);
  CHECK(repository->resolveCameraFrame(2, 0, 100).status ==
        crimson::zarr::EyeGeometryOverlayStatus::InvalidDimensions);

  auto input = crimson::zarr::makeEyeGeometryOverlaySceneInput(
      descriptor, frame, 0, 2, 0, 200, 100);
  CHECK(input.eye_geometry.size() == 2);
  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(scene.ready());
  CHECK(scene.count(crimson::overlay::PrimitiveType::Polygon) >= 4);
  CHECK(scene.textCount(crimson::overlay::CameraOverlayLayer::SubjectMasks) ==
        6);
  CHECK(scene.text_annotations[0].content ==
        std::string("Left eye-frame +10.0\xC2\xB0"));
  const crimson::overlay::SourceViewportTransform transform{{0, 0, 200, 100},
                                                            {0, 0, 400, 200}};
  const auto mesh =
      crimson::overlay::tessellateReadOnlyOverlayScene(scene, transform);
  CHECK(mesh.triangleCount() > 100);
  const auto labels =
      crimson::overlay::layoutReadOnlyOverlayText(scene, transform);
  CHECK(labels.size() == 6);
  CHECK(std::abs(labels[0].anchor.y - 136.0) < 1e-6);

  EyeGeometryOverlayBuffer buffer;
  CHECK(buffer.open(std::move(repository), 2, 4, &error));
  CHECK(buffer.requestFrame(2, 200, 100, true, &error));
  CHECK(buffer.waitForFrame(2, std::chrono::seconds(2)));
  CHECK(buffer.frame(2) != nullptr);
  CHECK(buffer.metrics().peak_cached_frames <= 4);
  CHECK(buffer.metrics().peak_pending_frames <= 3);
  buffer.close();
  return true;
}

bool TestLineageMismatch(const std::filesystem::path &root) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  CHECK(crimson::zarr::OpenEyeGeometryOverlayRepository(archive, {}, &error) ==
        nullptr);
  CHECK(error.find("lineage mismatch") != std::string::npos);
  return true;
}

bool TestChannelShapeMismatch(const std::filesystem::path &root) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  CHECK(crimson::zarr::OpenEyeGeometryOverlayRepository(archive, {}, &error) ==
        nullptr);
  CHECK(error.find("channel index exceeds") != std::string::npos);
  return true;
}

} // namespace

int main() {
  TemporaryDirectory good;
  TemporaryDirectory bad;
  TemporaryDirectory bad_channels;
  if (good.path().empty() || bad.path().empty() ||
      bad_channels.path().empty() || !WriteFixture(good.path(), false) ||
      !TestFixture(good.path()) || !WriteFixture(bad.path(), true) ||
      !TestLineageMismatch(bad.path()) ||
      !WriteFixture(bad_channels.path(), false, true) ||
      !TestChannelShapeMismatch(bad_channels.path())) {
    return 1;
  }
  std::cout << "eye_geometry_overlay_repository_tests: PASS\n";
  return 0;
}
