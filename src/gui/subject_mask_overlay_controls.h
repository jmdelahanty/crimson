#pragma once

#include "read_only_overlay_controls.h"

namespace crimson::gui {

struct SubjectMaskOverlayControlState {
  overlay::ReadOnlyMaskOverlayMode mode =
      overlay::ReadOnlyMaskOverlayMode::Review;
  bool show_subject_body = true;
  bool show_swim_bladder = true;
  bool show_left_eye = true;
  bool show_right_eye = true;
};

struct SubjectMaskOverlayControlCapabilities {
  bool show_mode = false;
  bool mode_available = false;
  bool show_components = false;
  bool subject_body_available = false;
  bool swim_bladder_available = false;
  bool left_eye_available = false;
  bool right_eye_available = false;
};

struct SubjectMaskOverlayControlOptions {
  const char *mode_tooltip = nullptr;
};

struct SubjectMaskOverlayControlResult {
  bool changed = false;
};

SubjectMaskOverlayControlResult drawSubjectMaskOverlayControls(
    SubjectMaskOverlayControlState &state,
    const SubjectMaskOverlayControlCapabilities &capabilities,
    const SubjectMaskOverlayControlOptions &options = {});

} // namespace crimson::gui
