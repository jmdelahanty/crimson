#include "gui/read_only_eye_geometry_controls_adapter.h"

namespace crimson::gui {

EyeGeometryOverlayControlState makeReadOnlyEyeGeometryOverlayControlState(
    const overlay::ReadOnlyOverlayControlState &source) {
  EyeGeometryOverlayControlState result;
  result.show_overlay = source.show_eye_geometry;
  result.show_direction_beams = source.show_eye_direction_beams;
  result.show_gaze_rays = source.show_eye_gaze_rays;
  result.show_angle_arcs = source.show_eye_angle_arcs;
  result.show_angle_labels = source.show_eye_angle_labels;
  return result;
}

void applyReadOnlyEyeGeometryOverlayControlState(
    const EyeGeometryOverlayControlState &source,
    overlay::ReadOnlyOverlayControlState *destination) {
  if (destination == nullptr) {
    return;
  }
  destination->show_eye_geometry = source.show_overlay;
  destination->show_eye_direction_beams = source.show_direction_beams;
  destination->show_eye_gaze_rays = source.show_gaze_rays;
  destination->show_eye_angle_arcs = source.show_angle_arcs;
  destination->show_eye_angle_labels = source.show_angle_labels;
}

} // namespace crimson::gui
