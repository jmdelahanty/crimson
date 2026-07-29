#include "zarr/canonical_detection_overlay_scene_adapter.h"

#include "coordinate_contract.h"

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
      full_frame_width <= 0 || full_frame_height <= 0 ||
      descriptor.source_width != static_cast<size_t>(full_frame_width) ||
      descriptor.source_height != static_cast<size_t>(full_frame_height)) {
    return input;
  }

  coordinates::TransformAuthority transform_authority;
  transform_authority.source_dimensions = {full_frame_width, full_frame_height};

  input.detections.reserve(frame.detections.size());
  for (const auto &metadata : frame.detections) {
    const auto &box = metadata.normalized_cxcywh;
    const auto source_box =
        coordinates::normalizedCenterSizeBoxToContinuousPixelXyxy(
            {box[0], box[1], box[2], box[3]}, transform_authority);
    if (!source_box) {
      continue;
    }
    overlay::DetectionOverlayInput detection;
    detection.instance_key = metadata.instance_key;
    detection.refined_row_id = metadata.refined_row_id;
    detection.source_kind_code = metadata.source_kind_code;
    detection.box = overlay::DetectionBoxInput{
        {source_box->x_min, source_box->y_min, source_box->width(),
         source_box->height()},
        metadata.class_id,
        metadata.source_kind_code == 3 ? overlay::BoxProvenance::Manual
                                       : overlay::BoxProvenance::Clean};
    input.detections.push_back(std::move(detection));
  }
  return input;
}

} // namespace crimson::zarr
