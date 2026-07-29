#include "zarr/crop_geometry_contract.h"

#include "zarr/canonical_json.h"
#include "zarr/coordinate_catalog_contract.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

using json = nlohmann::json;

void assignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

bool exactKeys(const json &value,
               std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  return std::all_of(
      expected.begin(), expected.end(),
      [&](std::string_view key) { return value.contains(std::string(key)); });
}

bool validPublication(const json &publication) {
  return publication.is_object() &&
         publication.value("artifact_class", "") == "geometry_only_analysis" &&
         publication.value("completion_contract", "") ==
             "palette.zarr_run_completion.v1" &&
         publication.value("completion_status", "") == "complete" &&
         publication.value("metadata_state", "") ==
             "direct_and_consolidated_validated" &&
         publication.value("metadata_declarations_digest_scope", "") ==
             "exact_group_and_array_declarations_with_attributes_redacting_"
             "only_run_manifest" &&
         publication.value("metadata_declarations_digest_algorithm", "") ==
             "sha256_canonical_json_v1" &&
         IsLowerSha256(publication.value("metadata_declarations_digest", "")) &&
         publication.contains("stage_selector_eligible") &&
         publication.at("stage_selector_eligible").is_boolean();
}

} // namespace

bool ValidateCropGeometryRunManifest(const json &manifest,
                                     const std::string &requested_run,
                                     CropGeometryManifestSummary *summary,
                                     std::string *error) {
  try {
    if (!exactKeys(manifest,
                   {"schema_id", "schema_version", "persisted_attribute",
                    "persisted_path", "digest_algorithm", "payload_digest",
                    "payload"}) ||
        manifest.value("schema_id", "") !=
            "palette.crop_geometry.run_manifest" ||
        manifest.value("schema_version", 0) != 2 ||
        manifest.value("persisted_attribute", "") != "run_manifest" ||
        manifest.value("persisted_path", "") !=
            "crop_runs/<run>/zarr.json.attributes.run_manifest" ||
        manifest.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        !manifest.at("payload").is_object()) {
      assignError(error, "Crop geometry run_manifest envelope is invalid");
      return false;
    }
    const auto &payload = manifest.at("payload");
    if (!exactKeys(payload, {"run_id", "stage", "publication", "logical_schema",
                             "storage_plan", "source_refined_snapshot",
                             "source_pixel_authority", "row_signature",
                             "logical_content", "coordinate_contract"}) ||
        payload.value("run_id", "") != requested_run ||
        payload.value("stage", "") != "crop") {
      assignError(error, "Crop geometry manifest identity is invalid");
      return false;
    }
    const std::string payload_digest = manifest.value("payload_digest", "");
    if (!IsLowerSha256(payload_digest) ||
        CanonicalJsonSha256(payload) != payload_digest) {
      assignError(error, "Crop geometry run_manifest digest mismatch");
      return false;
    }
    const auto &publication = payload.at("publication");
    if (!validPublication(publication)) {
      assignError(error, "Crop geometry publication state is invalid");
      return false;
    }
    const auto &logical = payload.at("logical_schema");
    if (logical.value("schema_id", "") != "palette.stage.crop_geometry" ||
        logical.value("schema_version", 0) != 1 ||
        logical.value("stage", "") != "crop" ||
        logical.value("layout", "") !=
            "geometry_only_sparse_rows_with_frame_row_offsets_v1" ||
        logical.value("base_path", "") != "crop_runs/<run>") {
      assignError(error, "Crop geometry logical schema is incompatible");
      return false;
    }
    const auto &dimensions = logical.at("dimensions");
    const size_t frames = dimensions.at("n_frames").get<size_t>();
    const size_t instances = dimensions.at("n_instances").get<size_t>();
    const size_t width = dimensions.at("source_width").get<size_t>();
    const size_t height = dimensions.at("source_height").get<size_t>();
    if (frames == 0 || width == 0 || height == 0 ||
        dimensions.at("n_frame_boundaries").get<size_t>() != frames + 1) {
      assignError(error, "Crop geometry dimensions are invalid");
      return false;
    }
    const auto &policy = logical.at("crop_policy");
    if (!exactKeys(policy,
                   {"payload", "payload_digest_algorithm", "payload_digest"}) ||
        policy.value("payload_digest_algorithm", "") !=
            "sha256_canonical_json_v1" ||
        !IsLowerSha256(policy.value("payload_digest", "")) ||
        CanonicalJsonSha256(policy.at("payload")) !=
            policy.value("payload_digest", "")) {
      assignError(error, "Crop geometry policy envelope is invalid");
      return false;
    }
    const auto &placement = policy.at("payload").at("placement");
    const auto output =
        placement.at("fixed_size_wh").get<std::vector<size_t>>();
    if (policy.at("payload").value("schema_id", "") !=
            "palette.crop_geometry_policy" ||
        policy.at("payload").value("schema_version", 0) != 1 ||
        placement.value("size_mode", "") != "fixed_per_run" ||
        placement.value("padding_mode", "") != "zero_outside_source_frame" ||
        output.size() != 2 || output[0] == 0 || output[1] == 0) {
      assignError(error, "Crop geometry placement policy is incompatible");
      return false;
    }
    const auto &refined = payload.at("source_refined_snapshot");
    if (refined.value("schema_id", "") !=
            "palette.crop_geometry.refined_source" ||
        refined.value("schema_version", 0) != 1 ||
        refined.value("authority_kind", "") != "refined_detection_run" ||
        refined.value("stage", "") != "refined_detect" ||
        refined.value("row_coverage", "") != "complete_instances_rowset" ||
        refined.value("run_id", "").empty() ||
        refined.value("run_manifest_digest_algorithm", "") !=
            "sha256_canonical_json_v1" ||
        !IsLowerSha256(refined.value("run_manifest_digest", ""))) {
      assignError(error, "Crop refined-snapshot binding is invalid");
      return false;
    }
    const auto &authority = payload.at("source_pixel_authority");
    if (authority.value("schema_id", "") !=
            "palette.crop_geometry.pixel_authority" ||
        authority.value("schema_version", 0) != 1 ||
        authority.value("frame_index_domain", "") !=
            "zero_based_acquisition_camera_frame" ||
        authority.value("n_frames", size_t{0}) != frames ||
        authority.value("source_width", size_t{0}) != width ||
        authority.value("source_height", size_t{0}) != height ||
        authority.value("authority_manifest_digest_algorithm", "") !=
            "sha256_canonical_json_v1" ||
        !IsLowerSha256(authority.value("authority_manifest_digest", ""))) {
      assignError(error, "Crop pixel-authority binding is invalid");
      return false;
    }
    const auto &pixels = authority.at("decoded_pixel_contract");
    if (pixels.value("dtype", "") != "uint8" ||
        pixels.value("channels", "") != "grayscale" ||
        pixels.value("axis_order", "") != "yx" ||
        pixels.value("crop_sampling", "") != "integer_half_open_xywh") {
      assignError(error, "Crop decoded-pixel contract is incompatible");
      return false;
    }
    CoordinateCatalogSummary coordinate_summary;
    if (!ValidateCoordinateCatalogEnvelope(
            payload.at("coordinate_contract"),
            CoordinateCatalogStage::GeometryOnlyCrop, &coordinate_summary,
            error)) {
      return false;
    }
    if (summary) {
      *summary = {requested_run,
                  payload_digest,
                  policy.value("payload_digest", ""),
                  refined.value("run_id", ""),
                  refined.value("run_manifest_digest", ""),
                  authority.value("authority_manifest_digest", ""),
                  frames,
                  instances,
                  width,
                  height,
                  output[0],
                  output[1],
                  publication.at("stage_selector_eligible").get<bool>(),
                  true};
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid crop geometry manifest: " +
                           std::string(exception.what()));
    return false;
  }
}

} // namespace crimson::zarr
