#pragma once

#include "gui/frame_inspect_subject_mask_module.h"
#include "zarr/subject_mask_overlay_repository.h"

namespace crimson::gui {

SubjectMaskInspectPresentation makeSubjectMaskOverlayInspectPresentation(
    const zarr::SubjectMaskOverlayDescriptor *descriptor,
    const zarr::SubjectMaskOverlayResolution *frame, int64_t presented_frame);

} // namespace crimson::gui
