#pragma once

#include "read_only_overlay_scene.h"
#include "zarr/subject_shape_overlay_repository.h"

namespace crimson::zarr {

bool appendSubjectShapeOverlaySceneInput(
    const SubjectShapeOverlayDescriptor &descriptor,
    const SubjectShapeOverlayResolution &resolution, int64_t surface_frame,
    overlay::ReadOnlyOverlayInput *input);

overlay::ReadOnlyOverlayInput makeSubjectShapeOverlaySceneInput(
    const SubjectShapeOverlayDescriptor &descriptor,
    const SubjectShapeOverlayResolution &resolution, int surface_view,
    int64_t surface_frame, int overlay_view, int full_frame_width,
    int full_frame_height);

} // namespace crimson::zarr
