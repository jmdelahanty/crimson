#pragma once

#include "gui/eye_geometry_overlay_controls.h"
#include "read_only_overlay_controls.h"

namespace crimson::gui {

EyeGeometryOverlayControlState makeReadOnlyEyeGeometryOverlayControlState(
    const overlay::ReadOnlyOverlayControlState &source);

void applyReadOnlyEyeGeometryOverlayControlState(
    const EyeGeometryOverlayControlState &source,
    overlay::ReadOnlyOverlayControlState *destination);

} // namespace crimson::gui
