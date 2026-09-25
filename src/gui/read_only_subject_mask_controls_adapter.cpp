#include "gui/read_only_subject_mask_controls_adapter.h"

namespace crimson::gui {

SubjectMaskOverlayControlState makeReadOnlySubjectMaskOverlayControlState(
    const overlay::ReadOnlyOverlayControlState &source) {
  SubjectMaskOverlayControlState result;
  result.mode = source.mask_mode;
  result.show_subject_body = source.show_subject_body_mask;
  result.show_swim_bladder = source.show_swim_bladder_mask;
  result.show_left_eye = source.show_eye_left_mask;
  result.show_right_eye = source.show_eye_right_mask;
  return result;
}

void applyReadOnlySubjectMaskOverlayControlState(
    const SubjectMaskOverlayControlState &source,
    overlay::ReadOnlyOverlayControlState *destination) {
  if (destination == nullptr) {
    return;
  }
  destination->mask_mode = source.mode;
  destination->show_subject_body_mask = source.show_subject_body;
  destination->show_swim_bladder_mask = source.show_swim_bladder;
  destination->show_eye_left_mask = source.show_left_eye;
  destination->show_eye_right_mask = source.show_right_eye;
}

} // namespace crimson::gui
