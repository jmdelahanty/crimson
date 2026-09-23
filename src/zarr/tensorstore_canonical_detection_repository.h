#pragma once

#include "zarr/canonical_detection_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

struct CanonicalDetectionRepositoryOpenMetrics {
  double total_ms = 0.0;
  double root_metadata_ms = 0.0;
  double exact_handle_open_ms = 0.0;
  double offset_read_ms = 0.0;
  size_t root_metadata_reads = 0;
  size_t direct_group_metadata_reads = 0;
  size_t consolidated_array_declarations = 0;
  size_t exact_handle_opens = 0;
  size_t fallback_metadata_reads = 0;
  size_t fallback_dtype_opens = 0;
  size_t offset_read_calls = 0;
  size_t retained_offset_bytes = 0;
};

std::unique_ptr<CanonicalDetectionRepository> OpenCanonicalDetectionRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const std::string &requested_run, std::string *error_message = nullptr,
    CanonicalDetectionRepositoryOpenMetrics *open_metrics = nullptr);

} // namespace crimson::zarr
