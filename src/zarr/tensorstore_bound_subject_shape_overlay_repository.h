#pragma once

#include "zarr/canonical_overlay_selection.h"
#include "zarr/subject_shape_overlay_repository.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;
class SharedMaskFrameIndex;

struct BoundSubjectShapeOverlayOpenRequest {
  std::shared_ptr<ArchiveContext> archive;
  CanonicalOverlaySelection selection;
  size_t max_observations_per_frame = 16;
  std::shared_ptr<const SharedMaskFrameIndex> shared_mask_frame_index;
  uint64_t max_decoded_frame_bytes = 16ULL * 1024ULL * 1024ULL;
};

struct SubjectShapeOverlayOpenMetrics {
  size_t exact_handle_opens = 0;
  size_t offset_read_calls = 0;
  size_t maximum_observations_per_frame = 0;
  uint64_t retained_offset_bytes = 0;
  uint64_t borrowed_offset_bytes = 0;
  uint64_t maximum_decoded_frame_bytes = 0;
};

// Opens only the exact shape-v5 and strict-mask pair already validated by the
// immutable canonical overlay selection. It never performs independent latest
// selection and retains only the mask frame offsets. Published shape-v5 points
// are already in the direct source-camera frame; mask placement is read only
// as row/provenance evidence. Payloads are read in bounded frame ranges.
std::unique_ptr<SubjectShapeOverlayRepository>
OpenBoundSubjectShapeOverlayRepository(
    const BoundSubjectShapeOverlayOpenRequest &request,
    std::string *error_message = nullptr,
    SubjectShapeOverlayOpenMetrics *open_metrics = nullptr);

} // namespace crimson::zarr
