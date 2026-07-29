#include "coordinate_contract.h"
#include "zarr/canonical_json.h"
#include "zarr/coordinate_catalog_contract.h"
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

} // namespace

int main() {
  const json fixture = readFixture();
  if (!validateFixtureIdentity(fixture) ||
      !validateBindingsAndPresentation(fixture) || !rejectTampering(fixture) ||
      !validateRefinedManifestV2(fixture)) {
    return 1;
  }
  std::cout << "coordinate_catalog_contract_tests: PASS\n";
  return 0;
}
