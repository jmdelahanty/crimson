#pragma once

#include "read_only_overlay_scene.h"
#include "zarr/canonical_detection_repository.h"

namespace crimson::zarr {

overlay::ReadOnlyOverlayInput makeCanonicalDetectionOverlaySceneInput(
    const CanonicalDetectionDescriptor &descriptor,
    const CanonicalDetectionFrame &frame, int surface_view,
    int64_t surface_frame, int overlay_view, int full_frame_width,
    int full_frame_height);

} // namespace crimson::zarr
