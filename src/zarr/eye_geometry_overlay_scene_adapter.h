#pragma once

#include "read_only_overlay_scene.h"
#include "zarr/eye_geometry_overlay_repository.h"

#include <cstdint>

namespace crimson::zarr {

bool appendEyeGeometryOverlaySceneInput(
    const EyeGeometryOverlayDescriptor &descriptor,
    const EyeGeometryOverlayResolution &resolution, int64_t surface_frame,
    overlay::ReadOnlyOverlayInput *input);

overlay::ReadOnlyOverlayInput
makeEyeGeometryOverlaySceneInput(const EyeGeometryOverlayDescriptor &descriptor,
                                 const EyeGeometryOverlayResolution &resolution,
                                 int surface_view, int64_t surface_frame,
                                 int overlay_view, int full_frame_width,
                                 int full_frame_height);

} // namespace crimson::zarr
