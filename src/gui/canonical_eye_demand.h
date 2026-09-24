#pragma once

#include "read_only_overlay_controls.h"
#include "zarr/eye_geometry_overlay_repository.h"

#include <string_view>

namespace crimson::gui {

// Keep scientific dependencies in NormalizeEyeGeometryFields; these functions
// express only which features the consumer will use.
inline zarr::EyeGeometryFieldMask canonicalCameraEyeFields(
    const overlay::ReadOnlyOverlayControlState& controls) {
  using namespace zarr::EyeGeometryFields;
  if (!controls.show_eye_geometry ||
      controls.mask_mode == overlay::ReadOnlyMaskOverlayMode::Realtime) return 0;
  zarr::EyeGeometryFieldMask fields = 0;
  const bool sides[] = {controls.show_eye_left_mask, controls.show_eye_right_mask};
  for (int eye = 0; eye < 2; ++eye) {
    if (!sides[eye]) continue;
    fields |= eye == 0 ? LeftGeometry : RightGeometry;
    if (controls.show_eye_gaze_rays || controls.show_eye_direction_beams)
      fields |= eye == 0 ? LeftGaze : RightGaze;
    // Signed angle is also the documented label fallback.
    if (controls.show_eye_angle_arcs || controls.show_eye_angle_labels)
      fields |= eye == 0 ? LeftSigned : RightSigned;
    if (controls.show_eye_angle_labels)
      fields |= eye == 0 ? LeftAngle : RightAngle;
  }
  // The scene places the vergence label between two rendered visual cones.
  if (controls.show_eye_angle_labels && controls.show_eye_direction_beams &&
      sides[0] && sides[1]) fields |= Vergence;
  return zarr::NormalizeEyeGeometryFields(fields);
}

inline zarr::EyeGeometryFieldMask canonicalInspectEyeFields(
    bool enabled, std::string_view representation) {
  using namespace zarr::EyeGeometryFields;
  if (!enabled) return 0;
  if (representation == "signed")
    return zarr::NormalizeEyeGeometryFields(LeftSigned | RightSigned);
  if (representation == "gaze")
    return zarr::NormalizeEyeGeometryFields(LeftGaze | RightGaze);
  // Empty/unknown representation follows the inspector's Eye frame default.
  return zarr::NormalizeEyeGeometryFields(LeftAngle | RightAngle | Vergence);
}

} // namespace crimson::gui
