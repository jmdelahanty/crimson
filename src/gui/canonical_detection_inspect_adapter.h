#pragma once

#include "gui/frame_inspect_detection_module.h"
#include "zarr/canonical_detection_repository.h"

namespace crimson::gui {

DetectionInspectPresentation makeCanonicalDetectionInspectPresentation(
    const zarr::CanonicalDetectionDescriptor *descriptor,
    const zarr::CanonicalDetectionFrame *frame, int64_t presented_frame);

} // namespace crimson::gui
