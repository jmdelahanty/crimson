#include "gui/eye_geometry_overlay_controls.h"

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

EyeGeometryOverlayControlResult drawEyeGeometryOverlayControls(
    EyeGeometryOverlayControlState &state,
    const EyeGeometryOverlayControlCapabilities &capabilities) {
  EyeGeometryOverlayControlResult result;
  if (capabilities.show_overlay_toggle) {
    result.changed |= drawAvailableCheckbox(
        "Show eye geometry", &state.show_overlay, capabilities.available);
  }

  const bool details_available =
      capabilities.available &&
      (!capabilities.show_overlay_toggle || state.show_overlay);
  result.changed |= drawAvailableCheckbox(
      "Eye visual cones", &state.show_direction_beams, details_available);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Draw translucent 163 degree visual-field cones from each eye, "
        "centered on Palette gaze vectors when available and falling back "
        "to the ellipse minor axis.");
  }
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox("Gaze rays", &state.show_gaze_rays,
                                          details_available);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Draw Palette eye-angle gaze vectors from left_gaze_xy/right_gaze_xy "
        "when available.");
  }
  result.changed |= drawAvailableCheckbox(
      "Eye angle arcs", &state.show_angle_arcs, details_available);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Draw signed gaze-angle arcs from the body-frame forward axis to the "
        "ellipse minor-axis gaze direction.");
  }
  ImGui::SameLine();
  result.changed |= drawAvailableCheckbox(
      "Angle labels", &state.show_angle_labels, details_available);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Draw text labels for eye-frame per-eye angles and vergence; legacy "
        "archives fall back to explicitly labeled gaze-signed values.");
  }
  return result;
}

} // namespace crimson::gui
