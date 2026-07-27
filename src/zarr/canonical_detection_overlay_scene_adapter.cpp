#include "zarr/canonical_detection_overlay_scene_adapter.h"

#include <cmath>

namespace crimson::zarr {

overlay::ReadOnlyOverlayInput makeCanonicalDetectionOverlaySceneInput(
    const CanonicalDetectionDescriptor &descriptor,
    const CanonicalDetectionFrame &frame, int surface_view,
    int64_t surface_frame, int overlay_view, int full_frame_width,
    int full_frame_height) {
  overlay::ReadOnlyOverlayInput input;
  input.identity = {surface_view, surface_frame, overlay_view,
                    frame.camera_frame};
  input.source_width = full_frame_width;
  input.source_height = full_frame_height;
  input.show_boxes = true;
  input.show_headings = false;
  input.show_keypoints = false;
  if (!descriptor.ready() || frame.camera_frame != surface_frame ||
      full_frame_width <= 0 || full_frame_height <= 0) {
    return input;
  }

  input.detections.reserve(frame.detections.size());
  for (const auto &metadata : frame.detections) {
    const auto &box = metadata.normalized_cxcywh;
    if (!std::isfinite(box[0]) || !std::isfinite(box[1]) ||
        !std::isfinite(box[2]) || !std::isfinite(box[3]) || box[2] <= 0.0f ||
        box[3] <= 0.0f) {
      continue;
    }
    const double width = box[2] * full_frame_width;
    const double height = box[3] * full_frame_height;
    const double x = box[0] * full_frame_width - width * 0.5;
    const double y = box[1] * full_frame_height - height * 0.5;
    overlay::DetectionOverlayInput detection;
    detection.instance_key = metadata.instance_key;
    detection.refined_row_id = metadata.refined_row_id;
    detection.source_kind_code = metadata.source_kind_code;
    detection.box = overlay::DetectionBoxInput{
        {x, y, width, height},
        metadata.class_id,
        metadata.source_kind_code == 3 ? overlay::BoxProvenance::Manual
                                       : overlay::BoxProvenance::Clean};
    input.detections.push_back(std::move(detection));
  }
  return input;
}

} // namespace crimson::zarr
