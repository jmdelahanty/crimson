#pragma once

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace crimson::zarr {

struct RefinedDetectionManifestSummary {
  std::string run_id;
  std::string payload_digest;
  std::string lineage_profile;
  size_t frame_count = 0;
  size_t instance_count = 0;
  size_t source_detection_count = 0;
  size_t source_width = 0;
  size_t source_height = 0;
  bool selector_eligible = false;
};

struct RefinedDetectionAuthoritySummary {
  std::string run_id;
  std::string run_manifest_digest;
  std::string intended_use;
};

std::string CanonicalJsonSha256(const nlohmann::json &value);

bool ValidateRefinedDetectionRunManifest(
    const nlohmann::json &manifest, const std::string &requested_run,
    bool allow_selector_ineligible, RefinedDetectionManifestSummary *summary,
    std::string *error = nullptr);

bool ValidateRefinedDetectionAuthority(
    const std::string &authoritative_run, const nlohmann::json &provenance,
    RefinedDetectionAuthoritySummary *summary, std::string *error = nullptr);

} // namespace crimson::zarr
