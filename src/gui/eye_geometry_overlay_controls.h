#pragma once

namespace crimson::gui {

struct EyeGeometryOverlayControlState {
  bool show_overlay = true;
  bool show_direction_beams = true;
  bool show_gaze_rays = true;
  bool show_angle_arcs = true;
  bool show_angle_labels = true;
};

struct EyeGeometryOverlayControlCapabilities {
  bool available = false;
  bool show_overlay_toggle = false;
};

struct EyeGeometryOverlayControlResult {
  bool changed = false;
};

EyeGeometryOverlayControlResult drawEyeGeometryOverlayControls(
    EyeGeometryOverlayControlState &state,
    const EyeGeometryOverlayControlCapabilities &capabilities);

} // namespace crimson::gui
