#pragma once

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace crimson::zarr {

struct CanonicalDetectionManifestSummary {
  std::string run_id;
  std::string payload_digest;
  size_t frame_count = 0;
  size_t instance_count = 0;
  size_t source_width = 0;
  size_t source_height = 0;
  bool selector_eligible = false;
  bool coordinate_catalog_validated = false;
};

bool ValidateCanonicalDetectionRunManifest(
    const nlohmann::json &manifest, const std::string &requested_run,
    CanonicalDetectionManifestSummary *summary, std::string *error = nullptr);

} // namespace crimson::zarr
