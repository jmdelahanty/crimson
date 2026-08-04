#pragma once

#include "gui/subject_shape_overlay_controls.h"
#include "read_only_overlay_controls.h"

namespace crimson::gui {

SubjectShapeOverlayControlState makeReadOnlySubjectShapeOverlayControlState(
    const overlay::ReadOnlyOverlayControlState &source);

void applyReadOnlySubjectShapeOverlayControlState(
    const SubjectShapeOverlayControlState &source,
    overlay::ReadOnlyOverlayControlState *destination);

} // namespace crimson::gui
