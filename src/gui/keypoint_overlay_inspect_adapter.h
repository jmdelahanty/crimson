#pragma once

#include "gui/frame_inspect_keypoint_module.h"
#include "zarr/keypoint_overlay_repository.h"

namespace crimson::gui {

KeypointInspectPresentation makeKeypointOverlayInspectPresentation(
    const zarr::KeypointOverlayDescriptor *descriptor,
    const zarr::KeypointOverlayResolution *frame, int64_t presented_frame);

} // namespace crimson::gui
