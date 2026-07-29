#include "zarr/canonical_detection_contract.h"

#include "zarr/canonical_json.h"
#include "zarr/coordinate_catalog_contract.h"

#include <algorithm>
#include <string_view>
#include <utility>

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
         publication.value("completion_contract", "") ==
             "palette.zarr_run_completion.v1" &&
         publication.value("completion_status", "") == "complete" &&
         publication.value("metadata_state", "") ==
             "direct_and_consolidated_validated" &&
         publication.value("metadata_declarations_digest_scope", "") ==
             "normalized_group_and_array_declarations_excluding_attributes" &&
         publication.value("metadata_declarations_digest_algorithm", "") ==
             "sha256_canonical_json_v1" &&
         IsLowerSha256(publication.value("metadata_declarations_digest", "")) &&
         publication.contains("stage_selector_eligible") &&
         publication.at("stage_selector_eligible").is_boolean();
}

} // namespace

bool ValidateCanonicalDetectionRunManifest(
    const json &manifest, const std::string &requested_run,
    CanonicalDetectionManifestSummary *summary, std::string *error) {
  try {
    if (!exactKeys(manifest,
                   {"schema_id", "schema_version", "persisted_attribute",
                    "digest_algorithm", "payload_digest", "payload"}) ||
        manifest.value("schema_id", "") !=
            "palette.canonical_detection.run_manifest" ||
        manifest.value("schema_version", 0) != 3 ||
        manifest.value("persisted_attribute", "") != "run_manifest" ||
        manifest.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        !manifest.at("payload").is_object()) {
      assignError(error,
                  "Canonical detection run_manifest envelope is invalid");
      return false;
    }
    const auto &payload = manifest.at("payload");
    if (!exactKeys(payload,
                   {"run_id", "stage", "publication", "logical_schema",
                    "storage_plan", "source_evidence_kind", "source_evidence",
                    "logical_content", "coordinate_contract"}) ||
        payload.value("run_id", "") != requested_run ||
        payload.value("stage", "") != "detect") {
      assignError(error, "Canonical detection manifest identity is invalid");
      return false;
    }
    const std::string payload_digest = manifest.value("payload_digest", "");
    if (!IsLowerSha256(payload_digest) ||
        CanonicalJsonSha256(payload) != payload_digest) {
      assignError(error, "Canonical detection run_manifest digest mismatch");
      return false;
    }
    const auto &publication = payload.at("publication");
    if (!validPublication(publication)) {
      assignError(error, "Canonical detection publication state is invalid");
      return false;
    }
    const auto &logical = payload.at("logical_schema");
    if (logical.value("schema_id", "") != "palette.stage.canonical_detection" ||
        logical.value("schema_version", 0) != 1 ||
        logical.value("stage", "") != "detect" ||
        logical.value("layout", "") !=
            "sparse_instances_with_frame_row_offsets_v1" ||
        logical.value("base_path", "") != "detect_runs/<run>" ||
        logical.value("instance_group", "") != "instances") {
      assignError(error, "Canonical detection logical schema is incompatible");
      return false;
    }
    const auto &dimensions = logical.at("dimensions");
    const size_t frames = dimensions.at("n_frames").get<size_t>();
    const size_t instances = dimensions.at("n_instances").get<size_t>();
    const size_t width = dimensions.at("source_width").get<size_t>();
    const size_t height = dimensions.at("source_height").get<size_t>();
    if (frames == 0 || width == 0 || height == 0 ||
        dimensions.at("n_frame_boundaries").get<size_t>() != frames + 1) {
      assignError(error, "Canonical detection dimensions are invalid");
      return false;
    }
    CoordinateCatalogSummary coordinate_summary;
    if (!ValidateCoordinateCatalogEnvelope(
            payload.at("coordinate_contract"),
            CoordinateCatalogStage::CanonicalDetection, &coordinate_summary,
            error)) {
      return false;
    }
    if (summary) {
      *summary = {requested_run,
                  payload_digest,
                  frames,
                  instances,
                  width,
                  height,
                  publication.at("stage_selector_eligible").get<bool>(),
                  true};
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid canonical detection manifest: " +
                           std::string(exception.what()));
    return false;
  }
}

} // namespace crimson::zarr
