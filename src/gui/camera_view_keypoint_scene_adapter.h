#pragma once

#include "read_only_overlay_scene.h"
#include "zarr/keypoint_overlay_repository.h"

namespace crimson::gui {

overlay::ReadOnlyOverlayScene makeCameraViewKeypointMarkerScene(
    const zarr::KeypointOverlayDescriptor &descriptor,
    const zarr::KeypointOverlayResolution &frame, int view_index,
    int64_t presented_frame, int full_frame_width, int full_frame_height,
    int64_t suppressed_detection_index = -1);

overlay::ReadOnlyOverlayScene makeCameraViewKeypointHeadingScene(
    const zarr::KeypointOverlayDescriptor &descriptor,
    const zarr::KeypointOverlayResolution &frame, int view_index,
    int64_t presented_frame, int full_frame_width, int full_frame_height);

} // namespace crimson::gui
