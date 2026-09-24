#include "zarr/archive_context.h"
#include "zarr/canonical_json.h"
#include "zarr/tensorstore_bound_eye_geometry_overlay_repository.h"

#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;
using namespace crimson::zarr;

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "CHECK failed: " #condition << " at " << __LINE__ << '\n'; \
  return false; } } while (false)

struct Temp {
  std::filesystem::path path;
  Temp() {
    path = std::filesystem::temp_directory_path() /
        ("crimson-bound-eye-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }
  ~Temp() { std::error_code e; std::filesystem::remove_all(path, e); }
};

bool writeJson(const std::filesystem::path &path, const json &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << value.dump(2) << '\n';
  return out.good();
}

template <typename T, size_t R>
bool writeArray(const std::filesystem::path &root, const std::string &path,
                const std::string &type, const std::array<ts::Index, R> &shape,
                const std::vector<T> &values) {
  json dims = json::array(); size_t count = 1;
  for (auto extent : shape) { dims.push_back(extent); count *= extent; }
  if (count != values.size()) return false;
  json bytes = {{"name", "bytes"}};
  if (sizeof(T) > 1) bytes["configuration"] = {{"endian", "little"}};
  json spec = {{"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path}, {"metadata", {{"shape", dims}, {"data_type", type},
          {"chunk_grid", {{"name", "regular"},
               {"configuration", {{"chunk_shape", dims}}}}},
          {"chunk_key_encoding", {{"name", "default"},
               {"configuration", {{"separator", "/"}}}}},
          {"fill_value", std::is_same_v<T, bool> ? json(false) : json(0)},
          {"codecs", json::array({bytes})}}}};
  auto opened = ts::Open<T, R>(spec, ts::OpenMode::open | ts::OpenMode::create,
                               ts::ReadWriteMode::read_write).result();
  if (!opened.ok()) { std::cerr << opened.status() << '\n'; return false; }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *opened).commit_future.result().ok();
}

struct Fixture {
  Temp temp;
  CanonicalOverlaySelection selection;
  std::string eye = "analysis/eye_angle_runs/eye_fixture";
  std::string shape = "analysis/subject_shape_runs/shape_fixture";
  std::string mask = "refined_subject_masks_runs/mask_fixture";
  std::string keypoint = "keypoints_runs/keypoint_fixture";
  bool mismatch_eye_key = false;
  bool mismatch_shape_frame = false;
  bool invalid_axis_order = false;
  bool invalid_parameter_order = false;
  bool duplicate_instance_key = false;
  bool missing_named_channel = false;
  bool unavailable_named_channel = false;
  bool invalid_body_frame = false;
  bool nonfinite_body_origin = false;
  bool nonfinite_gaze = false;

  bool create() {
    CHECK(writeJson(temp.path / "zarr.json",
                    {{"zarr_format", 3}, {"node_type", "group"}}));
    constexpr const char *hash =
        "1111111111111111111111111111111111111111111111111111111111111111";
    constexpr const char *mask_byte_digest =
        "3333333333333333333333333333333333333333333333333333333333333333";
    selection.eye = {true, "", "analysis/eye_angle_runs", "eye_fixture",
                     "analysis.eye_angle_runs", 7, hash};
    selection.eye.manifest_payload_digest = hash;
    selection.shape = {true, "", "analysis/subject_shape_runs", "shape_fixture",
                       "analysis.subject_shape_runs", 5};
    selection.mask = {true, "", "refined_subject_masks_runs", "mask_fixture",
                      "palette.stage.refined_subject_mask_dense_core", 1};
    selection.keypoints = {true, "", "keypoints_runs", "keypoint_fixture",
                           "palette.stage.keypoint_observations", 2};
    selection.shape.bound_source_run_id = "mask_fixture";
    selection.recording_id = "recording_fixture";
    selection.camera_id = "camera_fixture";
    selection.frame_count = 3;
    selection.observation_count = 3;
    selection.source_width = 400;
    selection.source_height = 200;
    selection.coordinate_descriptor_profile =
        "source_camera_image_px.top_left_y_down.v1";
    // The strict-mask manifest and row-identity contract use different digest
    // canonicalizations for the same rows.
    selection.instance_key_digest = mask_byte_digest;
    selection.acquisition_frame_digest = mask_byte_digest;
    json row_identity = {{"unique", true}, {"key_arrays", json::array({
        {{"content_sha256", hash}}})}};
    const std::string row_digest = CanonicalJsonSha256(row_identity);
    json binding = {{"frame_axis", {{"source_total_frames", 3}}},
                    {"roi_raster_extent", {{"width_px", 80},
                                            {"height_px", 40}}}};
    selection.shape.manifest_payload_digest = CanonicalJsonSha256(binding);
    selection.shape.identity_digest = hash;
    json shape_attrs = {{"publication_manifest_sha256", hash},
        {"subject_shape_source_binding_sha256", selection.shape.manifest_payload_digest},
        {"subject_shape_source_binding", binding},
        {"row_identity_contract", row_identity},
        {"row_identity_contract_sha256", row_digest},
        {"source_row_temporal_authority", {
            {"source_acquisition_frame_index", {{"content_sha256", hash}}}}}};
    CHECK(writeJson(temp.path / shape / "zarr.json",
        {{"zarr_format", 3}, {"node_type", "group"},
         {"attributes", shape_attrs}}));
    const std::string row_ref = "/" + shape + "@row_identity_contract";
    json descriptors = json::object();
    json allowed = json::object();
    json components = json::array();
    for (const std::string component : {"eye_left", "eye_right"}) {
      const std::string relative = "components/" + component + "/ellipse_params";
      const std::string params = shape + "/" + relative;
      const std::string success = shape + "/components/" + component +
                                  "/ellipse_success";
      json descriptor = {{"schema_id", "palette.coordinate_descriptor"},
          {"schema_version", 2},
          {"profile_id", selection.coordinate_descriptor_profile},
          {"space_id", "source_camera_image_px"},
          {"geometry_type", "ellipse_cxcy_wh_angle"},
          {"components", {"center_x", "center_y", "width", "height", "angle"}},
          {"component_units", {"px", "px", "px", "px", "deg"}},
          {"origin", "top_left"},
          {"positive_directions", {{"x", "right"}, {"y", "down"}}},
          {"reference_extent", {{"width", 400}, {"height", 200}}},
          {"row_identity", {{"record_ref", row_ref},
                            {"record_sha256", row_digest}}},
          {"source_camera_overlay", {{"status", "direct"}}}};
      const std::string digest = CanonicalJsonSha256(descriptor);
      const std::vector<float> ell = component == "eye_left" ?
          std::vector<float>{110, 25, invalid_axis_order ? 5.0f : 20.0f,
                             10, 0, 210, 35, 20, 10, 90,
                             310, 45, 20, 10, 45} :
          std::vector<float>{130, 25, 18, 9, 0, 230, 35, 18, 9, 90,
                             330, 45, 18, 9, 45};
      CHECK((writeArray<float, 2>(temp.path, params, "float32", {3, 5}, ell)));
      CHECK((writeArray<bool, 1>(temp.path, success, "bool", {3},
                                 {true, true, false})));
      std::ifstream array_stream(temp.path / params / "zarr.json");
      json array_meta;
      array_stream >> array_meta;
      array_meta["attributes"] = {{"coordinate_descriptor", descriptor},
                                    {"coordinate_descriptor_sha256", digest}};
      CHECK(writeJson(temp.path / params / "zarr.json", array_meta));
      descriptors[relative] = {{"record_ref", "/" + params +
                                 "@coordinate_descriptor"},
                               {"descriptor_sha256", digest}};
      allowed[relative] = {{"array_ref", "/" + params}, {"dtype", "<f4"},
          {"shape", {3, 5}}, {"content_sha256", hash}};
      allowed["components/" + component + "/ellipse_success"] = {
          {"array_ref", "/" + success}, {"dtype", "|b1"},
          {"shape", {3}}, {"content_sha256", hash}};
      components.push_back({{"component", component},
          {"ellipse_params_path", params}, {"ellipse_success_path", success}});
    }
    json authority = {{"source_subject_shape_run", "shape_fixture"},
        {"source_subject_shape_run_ref", "/" + shape},
        {"authority_scope", "eye_geometry_exact_digest_bound_staged_subset_only"},
        {"row_count", 3}, {"allowed_arrays", allowed},
        {"canonical_publication", {{"manifest_sha256", hash},
            {"row_identity_ref", row_ref},
            {"row_identity_sha256", row_digest},
            {"ellipse_coordinate_descriptors", descriptors}}}};
    json contract = {{"schema_id", "analysis.eye_angle_algorithm_contract"},
        {"schema_version", 1},
        {"ellipse_input", {{"parameter_order", {"center_x_px", "center_y_px",
            "major_axis_length_px", "minor_axis_length_px", "major_axis_angle_deg"}},
            {"parameter_normalization", "cv2.fitEllipse axes reordered so major >= minor and major-axis angle normalized to [0, 180) degrees"},
            {"component_sources", components}}},
        {"body_frame", {{"coordinate_space", "roi_pixels"}}}};
    if (invalid_parameter_order)
      contract["ellipse_input"]["parameter_order"][2] =
          "minor_axis_length_px";
    json eye_attrs = {{"palette_run_name", "eye_fixture"},
        {"lineage_hash", hash}, {"source_fingerprint", hash},
        {"source_lineage_hash", hash},
        {"staged_input_integrity_receipt_sha256", hash},
        {"source_subject_shape_run", "shape_fixture"},
        {"source_eye_geometry_run", "shape_fixture"},
        {"source_eye_geometry_stage", "analysis/subject_shape_runs"},
        {"source_geometry_kind", "subject_shape_eye_geometry"},
        {"source_refined_subject_masks_run", "mask_fixture"},
        {"source_base_keypoints_run", "keypoint_fixture"},
        {"palette_run_completion_status", "complete"},
        {"source_eye_geometry_authority_mode", "digest_bound_staged_subset"},
        {"body_frame_coordinate_space", "roi_pixels"},
        {"eye_angle_algorithm_contract", contract},
        {"eye_angle_source_contracts", {
            {"eye_geometry", {{"path", shape}, {"schema_version", 5},
                              {"source_authority", authority}}},
            {"keypoints", {{"canonical_keypoint_authority", {
                {"ordered_row_alignment", {
                {"shared_instance_key_content_sha256", hash},
                {"shared_frame_index_content_sha256", hash}}}}}}}}},
        {"eye_angle_array_schema", {{"dimensions", {{"n_roi_rows", 3},
            {"n_frames", 3}, {"n_angle_channels", 5},
            {"n_vector_channels", 2}, {"n_qa_channels", 3}}}}}};
    CHECK(writeJson(temp.path / eye / "zarr.json",
        {{"zarr_format", 3}, {"node_type", "group"},
         {"attributes", eye_attrs}}));
    CHECK((writeArray<int64_t, 1>(temp.path, mask + "/frame_row_offsets",
                                  "int64", {4}, {0, 2, 2, 3})));
    for (const std::string base : {eye + "/support", shape, mask}) {
      const std::string frame_path = base == eye + "/support" ?
          "/frame_indices" : "/source_acquisition_frame_index";
      CHECK((writeArray<int64_t, 1>(temp.path, base + frame_path,
                                    "int64", {3},
          base == shape && mismatch_shape_frame ?
            std::vector<int64_t>{0, 1, 2} : std::vector<int64_t>{0, 0, 2})));
      CHECK((writeArray<uint64_t, 1>(temp.path, base + "/instance_key",
                                     "uint64", {3},
          base == eye + "/support" && mismatch_eye_key ?
            std::vector<uint64_t>{22, 99, 33} :
            duplicate_instance_key ? std::vector<uint64_t>{22, 22, 33} :
            std::vector<uint64_t>{22, 11, 33})));
    }
    CHECK((writeArray<int64_t, 1>(temp.path, eye +
        "/support/source_acquisition_frame_index", "int64", {3}, {0, 0, 2})));
    for (const std::string base : {shape, mask})
      CHECK((writeArray<int64_t, 1>(temp.path, base + "/source_crop_row_ids",
                                    "int64", {3}, {2, 1, 3})));
    CHECK((writeArray<float, 2>(temp.path, mask + "/source_crop_xywh",
        "float32", {3, 4}, {100, 10, 80, 40, 200, 20, 80, 40,
                            300, 30, 80, 40})));
    CHECK((writeArray<bool, 1>(temp.path, eye + "/support/body_frame/valid",
                               "bool", {3},
         invalid_body_frame ? std::vector<bool>{false, true, true} :
                              std::vector<bool>{true, true, true})));
    for (const auto &item : std::array<std::pair<std::string, std::vector<float>>, 3>{{
        {"origin_xy", {nonfinite_body_origin ?
            std::numeric_limits<float>::quiet_NaN() : 30.0f,
            20, 30, 20, 30, 20}},
        {"forward_axis_xy", {1, 0, 1, 0, 1, 0}},
        {"left_axis_xy", {0, 1, 0, 1, 0, 1}}}})
      CHECK((writeArray<float, 2>(temp.path, eye + "/support/body_frame/" +
          item.first, "float32", {3, 2}, item.second)));
    const std::array<std::pair<std::string, std::vector<std::string>>, 3> channels{{
        {"angle_channel_index", {"right_gaze_signed_deg", "left_eye_angle_deg",
            "vergence_eye_angle_deg", "right_eye_angle_deg", "left_gaze_signed_deg"}},
        {"vector_channel_index", {"right_gaze_xy", "left_gaze_xy"}},
        {"qa_channel_index", {"valid_right", "valid_frame", "valid_left"}}}};
    for (const auto &item : channels) {
      auto labels = item.second;
      if (missing_named_channel && item.first == "angle_channel_index")
        labels[1] = "unknown_left_eye_angle";
      std::vector<uint8_t> packed(item.second.size() * 256);
      for (size_t i = 0; i < labels.size(); ++i)
        std::copy(labels[i].begin(), labels[i].end(),
                  packed.begin() + i * 256);
      CHECK((writeArray<uint8_t, 2>(temp.path, eye + "/" + item.first + "/name",
             "uint8", {static_cast<ts::Index>(item.second.size()), 256}, packed)));
      CHECK((writeArray<bool, 1>(temp.path, eye + "/" + item.first +
             "/roi_available", "bool", {static_cast<ts::Index>(item.second.size())},
             unavailable_named_channel && item.first == "angle_channel_index" ?
               std::vector<bool>{true, false, true, true, true} :
               std::vector<bool>(item.second.size(), true))));
    }
    CHECK((writeArray<float, 2>(temp.path, eye + "/roi_angles", "float32",
        {3, 5}, {-15, 10, -2, -12, 14, -16, 11, -2, -13, 15,
                 -17, 12, -2, -14, 16})));
    CHECK((writeArray<float, 3>(temp.path, eye + "/roi_vectors", "float32",
        {3, 2, 2}, {-1, 0,
          nonfinite_gaze ? std::numeric_limits<float>::quiet_NaN() : 1.0f,
          0, -1, 0, 1, 0, -1, 0, 1, 0})));
    CHECK((writeArray<uint16_t, 2>(temp.path, eye + "/roi_qa", "uint16",
        {3, 3}, {1, 1, 1, 1, 1, 0, 1, 1, 1})));
    return true;
  }

  std::unique_ptr<EyeGeometryOverlayRepository> openReader(std::string *error,
      size_t max_rows = 16) {
    auto archive = ArchiveContext::Open(temp.path, error);
    if (!archive) return {};
    BoundEyeGeometryOverlayOpenRequest request{archive, selection};
    request.max_observations_per_frame = max_rows;
    return OpenBoundEyeGeometryOverlayRepository(request, error);
  }
};

bool testBoundReader() {
  Fixture f;
  CHECK(f.create());
  std::string error;
  auto reader = f.openReader(&error);
  if (!reader) std::cerr << error << '\n';
  CHECK(reader != nullptr);
  const auto &d = reader->descriptor();
  CHECK(d.validated_instance_keys && d.coordinate_width == 80 &&
        d.coordinate_height == 40);
  auto frame = reader->resolveCameraFrame(0, 400, 200);
  CHECK(frame.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(frame.detections.size() == 2);
  CHECK(frame.detections[0].instance_key == 22 &&
        frame.detections[1].instance_key == 11);
  const auto &first = frame.detections[0];
  CHECK(first.eyes[0].valid && first.eyes[0].major_axis.valid);
  CHECK(std::abs(first.eyes[0].major_axis.start.x - 0.0) < 1e-5);
  CHECK(std::abs(first.eyes[0].major_axis.end.x - 20.0) < 1e-5);
  CHECK(std::abs(first.eyes[0].minor_axis.start.y - 10.0) < 1e-5);
  CHECK(first.eyes[0].gaze_valid && first.eyes[0].signed_angle_valid &&
        first.eyes[0].signed_angle_degrees == 14);
  CHECK(first.eyes[0].eye_frame_angle_degrees == 10);
  CHECK(!frame.detections[1].eyes[0].valid);
  CHECK(frame.detections[1].eyes[1].valid);
  CHECK(std::abs(frame.detections[1].eyes[1].major_axis.start.y - 6.0) < 1e-5);
  CHECK(std::abs(frame.detections[1].eyes[1].major_axis.end.y - 24.0) < 1e-5);
  CHECK(reader->resolveCameraFrame(1, 400, 200).status ==
        EyeGeometryOverlayStatus::Missing);
  auto invalid = reader->resolveCameraFrame(2, 400, 200);
  CHECK(invalid.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(!invalid.detections[0].eyes[0].valid &&
        !invalid.detections[0].eyes[1].valid);
  CHECK(reader->resolveCameraFrame(0, 399, 200).status ==
        EyeGeometryOverlayStatus::InvalidDimensions);
  auto metrics = reader->accessMetrics();
  CHECK(metrics.payload_read_calls > 0 && metrics.logical_payload_bytes_read > 0);
  reader->resolveCameraFrame(0, 400, 200);
  CHECK(reader->accessMetrics().cache_hits == metrics.cache_hits + 1);
  return true;
}

bool testRejections() {
  std::string error;
  {
    Fixture f; CHECK(f.create());
    CHECK(f.openReader(&error, 1) == nullptr);
    auto archive = ArchiveContext::Open(f.temp.path, &error);
    CHECK(archive != nullptr);
    BoundEyeGeometryOverlayOpenRequest limited{archive, f.selection};
    limited.max_storage_chunk_bytes = 64;
    CHECK(OpenBoundEyeGeometryOverlayRepository(limited, &error) == nullptr);
    f.selection.eye.identity_digest =
        "2222222222222222222222222222222222222222222222222222222222222222";
    CHECK(f.openReader(&error) == nullptr);
  }
  {
    Fixture f; f.mismatch_eye_key = true; CHECK(f.create());
    auto reader = f.openReader(&error); CHECK(reader != nullptr);
    CHECK(reader->resolveCameraFrame(0, 400, 200).status ==
          EyeGeometryOverlayStatus::ReadFailed);
  }
  {
    Fixture f; f.mismatch_shape_frame = true; CHECK(f.create());
    auto reader = f.openReader(&error); CHECK(reader != nullptr);
    CHECK(reader->resolveCameraFrame(0, 400, 200).status ==
          EyeGeometryOverlayStatus::ReadFailed);
  }
  {
    Fixture f; f.invalid_axis_order = true; CHECK(f.create());
    auto reader = f.openReader(&error); CHECK(reader != nullptr);
    auto frame = reader->resolveCameraFrame(0, 400, 200);
    CHECK(frame.status == EyeGeometryOverlayStatus::Mapped);
    CHECK(!frame.detections[0].eyes[0].valid);
  }
  {
    Fixture f; f.invalid_parameter_order = true; CHECK(f.create());
    CHECK(f.openReader(&error) == nullptr);
  }
  {
    Fixture f; f.duplicate_instance_key = true; CHECK(f.create());
    auto reader = f.openReader(&error); CHECK(reader != nullptr);
    CHECK(reader->resolveCameraFrame(0, 400, 200).status ==
          EyeGeometryOverlayStatus::ReadFailed);
  }
  {
    Fixture f; f.missing_named_channel = true; CHECK(f.create());
    CHECK(f.openReader(&error) == nullptr);
  }
  {
    Fixture f; f.unavailable_named_channel = true; CHECK(f.create());
    CHECK(f.openReader(&error) == nullptr);
  }
  {
    Fixture f; f.invalid_body_frame = true; CHECK(f.create());
    auto reader = f.openReader(&error); CHECK(reader != nullptr);
    auto frame = reader->resolveCameraFrame(0, 400, 200);
    CHECK(frame.status == EyeGeometryOverlayStatus::Mapped);
    CHECK(!frame.detections[0].body_frame_valid);
    CHECK(!frame.detections[0].eyes[0].gaze_valid);
    CHECK(!frame.detections[0].eyes[0].signed_angle_valid);
  }
  {
    Fixture f; f.nonfinite_gaze = true; CHECK(f.create());
    auto reader = f.openReader(&error); CHECK(reader != nullptr);
    auto frame = reader->resolveCameraFrame(0, 400, 200);
    CHECK(frame.status == EyeGeometryOverlayStatus::Mapped);
    CHECK(!frame.detections[0].eyes[0].gaze_valid);
  }
  {
    Fixture f; CHECK(f.create());
    auto archive = ArchiveContext::Open(f.temp.path, &error);
    CHECK(archive != nullptr);
    BoundEyeGeometryOverlayOpenRequest limits{archive, f.selection};
    limits.max_decoded_frame_bytes = 512;
    auto reader = OpenBoundEyeGeometryOverlayRepository(limits, &error);
    CHECK(reader != nullptr);
    CHECK(reader->resolveCameraFrame(0, 400, 200).status ==
          EyeGeometryOverlayStatus::ReadFailed);
    limits.max_decoded_frame_bytes = 4096;
    limits.max_cached_decoded_bytes = 1;
    reader = OpenBoundEyeGeometryOverlayRepository(limits, &error);
    CHECK(reader != nullptr);
    CHECK(reader->resolveCameraFrame(0, 400, 200).status ==
          EyeGeometryOverlayStatus::Mapped);
    const auto first = reader->accessMetrics();
    CHECK(first.retained_decoded_cache_bytes == 0);
    reader->resolveCameraFrame(0, 400, 200);
    CHECK(reader->accessMetrics().payload_read_calls > first.payload_read_calls);
  }
  return true;
}

bool testSelectiveDemand() {
  Fixture f;
  CHECK(f.create());
  std::string error;
  auto reader = f.openReader(&error);
  CHECK(reader != nullptr);
  using namespace EyeGeometryFields;
  auto axes = reader->resolveCameraFrameFields(0, 400, 200, LeftGeometry);
  CHECK(axes.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(axes.loaded_fields == LeftGeometry);
  CHECK(axes.detections[0].eyes[0].valid);
  CHECK(!axes.detections[0].eyes[0].gaze_valid);
  CHECK(!axes.detections[0].eyes[1].valid);
  CHECK(!axes.detections[0].body_frame_valid);
  const auto axes_metrics = reader->accessMetrics();
  CHECK(axes_metrics.payload_read_calls == 14);
  CHECK(axes_metrics.peak_inflight_payload_reads > 1 &&
        axes_metrics.peak_inflight_payload_reads <= 4);
  for (const auto &array : axes_metrics.per_array) {
    CHECK(array.calls > 0 && array.future_elapsed_count == array.calls);
    CHECK(array.array != "E/roi_angles" && array.array != "E/roi_vectors");
    CHECK(array.array.find("body_frame") == std::string::npos);
    CHECK(array.array.find("eye_right/ellipse") == std::string::npos);
  }
  auto axes_repeat = reader->resolveCameraFrameFields(0, 400, 200, LeftGeometry);
  CHECK(axes_repeat.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(reader->accessMetrics().payload_read_calls == axes_metrics.payload_read_calls);
  auto signed_angle = reader->resolveCameraFrameFields(0, 400, 200, LeftSigned);
  CHECK(signed_angle.status == EyeGeometryOverlayStatus::Mapped);
  CHECK((signed_angle.loaded_fields & (LeftGeometry | BodyFrame | LeftSigned)) ==
        (LeftGeometry | BodyFrame | LeftSigned));
  CHECK(signed_angle.detections[0].eyes[0].signed_angle_valid);
  CHECK(!signed_angle.detections[0].eyes[0].gaze_valid);
  CHECK(reader->accessMetrics().payload_read_calls > axes_metrics.payload_read_calls);
  auto full = reader->resolveCameraFrame(0, 400, 200);
  CHECK(full.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(full.loaded_fields == All);
  CHECK(full.detections[0].eyes[0].gaze_valid);
  CHECK(full.detections[0].eyes[0].eye_frame_angle_valid);
  CHECK(full.detections[0].vergence_valid);
  const auto before_downgrade = reader->accessMetrics();
  auto downgrade = reader->resolveCameraFrameFields(0, 400, 200, LeftGeometry);
  CHECK(downgrade.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(downgrade.loaded_fields == All);
  CHECK(reader->accessMetrics().payload_read_calls ==
        before_downgrade.payload_read_calls);
  return true;
}

bool testBodyDependencyStillGatesMeasuredFields() {
  Fixture f;
  f.nonfinite_body_origin = true;
  CHECK(f.create());
  std::string error;
  auto reader = f.openReader(&error);
  CHECK(reader != nullptr);
  auto axes = reader->resolveCameraFrameFields(0, 400, 200,
      EyeGeometryFields::LeftGeometry);
  CHECK(axes.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(axes.detections[0].eyes[0].valid);
  auto measured = reader->resolveCameraFrameFields(0, 400, 200,
      EyeGeometryFields::LeftGaze | EyeGeometryFields::LeftSigned |
      EyeGeometryFields::LeftAngle);
  CHECK(measured.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(measured.detections[0].eyes[0].valid);
  CHECK(!measured.detections[0].body_frame_valid);
  CHECK(!measured.detections[0].eyes[0].gaze_valid);
  CHECK(!measured.detections[0].eyes[0].signed_angle_valid);
  CHECK(!measured.detections[0].eyes[0].eye_frame_angle_valid);
  return true;
}

bool testCancellationBeforePayload() {
  Fixture f;
  CHECK(f.create());
  std::string error;
  auto reader = f.openReader(&error);
  CHECK(reader != nullptr);
  int checks = 0;
  auto result = reader->resolveCameraFrameFields(0, 400, 200,
      EyeGeometryFields::All, [&] { return ++checks >= 5; });
  CHECK(result.status == EyeGeometryOverlayStatus::ReadFailed);
  CHECK(result.loaded_fields == 0 && result.detections.empty());
  const auto metrics = reader->accessMetrics();
  CHECK(metrics.payload_read_calls == 10);
  for (const auto &array : metrics.per_array)
    CHECK(array.array.find("ellipse") == std::string::npos &&
          array.array.find("body_frame") == std::string::npos &&
          array.array != "E/roi_angles" && array.array != "E/roi_vectors" &&
          array.array != "E/roi_qa");
  return true;
}

bool testCancellationAfterPayloadDrain() {
  Fixture f;
  CHECK(f.create());
  std::string error;
  auto reader = f.openReader(&error);
  CHECK(reader != nullptr);
  int checks = 0;
  auto result = reader->resolveCameraFrameFields(0, 400, 200,
      EyeGeometryFields::All, [&] { return ++checks >= 10; });
  CHECK(result.status == EyeGeometryOverlayStatus::ReadFailed);
  CHECK(result.loaded_fields == 0 && result.detections.empty());
  const auto metrics = reader->accessMetrics();
  CHECK(metrics.payload_read_calls == 28);
  CHECK(metrics.peak_inflight_payload_reads == 4);
  auto retry = reader->resolveCameraFrameFields(0, 400, 200,
      EyeGeometryFields::LeftGeometry);
  CHECK(retry.status == EyeGeometryOverlayStatus::Mapped);
  CHECK(retry.loaded_fields == EyeGeometryFields::LeftGeometry);
  CHECK(reader->accessMetrics().payload_read_calls > metrics.payload_read_calls);
  return true;
}

bool testCancellationBetweenPayloadWaves() {
  Fixture f;
  CHECK(f.create());
  std::string error;
  auto reader = f.openReader(&error);
  CHECK(reader != nullptr);
  int checks = 0;
  auto result = reader->resolveCameraFrameFields(0, 400, 200,
      EyeGeometryFields::All, [&] { return ++checks >= 6; });
  CHECK(result.status == EyeGeometryOverlayStatus::ReadFailed);
  CHECK(result.loaded_fields == 0 && result.detections.empty());
  const auto metrics = reader->accessMetrics();
  CHECK(metrics.payload_read_calls == 14);
  CHECK(metrics.peak_inflight_payload_reads == 4);
  return true;
}

} // namespace

int main() {
  if (!testBoundReader() || !testRejections() ||
      !testSelectiveDemand() || !testCancellationBeforePayload() ||
      !testCancellationAfterPayloadDrain() ||
      !testCancellationBetweenPayloadWaves() ||
      !testBodyDependencyStillGatesMeasuredFields()) return 1;
  std::cout << "bound eye geometry overlay tests passed\n";
  return 0;
}
