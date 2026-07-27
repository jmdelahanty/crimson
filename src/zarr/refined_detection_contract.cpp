#include "zarr/refined_detection_contract.h"

#include <tensorstore/internal/digest/sha256.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string_view>
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

bool isLowerSha256(const std::string &value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool strictJson(const json &value) {
  if (value.is_number_float() && !std::isfinite(value.get<double>())) {
    return false;
  }
  if (value.is_array()) {
    return std::all_of(value.begin(), value.end(), strictJson);
  }
  if (value.is_object()) {
    return std::all_of(value.begin(), value.end(),
                       [](const auto &item) { return strictJson(item); });
  }
  return true;
}

bool validateReasonRegistry(const json &registry, std::string_view expected_id,
                            std::string *error) {
  if (!exactKeys(registry, {"schema_id", "schema_version", "registry_id",
                            "codes", "digest_algorithm", "digest"}) ||
      registry.value("schema_id", "") !=
          "palette.refined_detection.reason_registry" ||
      registry.value("schema_version", 0) != 1 ||
      registry.value("registry_id", "") != expected_id ||
      registry.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
      !registry.at("codes").is_object()) {
    assignError(error, "Refined detection reason registry is incompatible");
    return false;
  }
  const auto &codes = registry.at("codes");
  if (!codes.contains("0") || codes.at("0") != "none") {
    assignError(error, "Refined detection reason registry lacks code zero");
    return false;
  }
  json payload = {{"schema_id", registry.at("schema_id")},
                  {"schema_version", registry.at("schema_version")},
                  {"registry_id", registry.at("registry_id")},
                  {"codes", codes}};
  if (CanonicalJsonSha256(payload) != registry.value("digest", "")) {
    assignError(error, "Refined detection reason registry digest mismatch");
    return false;
  }
  return true;
}

} // namespace

std::string CanonicalJsonSha256(const json &value) {
  if (!strictJson(value)) {
    return {};
  }
  const std::string payload = value.dump();
  tensorstore::internal::SHA256Digester digester;
  digester.Write(payload);
  const auto digest = digester.Digest();
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded(digest.size() * 2, '0');
  for (size_t index = 0; index < digest.size(); ++index) {
    encoded[index * 2] = kHex[digest[index] >> 4];
    encoded[index * 2 + 1] = kHex[digest[index] & 0x0f];
  }
  return encoded;
}

bool ValidateRefinedDetectionRunManifest(
    const json &manifest, const std::string &requested_run,
    bool allow_selector_ineligible, RefinedDetectionManifestSummary *summary,
    std::string *error) {
  try {
    if (!exactKeys(manifest,
                   {"schema_id", "schema_version", "persisted_attribute",
                    "digest_algorithm", "payload_digest", "payload"}) ||
        manifest.value("schema_id", "") !=
            "palette.refined_detection.run_manifest" ||
        manifest.value("schema_version", 0) != 1 ||
        manifest.value("persisted_attribute", "") != "run_manifest" ||
        manifest.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        !manifest.at("payload").is_object()) {
      assignError(error, "Refined detection run_manifest envelope is invalid");
      return false;
    }
    const auto &payload = manifest.at("payload");
    if (!exactKeys(payload, {"run_id", "stage", "publication", "logical_schema",
                             "storage_plan", "snapshot_lineage",
                             "source_detection", "reason_registries"}) ||
        payload.value("run_id", "") != requested_run ||
        payload.value("stage", "") != "refined_detect") {
      assignError(error, "Refined detection manifest run identity is invalid");
      return false;
    }
    const std::string payload_digest = manifest.value("payload_digest", "");
    if (!isLowerSha256(payload_digest) ||
        CanonicalJsonSha256(payload) != payload_digest) {
      assignError(error, "Refined detection run_manifest digest mismatch");
      return false;
    }
    const auto &publication = payload.at("publication");
    if (publication.value("completion_contract", "") !=
            "palette.zarr_run_completion.v1" ||
        publication.value("completion_status", "") != "complete" ||
        publication.value("metadata_state", "") !=
            "direct_and_consolidated_validated" ||
        publication.value("metadata_declarations_digest_scope", "") !=
            "normalized_group_and_array_declarations_excluding_attributes" ||
        publication.value("metadata_declarations_digest_algorithm", "") !=
            "sha256_canonical_json_v1" ||
        !isLowerSha256(publication.value("metadata_declarations_digest", "")) ||
        !publication.contains("stage_selector_eligible") ||
        !publication.at("stage_selector_eligible").is_boolean()) {
      assignError(error, "Refined detection publication state is invalid");
      return false;
    }
    const bool selector_eligible =
        publication.at("stage_selector_eligible").get<bool>();
    if (!selector_eligible && !allow_selector_ineligible) {
      assignError(error, "Refined detection run is selector-ineligible");
      return false;
    }
    const auto &logical = payload.at("logical_schema");
    if (logical.value("schema_id", "") != "palette.stage.refined_detection" ||
        logical.value("schema_version", 0) != 1 ||
        logical.value("stage", "") != "refined_detect" ||
        logical.value("layout", "") != "immutable_sparse_instances_with_source_"
                                       "audit_and_frame_row_offsets_v1" ||
        logical.value("base_path", "") != "refined_detect_runs/<run>") {
      assignError(error, "Refined detection logical schema is incompatible");
      return false;
    }
    const auto &dimensions = logical.at("dimensions");
    const size_t frames = dimensions.at("n_frames").get<size_t>();
    const size_t instances = dimensions.at("n_instances").get<size_t>();
    const size_t source_rows =
        dimensions.at("n_source_detections").get<size_t>();
    const size_t width = dimensions.at("source_width").get<size_t>();
    const size_t height = dimensions.at("source_height").get<size_t>();
    const std::string lineage = dimensions.value("lineage_profile", "");
    if (frames == 0 || width == 0 || height == 0 ||
        dimensions.at("n_frame_boundaries").get<size_t>() != frames + 1 ||
        (lineage != "full_acquisition" &&
         lineage != "clipped_recording_snapshot")) {
      assignError(error, "Refined detection dimensions are invalid");
      return false;
    }
    if ((lineage == "clipped_recording_snapshot") !=
        !logical.at("clipped_binding").is_null()) {
      assignError(error, "Refined detection clipped binding is inconsistent");
      return false;
    }
    const auto &registries = payload.at("reason_registries");
    if (!exactKeys(registries, {"instances", "source_detections"}) ||
        !validateReasonRegistry(registries.at("instances"),
                                "instances.reason_codes.v1", error) ||
        !validateReasonRegistry(registries.at("source_detections"),
                                "source_detections.reason_codes.v1", error)) {
      return false;
    }
    if (summary) {
      *summary = {requested_run, payload_digest, lineage,
                  frames,        instances,      source_rows,
                  width,         height,         selector_eligible};
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid refined detection manifest: " +
                           std::string(exception.what()));
    return false;
  }
}

bool ValidateRefinedDetectionAuthority(
    const std::string &authoritative_run, const json &provenance,
    RefinedDetectionAuthoritySummary *summary, std::string *error) {
  try {
    if (authoritative_run.empty() ||
        authoritative_run.find('/') != std::string::npos ||
        !exactKeys(provenance,
                   {"schema_id", "schema_version", "digest_algorithm",
                    "payload_digest", "payload"}) ||
        provenance.value("schema_id", "") !=
            "palette.refined_detection.authoritative_selection" ||
        provenance.value("schema_version", 0) != 1 ||
        provenance.value("digest_algorithm", "") !=
            "sha256_canonical_json_v1" ||
        !provenance.at("payload").is_object()) {
      assignError(error, "Refined detection authority envelope is invalid");
      return false;
    }
    const auto &payload = provenance.at("payload");
    if (!exactKeys(payload, {"run_id", "run_manifest_digest", "review_state",
                             "review_method", "intended_use", "approved_by",
                             "approved_at_utc", "git_sha", "note"}) ||
        payload.value("run_id", "") != authoritative_run ||
        payload.value("review_state", "") != "approved" ||
        payload.value("review_method", "").empty() ||
        payload.value("approved_by", "").empty() ||
        payload.value("approved_at_utc", "").empty()) {
      assignError(error, "Refined detection authority payload is invalid");
      return false;
    }
    const std::string intended_use = payload.value("intended_use", "");
    if (intended_use != "analysis" && intended_use != "training" &&
        intended_use != "analysis_and_training") {
      assignError(error, "Refined detection authority intended_use is invalid");
      return false;
    }
    const std::string run_digest = payload.value("run_manifest_digest", "");
    if (!isLowerSha256(run_digest) ||
        CanonicalJsonSha256(payload) !=
            provenance.value("payload_digest", "")) {
      assignError(error, "Refined detection authority digest mismatch");
      return false;
    }
    if (summary) {
      *summary = {authoritative_run, run_digest, intended_use};
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid refined detection authority: " +
                           std::string(exception.what()));
    return false;
  }
}

} // namespace crimson::zarr
