#include "read_only_overlay_scene.h"
#include "zarr/keypoint_overlay_scene_adapter.h"
#include "zarr/keypoint_v2_contract.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':'    \
                << __LINE__ << '\n';                                           \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool TestMultiObservationFrameIndex() {
  const std::vector<int64_t> offsets = {0, 2, 2, 3, 6};
  const std::vector<int64_t> frames = {0, 0, 2, 3, 3, 3};
  std::string error;
  CHECK(crimson::zarr::ValidateKeypointV2Offsets(offsets, 4, 6, &error));
  CHECK(crimson::zarr::ValidateKeypointV2FrameIndex(offsets, frames, 4, 6,
                                                    &error));
  CHECK(offsets[1] - offsets[0] == 2);
  CHECK(offsets[2] - offsets[1] == 0);
  CHECK(offsets[3] - offsets[2] == 1);
  CHECK(offsets[4] - offsets[3] == 3);

  auto malformed = offsets;
  malformed[2] = 1;
  error.clear();
  CHECK(!crimson::zarr::ValidateKeypointV2Offsets(malformed, 4, 6, &error));
  CHECK(!error.empty());
  malformed = offsets;
  malformed.back() = 5;
  CHECK(!crimson::zarr::ValidateKeypointV2Offsets(malformed, 4, 6));
  auto wrong_frames = frames;
  wrong_frames[2] = 1;
  CHECK(!crimson::zarr::ValidateKeypointV2FrameIndex(offsets, wrong_frames, 4,
                                                     6));
  CHECK(crimson::zarr::ValidateKeypointV2Offsets({0, 0, 0, 0}, 3, 0));
  CHECK(crimson::zarr::ValidateKeypointV2FrameIndex({0, 0, 0, 0}, {}, 3, 0));
  return true;
}

bool TestStableObservationKeys() {
  const std::vector<uint64_t> keys = {17, 23, 29, 31, 37, 41};
  CHECK(crimson::zarr::ValidateKeypointV2InstanceKeys(keys, keys.size()));
  auto duplicate = keys;
  duplicate.back() = duplicate.front();
  std::string error;
  CHECK(!crimson::zarr::ValidateKeypointV2InstanceKeys(
      duplicate, duplicate.size(), &error));
  CHECK(!error.empty());
  CHECK(!crimson::zarr::ValidateKeypointV2InstanceKeys(keys, keys.size() - 1));
  return true;
}

bool TestPresentationPreservesRefinedState() {
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  descriptor.source_group = "refined_keypoints_runs";
  descriptor.run_name = "refined_v2";
  descriptor.coordinate_space = crimson::zarr::KeypointCoordinateSpace::Image;
  descriptor.refined = true;
  descriptor.keypoint_labels = {"center", "left", "right"};
  descriptor.skeleton_edges = {{0, 1}, {0, 2}};
  descriptor.camera_frame_count = 4;
  descriptor.row_count = 6;

  crimson::zarr::KeypointOverlayResolution resolution;
  resolution.status = crimson::zarr::KeypointOverlayStatus::Mapped;
  resolution.camera_frame = 3;
  crimson::zarr::KeypointOverlayDetection detection;
  detection.instance_key = 0xfedcba9876543210ULL;
  detection.detection_index = 5;
  detection.keypoints = {{10.0, 20.0}, {12.0, 18.0}, {14.0, 18.0}};
  detection.heading_origin = crimson::zarr::KeypointOverlayPoint{13.0, 18.0};
  detection.heading_degrees = 35.0;
  detection.heading_valid = true;
  detection.heading_from_body_frame = true;
  detection.refined_keypoints = true;
  detection.source_success = false;
  detection.refined_success = true;
  detection.confidence_valid = false;
  detection.geometry_valid = true;
  detection.keypoint_usable = true;
  detection.review_state_code = 1;
  detection.reason_code = 3;
  detection.keypoint_edit_flags = {1, 1, 1};
  resolution.detections.push_back(detection);

  const auto input = crimson::zarr::makeKeypointOverlaySceneInput(
      descriptor, resolution, 0, 3, 0, 4512, 4512);
  CHECK(input.detections.size() == 1);
  const auto &adapted = input.detections.front();
  CHECK(adapted.instance_key == detection.instance_key);
  CHECK(adapted.source_success == detection.source_success);
  CHECK(adapted.refined_success == detection.refined_success);
  CHECK(adapted.review_state_code == detection.review_state_code);
  CHECK(adapted.reason_code == detection.reason_code);
  CHECK(adapted.keypoint_edit_flags == detection.keypoint_edit_flags);
  CHECK(adapted.heading_from_body_frame);

  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(scene.ready());
  CHECK(scene.count(crimson::overlay::PrimitiveType::Arrow) == 1);
  CHECK(scene.count(crimson::overlay::PrimitiveType::Marker) == 3);
  CHECK(std::all_of(scene.primitives.begin(), scene.primitives.end(),
                    [&](const auto &primitive) {
                      return primitive.instance_key == detection.instance_key;
                    }));
  return true;
}

} // namespace

int main() {
  if (!TestMultiObservationFrameIndex() || !TestStableObservationKeys() ||
      !TestPresentationPreservesRefinedState()) {
    return 1;
  }
  std::cout << "keypoint_v2_contract_tests: PASS\n";
  return 0;
}
