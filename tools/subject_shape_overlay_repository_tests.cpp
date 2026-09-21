#include "read_only_overlay_scene.h"
#include "subject_shape_overlay_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_json.h"
#include "zarr/subject_shape_overlay_scene_adapter.h"
#include "zarr/tensorstore_bound_subject_shape_overlay_repository.h"
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
                const std::vector<T> &values,
                const json &attributes = json::object()) {
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
        {"fill_value", data_type == "bool" ? json(false) : json(0)},
        {"attributes", attributes},
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

bool RewriteArrayAttributes(const std::filesystem::path &root,
                            const std::string &path,
                            const json &attributes) {
  const auto metadata_path = root / path / "zarr.json";
  std::ifstream input(metadata_path);
  json metadata;
  if (!input.good()) {
    return false;
  }
  try {
    input >> metadata;
  } catch (const json::exception &) {
    return false;
  }
  if (!metadata.is_object() || metadata.value("node_type", "") != "array") {
    return false;
  }
  input.close();
  metadata["attributes"] = attributes;
  return WriteJson(metadata_path, metadata);
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
  CHECK(!descriptor.geometry_in_source_camera_coordinates);
  CHECK(!descriptor.validated_instance_keys);

  const auto frame = repository->resolveCameraFrame(2, 200, 100);
  CHECK(frame.status == crimson::zarr::SubjectShapeOverlayStatus::Mapped);
  CHECK(frame.detections.size() == 2);
  CHECK(frame.detections[0].shape_row == 0);
  CHECK(frame.detections[0].source_refined_row_id == 102);
  CHECK(frame.detections[0].source_crop_row_id == 2);
  CHECK(!frame.detections[0].instance_key_valid);
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

json DigestEntry(std::vector<size_t> shape, const char *dtype, char value) {
  return {{"shape", std::move(shape)},
          {"dtype", dtype},
          {"digest_algorithm", "sha256_c_contiguous_bytes_v1"},
          {"sha256", std::string(64, value)}};
}

json CoordinateAttributes(const char *profile, const char *geometry_type,
                          const char *origin, const char *overlay_status) {
  json descriptor = {
      {"schema_id", "palette.coordinate_descriptor"},
      {"schema_version", 2},
      {"profile_id", profile},
      {"space_id", "source_camera_image_px"},
      {"geometry_type", geometry_type},
      {"origin", origin},
      {"positive_directions", {{"x", "right"}, {"y", "down"}}},
      {"reference_extent", {{"width", 100}, {"height", 80}, {"units", "px"}}},
      {"source_camera_overlay", {{"status", overlay_status}}},
  };
  return {{"coordinate_descriptor_sha256",
           crimson::zarr::CanonicalJsonSha256(descriptor)},
          {"coordinate_descriptor", std::move(descriptor)}};
}

struct V5Fixture {
  crimson::zarr::CanonicalOverlaySelection selection;
  std::string shape_base;
  std::string mask_base;
};

bool WriteV5Fixture(const std::filesystem::path &root, V5Fixture *fixture) {
  CHECK(fixture != nullptr);
  constexpr const char *shape_run = "shape_v5_fixture";
  constexpr const char *mask_run = "mask_v5_fixture";
  fixture->shape_base =
      std::string("analysis/subject_shape_runs/") + shape_run;
  fixture->mask_base = std::string("refined_subject_masks_runs/") + mask_run;

  const json row_arrays = {
      {"instance_key", DigestEntry({3}, "uint64", '1')},
      {"source_acquisition_frame_index", DigestEntry({3}, "int64", '2')},
      {"frame_row_offsets", DigestEntry({5}, "int64", '3')},
      {"source_crop_row_ids", DigestEntry({3}, "int64", '4')},
      {"source_crop_xywh", DigestEntry({3, 4}, "float32", '5')},
  };
  const json binding = {
      {"schema_id", "palette.subject_shape.recording_mask_bundle_source"},
      {"schema_version", 1},
      {"source_kind", "recording_subject_mask_bundle_v3"},
      {"recording_identity", "recording_fixture"},
      {"camera_identity", "camera_fixture"},
      {"frame_axis",
       {{"domain", "zero_based_acquisition_camera_frame"},
        {"source_total_frames", 4},
        {"frame_row_offsets_length", 5}}},
      {"source_camera_extent", {{"width_px", 100}, {"height_px", 80}}},
      {"roi_raster_extent", {{"width_px", 8}, {"height_px", 4}}},
      {"row_count", 3},
      {"component_labels",
       {"subject_body", "eye_left", "eye_right", "swim_bladder"}},
      {"row_arrays", row_arrays},
      {"coordinate_transform",
       {{"profile", "rowwise_roi_to_source_camera_translation_v1"},
        {"source", "refined_subject_mask_core.source_crop_xywh"},
        {"size_policy", "source_crop_wh_must_equal_dense_roi_extent"},
        {"continuous_points", "add_source_crop_xy"}}},
      {"authorities",
       {{"crop_run_path", "crop_runs/crop_fixture"},
        {"refined_run_path", fixture->mask_base},
        {"refined_manifest_payload_digest", "pending"}}},
  };
  json mask_payload = {
      {"logical_content", {{"document", {{"arrays", row_arrays}}}}},
  };
  const std::string mask_digest =
      crimson::zarr::CanonicalJsonSha256(mask_payload);
  json final_binding = binding;
  final_binding["authorities"]["refined_manifest_payload_digest"] =
      mask_digest;
  const std::string binding_digest =
      crimson::zarr::CanonicalJsonSha256(final_binding);
  const std::string heading_path = fixture->shape_base + "/body_frame/heading_deg";
  const json heading_semantics = {
      {"schema_id", "palette.subject_shape_row_bound_heading_semantics"},
      {"schema_version", 1},
      {"heading",
       {{"relative_ref", "body_frame/heading_deg"},
        {"dtype", "<f4"},
        {"shape", {3}}}},
      {"axis_valid",
       {{"relative_ref", "body_frame/axis_valid"},
        {"dtype", "|b1"},
        {"shape", {3}}}},
      {"units", "deg"},
      {"formula", "degrees(atan2(-forward_y, forward_x))"},
      {"zero_direction", "source_camera_positive_x_right"},
      {"positive_rotation", "counterclockwise_after_source_camera_y_flip"},
      {"invalid_row_value", "nan_when_axis_valid_false"},
  };
  const std::string heading_digest =
      crimson::zarr::CanonicalJsonSha256(heading_semantics);
  const json heading_attributes = {
      {"subject_shape_heading_semantics", heading_semantics},
      {"subject_shape_heading_semantics_sha256", heading_digest},
  };
  const json publication = {
      {"kind", "shape-v5-fixture"},
      {"source_binding_sha256", binding_digest},
      {"heading_semantics",
       {{"record_ref", "/" + heading_path +
                           "@subject_shape_heading_semantics"},
        {"record_sha256", heading_digest}}},
  };
  const std::string publication_digest =
      crimson::zarr::CanonicalJsonSha256(publication);
  const json derivation = {
      {"schema_id", "palette.subject_shape_coordinate_derivation"},
      {"schema_version", 2},
      {"transform_direction", "roi_local_px_to_source_camera_image_px"},
      {"transform_policy", "exact_translation_only_v1"},
      {"roi_local_point_arrays_retained", false},
  };
  CHECK(WriteJson(root / fixture->shape_base / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"schema_id", "analysis.subject_shape_runs"},
                     {"schema_version", 5},
                     {"palette_run_name", shape_run},
                     {"palette_run_completion_status", "complete"},
                     {"stage_selector_eligible", true},
                     {"row_axis", "recording_subject_mask_bundle_rows"},
                     {"source_refined_subject_masks_run", mask_run},
                     {"coordinate_contract", "canonical_v2"},
                     {"coordinate_binding_status", "bound_canonical_v2"},
                     {"publication_manifest_sha256", publication_digest},
                     {"subject_shape_publication_manifest_sha256",
                      publication_digest},
                     {"subject_shape_publication_manifest", publication},
                     {"subject_shape_source_binding_sha256", binding_digest},
                     {"subject_shape_source_binding", final_binding},
                     {"subject_shape_coordinate_derivation", derivation},
                     {"method", "shape-fixture"},
                     {"method_version", 12},
                     {"head_endpoint_semantics", "validated_snout_tip"}}}}));
  CHECK(WriteJson(
      root / fixture->mask_base / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"run_manifest",
          {{"schema_id", "palette.subject_mask_core.run_manifest"},
           {"schema_version", 5},
           {"payload_digest", mask_digest},
           {"payload", mask_payload}}}}}}));

  const std::vector<int64_t> frames = {0, 2, 2};
  const std::vector<uint64_t> keys = {
      0, std::numeric_limits<uint64_t>::max() - 2, 17};
  const std::vector<int64_t> crop_rows = {5, 6, 7};
  CHECK((WriteArray<int64_t, 1>(root,
                                fixture->shape_base +
                                    "/source_acquisition_frame_index",
                                "int64", {3}, frames)));
  CHECK((WriteArray<uint64_t, 1>(root, fixture->shape_base + "/instance_key",
                                 "uint64", {3}, keys)));
  CHECK((WriteArray<int64_t, 1>(root,
                                fixture->shape_base + "/source_crop_row_ids",
                                "int64", {3}, crop_rows)));
  CHECK((WriteArray<int64_t, 1>(root,
                                fixture->mask_base +
                                    "/source_acquisition_frame_index",
                                "int64", {3}, frames)));
  CHECK((WriteArray<uint64_t, 1>(root, fixture->mask_base + "/instance_key",
                                 "uint64", {3}, keys)));
  CHECK((WriteArray<int64_t, 1>(root,
                                fixture->mask_base + "/source_crop_row_ids",
                                "int64", {3}, crop_rows)));
  CHECK((WriteArray<int64_t, 1>(root,
                                fixture->mask_base + "/frame_row_offsets",
                                "int64", {5}, {0, 1, 1, 3, 3})));
  CHECK((WriteArray<float, 2>(root, fixture->mask_base + "/source_crop_xywh",
                              "float32", {3, 4},
                              {10, 20, 8, 4, 30, 40, 8, 4, 50, 60, 8, 4})));

  const json point = CoordinateAttributes(
      "source_camera_image_px.top_left_y_down.v1", "point_xy", "top_left",
      "direct");
  const json polyline = CoordinateAttributes(
      "source_camera_image_px.top_left_y_down.v1", "polyline_xy", "top_left",
      "direct");
  const json vector = CoordinateAttributes(
      "source_camera_image_px.unit_vector_y_down.v1", "vector_xy",
      "not_applicable", "not_suitable");
  const json vector_sequence = CoordinateAttributes(
      "source_camera_image_px.unit_vector_y_down.v1", "vector_sequence_xy",
      "not_applicable", "not_suitable");
  const std::vector<bool> valid = {true, true, true};
  const std::string body = fixture->shape_base + "/body_frame";
  CHECK((WriteArray<bool, 1>(root, body + "/valid", "bool", {3}, valid)));
  CHECK((WriteArray<bool, 1>(root, body + "/axis_valid", "bool", {3},
                             {true, true, false})));
  CHECK((WriteArray<float, 1>(root, body + "/heading_deg", "float32", {3},
                              {0, 90, 180}, heading_attributes)));
  CHECK((WriteArray<float, 2>(root, body + "/origin_xy", "float32", {3, 2},
                              {11, 21, 32, 42, 53, 63}, point)));
  CHECK((WriteArray<float, 2>(root, body + "/forward_axis_xy", "float32",
                              {3, 2}, {1, 0, 0, -1, -1, 0}, vector)));
  CHECK((WriteArray<float, 2>(root, body + "/left_axis_xy", "float32",
                              {3, 2}, {0, 1, 1, 0, 0, -1}, vector)));

  const std::string subject = fixture->shape_base + "/components/subject_body";
  for (const auto *name :
       {"snout_tip_valid", "tail_base_valid", "centerline_valid",
        "centerline_reaches_snout", "bspline_valid", "tail_sample_valid"}) {
    CHECK((WriteArray<bool, 1>(root, subject + "/" + name, "bool", {3},
                               valid)));
  }
  CHECK((WriteArray<float, 2>(root, subject + "/snout_tip_xy", "float32",
                              {3, 2}, {12, 21, 33, 42, 54, 63}, point)));
  CHECK((WriteArray<float, 2>(root, subject + "/tail_base_xy", "float32",
                              {3, 2}, {11, 22, 32, 43, 53, 62}, point)));
  CHECK((WriteArray<float, 2>(root, subject + "/tail_tip_xy", "float32",
                              {3, 2}, {11, 23, 32, 44, 53, 61}, point)));
  const std::vector<float> sequences = {
      11, 21, 12, 22, 32, 42, 33, 43, 53, 63, 54, 62};
  for (const auto *name : {"centerline_xy", "bspline_sample_xy",
                           "bspline_control_points_xy", "tail_sample_xy"}) {
    CHECK((WriteArray<float, 3>(root, subject + "/" + name, "float32",
                                {3, 2, 2}, sequences, polyline)));
  }
  CHECK((WriteArray<float, 3>(root, subject + "/tail_normal_xy", "float32",
                              {3, 2, 2},
                              {1, 0, 1, 0, 0, 1, 0, 1, -1, 0, -1, 0},
                              vector_sequence)));
  const std::string bladder =
      fixture->shape_base + "/components/swim_bladder";
  CHECK((WriteArray<bool, 1>(root, bladder + "/caudal_contour_valid", "bool",
                             {3}, valid)));
  CHECK((WriteArray<float, 2>(root, bladder + "/caudal_contour_point_xy",
                              "float32", {3, 2},
                              {11, 22, 32, 43, 53, 62}, point)));

  auto &selection = fixture->selection;
  selection.archive_identity = root.string();
  selection.recording_id = "recording_fixture";
  selection.camera_id = "camera_fixture";
  selection.first_acquisition_frame = 0;
  selection.frame_count = 4;
  selection.observation_count = 3;
  selection.source_width = 100;
  selection.source_height = 80;
  selection.coordinate_surface_id = "source_camera_point_xy_v1";
  selection.coordinate_descriptor_profile =
      "source_camera_image_px.top_left_y_down.v1";
  selection.instance_key_digest = std::string(64, '1');
  selection.acquisition_frame_digest = std::string(64, '2');
  selection.frame_row_offsets_digest = std::string(64, '3');
  selection.mask.valid = true;
  selection.mask.group = "refined_subject_masks_runs";
  selection.mask.run_id = mask_run;
  selection.mask.schema_id = "palette.stage.refined_subject_mask_dense_core";
  selection.mask.schema_version = 1;
  selection.mask.identity_digest = mask_digest;
  selection.mask.manifest_payload_digest = mask_digest;
  selection.mask.bound_selector_exception = true;
  selection.shape.valid = true;
  selection.shape.group = "analysis/subject_shape_runs";
  selection.shape.run_id = shape_run;
  selection.shape.schema_id = "analysis.subject_shape_runs";
  selection.shape.schema_version = 5;
  selection.shape.identity_digest = publication_digest;
  selection.shape.manifest_payload_digest = binding_digest;
  selection.shape.bound_source_run_id = mask_run;
  selection.shape.bound_source_payload_digest = mask_digest;
  selection.shape.selector_eligible = true;
  return true;
}

bool TestBoundV5Repository() {
  TemporaryDirectory temporary;
  V5Fixture fixture;
  CHECK(!temporary.path().empty());
  CHECK(WriteV5Fixture(temporary.path(), &fixture));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(temporary.path(), &error);
  CHECK(archive != nullptr);
  crimson::zarr::BoundSubjectShapeOverlayOpenRequest request;
  request.archive = archive;
  request.selection = fixture.selection;
  crimson::zarr::SubjectShapeOverlayOpenMetrics metrics;
  auto repository = crimson::zarr::OpenBoundSubjectShapeOverlayRepository(
      request, &error, &metrics);
  CHECK(repository != nullptr);
  CHECK(metrics.exact_handle_opens == 30);
  CHECK(metrics.offset_read_calls == 1);
  CHECK(metrics.maximum_observations_per_frame == 2);
  CHECK(metrics.retained_offset_bytes >= 5 * sizeof(int64_t));
  const auto &descriptor = repository->descriptor();
  CHECK(descriptor.schema_version == 5);
  CHECK(descriptor.row_axis == "recording_subject_mask_bundle_rows");
  CHECK(descriptor.geometry_in_source_camera_coordinates);
  CHECK(descriptor.validated_instance_keys);
  CHECK(descriptor.coordinate_width == 100);
  CHECK(descriptor.coordinate_height == 80);
  CHECK(descriptor.maximum_observations_per_frame == 2);
  CHECK(descriptor.source_refined_subject_masks_run == "mask_v5_fixture");
  CHECK(descriptor.source_binding_digest ==
        fixture.selection.shape.manifest_payload_digest);
  CHECK(repository->memoryMetrics().retained_index_bytes >=
        5 * sizeof(int64_t));

  const auto frame0 = repository->resolveCameraFrame(0, 100, 80);
  CHECK(frame0.status == crimson::zarr::SubjectShapeOverlayStatus::Mapped);
  CHECK(frame0.detections.size() == 1);
  CHECK(frame0.detections[0].instance_key_valid);
  CHECK(frame0.detections[0].instance_key == 0);
  CHECK(frame0.detections[0].geometry.body_origin.x == 11.0);
  CHECK(frame0.detections[0].geometry.body_origin.y == 21.0);
  CHECK(frame0.detections[0].geometry.heading_degrees == 0.0);
  CHECK(frame0.detections[0].roi_x == 10.0);
  CHECK(frame0.detections[0].roi_y == 20.0);

  const auto frame2 = repository->resolveCameraFrame(2, 100, 80);
  CHECK(frame2.status == crimson::zarr::SubjectShapeOverlayStatus::Mapped);
  CHECK(frame2.detections.size() == 2);
  CHECK(frame2.detections[0].instance_key ==
        std::numeric_limits<uint64_t>::max() - 2);
  CHECK(frame2.detections[0].geometry.body_origin.x == 32.0);
  CHECK(frame2.detections[0].geometry.heading_degrees == 90.0);
  CHECK(frame2.detections[1].geometry.body_frame_valid);
  CHECK(!frame2.detections[1].geometry.body_axis_valid);
  CHECK(!frame2.detections[1].geometry.heading_degrees.has_value());
  CHECK(frame2.detections[1].geometry.tail_normals[0].x == -1.0);
  CHECK(repository->resolveCameraFrame(1, 100, 80).status ==
        crimson::zarr::SubjectShapeOverlayStatus::Missing);
  CHECK(repository->resolveCameraFrame(2, 99, 80).status ==
        crimson::zarr::SubjectShapeOverlayStatus::InvalidDimensions);

  auto input = crimson::zarr::makeSubjectShapeOverlaySceneInput(
      descriptor, frame0, 0, 0, 0, 100, 80);
  CHECK(input.subject_shapes.size() == 1);
  CHECK(input.subject_shapes[0].instance_key_valid);
  CHECK(input.subject_shapes[0].instance_key == 0);
  CHECK(input.subject_shapes[0].source_rect.x == 0.0);
  CHECK(input.subject_shapes[0].source_rect.y == 0.0);
  CHECK(input.subject_shapes[0].source_rect.width == 100.0);
  CHECK(input.subject_shapes[0].source_rect.height == 80.0);
  CHECK(input.subject_shapes[0].body_origin.x == 11.0);

  auto wrong_binding = request;
  wrong_binding.selection.shape.bound_source_payload_digest =
      std::string(64, 'f');
  CHECK(crimson::zarr::OpenBoundSubjectShapeOverlayRepository(
            wrong_binding, &error) == nullptr);
  auto too_narrow = request;
  too_narrow.max_observations_per_frame = 1;
  CHECK(crimson::zarr::OpenBoundSubjectShapeOverlayRepository(too_narrow,
                                                              &error) ==
        nullptr);

  CHECK((WriteArray<float, 2>(temporary.path(),
                              fixture.mask_base + "/source_crop_xywh",
                              "float32", {3, 4},
                              {10, 20, 8, 4, 30, 40, 8, 4, 50, 60, 7, 4})));
  auto changed_archive =
      crimson::zarr::ArchiveContext::Open(temporary.path(), &error);
  CHECK(changed_archive != nullptr);
  auto changed_request = request;
  changed_request.archive = changed_archive;
  auto changed_repository =
      crimson::zarr::OpenBoundSubjectShapeOverlayRepository(changed_request,
                                                            &error);
  CHECK(changed_repository != nullptr);
  CHECK(changed_repository->resolveCameraFrame(2, 100, 80).status ==
        crimson::zarr::SubjectShapeOverlayStatus::ReadFailed);

  const json roi_local = CoordinateAttributes(
      "roi_local_image_px.top_left_y_down.v1", "point_xy", "top_left",
      "not_suitable");
  CHECK(RewriteArrayAttributes(temporary.path(),
                               fixture.shape_base + "/body_frame/origin_xy",
                               roi_local));
  auto invalid_coordinate_archive =
      crimson::zarr::ArchiveContext::Open(temporary.path(), &error);
  CHECK(invalid_coordinate_archive != nullptr);
  auto invalid_coordinate_request = request;
  invalid_coordinate_request.archive = invalid_coordinate_archive;
  CHECK(crimson::zarr::OpenBoundSubjectShapeOverlayRepository(
            invalid_coordinate_request, &error) == nullptr);
  return true;
}

bool TestBoundV5FrameAssociationRejected() {
  TemporaryDirectory temporary;
  V5Fixture fixture;
  CHECK(!temporary.path().empty());
  CHECK(WriteV5Fixture(temporary.path(), &fixture));
  CHECK((WriteArray<int64_t, 1>(temporary.path(),
                                fixture.shape_base +
                                    "/source_acquisition_frame_index",
                                "int64", {3}, {0, 1, 2})));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(temporary.path(), &error);
  CHECK(archive != nullptr);
  crimson::zarr::BoundSubjectShapeOverlayOpenRequest request{archive,
                                                             fixture.selection};
  auto repository = crimson::zarr::OpenBoundSubjectShapeOverlayRepository(
      request, &error);
  CHECK(repository != nullptr);
  CHECK(repository->resolveCameraFrame(2, 100, 80).status ==
        crimson::zarr::SubjectShapeOverlayStatus::ReadFailed);
  return true;
}

bool TestBoundV5DuplicateKeysRejected() {
  TemporaryDirectory temporary;
  V5Fixture fixture;
  CHECK(!temporary.path().empty());
  CHECK(WriteV5Fixture(temporary.path(), &fixture));
  const std::vector<uint64_t> duplicates = {0, 17, 17};
  CHECK((WriteArray<uint64_t, 1>(temporary.path(),
                                 fixture.shape_base + "/instance_key",
                                 "uint64", {3}, duplicates)));
  CHECK((WriteArray<uint64_t, 1>(temporary.path(),
                                 fixture.mask_base + "/instance_key",
                                 "uint64", {3}, duplicates)));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(temporary.path(), &error);
  CHECK(archive != nullptr);
  crimson::zarr::BoundSubjectShapeOverlayOpenRequest request{archive,
                                                             fixture.selection};
  auto repository = crimson::zarr::OpenBoundSubjectShapeOverlayRepository(
      request, &error);
  CHECK(repository != nullptr);
  CHECK(repository->resolveCameraFrame(2, 100, 80).status ==
        crimson::zarr::SubjectShapeOverlayStatus::ReadFailed);
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
      !TestBoundV5Repository() ||
      !TestBoundV5FrameAssociationRejected() ||
      !TestBoundV5DuplicateKeysRejected() ||
      !TestBoundedAsyncBuffer()) {
    return 1;
  }
  std::cout << "subject_shape_overlay_repository_tests: PASS\n";
  return 0;
}
