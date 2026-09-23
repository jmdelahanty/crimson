#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace crimson::zarr {

struct SubjectMaskBundleV2Summary {
  std::string bundle_id;
  std::string payload_digest;
  std::string recording_identity;
  std::string raw_run;
  std::string refined_run;
  std::string quality_run;
  size_t frame_count = 0;
  size_t row_count = 0;
  size_t raw_channel_count = 0;
  size_t refined_channel_count = 0;
  size_t mask_height = 0;
  size_t mask_width = 0;
  std::vector<std::string> raw_component_labels;
  std::vector<std::string> refined_component_labels;
  size_t validated_array_declarations = 0;
  bool selector_eligible = true;
  bool activation_deferred = false;
  bool quality_payload_opened = false;
};

struct SubjectMaskBundleV4Summary {
  std::string bundle_id;
  std::string payload_digest;
  std::string recording_identity;
  std::string raw_run;
  std::string refined_run;
  std::string quality_run;
  std::string presentation_cache_run;
  std::string refined_manifest_payload_digest;
  std::string presentation_cache_manifest_payload_digest;
  int bundle_manifest_schema_version = 0;
  size_t frame_count = 0;
  size_t row_count = 0;
  size_t raw_channel_count = 0;
  size_t refined_channel_count = 0;
  size_t mask_height = 0;
  size_t mask_width = 0;
  size_t validated_array_declarations = 0;
  bool selector_eligible = true;
  bool activation_deferred = false;
  bool ordinary_selection_absent = false;
  bool quality_payload_opened = false;
  bool full_ragged_contours_present = false;
};

bool ValidateSubjectMaskBundleV2Archive(
    const std::filesystem::path &archive_root,
    const std::string &requested_bundle,
    const std::string &expected_payload_digest,
    SubjectMaskBundleV2Summary *summary, std::string *error = nullptr);

bool ValidateSubjectMaskBundleV4Archive(
    const std::filesystem::path &archive_root,
    const std::string &requested_bundle,
    const std::string &expected_payload_digest,
    SubjectMaskBundleV4Summary *summary, std::string *error = nullptr);

} // namespace crimson::zarr
