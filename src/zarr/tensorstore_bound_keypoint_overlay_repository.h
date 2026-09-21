#pragma once

#include "zarr/archive_context.h"
#include "zarr/canonical_overlay_selection.h"
#include "zarr/keypoint_overlay_repository.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crimson::zarr {

struct BoundKeypointOverlayOpenRequest {
  std::shared_ptr<ArchiveContext> archive;
  CanonicalOverlaySelection selection;
  size_t max_rows_per_frame = 64;
  uint64_t max_frame_payload_bytes = 1ULL * 1024ULL * 1024ULL;
};

struct BoundKeypointOverlayOpenMetrics {
  double total_ms = 0.0;
  size_t metadata_reads = 0;
  size_t exact_handle_opens = 0;
  size_t offset_read_calls = 0;
  size_t retained_offset_bytes = 0;
  size_t maximum_rows_in_frame = 0;
};

struct BoundKeypointOverlayAccessMetrics {
  uint64_t frame_requests = 0;
  uint64_t rows_resolved = 0;
  uint64_t payload_read_batches = 0;
  uint64_t payload_read_calls = 0;
  uint64_t decoded_payload_bytes = 0;
  uint64_t read_failures = 0;
};

class BoundKeypointOverlayRepository : public KeypointOverlayRepository {
public:
  ~BoundKeypointOverlayRepository() override = default;
  virtual BoundKeypointOverlayAccessMetrics accessMetrics() const = 0;
};

std::unique_ptr<BoundKeypointOverlayRepository>
OpenBoundKeypointOverlayRepository(
    const BoundKeypointOverlayOpenRequest &request,
    std::string *error_message = nullptr,
    BoundKeypointOverlayOpenMetrics *open_metrics = nullptr);

} // namespace crimson::zarr
