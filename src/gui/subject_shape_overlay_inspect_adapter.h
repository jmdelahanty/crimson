#pragma once

#include "gui/frame_inspect_subject_shape_module.h"
#include "zarr/subject_shape_overlay_repository.h"

namespace crimson::gui {

SubjectShapeInspectPresentation makeSubjectShapeOverlayInspectPresentation(
    const zarr::SubjectShapeOverlayDescriptor *descriptor,
    const zarr::SubjectShapeOverlayResolution *frame, int64_t presented_frame);

} // namespace crimson::gui
