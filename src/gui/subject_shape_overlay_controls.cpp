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
  result.changed |= drawAvailableCheckbox(
      "Show subject shape", &state.show_overlay, capabilities.available);

  const bool details_available = capabilities.available && state.show_overlay;
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
                            &state.show_bspline_sample, details_available);
  result.changed |= drawAvailableCheckbox(
      "Body frame axes", &state.show_body_frame_axes, details_available);

  if (capabilities.body_contour) {
    result.changed |= drawAvailableCheckbox(
        "Body contour", &state.show_body_contour, details_available);
  }
  if (capabilities.swim_bladder_contour) {
    if (capabilities.body_contour) {
      ImGui::SameLine();
    }
    result.changed |= drawAvailableCheckbox("Swim-bladder contour",
                                            &state.show_swim_bladder_contour,
                                            details_available);
  }
  if (capabilities.eye_contours) {
    result.changed |= drawAvailableCheckbox(
        "Eye contours", &state.show_eye_contours, details_available);
  }

  result.changed |= drawAvailableCheckbox("Spline debug points",
                                          &state.show_bspline_debug_points,
                                          details_available);
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox(
      "Control points", &state.show_bspline_control_points, details_available);
  result.changed |= drawAvailableCheckbox(
      "Tail samples", &state.show_tail_samples, details_available);
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox(
      "Tail normals", &state.show_tail_normals, details_available);
  return result;
}

} // namespace crimson::gui
