#include "gui/camera_view_subject_shape_controls_adapter.h"

crimson::gui::SubjectShapeOverlayControlState
makeCameraViewSubjectShapeOverlayControlState(
    const CameraViewSubjectShapeOverlayOptions &source) {
  crimson::gui::SubjectShapeOverlayControlState result;
  result.show_overlay = source.show_overlay;
  result.show_body_contour = source.show_body_contour;
  result.show_swim_bladder_contour = source.show_swim_bladder_contour;
  result.show_eye_contours = source.show_eye_contours;
  result.show_body_frame_axes = source.show_body_frame_axes;
  result.show_snout_tip = source.show_snout_tip;
  result.show_caudal_anchor = source.show_caudal_anchor;
  result.show_tail_base = source.show_tail_base;
  result.show_tail_tip = source.show_tail_tip;
  result.show_centerline = source.show_centerline;
  result.show_bspline_sample = source.show_bspline_sample;
  result.show_bspline_debug_points = source.show_bspline_debug_points;
  result.show_bspline_control_points = source.show_bspline_control_points;
  result.show_tail_samples = source.show_tail_samples;
  result.show_tail_normals = source.show_tail_normals;
  return result;
}

void applyCameraViewSubjectShapeOverlayControlState(
    const crimson::gui::SubjectShapeOverlayControlState &source,
    CameraViewSubjectShapeOverlayOptions *destination) {
  if (destination == nullptr) {
    return;
  }
  destination->show_overlay = source.show_overlay;
  destination->show_body_contour = source.show_body_contour;
  destination->show_swim_bladder_contour = source.show_swim_bladder_contour;
  destination->show_eye_contours = source.show_eye_contours;
  destination->show_body_frame_axes = source.show_body_frame_axes;
  destination->show_snout_tip = source.show_snout_tip;
  destination->show_caudal_anchor = source.show_caudal_anchor;
  destination->show_tail_base = source.show_tail_base;
  destination->show_tail_tip = source.show_tail_tip;
  destination->show_centerline = source.show_centerline;
  destination->show_bspline_sample = source.show_bspline_sample;
  destination->show_bspline_debug_points = source.show_bspline_debug_points;
  destination->show_bspline_control_points = source.show_bspline_control_points;
  destination->show_tail_samples = source.show_tail_samples;
  destination->show_tail_normals = source.show_tail_normals;
}
