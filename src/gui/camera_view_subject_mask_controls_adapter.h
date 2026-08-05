#pragma once

#include "gui/camera_view_overlay_renderer.h"
#include "gui/subject_mask_overlay_controls.h"

crimson::gui::SubjectMaskOverlayControlState
makeCameraViewSubjectMaskOverlayControlState(
    const CameraViewMaskOverlayOptions &source);

void applyCameraViewSubjectMaskOverlayControlState(
    const crimson::gui::SubjectMaskOverlayControlState &source,
    CameraViewMaskOverlayOptions *destination);
