#pragma once

#include "read_only_overlay_scene.h"

namespace crimson::gui {

struct CameraViewVectorSceneDrawCounts {
  int visual_cones = 0;
  int visual_cone_overlaps = 0;
  int angle_labels = 0;
};

// Call inside an active ImPlot plot. Polygons precede lines; text follows them.
CameraViewVectorSceneDrawCounts drawCameraViewScenePolygons(
    const overlay::ReadOnlyOverlayScene &scene, float image_height_px);
CameraViewVectorSceneDrawCounts drawCameraViewSceneText(
    const overlay::ReadOnlyOverlayScene &scene, float image_height_px);

} // namespace crimson::gui
