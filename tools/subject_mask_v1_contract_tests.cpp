#include "zarr/canonical_json.h"
#include "zarr/subject_mask_v1_contract.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':'    \
                << __LINE__ << '\n';                                           \
      return false;                                                            \
    }                                                                          \
  } while (false)

using json = nlohmann::json;

json Dimensions() {
  return {{"n_frames", 4}, {"n_frame_boundaries", 5}, {"n_instances", 6},
          {"n_rois", 6},   {"n_channels", 4},         {"H", 8},
          {"W", 8},        {"roi_height", 8},         {"roi_width", 8}};
}

json Components() {
  return {{"schema_id", "palette.subject_mask.component_registry"},
          {"schema_version", 1},
          {"labels", {"subject_body", "eye_left", "eye_right", "swim_bladder"}},
          {"channel_axis", 1},
          {"ordering", "persisted_exact_order"}};
}

json Bindings() {
  static const std::vector<std::string> contract_ids = {
      "palette.array.subject_mask.source_crop_row_ids",
      "palette.array.detection.instance_key",
      "palette.array.detection.source_acquisition_frame_index",
      "palette.array.frame_row_offsets",
      "palette.array.crop.source_crop_xywh",
      "palette.array.subject_masks_roi_dense",
      "palette.array.subject_mask.available_channels",
      "palette.array.subject_mask.mask_present",
      "palette.array.subject_mask.area_px",
      "palette.array.subject_mask.centroid_xy",
      "palette.array.subject_mask.centroid_valid",
      "palette.array.subject_mask.bbox_xyxy",
      "palette.array.subject_mask.bbox_valid"};
  json bindings = json::array();
  for (size_t index = 0;
       index < crimson::zarr::kSubjectMaskV1ArrayDeclarations.size(); ++index) {
    bindings.push_back(
        {{"path", crimson::zarr::kSubjectMaskV1ArrayDeclarations[index].path},
         {"contract_id", contract_ids[index]},
         {"contract_version", 1},
         {"required", true}});
  }
  return bindings;
}

json ValidManifest() {
  crimson::zarr::SubjectMaskV1ManifestSummary summary;
  summary.frame_count = 4;
  summary.row_count = 6;
  summary.channel_count = 4;
  summary.mask_height = 8;
  summary.mask_width = 8;
  json arrays = json::object();
  for (const auto &declaration :
       crimson::zarr::kSubjectMaskV1ArrayDeclarations) {
    arrays[declaration.path] = {
        {"shape",
         crimson::zarr::ExpectedSubjectMaskV1Shape(declaration, summary)},
        {"dtype", declaration.dtype},
        {"digest_algorithm", "sha256_c_contiguous_bytes_v1"},
        {"sha256", std::string(64, 'b')}};
  }
  json content_document = {
      {"schema_id", "palette.subject_mask_core.logical_content"},
      {"schema_version", 1},
      {"kind", "refined_dense_core"},
      {"dimensions", Dimensions()},
      {"components", Components()},
      {"arrays", std::move(arrays)}};
  json payload = {
      {"run_id", "mask_v1"},
      {"stage_family", "refined_subject_masks_runs"},
      {"kind", "refined_dense_core"},
      {"publication",
       {{"completion_contract", "palette.zarr_run_completion.v1"},
        {"completion_status", "complete"},
        {"stage_selector_eligible", false},
        {"metadata_state", "direct_and_consolidated_validated"},
        {"metadata_digest_scope",
         "exact_run_group_and_array_declarations_redacting_only_run_manifest"},
        {"metadata_digest", std::string(64, 'c')}}},
      {"logical_schema",
       {{"schema_id", "palette.stage.refined_subject_mask_dense_core"},
        {"schema_version", 1},
        {"layout", "recording_observations_with_frame_row_offsets_v1"},
        {"dimensions", Dimensions()},
        {"components", Components()},
        {"authority", "dense_binary_masks_roi"},
        {"bindings", Bindings()},
        {"invariants",
         {{"instances_per_frame", "zero_one_or_many"},
          {"frame_index", "retained_int64_f_plus_one_offsets"},
          {"row_order", "nondecreasing_source_acquisition_frame_index"},
          {"crop_contract", "palette.stage.crop_geometry_v1_float32_placement"},
          {"derived_surfaces", "must_exactly_match_dense_authority"},
          {"legacy_aliases", "forbidden"}}}}},
      {"storage_plan", json::object()},
      {"source", json::object()},
      {"write_receipt", json::object()},
      {"logical_content",
       {{"digest_algorithm", "sha256_canonical_json_v1"},
        {"digest", crimson::zarr::CanonicalJsonSha256(content_document)},
        {"document", std::move(content_document)}}}};
  return {{"schema_id", "palette.subject_mask_core.run_manifest"},
          {"schema_version", 2},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"payload_digest", crimson::zarr::CanonicalJsonSha256(payload)},
          {"payload", std::move(payload)}};
}

bool TestManifest() {
  auto manifest = ValidManifest();
  crimson::zarr::SubjectMaskV1ManifestSummary summary;
  std::string error;
  CHECK(crimson::zarr::ValidateSubjectMaskV1Manifest(manifest, "mask_v1",
                                                     &summary, &error));
  CHECK(summary.frame_count == 4);
  CHECK(summary.row_count == 6);
  CHECK(summary.component_labels.size() == 4);
  CHECK(!summary.selector_eligible);
  CHECK(summary.metadata_digest_scope ==
        "exact_run_group_and_array_declarations_redacting_only_run_manifest");

  auto maintained = manifest;
  maintained["payload"]["publication"]["metadata_digest_scope"] =
      "exact_run_group_and_array_declarations_redacting_manifest_lifecycle_"
      "and_transport_publication_attrs";
  maintained["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(maintained["payload"]);
  CHECK(crimson::zarr::ValidateSubjectMaskV1Manifest(
      maintained, "mask_v1", &summary, &error));

  auto unknown_scope = maintained;
  unknown_scope["payload"]["publication"]["metadata_digest_scope"] =
      "arbitrary_scope";
  unknown_scope["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(unknown_scope["payload"]);
  CHECK(!crimson::zarr::ValidateSubjectMaskV1Manifest(
      unknown_scope, "mask_v1", &summary, &error));

  auto invalid = manifest;
  invalid["payload"]["logical_content"]["document"]["arrays"]["masks_roi"]
         ["shape"][0] = 5;
  invalid["payload"]["logical_content"]["digest"] =
      crimson::zarr::CanonicalJsonSha256(
          invalid["payload"]["logical_content"]["document"]);
  invalid["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(invalid["payload"]);
  CHECK(!crimson::zarr::ValidateSubjectMaskV1Manifest(invalid, "mask_v1",
                                                      &summary, &error));

  invalid = manifest;
  invalid["payload"]["logical_content"]["document"]["arrays"]["masks_roi"]
         ["shape"] = "not-a-shape";
  invalid["payload"]["logical_content"]["digest"] =
      crimson::zarr::CanonicalJsonSha256(
          invalid["payload"]["logical_content"]["document"]);
  invalid["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(invalid["payload"]);
  CHECK(!crimson::zarr::ValidateSubjectMaskV1Manifest(invalid, "mask_v1",
                                                      &summary, &error));

  invalid = manifest;
  invalid["payload"]["logical_schema"]["bindings"][0]["required"] = false;
  invalid["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(invalid["payload"]);
  CHECK(!crimson::zarr::ValidateSubjectMaskV1Manifest(invalid, "mask_v1",
                                                      &summary, &error));
  CHECK(!crimson::zarr::ValidateSubjectMaskV1Manifest(manifest, "other_run",
                                                      &summary, &error));
  return true;
}

bool TestMultiObservationIndexAndKeys() {
  const std::vector<int64_t> offsets = {0, 2, 2, 3, 6};
  const std::vector<int64_t> frames = {0, 0, 2, 3, 3, 3};
  const std::vector<uint64_t> keys = {11, 12, 13, 14, 15, 16};
  std::string error;
  CHECK(crimson::zarr::ValidateSubjectMaskV1FrameIndex(offsets, frames, 4, 6,
                                                       &error));
  CHECK(crimson::zarr::ValidateSubjectMaskV1InstanceKeys(keys, 6, &error));
  CHECK(offsets[1] - offsets[0] == 2);
  CHECK(offsets[2] - offsets[1] == 0);
  CHECK(offsets[4] - offsets[3] == 3);

  auto malformed = offsets;
  malformed[2] = 1;
  CHECK(!crimson::zarr::ValidateSubjectMaskV1FrameIndex(malformed, frames, 4, 6,
                                                        &error));
  auto wrong_frames = frames;
  wrong_frames[2] = 1;
  CHECK(!crimson::zarr::ValidateSubjectMaskV1FrameIndex(offsets, wrong_frames,
                                                        4, 6, &error));
  auto duplicate = keys;
  duplicate.back() = duplicate.front();
  CHECK(
      !crimson::zarr::ValidateSubjectMaskV1InstanceKeys(duplicate, 6, &error));
  return true;
}

} // namespace

int main() {
  if (!TestManifest() || !TestMultiObservationIndexAndKeys()) {
    return 1;
  }
  std::cout << "subject_mask_v1_contract_tests: PASS\n";
  return 0;
}
