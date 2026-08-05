#pragma once

#include "gui/subject_mask_overlay_controls.h"
#include "read_only_overlay_controls.h"

namespace crimson::gui {

SubjectMaskOverlayControlState makeReadOnlySubjectMaskOverlayControlState(
    const overlay::ReadOnlyOverlayControlState &source);

void applyReadOnlySubjectMaskOverlayControlState(
    const SubjectMaskOverlayControlState &source,
    overlay::ReadOnlyOverlayControlState *destination);

} // namespace crimson::gui
