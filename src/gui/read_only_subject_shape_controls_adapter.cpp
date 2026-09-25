#include "gui/read_only_subject_shape_controls_adapter.h"

namespace crimson::gui {

SubjectShapeOverlayControlState makeReadOnlySubjectShapeOverlayControlState(
    const overlay::ReadOnlyOverlayControlState &source) {
  SubjectShapeOverlayControlState result;
  result.show_overlay = source.show_subject_shape;
  result.show_body_frame_axes = source.show_subject_shape_body_axes;
  result.show_snout_tip = source.show_subject_shape_snout_tip;
  result.show_caudal_anchor = source.show_subject_shape_caudal_anchor;
  result.show_tail_base = source.show_subject_shape_tail_base;
  result.show_tail_tip = source.show_subject_shape_tail_tip;
  result.show_centerline = source.show_subject_shape_centerline;
  result.show_bspline_sample = source.show_subject_shape_bspline;
  result.show_bspline_debug_points =
      source.show_subject_shape_bspline_debug_points;
  result.show_bspline_control_points =
      source.show_subject_shape_bspline_control_points;
  result.show_tail_samples = source.show_subject_shape_tail_samples;
  result.show_tail_normals = source.show_subject_shape_tail_normals;
  return result;
}

void applyReadOnlySubjectShapeOverlayControlState(
    const SubjectShapeOverlayControlState &source,
    overlay::ReadOnlyOverlayControlState *destination) {
  if (destination == nullptr) {
    return;
  }
  destination->show_subject_shape = source.show_overlay;
  destination->show_subject_shape_body_axes = source.show_body_frame_axes;
  destination->show_subject_shape_snout_tip = source.show_snout_tip;
  destination->show_subject_shape_caudal_anchor = source.show_caudal_anchor;
  destination->show_subject_shape_tail_base = source.show_tail_base;
  destination->show_subject_shape_tail_tip = source.show_tail_tip;
  destination->show_subject_shape_centerline = source.show_centerline;
  destination->show_subject_shape_bspline = source.show_bspline_sample;
  destination->show_subject_shape_bspline_debug_points =
      source.show_bspline_debug_points;
  destination->show_subject_shape_bspline_control_points =
      source.show_bspline_control_points;
  destination->show_subject_shape_tail_samples = source.show_tail_samples;
  destination->show_subject_shape_tail_normals = source.show_tail_normals;
}

} // namespace crimson::gui
