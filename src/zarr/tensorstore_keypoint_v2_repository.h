#pragma once

#include "zarr/archive_context.h"
#include "zarr/keypoint_overlay_repository.h"
#include "zarr/keypoint_v2_contract.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crimson::zarr {

struct KeypointV2RepositoryOpenRequest {
  std::shared_ptr<ArchiveContext> raw_archive;
  std::string raw_run;
  std::shared_ptr<ArchiveContext> quality_archive;
  std::string quality_run;
  std::shared_ptr<ArchiveContext> refined_archive;
  std::string refined_run;
  std::shared_ptr<ArchiveContext> body_frame_archive;
  std::string body_frame_run;
  std::string expected_raw_manifest_digest;
  std::string expected_quality_manifest_digest;
  std::string expected_refined_manifest_digest;
  std::string expected_body_frame_manifest_digest;
  bool allow_selector_ineligible = false;
  bool deep_validate_identity = false;
};

struct KeypointV2RepositoryDescriptor {
  KeypointV2ManifestSummary raw;
  KeypointV2ManifestSummary quality;
  KeypointV2ManifestSummary selected;
  KeypointV2ManifestSummary body_frame;
  bool refined = false;
  bool consolidated_metadata = false;
  bool stable_identity = false;
  bool page_identity_validation = false;
  bool deep_identity_validated = false;
  bool quality_payload_lazy = true;
  size_t raw_offset_read_calls = 0;
  size_t selected_offset_read_calls = 0;
  size_t quality_offset_read_calls = 0;
  size_t body_frame_offset_read_calls = 0;
};

struct KeypointV2RepositoryOpenMetrics {
  double total_ms = 0.0;
  double metadata_ms = 0.0;
  double exact_handle_open_ms = 0.0;
  double identity_validation_ms = 0.0;
  size_t root_metadata_reads = 0;
  size_t direct_metadata_reads = 0;
  size_t consolidated_array_declarations = 0;
  size_t exact_handle_opens = 0;
  size_t fallback_metadata_reads = 0;
  size_t fallback_dtype_opens = 0;
  size_t raw_offset_read_calls = 0;
  size_t selected_offset_read_calls = 0;
  size_t quality_offset_read_calls = 0;
  size_t body_frame_offset_read_calls = 0;
  size_t quality_payload_reads = 0;
  size_t retained_offset_bytes = 0;
};

struct KeypointV2RepositoryAccessMetrics {
  uint64_t frame_requests = 0;
  uint64_t rows_resolved = 0;
  uint64_t payload_read_calls = 0;
  uint64_t payload_read_batches = 0;
  uint64_t maximum_columns_per_batch = 0;
  uint64_t quality_payload_read_calls = 0;
  uint64_t read_failures = 0;
};

class KeypointV2Repository : public KeypointOverlayRepository {
public:
  ~KeypointV2Repository() override = default;

  virtual const KeypointV2RepositoryDescriptor &v2Descriptor() const = 0;
  virtual KeypointV2RepositoryAccessMetrics accessMetrics() const = 0;
  virtual bool validateQualityPayloadBindings(std::string *error = nullptr) = 0;
};

std::unique_ptr<KeypointV2Repository> OpenKeypointV2Repository(
    const KeypointV2RepositoryOpenRequest &request,
    std::string *error_message = nullptr,
    KeypointV2RepositoryOpenMetrics *open_metrics = nullptr);

} // namespace crimson::zarr
