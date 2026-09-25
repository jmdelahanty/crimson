#include "gui/canonical_eye_demand.h"
#include "gui/eye_geometry_overlay_inspect_adapter.h"

#include <iostream>

using namespace crimson;
using namespace crimson::zarr::EyeGeometryFields;
#define CHECK(value) do { if (!(value)) { \
  std::cerr << "CHECK failed: " #value << " line " << __LINE__ << '\n'; \
  return 1; } } while (false)

int main() {
  overlay::ReadOnlyOverlayControlState controls;
  CHECK(gui::canonicalCameraEyeFields(controls) == All);
  controls.mask_mode = overlay::ReadOnlyMaskOverlayMode::Realtime;
  CHECK(gui::canonicalCameraEyeFields(controls) == 0);
  controls.mask_mode = overlay::ReadOnlyMaskOverlayMode::Review;
  controls.show_eye_geometry = false;
  CHECK(gui::canonicalCameraEyeFields(controls) == 0);
  controls.show_eye_geometry = true;
  controls.show_eye_gaze_rays = controls.show_eye_direction_beams = false;
  controls.show_eye_angle_arcs = controls.show_eye_angle_labels = false;
  CHECK(gui::canonicalCameraEyeFields(controls) == (LeftGeometry | RightGeometry));
  controls.show_eye_left_mask = false;
  CHECK(gui::canonicalCameraEyeFields(controls) == RightGeometry);
  controls.show_eye_direction_beams = true;
  // Cone direction consumes gaze even when the ray itself is hidden.
  CHECK(gui::canonicalCameraEyeFields(controls) ==
        zarr::NormalizeEyeGeometryFields(RightGaze));
  controls.show_eye_direction_beams = false;
  controls.show_eye_angle_labels = true;
  CHECK(gui::canonicalCameraEyeFields(controls) ==
        zarr::NormalizeEyeGeometryFields(RightAngle | RightSigned));
  controls.show_eye_left_mask = true;
  CHECK(!(gui::canonicalCameraEyeFields(controls) & Vergence));
  controls.show_eye_direction_beams = true;
  CHECK(gui::canonicalCameraEyeFields(controls) & Vergence);
  controls.show_eye_left_mask = controls.show_eye_right_mask = false;
  CHECK(gui::canonicalCameraEyeFields(controls) == 0);
  CHECK(gui::canonicalInspectEyeFields(false, "gaze") == 0);
  CHECK(gui::canonicalInspectEyeFields(true, "gaze") ==
        zarr::NormalizeEyeGeometryFields(LeftGaze | RightGaze));
  CHECK(gui::canonicalInspectEyeFields(true, "signed") ==
        zarr::NormalizeEyeGeometryFields(LeftSigned | RightSigned));
  CHECK(gui::canonicalInspectEyeFields(true, "") ==
        zarr::NormalizeEyeGeometryFields(LeftAngle | RightAngle | Vergence));
  CHECK(!zarr::EyeGeometryFieldsCover(LeftGeometry, LeftAngle));
  CHECK(zarr::EyeGeometryFieldsCover(All, RightAngle));

  zarr::EyeGeometryOverlayDescriptor descriptor;
  descriptor.run_name = "fixture";
  zarr::EyeGeometryOverlayResolution frame;
  frame.camera_frame = 7;
  frame.status = zarr::EyeGeometryOverlayStatus::Mapped;
  frame.loaded_fields = LeftGeometry;
  frame.detections.emplace_back();
  frame.detections[0].frame_valid = true;
  frame.detections[0].eyes[0].valid = true;
  auto inspect = gui::makeEyeGeometryOverlayInspectPresentation(&descriptor, &frame, 7);
  CHECK(inspect.frame_ready && inspect.observations.size() == 1);
  CHECK(inspect.observations[0].left_valid_known);
  CHECK(!inspect.observations[0].right_valid_known);
  for (const auto& field : inspect.observations[0].fields)
    CHECK(!field.loaded && !field.valid);
  frame.loaded_fields = zarr::NormalizeEyeGeometryFields(LeftAngle | RightAngle | Vergence);
  inspect = gui::makeEyeGeometryOverlayInspectPresentation(&descriptor, &frame, 7);
  CHECK(inspect.observations[0].fields[0].loaded);
  CHECK(!inspect.observations[0].fields[0].valid); // A loaded invalid measurement.
  CHECK(!inspect.observations[0].fields[3].loaded); // Not an invalid signed angle.
  frame.loaded_fields = All;
  inspect = gui::makeEyeGeometryOverlayInspectPresentation(&descriptor, &frame, 7);
  for (const auto& field : inspect.observations[0].fields) CHECK(field.loaded);
  CHECK(!gui::makeEyeGeometryOverlayInspectPresentation(&descriptor, &frame, 8).frame_ready);
  std::cout << "canonical_eye_demand_tests passed\n";
}
