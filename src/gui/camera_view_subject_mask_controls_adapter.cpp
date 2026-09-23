#include "gui/camera_view_subject_mask_controls_adapter.h"

crimson::gui::SubjectMaskOverlayControlState
makeCameraViewSubjectMaskOverlayControlState(
    const CameraViewMaskOverlayOptions &source) {
  crimson::gui::SubjectMaskOverlayControlState result;
  result.mode = source.mode;
  result.show_subject_body = source.show_subject_body;
  result.show_swim_bladder = source.show_swim_bladder;
  result.show_left_eye = source.show_eye_left;
  result.show_right_eye = source.show_eye_right;
  return result;
}

void applyCameraViewSubjectMaskOverlayControlState(
    const crimson::gui::SubjectMaskOverlayControlState &source,
    CameraViewMaskOverlayOptions *destination) {
  if (destination == nullptr) {
    return;
  }
  destination->mode = source.mode;
  destination->show_subject_body = source.show_subject_body;
  destination->show_swim_bladder = source.show_swim_bladder;
  destination->show_eye_left = source.show_left_eye;
  destination->show_eye_right = source.show_right_eye;
}
