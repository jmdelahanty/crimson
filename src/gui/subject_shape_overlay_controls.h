#pragma once

namespace crimson::gui {

struct SubjectShapeOverlayControlState {
  bool show_overlay = true;
  bool show_body_contour = false;
  bool show_swim_bladder_contour = false;
  bool show_eye_contours = false;
  bool show_body_frame_axes = false;
  bool show_snout_tip = true;
  bool show_caudal_anchor = true;
  bool show_tail_base = true;
  bool show_tail_tip = true;
  bool show_centerline = true;
  bool show_bspline_sample = true;
  bool show_bspline_debug_points = false;
  bool show_bspline_control_points = false;
  bool show_tail_samples = false;
  bool show_tail_normals = false;
};

struct SubjectShapeOverlayControlCapabilities {
  bool available = false;
  bool body_contour = false;
  bool swim_bladder_contour = false;
  bool eye_contours = false;
};

struct SubjectShapeOverlayControlResult {
  bool changed = false;
};

SubjectShapeOverlayControlResult drawSubjectShapeOverlayControls(
    SubjectShapeOverlayControlState &state,
    const SubjectShapeOverlayControlCapabilities &capabilities);

} // namespace crimson::gui
