#include "gui/camera_view_keypoint_scene_adapter.h"

#include "zarr/keypoint_overlay_scene_adapter.h"

namespace crimson::gui {

overlay::ReadOnlyOverlayScene makeCameraViewKeypointMarkerScene(
    const zarr::KeypointOverlayDescriptor &descriptor,
    const zarr::KeypointOverlayResolution &frame, int view_index,
    int64_t presented_frame, int full_frame_width, int full_frame_height,
    int64_t suppressed_detection_index) {
  auto input = zarr::makeKeypointOverlaySceneInput(
      descriptor, frame, view_index, presented_frame, view_index,
      full_frame_width, full_frame_height);
  input.show_headings = false;
  for (size_t index = 0;
       index < input.detections.size() && index < frame.detections.size();
       ++index) {
    input.detections[index].suppress_keypoint_markers =
        suppressed_detection_index >= 0 &&
        frame.detections[index].detection_index == suppressed_detection_index;
  }
  return overlay::buildReadOnlyOverlayScene(input);
}

overlay::ReadOnlyOverlayScene makeCameraViewKeypointHeadingScene(
    const zarr::KeypointOverlayDescriptor &descriptor,
    const zarr::KeypointOverlayResolution &frame, int view_index,
    int64_t presented_frame, int full_frame_width, int full_frame_height) {
  auto input = zarr::makeKeypointOverlaySceneInput(
      descriptor, frame, view_index, presented_frame, view_index,
      full_frame_width, full_frame_height);
  input.show_keypoints = false;
  return overlay::buildReadOnlyOverlayScene(input);
}

} // namespace crimson::gui
