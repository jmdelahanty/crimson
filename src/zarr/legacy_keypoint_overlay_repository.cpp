#include "zarr/legacy_keypoint_overlay_repository.h"

#include "zarr_loader.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace crimson::zarr {

LegacyKeypointOverlayRepository::LegacyKeypointOverlayRepository(
    const ZarrDetectionLoader &loader)
    : loader_(loader) {
  refreshDescriptor();
}

void LegacyKeypointOverlayRepository::refreshDescriptor() const {
  descriptor_.source_group = loader_.isRefinedKeypoints()
                                 ? "refined_keypoints_runs"
                                 : "keypoints_runs";
  descriptor_.run_name = loader_.getKeypointsRunName();
  descriptor_.coordinate_space = KeypointCoordinateSpace::Image;
  descriptor_.refined = loader_.isRefinedKeypoints();
  descriptor_.keypoint_labels = loader_.getKeypointLabels();
  descriptor_.skeleton_edges = loader_.getSkeletonEdges();
  descriptor_.camera_frame_count = loader_.getTotalFrames();
  descriptor_.row_count = 0;
}

const KeypointOverlayDescriptor &
LegacyKeypointOverlayRepository::descriptor() const {
  refreshDescriptor();
  return descriptor_;
}

KeypointOverlayResolution LegacyKeypointOverlayRepository::resolveCameraFrame(
    int64_t camera_frame, int full_frame_width, int full_frame_height) const {
  refreshDescriptor();
  KeypointOverlayResolution result;
  result.camera_frame = camera_frame;
  if (!loader_.hasKeypointData() || descriptor_.run_name.empty()) {
    result.status = KeypointOverlayStatus::Missing;
    return result;
  }
  if (camera_frame < 0 ||
      static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
    result.status = KeypointOverlayStatus::OutOfRange;
    return result;
  }
  if (full_frame_width <= 0 || full_frame_height <= 0) {
    result.status = KeypointOverlayStatus::InvalidDimensions;
    return result;
  }

  const auto legacy = loader_.getRawDetections(
      static_cast<size_t>(camera_frame), false, false, false, true, true);
  result.status = legacy.keypoints_pixels.empty()
                      ? KeypointOverlayStatus::Missing
                      : KeypointOverlayStatus::Mapped;
  result.detections.reserve(legacy.keypoints_pixels.size());
  for (size_t index = 0; index < legacy.keypoints_pixels.size(); ++index) {
    KeypointOverlayDetection detection;
    detection.detection_index = static_cast<int64_t>(index);
    detection.refined_keypoints = legacy.is_refined_keypoints;
    detection.detection_interpolated = index < legacy.detection_source.size() &&
                                       legacy.detection_source[index] != 0;
    detection.keypoint_detection_interpolated =
        index < legacy.keypoint_detection_source.size() &&
        legacy.keypoint_detection_source[index] != 0;
    detection.keypoint_flip_corrected =
        index < legacy.keypoint_flip_corrected.size() &&
        legacy.keypoint_flip_corrected[index] != 0;
    detection.keypoint_usable = !legacy.is_refined_keypoints ||
                                (index < legacy.keypoint_usable.size() &&
                                 legacy.keypoint_usable[index] != 0);
    detection.refined_success =
        index < legacy.keypoint_refined_success.size() &&
        legacy.keypoint_refined_success[index] != 0;

    detection.keypoints.reserve(legacy.keypoints_pixels[index].size());
    detection.keypoint_valid.reserve(legacy.keypoints_pixels[index].size());
    bool any_finite_point = false;
    for (const auto &point : legacy.keypoints_pixels[index]) {
      const bool valid = std::isfinite(point[0]) && std::isfinite(point[1]);
      detection.keypoints.push_back(
          {static_cast<double>(point[0]), static_cast<double>(point[1])});
      detection.keypoint_valid.push_back(valid ? uint8_t{1} : uint8_t{0});
      any_finite_point = any_finite_point || valid;
    }
    detection.source_success = any_finite_point;
    detection.geometry_valid = any_finite_point;

    if (index < legacy.boxes.size()) {
      const auto &box = legacy.boxes[index];
      const double width = static_cast<double>(box[2] - box[0]);
      const double height = static_cast<double>(box[3] - box[1]);
      if (std::all_of(box.begin(), box.end(),
                      [](float value) { return std::isfinite(value); }) &&
          width > 0.0 && height > 0.0) {
        detection.full_frame_box_xywh =
            std::array<double, 4>{box[0], box[1], width, height};
      }
    }
    if (index < legacy.swim_bladder_pixels.size()) {
      const auto &origin = legacy.swim_bladder_pixels[index];
      if (std::isfinite(origin[0]) && std::isfinite(origin[1])) {
        detection.heading_origin = KeypointOverlayPoint{origin[0], origin[1]};
      }
    }
    if (index < legacy.headings_deg.size() &&
        std::isfinite(legacy.headings_deg[index])) {
      detection.heading_degrees = legacy.headings_deg[index];
    }
    detection.heading_valid = index < legacy.heading_valid.size() &&
                              legacy.heading_valid[index] != 0 &&
                              detection.heading_degrees.has_value();
    result.detections.push_back(std::move(detection));
  }
  return result;
}

} // namespace crimson::zarr
