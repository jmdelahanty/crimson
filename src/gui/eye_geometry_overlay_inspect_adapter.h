#pragma once

#include "gui/frame_inspect_eye_angle_module.h"
#include "zarr/eye_geometry_overlay_repository.h"

namespace crimson::gui {

EyeAngleInspectPresentation makeEyeGeometryOverlayInspectPresentation(
    const zarr::EyeGeometryOverlayDescriptor *descriptor,
    const zarr::EyeGeometryOverlayResolution *frame, int64_t presented_frame);

} // namespace crimson::gui
