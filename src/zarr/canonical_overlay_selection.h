#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crimson::zarr {

class ArchiveContext;

struct CanonicalOverlayProductBinding {
  bool valid = false;
  std::string error;
  std::string group;
  std::string run_id;
  std::string schema_id;
  int schema_version = 0;
  // The product's strongest published immutable identity: a canonical run
  // manifest digest where one exists, otherwise the product's published
  // lineage/publication digest.
  std::string identity_digest;
  std::string manifest_payload_digest;
  std::string bound_source_run_id;
  std::string bound_source_payload_digest;
  bool selector_eligible = false;
  bool bound_selector_exception = false;
};

struct CanonicalOverlaySelection {
  std::string archive_identity;
  std::string recording_id;
  std::string camera_id;
  int64_t first_acquisition_frame = 0;
  size_t frame_count = 0;
  size_t observation_count = 0;
  size_t source_width = 0;
  size_t source_height = 0;
  std::string coordinate_surface_id;
  std::string coordinate_descriptor_profile;
  std::vector<std::string> keypoint_labels;

  CanonicalOverlayProductBinding eye;
  CanonicalOverlayProductBinding keypoints;
  CanonicalOverlayProductBinding mask;
  CanonicalOverlayProductBinding shape;

  std::string instance_key_digest;
  std::string acquisition_frame_digest;
  std::string frame_row_offsets_digest;
};

struct CanonicalOverlaySelectionRequest {
  // Empty means the exact eye-angle parent selector. A non-empty value must
  // agree with that selector; it is not an arbitrary-run escape hatch.
  std::string eye_run;
  std::string expected_recording_id;
  size_t expected_frame_count = 0;
  size_t expected_source_width = 0;
  size_t expected_source_height = 0;
};

// Document bundle used by focused contract tests and by the archive-backed
// selector after it has resolved only the exact eye-bound run names.
struct CanonicalOverlaySelectionDocuments {
  std::string archive_identity;
  nlohmann::json root_attributes;
  nlohmann::json eye_group_attributes;
  nlohmann::json eye_attributes;
  nlohmann::json keypoint_attributes;
  nlohmann::json mask_attributes;
  nlohmann::json shape_group_attributes;
  nlohmann::json shape_attributes;
};

std::optional<CanonicalOverlaySelection>
ValidateCanonicalOverlaySelectionDocuments(
    const CanonicalOverlaySelectionDocuments &documents,
    const CanonicalOverlaySelectionRequest &request = {},
    std::string *error_message = nullptr);

std::optional<CanonicalOverlaySelection> SelectCanonicalOverlaySources(
    const std::shared_ptr<ArchiveContext> &archive,
    const CanonicalOverlaySelectionRequest &request = {},
    std::string *error_message = nullptr);

} // namespace crimson::zarr
