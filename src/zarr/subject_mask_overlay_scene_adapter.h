#pragma once

#include "read_only_overlay_scene.h"
#include "zarr/subject_mask_overlay_repository.h"

namespace crimson::zarr {

overlay::ReadOnlyOverlayInput
makeSubjectMaskOverlaySceneInput(const SubjectMaskOverlayDescriptor &descriptor,
                                 const SubjectMaskOverlayResolution &resolution,
                                 int surface_view, int64_t surface_frame,
                                 int overlay_view, int full_frame_width,
                                 int full_frame_height);

bool appendSubjectMaskOverlaySceneInput(
    const SubjectMaskOverlayDescriptor &descriptor,
    const SubjectMaskOverlayResolution &resolution, int64_t surface_frame,
    overlay::ReadOnlyOverlayInput *input);

} // namespace crimson::zarr
