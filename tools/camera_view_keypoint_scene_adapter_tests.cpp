#include "gui/camera_view_keypoint_scene_adapter.h"

#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

crimson::zarr::KeypointOverlayDetection
detection(uint64_t key, int64_t detection_index, double offset) {
  crimson::zarr::KeypointOverlayDetection value;
  value.instance_key = key;
  value.detection_index = detection_index;
  value.keypoints = {{10.0 + offset, 20.0}, {12.0 + offset, 18.0}};
  value.keypoint_valid = {1, 1};
  value.heading_origin =
      crimson::zarr::KeypointOverlayPoint{11.0 + offset, 19.0};
  value.heading_degrees = 35.0;
  value.heading_valid = true;
  value.keypoint_usable = true;
  return value;
}

bool testLayerSeparationAndCompleteFrame() {
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  descriptor.run_name = "refined_fixture";
  descriptor.refined = true;
  descriptor.keypoint_labels = {"center", "eye"};
  descriptor.skeleton_edges = {{0, 1}};

  crimson::zarr::KeypointOverlayResolution frame;
  frame.status = crimson::zarr::KeypointOverlayStatus::Mapped;
  frame.camera_frame = 7;
  frame.detections = {detection(101, 4, 0.0), detection(202, 9, 20.0)};

  const auto markers = crimson::gui::makeCameraViewKeypointMarkerScene(
      descriptor, frame, 0, 7, 100, 80, 9);
  CHECK(markers.ready());
  CHECK(markers.count(crimson::overlay::PrimitiveType::Arrow) == 0);
  CHECK(markers.count(crimson::overlay::PrimitiveType::Marker) == 2);
  CHECK(markers.count(crimson::overlay::PrimitiveType::Polyline) == 2);

  const auto headings = crimson::gui::makeCameraViewKeypointHeadingScene(
      descriptor, frame, 0, 7, 100, 80);
  CHECK(headings.ready());
  CHECK(headings.count(crimson::overlay::PrimitiveType::Arrow) == 2);
  CHECK(headings.count(crimson::overlay::PrimitiveType::Marker) == 0);
  return true;
}

bool testStaleFrameDoesNotPublish() {
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  descriptor.run_name = "fixture";
  descriptor.keypoint_labels = {"point"};
  crimson::zarr::KeypointOverlayResolution frame;
  frame.status = crimson::zarr::KeypointOverlayStatus::Mapped;
  frame.camera_frame = 5;
  frame.detections = {detection(303, 0, 0.0)};
  const auto scene = crimson::gui::makeCameraViewKeypointMarkerScene(
      descriptor, frame, 0, 6, 100, 80);
  CHECK(!scene.ready());
  CHECK(scene.primitives.empty());
  return true;
}

} // namespace

int main() {
  if (!testLayerSeparationAndCompleteFrame() ||
      !testStaleFrameDoesNotPublish()) {
    return 1;
  }
  std::cout << "camera_view_keypoint_scene_adapter_tests: PASS\n";
  return 0;
}
