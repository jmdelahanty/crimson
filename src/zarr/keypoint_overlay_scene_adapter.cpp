#include "zarr/keypoint_overlay_scene_adapter.h"

#include <utility>

namespace crimson::zarr {

overlay::ReadOnlyOverlayInput makeKeypointOverlaySceneInput(
    const KeypointOverlayDescriptor& descriptor,
    const KeypointOverlayResolution& resolution,
    int surface_view,
    int64_t surface_frame,
    int overlay_view,
    int full_frame_width,
    int full_frame_height) {
  overlay::ReadOnlyOverlayInput input;
  input.identity = {surface_view, surface_frame, overlay_view,
                    resolution.camera_frame};
  input.source_width = full_frame_width;
  input.source_height = full_frame_height;
  input.keypoint_labels = descriptor.keypoint_labels;
  input.skeleton_edges = descriptor.skeleton_edges;
  input.show_boxes = false;
  if (resolution.status != KeypointOverlayStatus::Mapped ||
      resolution.camera_frame != surface_frame) {
    return input;
  }

  input.detections.reserve(resolution.detections.size());
  for (const auto& metadata : resolution.detections) {
    overlay::DetectionOverlayInput detection;
    if (metadata.full_frame_box_xywh) {
      const auto& box = *metadata.full_frame_box_xywh;
      detection.box = overlay::DetectionBoxInput{
          {box[0], box[1], box[2], box[3]}, 0,
          metadata.detection_interpolated
              ? overlay::BoxProvenance::Interpolated
              : overlay::BoxProvenance::Clean};
    }
    detection.keypoints.reserve(metadata.keypoints.size());
    for (const auto point : metadata.keypoints) {
      detection.keypoints.push_back({point.x, point.y});
    }
    if (metadata.heading_origin) {
      detection.heading_origin =
          overlay::Point{metadata.heading_origin->x,
                         metadata.heading_origin->y};
    }
    detection.heading_degrees = metadata.heading_degrees;
    detection.heading_valid = metadata.heading_valid;
    detection.detection_interpolated = metadata.detection_interpolated;
    detection.refined_keypoints = metadata.refined_keypoints;
    detection.keypoint_usable = metadata.keypoint_usable;
    detection.keypoint_detection_interpolated =
        metadata.keypoint_detection_interpolated;
    detection.keypoint_flip_corrected = metadata.keypoint_flip_corrected;
    input.detections.push_back(std::move(detection));
  }
  return input;
}

}  // namespace crimson::zarr
