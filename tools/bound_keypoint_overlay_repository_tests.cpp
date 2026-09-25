#include "zarr/archive_context.h"
#include "zarr/canonical_json.h"
#include "zarr/canonical_overlay_selection.h"
#include "zarr/tensorstore_bound_keypoint_overlay_repository.h"

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
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

constexpr const char *kRun = "bound_raw_v2_fixture";
constexpr size_t kFrames = 4;
constexpr size_t kRows = 3;
constexpr size_t kKeypoints = 5;
constexpr size_t kWidth = 100;
constexpr size_t kHeight = 80;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-bound-keypoint-" + std::to_string(seed) + "-" +
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

bool writeJson(const std::filesystem::path &path, const json &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value.dump(2) << '\n';
  return output.good();
}

json readJson(const std::filesystem::path &path) {
  std::ifstream input(path);
  json value;
  input >> value;
  return value;
}

template <typename T, size_t Rank>
bool writeArray(const std::filesystem::path &root, const std::string &path,
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
  json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata",
       {{"shape", shape_json},
        {"data_type", data_type},
        {"chunk_grid",
         {{"name", "regular"},
          {"configuration", {{"chunk_shape", chunk_shape}}}}},
        {"chunk_key_encoding",
         {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", 0},
        {"codecs", json::array({bytes_codec})}}}};
  auto store = ts::Open<T, Rank>(
                   spec, ts::OpenMode::open | ts::OpenMode::create,
                   ts::ReadWriteMode::read_write)
                   .result();
  if (!store.ok()) {
    std::cerr << "Array create failed: " << path << ": " << store.status()
              << '\n';
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

template <size_t Rank>
bool writeBoolArray(const std::filesystem::path &root,
                    const std::string &path,
                    const std::array<ts::Index, Rank> &shape,
                    const std::vector<uint8_t> &values) {
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
  json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata",
       {{"shape", shape_json},
        {"data_type", "bool"},
        {"chunk_grid",
         {{"name", "regular"},
          {"configuration", {{"chunk_shape", chunk_shape}}}}},
        {"chunk_key_encoding",
         {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", false},
        {"codecs", json::array({{{"name", "bytes"}}})}}}};
  auto store = ts::Open<bool, Rank>(
                   spec, ts::OpenMode::open | ts::OpenMode::create,
                   ts::ReadWriteMode::read_write)
                   .result();
  if (!store.ok()) {
    std::cerr << "Boolean array create failed: " << path << ": "
              << store.status() << '\n';
    return false;
  }
  auto source = ts::AllocateArray<bool>(shape);
  for (size_t index = 0; index < values.size(); ++index) {
    source.data()[index] = values[index] != 0;
  }
  return ts::Write(source, *store).commit_future.result().ok();
}

json rawBindings() {
  return json::array(
      {{{"path", "instance_key"},
        {"contract_id", "palette.array.keypoint.instance_key"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "source_crop_row_ids"},
        {"contract_id", "palette.array.keypoint.source_crop_row_ids"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "source_acquisition_frame_index"},
        {"contract_id",
         "palette.array.keypoint.source_acquisition_frame_index"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "frame_indices"},
        {"contract_id", "palette.array.keypoint.frame_indices"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "frame_row_offsets"},
        {"contract_id", "palette.array.frame_row_offsets"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "source_crop_row_signature"},
        {"contract_id", "palette.array.keypoint.source_crop_row_signature"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "keypoint_row_signature"},
        {"contract_id", "palette.array.keypoint.keypoint_row_signature"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "keypoints_roi"},
        {"contract_id", "palette.array.keypoints_roi"},
        {"contract_version", 2},
        {"required", true}},
       {{"path", "keypoints_img"},
        {"contract_id", "palette.array.keypoints_img"},
        {"contract_version", 2},
        {"required", true}},
       {{"path", "keypoint_confidences"},
        {"contract_id", "palette.array.keypoint.keypoint_confidences"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "keypoint_valid"},
        {"contract_id", "palette.array.keypoint.keypoint_valid"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "pose_confidence"},
        {"contract_id", "palette.array.keypoint.pose_confidence"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "pose_bbox_xyxy_roi"},
        {"contract_id", "palette.array.keypoint.pose_bbox_xyxy_roi"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "pose_bbox_xyxy_img"},
        {"contract_id", "palette.array.keypoint.pose_bbox_xyxy_img"},
        {"contract_version", 1},
        {"required", true}},
       {{"path", "pose_success"},
        {"contract_id", "palette.array.keypoint.pose_success"},
        {"contract_version", 1},
        {"required", true}}});
}

json coordinateContract() {
  json document = {
      {"schema_id", "palette.array_coordinate_catalog"},
      {"schema_version", 1},
      {"bindings",
       {{{"array_contract_id", "palette.array.keypoints_img"},
         {"array_contract_version", 2},
         {"surface_id", "source_camera_point_xy_v1"},
         {"semantic_role", "exact_derived_numeric_surface"}}}},
      {"surfaces",
       {{{"surface_id", "source_camera_point_xy_v1"},
         {"domain_id", "source_camera_image_px"},
         {"geometry_type", "point_xy"},
         {"components", {"x", "y"}},
         {"component_units", {"px", "px"}},
         {"pixel_convention", "continuous"},
         {"source_camera_mapping", "direct_source_camera_continuous_pixels"},
         {"descriptor_profile_id",
          "source_camera_image_px.top_left_y_down.v1"},
         {"descriptor_overlay_status", "direct"}}}}};
  return {{"schema_id", "palette.persisted_coordinate_catalog"},
          {"schema_version", 1},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"digest", crimson::zarr::CanonicalJsonSha256(document)},
          {"document", std::move(document)}};
}

json rawManifest(const std::string &metadata_digest) {
  const json forbidden = {
      "confidence",          "detection_indices",
      "detection_success",   "frame_counts",
      "heading",             "heading_delta_next_deg",
      "heading_delta_prev_deg", "heading_finite",
      "heading_temporal_outlier", "heading_usable",
      "keypoints_norm",      "n_keypoints",
      "n_rois",              "quality_labels",
      "triangle_angles",     "triangle_angles_raw",
      "triangle_area"};
  const json labels = {"swim_bladder", "eye_left", "eye_right",
                       "snout_tip", "tail_tip"};
  const json nodes = {{{"id", 0}, {"name", labels[0]}},
                      {{"id", 1}, {"name", labels[1]}},
                      {{"id", 2}, {"name", labels[2]}},
                      {{"id", 3}, {"name", labels[3]}},
                      {{"id", 4}, {"name", labels[4]}}};
  json logical_document = {
      {"schema_id", "palette.keypoint.logical_content"},
      {"schema_version", 1},
      {"skeleton_digest", std::string(64, '5')},
      {"arrays",
       {{"instance_key",
         {{"shape", {kRows}},
          {"dtype", "uint64"},
          {"digest_algorithm", "sha256_c_contiguous_bytes_v1"},
          {"sha256", std::string(64, '1')}}},
        {"source_acquisition_frame_index",
         {{"shape", {kRows}},
          {"dtype", "int64"},
          {"digest_algorithm", "sha256_c_contiguous_bytes_v1"},
          {"sha256", std::string(64, '2')}}},
        {"frame_row_offsets",
         {{"shape", {kFrames + 1}},
          {"dtype", "int64"},
          {"digest_algorithm", "sha256_c_contiguous_bytes_v1"},
          {"sha256", std::string(64, '3')}}},
        {"keypoint_row_signature",
         {{"shape", {kRows, 32}},
          {"dtype", "uint8"},
          {"digest_algorithm", "sha256_c_contiguous_bytes_v1"},
          {"sha256", std::string(64, '4')}}}}}};
  json logical_content = {
      {"digest_algorithm", "sha256_canonical_json_v1"},
      {"digest", crimson::zarr::CanonicalJsonSha256(logical_document)},
      {"document", std::move(logical_document)}};
  json payload = {
      {"run_id", kRun},
      {"stage", "keypoints"},
      {"publication",
       {{"artifact_class", "raw_keypoint_observations"},
        {"completion_contract", "palette.zarr_run_completion.v1"},
        {"completion_status", "complete"},
        {"stage_selector_eligible", false},
        {"keypoint_authority", false},
        {"metadata_state", "direct_and_consolidated_validated"},
        {"metadata_declarations_digest_scope",
         "exact_group_and_array_declarations_with_attributes_redacting_only_"
         "run_manifest"},
        {"metadata_declarations_digest_algorithm",
         "sha256_canonical_json_v1"},
        {"metadata_declarations_digest", metadata_digest}}},
      {"logical_schema",
       {{"schema_id", "palette.stage.keypoint_observations"},
        {"schema_version", 2},
        {"stage", "keypoints"},
        {"layout", "sparse_observations_with_frame_row_offsets_v2"},
        {"base_path", "keypoints_runs/<run>"},
        {"bindings", rawBindings()},
        {"dimensions",
         {{"n_frames", kFrames},
          {"n_frame_boundaries", kFrames + 1},
          {"n_instances", kRows},
          {"n_keypoints", kKeypoints},
          {"source_width", kWidth},
          {"source_height", kHeight}}},
        {"invariants",
         {{"instances_per_frame", "zero_one_or_many"},
          {"frame_lookup", "frame_row_offsets_csr"},
          {"heading_payload", "forbidden_use_bound_body_frame_run"},
          {"instance_key_semantics",
           "observation_identity_not_subject_identity"}}},
        {"forbidden_v1_arrays", forbidden}}},
      {"storage_plan", json::object()},
      {"source_crop_snapshot", json::object()},
      {"pose_model_schema_binding",
       {{"pose_schema",
         {{"skeleton_id", "pose_skel_traditional_v2"},
          {"nodes", nodes},
          {"edges", {{0, 1}, {0, 2}, {1, 2}, {1, 3}, {2, 3}, {0, 4}}}}}}},
      {"preprocessing", json::object()},
      {"logical_content", std::move(logical_content)},
      {"coordinate_contract", coordinateContract()}};
  return {{"schema_id", "palette.keypoint.run_manifest"},
          {"schema_version", 1},
          {"persisted_attribute", "run_manifest"},
          {"persisted_path",
           "keypoints_runs/<run>/zarr.json.attributes.run_manifest"},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"payload_digest", crimson::zarr::CanonicalJsonSha256(payload)},
          {"payload", std::move(payload)}};
}

std::vector<std::string> arrayPaths() {
  return {"instance_key",
          "source_crop_row_ids",
          "source_acquisition_frame_index",
          "frame_indices",
          "frame_row_offsets",
          "source_crop_row_signature",
          "keypoint_row_signature",
          "keypoints_roi",
          "keypoints_img",
          "keypoint_confidences",
          "keypoint_valid",
          "pose_confidence",
          "pose_bbox_xyxy_roi",
          "pose_bbox_xyxy_img",
          "pose_success"};
}

std::vector<std::string> coordinateSuccessorPaths() {
  return {"coordinate_frames", "keypoints_norm"};
}

bool buildFixture(const std::filesystem::path &root, bool duplicate_keys) {
  std::filesystem::create_directories(root);
  const std::string base = std::string("keypoints_runs/") + kRun;
  const uint64_t high_key = std::numeric_limits<uint64_t>::max() - 7;
  CHECK((writeArray<uint64_t, 1>(
      root, base + "/instance_key", "uint64", {kRows},
      duplicate_keys ? std::vector<uint64_t>{0, 0, 9}
                     : std::vector<uint64_t>{0, high_key, 9})));
  CHECK((writeArray<int64_t, 1>(root, base + "/source_crop_row_ids", "int64",
                                {kRows}, {10, 11, 12})));
  CHECK((writeArray<int64_t, 1>(
      root, base + "/source_acquisition_frame_index", "int64", {kRows},
      {1, 1, 2})));
  CHECK((writeArray<int64_t, 1>(root, base + "/frame_indices", "int64",
                                {kRows}, {1, 1, 2})));
  CHECK((writeArray<int64_t, 1>(root, base + "/frame_row_offsets", "int64",
                                {kFrames + 1}, {0, 0, 2, 3, 3})));
  CHECK((writeArray<uint8_t, 2>(root, base + "/source_crop_row_signature",
                                "uint8", {kRows, 32},
                                std::vector<uint8_t>(kRows * 32, 1))));
  CHECK((writeArray<uint8_t, 2>(root, base + "/keypoint_row_signature",
                                "uint8", {kRows, 32},
                                std::vector<uint8_t>(kRows * 32, 2))));
  std::vector<float> points(kRows * kKeypoints * 2);
  for (size_t index = 0; index < points.size(); ++index) {
    points[index] = static_cast<float>(index + 1);
  }
  CHECK((writeArray<float, 3>(root, base + "/keypoints_roi", "float32",
                              {kRows, kKeypoints, 2}, points)));
  CHECK((writeArray<float, 3>(root, base + "/keypoints_img", "float32",
                              {kRows, kKeypoints, 2}, points)));
  std::vector<float> confidences(kRows * kKeypoints);
  for (size_t index = 0; index < confidences.size(); ++index) {
    confidences[index] = static_cast<float>(index) / 20.0f;
  }
  CHECK((writeArray<float, 2>(root, base + "/keypoint_confidences",
                              "float32", {kRows, kKeypoints}, confidences)));
  std::vector<uint8_t> valid(kRows * kKeypoints, 1);
  valid[1] = 0;
  CHECK((writeBoolArray<2>(root, base + "/keypoint_valid",
                           {kRows, kKeypoints}, valid)));
  CHECK((writeArray<float, 1>(root, base + "/pose_confidence", "float32",
                              {kRows}, {0.25f, 0.5f, 0.75f})));
  CHECK((writeArray<float, 2>(root, base + "/pose_bbox_xyxy_roi", "float32",
                              {kRows, 4}, std::vector<float>(kRows * 4, 1))));
  CHECK((writeArray<float, 2>(root, base + "/pose_bbox_xyxy_img", "float32",
                              {kRows, 4}, std::vector<float>(kRows * 4, 2))));
  CHECK((writeBoolArray<1>(root, base + "/pose_success", {kRows},
                           {1, 0, 1})));
  CHECK(writeJson(root / base / "coordinate_frames/zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", json::object()}}));
  CHECK((writeArray<float, 3>(root, base + "/keypoints_norm", "float32",
                              {kRows, kKeypoints, 2}, points)));
  auto successor = readJson(root / base / "keypoints_norm/zarr.json");
  successor["attributes"] = {
      {"coordinate_successor_auxiliary", true},
      {"coordinate_successor_auxiliary_policy",
       "successor_owned_derived_coordinates_not_keypoint_v2_logical_payload_"
       "v2"}};
  CHECK(writeJson(root / base / "keypoints_norm/zarr.json", successor));

  json group = {{"zarr_format", 3},
                {"node_type", "group"},
                {"attributes", {{"run_manifest", rawManifest(std::string(64, '0'))}}}};
  CHECK(writeJson(root / base / "zarr.json", group));
  json declarations = json::object();
  json normalized_group = group;
  normalized_group["attributes"].erase("run_manifest");
  declarations[""] = normalized_group;
  for (const auto &path : arrayPaths()) {
    declarations[path] = readJson(root / base / path / "zarr.json");
  }
  const json digest_document = {
      {"scope",
       "exact_group_and_array_declarations_with_attributes_redacting_only_"
       "run_manifest"},
      {"declarations", declarations}};
  group["attributes"]["run_manifest"] =
      rawManifest(crimson::zarr::CanonicalJsonSha256(digest_document));
  CHECK(writeJson(root / base / "zarr.json", group));

  json consolidated = json::object();
  consolidated[base] = group;
  for (const auto &path : arrayPaths()) {
    consolidated[base + "/" + path] =
        readJson(root / base / path / "zarr.json");
  }
  for (const auto &path : coordinateSuccessorPaths()) {
    consolidated[base + "/" + path] =
        readJson(root / base / path / "zarr.json");
  }
  const json root_metadata = {
      {"zarr_format", 3},
      {"node_type", "group"},
      {"attributes",
       {{"recording_id", "recording_fixture"},
        {"camera_id", "93"},
        {"source_video_metadata",
         {{"total_frames", kFrames},
          {"width", kWidth},
          {"height", kHeight}}}}},
      {"consolidated_metadata",
       {{"kind", "inline"},
        {"must_understand", false},
        {"metadata", std::move(consolidated)}}}};
  return writeJson(root / "zarr.json", root_metadata);
}

crimson::zarr::CanonicalOverlaySelection
makeSelection(const std::filesystem::path &root) {
  const json group =
      readJson(root / "keypoints_runs" / kRun / "zarr.json");
  const json &manifest = group.at("attributes").at("run_manifest");
  crimson::zarr::CanonicalOverlaySelection selection;
  selection.archive_identity = root.lexically_normal().string();
  selection.recording_id = "recording_fixture";
  selection.camera_id = "93";
  selection.first_acquisition_frame = 0;
  selection.frame_count = kFrames;
  selection.observation_count = kRows;
  selection.source_width = kWidth;
  selection.source_height = kHeight;
  selection.coordinate_surface_id = "source_camera_point_xy_v1";
  selection.coordinate_descriptor_profile =
      "source_camera_image_px.top_left_y_down.v1";
  selection.keypoint_labels = {"swim_bladder", "eye_left", "eye_right",
                               "snout_tip", "tail_tip"};
  selection.eye.valid = true;
  selection.eye.group = "analysis/eye_angle_runs";
  selection.eye.run_id = "eye_fixture";
  selection.eye.schema_id = "analysis.eye_angle_runs";
  selection.eye.schema_version = 7;
  selection.eye.identity_digest = std::string(64, 'e');
  selection.eye.selector_eligible = true;
  selection.keypoints.valid = true;
  selection.keypoints.group = "keypoints_runs";
  selection.keypoints.run_id = kRun;
  selection.keypoints.schema_id = "palette.stage.keypoint_observations";
  selection.keypoints.schema_version = 2;
  selection.keypoints.identity_digest =
      crimson::zarr::CanonicalJsonSha256(manifest);
  selection.keypoints.manifest_payload_digest =
      manifest.at("payload_digest").get<std::string>();
  selection.keypoints.selector_eligible = false;
  selection.keypoints.bound_selector_exception = true;
  selection.keypoints.bound_source_run_id = selection.eye.run_id;
  return selection;
}

bool testSparseRawV2ReadAndIdentity() {
  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto root = temporary.path() / "analysis.zarr";
  CHECK(buildFixture(root, false));
  std::string error;
  const auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  crimson::zarr::BoundKeypointOverlayOpenRequest request;
  request.archive = archive;
  request.selection = makeSelection(root);
  request.max_rows_per_frame = 4;
  request.max_frame_payload_bytes = 4096;
  crimson::zarr::BoundKeypointOverlayOpenMetrics open_metrics;
  auto repository = crimson::zarr::OpenBoundKeypointOverlayRepository(
      request, &error, &open_metrics);
  CHECK(repository != nullptr);
  CHECK(error.empty());
  CHECK(open_metrics.offset_read_calls == 1);
  CHECK(open_metrics.exact_handle_opens == 9);
  CHECK(open_metrics.maximum_rows_in_frame == 2);
  CHECK(repository->descriptor().stable_instance_keys);
  CHECK(repository->descriptor().recording_id == "recording_fixture");
  CHECK(repository->descriptor().coordinate_space ==
        crimson::zarr::KeypointCoordinateSpace::Image);

  const auto empty = repository->resolveCameraFrame(0, kWidth, kHeight);
  CHECK(empty.status == crimson::zarr::KeypointOverlayStatus::Missing);
  const auto frame = repository->resolveCameraFrame(1, kWidth, kHeight);
  CHECK(frame.status == crimson::zarr::KeypointOverlayStatus::Mapped);
  CHECK(frame.detections.size() == 2);
  CHECK(frame.detections[0].instance_key == 0);
  CHECK(frame.detections[0].instance_key_valid);
  CHECK(frame.detections[1].instance_key ==
        std::numeric_limits<uint64_t>::max() - 7);
  CHECK(frame.detections[1].instance_key_valid);
  CHECK(frame.detections[0].acquisition_frame == 1);
  CHECK(frame.detections[0].source_crop_row_id == 10);
  CHECK(frame.detections[0].source_success);
  CHECK(!frame.detections[1].source_success);
  CHECK(frame.detections[0].keypoint_valid.size() == kKeypoints);
  CHECK(frame.detections[0].keypoint_valid[1] == 0);
  CHECK(frame.detections[0].keypoints[1].x == 3.0);
  CHECK(frame.detections[0].keypoints[1].y == 4.0);
  CHECK(frame.detections[0].keypoint_confidences[1] == 0.05f);
  CHECK(!frame.detections[0].heading_degrees.has_value());
  CHECK(!frame.detections[0].heading_valid);

  const auto wrong_dimensions =
      repository->resolveCameraFrame(1, kWidth + 1, kHeight);
  CHECK(wrong_dimensions.status ==
        crimson::zarr::KeypointOverlayStatus::InvalidDimensions);
  const auto out_of_range =
      repository->resolveCameraFrame(kFrames, kWidth, kHeight);
  CHECK(out_of_range.status ==
        crimson::zarr::KeypointOverlayStatus::OutOfRange);
  const auto access = repository->accessMetrics();
  CHECK(access.frame_requests == 4);
  CHECK(access.rows_resolved == 2);
  CHECK(access.payload_read_batches == 1);
  CHECK(access.payload_read_calls == 8);
  CHECK(access.decoded_payload_bytes > 0);
  return true;
}

bool testDuplicateAndAdmissionFailures() {
  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto root = temporary.path() / "analysis.zarr";
  CHECK(buildFixture(root, true));
  std::string error;
  const auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  crimson::zarr::BoundKeypointOverlayOpenRequest request;
  request.archive = archive;
  request.selection = makeSelection(root);
  request.max_rows_per_frame = 4;
  request.max_frame_payload_bytes = 4096;
  auto repository = crimson::zarr::OpenBoundKeypointOverlayRepository(
      request, &error);
  CHECK(repository != nullptr);
  const auto duplicate = repository->resolveCameraFrame(1, kWidth, kHeight);
  CHECK(duplicate.status == crimson::zarr::KeypointOverlayStatus::ReadFailed);
  CHECK(duplicate.error.find("duplicate") != std::string::npos);

  request.max_rows_per_frame = 1;
  error.clear();
  CHECK(!crimson::zarr::OpenBoundKeypointOverlayRepository(request, &error));
  CHECK(error.find("admission") != std::string::npos);

  request.max_rows_per_frame = 4;
  request.selection.keypoints.bound_source_run_id = "unvalidated_eye_parent";
  error.clear();
  CHECK(!crimson::zarr::OpenBoundKeypointOverlayRepository(request, &error));
  CHECK(error.find("request is invalid") != std::string::npos);

  request.selection.keypoints.bound_source_run_id =
      request.selection.eye.run_id;
  request.selection.keypoints.identity_digest = std::string(64, 'f');
  error.clear();
  CHECK(!crimson::zarr::OpenBoundKeypointOverlayRepository(request, &error));
  CHECK(error.find("identity changed") != std::string::npos);
  return true;
}

} // namespace

int main() {
  if (!testSparseRawV2ReadAndIdentity() ||
      !testDuplicateAndAdmissionFailures()) {
    return 1;
  }
  std::cout << "bound_keypoint_overlay_repository_tests: PASS\n";
  return 0;
}
