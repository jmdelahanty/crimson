#pragma once

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace crimson::zarr {

struct CropGeometryManifestSummary {
  std::string run_id;
  std::string payload_digest;
  std::string crop_policy_digest;
  std::string source_refined_run;
  std::string source_refined_manifest_digest;
  std::string source_pixel_authority_manifest_digest;
  size_t frame_count = 0;
  size_t instance_count = 0;
  size_t source_width = 0;
  size_t source_height = 0;
  size_t output_width = 0;
  size_t output_height = 0;
  bool selector_eligible = false;
  bool coordinate_catalog_validated = false;
};

bool ValidateCropGeometryRunManifest(const nlohmann::json &manifest,
                                     const std::string &requested_run,
                                     CropGeometryManifestSummary *summary,
                                     std::string *error = nullptr);

} // namespace crimson::zarr
