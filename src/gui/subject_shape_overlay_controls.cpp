#include "gui/subject_shape_overlay_controls.h"

#include "imgui.h"

namespace crimson::gui {
namespace {

bool drawAvailableCheckbox(const char *label, bool *value, bool available) {
  ImGui::BeginDisabled(!available);
  const bool changed = ImGui::Checkbox(label, value);
  ImGui::EndDisabled();
  return changed;
}

} // namespace

SubjectShapeOverlayControlResult drawSubjectShapeOverlayControls(
    SubjectShapeOverlayControlState &state,
    const SubjectShapeOverlayControlCapabilities &capabilities) {
  SubjectShapeOverlayControlResult result;
  const bool any_contour_available = capabilities.body_contour ||
      capabilities.swim_bladder_contour || capabilities.eye_contours;
  result.changed |= drawAvailableCheckbox(
      capabilities.master_label, &state.show_overlay,
      capabilities.available || any_contour_available);

  const bool details_available = capabilities.available && state.show_overlay;
  const bool contours_enabled = state.show_overlay;
  result.changed |= drawAvailableCheckbox("Snout tip", &state.show_snout_tip,
                                          details_available);
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox("Tail base", &state.show_tail_base,
                                          details_available);
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox("Tail tip", &state.show_tail_tip,
                                          details_available);
  result.changed |= drawAvailableCheckbox(
      "Caudal anchor", &state.show_caudal_anchor, details_available);
  result.changed |= drawAvailableCheckbox("Centerline", &state.show_centerline,
                                          details_available);
  result.changed |=
      drawAvailableCheckbox("Dense B-spline centerline",
                            &state.show_bspline_sample,
                            details_available && capabilities.bspline_sample);
  result.changed |= drawAvailableCheckbox(
      "Body frame axes", &state.show_body_frame_axes, details_available);

  if (capabilities.body_contour) {
    result.changed |= drawAvailableCheckbox(
        "Body contour", &state.show_body_contour, contours_enabled);
  }
  if (capabilities.swim_bladder_contour) {
    if (capabilities.body_contour) {
      ImGui::SameLine();
    }
    result.changed |= drawAvailableCheckbox("Swim-bladder contour",
                                            &state.show_swim_bladder_contour,
                                            contours_enabled);
  }
  if (capabilities.eye_contours) {
    result.changed |= drawAvailableCheckbox(
        "Eye contours", &state.show_eye_contours, contours_enabled);
  }

  result.changed |= drawAvailableCheckbox("Spline debug points",
                                          &state.show_bspline_debug_points,
                                          details_available && capabilities.bspline_sample);
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox(
      "Control points", &state.show_bspline_control_points,
      details_available && capabilities.bspline_control_points);
  result.changed |= drawAvailableCheckbox(
      "Tail samples", &state.show_tail_samples,
      details_available && capabilities.tail_samples);
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox(
      "Tail normals", &state.show_tail_normals,
      details_available && capabilities.tail_samples);
  return result;
}

} // namespace crimson::gui
