#pragma once

#include "read_only_overlay_scene.h"
#include "zarr/keypoint_overlay_repository.h"

#include <cstdint>

namespace crimson::zarr {

overlay::ReadOnlyOverlayInput makeKeypointOverlaySceneInput(
    const KeypointOverlayDescriptor& descriptor,
    const KeypointOverlayResolution& resolution,
    int surface_view,
    int64_t surface_frame,
    int overlay_view,
    int full_frame_width,
    int full_frame_height);

}  // namespace crimson::zarr
