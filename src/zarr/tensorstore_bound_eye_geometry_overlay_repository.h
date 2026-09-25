#pragma once

#include "zarr/canonical_overlay_selection.h"
#include "zarr/eye_geometry_overlay_repository.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;
class SharedMaskFrameIndex;

struct BoundEyeGeometryOverlayOpenRequest {
  std::shared_ptr<ArchiveContext> archive;
  CanonicalOverlaySelection selection;
  size_t max_observations_per_frame = 16;
  uint64_t max_decoded_frame_bytes = 16ULL * 1024ULL * 1024ULL;
  uint64_t max_cached_decoded_bytes = 32ULL * 1024ULL * 1024ULL;
  uint64_t max_storage_chunk_bytes = 2ULL * 1024ULL * 1024ULL;
  std::shared_ptr<const SharedMaskFrameIndex> shared_mask_frame_index;
};

struct BoundEyeGeometryOverlayOpenMetrics {
  size_t exact_handle_opens = 0;
  size_t offset_read_calls = 0;
  size_t maximum_observations_per_frame = 0;
  uint64_t retained_offset_bytes = 0;
  bool reused_shared_mask_index = false;
};

// Reads only the eye run and its exact shape/mask sources in an already
// validated canonical selection. Payloads are requested lazily per frame.
std::unique_ptr<EyeGeometryOverlayRepository>
OpenBoundEyeGeometryOverlayRepository(
    const BoundEyeGeometryOverlayOpenRequest &request,
    std::string *error_message = nullptr,
    BoundEyeGeometryOverlayOpenMetrics *open_metrics = nullptr);

} // namespace crimson::zarr
