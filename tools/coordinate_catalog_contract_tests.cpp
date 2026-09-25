#include "coordinate_contract.h"
#include "zarr/canonical_detection_contract.h"
#include "zarr/canonical_json.h"
#include "zarr/coordinate_catalog_contract.h"
#include "zarr/crop_geometry_contract.h"
#include "zarr/refined_detection_contract.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;
using crimson::zarr::CoordinateCatalogStage;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "   \
                << #condition << '\n';                                         \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(double left, double right) { return std::abs(left - right) < 1e-9; }

json readFixture() {
  std::ifstream input(CRIMSON_COORDINATE_FIXTURE_PATH);
  if (!input) {
    return {};
  }
  return json::parse(input, nullptr, false);
}

const json *envelopeFor(const json &fixture, std::string_view stage) {
  if (!fixture.is_object() || !fixture.contains("catalogs") ||
      !fixture.at("catalogs").is_array()) {
    return nullptr;
  }
  for (const auto &entry : fixture.at("catalogs")) {
    if (entry.value("stage", "") == stage && entry.contains("envelope")) {
      return &entry.at("envelope");
    }
  }
  return nullptr;
}

bool validateFixtureIdentity(const json &fixture) {
  CHECK(fixture.value("schema_id", "") ==
        "crimson.palette_coordinate_catalog_cross_language_fixture");
  CHECK(fixture.value("schema_version", 0) == 1);
  CHECK(fixture.value("palette_commit", "") ==
        "154d788892aa8264d309258cc507d0d82a06e622");

  struct Expected {
    std::string_view name;
    CoordinateCatalogStage stage;
    size_t bindings;
    size_t surfaces;
  };
  constexpr Expected expected[] = {
      {"canonical_detection", CoordinateCatalogStage::CanonicalDetection, 3, 3},
      {"refined_detection", CoordinateCatalogStage::RefinedDetection, 6, 3},
      {"geometry_only_crop", CoordinateCatalogStage::GeometryOnlyCrop, 7, 7},
  };
  for (const auto &item : expected) {
    const auto *envelope = envelopeFor(fixture, item.name);
    CHECK(envelope != nullptr);
    crimson::zarr::CoordinateCatalogSummary summary;
    std::string error;
    CHECK(crimson::zarr::ValidateCoordinateCatalogEnvelope(
        *envelope, item.stage, &summary, &error));
    CHECK(error.empty());
    CHECK(summary.document_digest ==
          crimson::zarr::ExpectedCoordinateCatalogDigest(item.stage));
    CHECK(summary.bindings.size() == item.bindings);
    CHECK(summary.surfaces.size() == item.surfaces);
  }
  return true;
}

bool validateBindingsAndPresentation(const json &fixture) {
  const auto *canonical = envelopeFor(fixture, "canonical_detection");
  const auto *crop = envelopeFor(fixture, "geometry_only_crop");
  CHECK(canonical != nullptr);
  CHECK(crop != nullptr);

  crimson::zarr::CoordinateCatalogSummary canonical_summary;
  CHECK(crimson::zarr::ValidateCoordinateCatalogEnvelope(
      *canonical, CoordinateCatalogStage::CanonicalDetection,
      &canonical_summary));
  const auto *box = canonical_summary.findBinding(
      "palette.array.detection.bbox_norm_coords", 1);
  CHECK(box != nullptr);
  CHECK(box->semantic_role == "authoritative_numeric_surface");
  CHECK(box->surface_id == "source_camera_normalized_bbox_cxcywh_v1");
  const auto *box_surface = canonical_summary.findSurface(box->surface_id);
  CHECK(box_surface != nullptr);
  CHECK(box_surface->domain_id == "source_camera_normalized_xy");
  CHECK(box_surface->geometry_type == "bbox_cxcywh");
  CHECK(box_surface->components ==
        std::vector<std::string>({"center_x", "center_y", "width", "height"}));
  CHECK(box_surface->source_camera_mapping == "scale_by_source_camera_extent");

  crimson::coordinates::TransformAuthority source;
  source.source_dimensions = {4512, 3000};
  const auto presented =
      crimson::coordinates::normalizedCenterSizeBoxToContinuousPixelXyxy(
          {0.5, 0.25, 0.25, 0.1}, source);
  CHECK(presented.has_value());
  CHECK(near(presented->x_min, 1692.0));
  CHECK(near(presented->x_max, 2820.0));
  CHECK(near(presented->y_min, 600.0));
  CHECK(near(presented->y_max, 900.0));

  crimson::zarr::CoordinateCatalogSummary crop_summary;
  CHECK(crimson::zarr::ValidateCoordinateCatalogEnvelope(
      *crop, CoordinateCatalogStage::GeometryOnlyCrop, &crop_summary));
  const auto *origin =
      crop_summary.findBinding("palette.array.crop.roi_coordinates_full", 1);
  const auto *extent =
      crop_summary.findBinding("palette.array.crop.roi_sizes_full", 1);
  const auto *roi_box =
      crop_summary.findBinding("palette.array.crop.bbox_roi_xyxy", 1);
  CHECK(origin != nullptr && extent != nullptr && roi_box != nullptr);
  CHECK(origin->legacy_coordinate_space == "source_camera_pixel_index");
  CHECK(extent->legacy_coordinate_space == "source_camera_pixel_extent");
  CHECK(crop_summary.findSurface(extent->surface_id)->source_camera_mapping ==
        "not_positional_geometry");
  CHECK(crop_summary.findSurface(roi_box->surface_id)->source_camera_mapping ==
        "rowwise_roi_to_source_camera_transform");

  crimson::coordinates::RoiPlacement placement;
  placement.source_window = {100, 200, 512, 256};
  placement.authority.source_dimensions = {4512, 3000};
  placement.authority.crop_manifest_digest = "manifest";
  placement.authority.crop_policy_digest = "policy";
  const auto roi_presented =
      crimson::coordinates::roiNormalizedPointToSourceCamera({0.5, 0.25},
                                                             placement);
  CHECK(roi_presented.has_value());
  CHECK(near(roi_presented->x, 356.0));
  CHECK(near(roi_presented->y, 264.0));
  return true;
}

bool rejectTampering(const json &fixture) {
  const auto *original = envelopeFor(fixture, "refined_detection");
  CHECK(original != nullptr);
  std::string error;

  json tampered = *original;
  tampered["digest"] = std::string(64, '0');
  CHECK(!crimson::zarr::ValidateCoordinateCatalogEnvelope(
      tampered, CoordinateCatalogStage::RefinedDetection, nullptr, &error));

  tampered = *original;
  tampered["document"]["bindings"][0]["semantic_role"] =
      "authoritative_numeric_surface";
  tampered["digest"] = crimson::zarr::CanonicalJsonSha256(tampered["document"]);
  error.clear();
  CHECK(!crimson::zarr::ValidateCoordinateCatalogEnvelope(
      tampered, CoordinateCatalogStage::RefinedDetection, nullptr, &error));

  tampered = *original;
  tampered["extra"] = true;
  error.clear();
  CHECK(!crimson::zarr::ValidateCoordinateCatalogEnvelope(
      tampered, CoordinateCatalogStage::RefinedDetection, nullptr, &error));

  tampered = *original;
  tampered["document"]["non_finite"] = std::numeric_limits<double>::infinity();
  tampered["digest"] = crimson::zarr::CanonicalJsonSha256(tampered["document"]);
  CHECK(tampered["digest"].get<std::string>().empty());
  error.clear();
  CHECK(!crimson::zarr::ValidateCoordinateCatalogEnvelope(
      tampered, CoordinateCatalogStage::RefinedDetection, nullptr, &error));

  error.clear();
  CHECK(!crimson::zarr::ValidateCoordinateCatalogEnvelope(
      *original, CoordinateCatalogStage::CanonicalDetection, nullptr, &error));
  return true;
}

json reasonRegistry(std::string_view registry_id) {
  json registry = {
      {"schema_id", "palette.refined_detection.reason_registry"},
      {"schema_version", 1},
      {"registry_id", registry_id},
      {"codes", {{"0", "none"}}},
  };
  const std::string digest = crimson::zarr::CanonicalJsonSha256(registry);
  registry["digest_algorithm"] = "sha256_canonical_json_v1";
  registry["digest"] = digest;
  return registry;
}

json refinedManifestV2(const json &coordinate_envelope) {
  json payload = {
      {"run_id", "coordinate_refined_fixture"},
      {"stage", "refined_detect"},
      {"publication",
       {{"completion_contract", "palette.zarr_run_completion.v1"},
        {"completion_status", "complete"},
        {"stage_selector_eligible", false},
        {"metadata_state", "direct_and_consolidated_validated"},
        {"metadata_declarations_digest_scope",
         "normalized_group_and_array_declarations_excluding_attributes"},
        {"metadata_declarations_digest_algorithm", "sha256_canonical_json_v1"},
        {"metadata_declarations_digest", std::string(64, 'a')}}},
      {"logical_schema",
       {{"schema_id", "palette.stage.refined_detection"},
        {"schema_version", 1},
        {"stage", "refined_detect"},
        {"layout", "immutable_sparse_instances_with_source_audit_and_frame_row_"
                   "offsets_v1"},
        {"base_path", "refined_detect_runs/<run>"},
        {"clipped_binding", nullptr},
        {"dimensions",
         {{"n_frames", 4},
          {"n_instances", 6},
          {"n_source_detections", 5},
          {"n_frame_boundaries", 5},
          {"source_width", 4512},
          {"source_height", 3000},
          {"lineage_profile", "full_acquisition"}}}}},
      {"storage_plan", json::object()},
      {"snapshot_lineage", json::object()},
      {"source_detection", json::object()},
      {"reason_registries",
       {{"instances", reasonRegistry("instances.reason_codes.v1")},
        {"source_detections",
         reasonRegistry("source_detections.reason_codes.v1")}}},
      {"coordinate_contract", coordinate_envelope},
  };
  return {
      {"schema_id", "palette.refined_detection.run_manifest"},
      {"schema_version", 2},
      {"persisted_attribute", "run_manifest"},
      {"digest_algorithm", "sha256_canonical_json_v1"},
      {"payload_digest", crimson::zarr::CanonicalJsonSha256(payload)},
      {"payload", std::move(payload)},
  };
}

bool validateRefinedManifestV2(const json &fixture) {
  const auto *coordinate = envelopeFor(fixture, "refined_detection");
  CHECK(coordinate != nullptr);
  auto manifest = refinedManifestV2(*coordinate);
  crimson::zarr::RefinedDetectionManifestSummary summary;
  std::string error;
  if (!crimson::zarr::ValidateRefinedDetectionRunManifest(
          manifest, "coordinate_refined_fixture", true, &summary, &error)) {
    std::cerr << "Refined v2 validation failed: " << error << '\n';
    return false;
  }
  CHECK(summary.manifest_schema_version == 2);
  CHECK(summary.coordinate_catalog_validated);

  manifest["payload"]["coordinate_contract"]["document"]["bindings"][0]
          ["semantic_role"] = "authoritative_numeric_surface";
  manifest["payload"]["coordinate_contract"]["digest"] =
      crimson::zarr::CanonicalJsonSha256(
          manifest["payload"]["coordinate_contract"]["document"]);
  manifest["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(manifest["payload"]);
  error.clear();
  CHECK(!crimson::zarr::ValidateRefinedDetectionRunManifest(
      manifest, "coordinate_refined_fixture", true, nullptr, &error));
  return true;
}

json publication(std::string_view scope, bool crop = false) {
  json result = {
      {"completion_contract", "palette.zarr_run_completion.v1"},
      {"completion_status", "complete"},
      {"stage_selector_eligible", false},
      {"metadata_state", "direct_and_consolidated_validated"},
      {"metadata_declarations_digest_scope", scope},
      {"metadata_declarations_digest_algorithm", "sha256_canonical_json_v1"},
      {"metadata_declarations_digest", std::string(64, 'a')},
  };
  if (crop) {
    result["artifact_class"] = "geometry_only_analysis";
  }
  return result;
}

json canonicalManifestV3(const json &coordinate_envelope) {
  json payload = {
      {"run_id", "coordinate_canonical_fixture"},
      {"stage", "detect"},
      {"publication",
       publication(
           "normalized_group_and_array_declarations_excluding_attributes")},
      {"logical_schema",
       {{"schema_id", "palette.stage.canonical_detection"},
        {"schema_version", 1},
        {"stage", "detect"},
        {"layout", "sparse_instances_with_frame_row_offsets_v1"},
        {"base_path", "detect_runs/<run>"},
        {"instance_group", "instances"},
        {"dimensions",
         {{"n_frames", 4},
          {"n_instances", 6},
          {"n_frame_boundaries", 5},
          {"source_width", 4512},
          {"source_height", 3000}}}}},
      {"storage_plan", json::object()},
      {"source_evidence_kind", "legacy_conversion"},
      {"source_evidence", json::object()},
      {"logical_content", json::object()},
      {"coordinate_contract", coordinate_envelope},
  };
  return {{"schema_id", "palette.canonical_detection.run_manifest"},
          {"schema_version", 3},
          {"persisted_attribute", "run_manifest"},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"payload_digest", crimson::zarr::CanonicalJsonSha256(payload)},
          {"payload", std::move(payload)}};
}

json cropManifestV2(const json &coordinate_envelope) {
  json crop_policy_payload = {
      {"schema_id", "palette.crop_geometry_policy"},
      {"schema_version", 1},
      {"purpose", "contract_test"},
      {"placement",
       {{"center_source", "persisted_centers_img_xy"},
        {"center_rounding", "numpy_round_ties_to_even_v1"},
        {"top_left_rule", "rounded_center_minus_floor_size_over_two"},
        {"size_mode", "fixed_per_run"},
        {"fixed_size_wh", {512, 512}},
        {"padding_mode", "zero_outside_source_frame"}}},
  };
  json payload = {
      {"run_id", "coordinate_crop_fixture"},
      {"stage", "crop"},
      {"publication",
       publication("exact_group_and_array_declarations_with_attributes_"
                   "redacting_only_run_manifest",
                   true)},
      {"logical_schema",
       {{"schema_id", "palette.stage.crop_geometry"},
        {"schema_version", 1},
        {"stage", "crop"},
        {"layout", "geometry_only_sparse_rows_with_frame_row_offsets_v1"},
        {"base_path", "crop_runs/<run>"},
        {"dimensions",
         {{"n_frames", 4},
          {"n_instances", 6},
          {"n_frame_boundaries", 5},
          {"source_width", 4512},
          {"source_height", 3000}}},
        {"crop_policy",
         {{"payload", crop_policy_payload},
          {"payload_digest_algorithm", "sha256_canonical_json_v1"},
          {"payload_digest",
           crimson::zarr::CanonicalJsonSha256(crop_policy_payload)}}}}},
      {"storage_plan", json::object()},
      {"source_refined_snapshot",
       {{"schema_id", "palette.crop_geometry.refined_source"},
        {"schema_version", 1},
        {"authority_kind", "refined_detection_run"},
        {"stage", "refined_detect"},
        {"row_coverage", "complete_instances_rowset"},
        {"run_id", "coordinate_refined_fixture"},
        {"run_manifest_digest_algorithm", "sha256_canonical_json_v1"},
        {"run_manifest_digest", std::string(64, 'b')}}},
      {"source_pixel_authority",
       {{"schema_id", "palette.crop_geometry.pixel_authority"},
        {"schema_version", 1},
        {"frame_index_domain", "zero_based_acquisition_camera_frame"},
        {"n_frames", 4},
        {"source_width", 4512},
        {"source_height", 3000},
        {"authority_manifest_digest_algorithm", "sha256_canonical_json_v1"},
        {"authority_manifest_digest", std::string(64, 'c')},
        {"decoded_pixel_contract",
         {{"dtype", "uint8"},
          {"channels", "grayscale"},
          {"axis_order", "yx"},
          {"crop_sampling", "integer_half_open_xywh"}}}}},
      {"row_signature", json::object()},
      {"logical_content", json::object()},
      {"coordinate_contract", coordinate_envelope},
  };
  return {
      {"schema_id", "palette.crop_geometry.run_manifest"},
      {"schema_version", 2},
      {"persisted_attribute", "run_manifest"},
      {"persisted_path", "crop_runs/<run>/zarr.json.attributes.run_manifest"},
      {"digest_algorithm", "sha256_canonical_json_v1"},
      {"payload_digest", crimson::zarr::CanonicalJsonSha256(payload)},
      {"payload", std::move(payload)}};
}

bool validateCoordinateAwareManifests(const json &fixture) {
  const auto *canonical = envelopeFor(fixture, "canonical_detection");
  const auto *crop = envelopeFor(fixture, "geometry_only_crop");
  CHECK(canonical != nullptr && crop != nullptr);

  auto canonical_manifest = canonicalManifestV3(*canonical);
  crimson::zarr::CanonicalDetectionManifestSummary canonical_summary;
  std::string error;
  CHECK(crimson::zarr::ValidateCanonicalDetectionRunManifest(
      canonical_manifest, "coordinate_canonical_fixture", &canonical_summary,
      &error));
  CHECK(canonical_summary.coordinate_catalog_validated);
  canonical_manifest["payload"]["logical_schema"]["dimensions"]
                    ["n_frame_boundaries"] = 4;
  canonical_manifest["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(canonical_manifest["payload"]);
  CHECK(!crimson::zarr::ValidateCanonicalDetectionRunManifest(
      canonical_manifest, "coordinate_canonical_fixture", nullptr, &error));

  auto crop_manifest = cropManifestV2(*crop);
  crimson::zarr::CropGeometryManifestSummary crop_summary;
  error.clear();
  CHECK(crimson::zarr::ValidateCropGeometryRunManifest(
      crop_manifest, "coordinate_crop_fixture", &crop_summary, &error));
  CHECK(crop_summary.coordinate_catalog_validated);
  CHECK(crop_summary.output_width == 512 && crop_summary.output_height == 512);
  crop_manifest["payload"]["source_pixel_authority"]["source_width"] = 4000;
  crop_manifest["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(crop_manifest["payload"]);
  error.clear();
  CHECK(!crimson::zarr::ValidateCropGeometryRunManifest(
      crop_manifest, "coordinate_crop_fixture", nullptr, &error));
  return true;
}

} // namespace

int main() {
  const json fixture = readFixture();
  if (!validateFixtureIdentity(fixture) ||
      !validateBindingsAndPresentation(fixture) || !rejectTampering(fixture) ||
      !validateRefinedManifestV2(fixture) ||
      !validateCoordinateAwareManifests(fixture)) {
    return 1;
  }
  std::cout << "coordinate_catalog_contract_tests: PASS\n";
  return 0;
}
