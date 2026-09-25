#pragma once

#include "gui/camera_view_overlay_renderer.h"
#include "gui/subject_shape_overlay_controls.h"

crimson::gui::SubjectShapeOverlayControlState
makeCameraViewSubjectShapeOverlayControlState(
    const CameraViewSubjectShapeOverlayOptions &source);

void applyCameraViewSubjectShapeOverlayControlState(
    const crimson::gui::SubjectShapeOverlayControlState &source,
    CameraViewSubjectShapeOverlayOptions *destination);

bool cameraViewCanonicalMaskContoursRequested(
    const CameraViewSubjectShapeOverlayOptions &source);

void applyCameraViewCanonicalSubjectShapeControls(
    const CameraViewSubjectShapeOverlayOptions &source,
    crimson::overlay::ReadOnlyOverlayControlState *destination);
