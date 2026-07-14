#include "zarr/subject_shape_overlay_scene_adapter.h"

#include <utility>

namespace crimson::zarr {
namespace {

overlay::Point point(SubjectShapeOverlayPoint value) {
  return {value.x, value.y};
}

std::vector<overlay::Point>
points(const std::vector<SubjectShapeOverlayPoint> &values) {
  std::vector<overlay::Point> result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.push_back(point(value));
  }
  return result;
}

} // namespace

bool appendSubjectShapeOverlaySceneInput(
    const SubjectShapeOverlayDescriptor &descriptor,
    const SubjectShapeOverlayResolution &resolution, int64_t surface_frame,
    overlay::ReadOnlyOverlayInput *input) {
  if (input == nullptr ||
      resolution.status != SubjectShapeOverlayStatus::Mapped ||
      resolution.camera_frame != surface_frame ||
      input->identity.surface_frame != surface_frame ||
      input->identity.overlay_frame != surface_frame ||
      descriptor.coordinate_width == 0 || descriptor.coordinate_height == 0) {
    return false;
  }
  const size_t initial_size = input->subject_shapes.size();
  for (const auto &detection : resolution.detections) {
    overlay::SubjectShapeInput shape;
    shape.shape_row = detection.shape_row;
    shape.detection_index = detection.detection_index;
    shape.source_refined_row_id = detection.source_refined_row_id;
    shape.source_crop_row_id = detection.source_crop_row_id;
    shape.source_rect = {detection.roi_x, detection.roi_y,
                         detection.roi_width, detection.roi_height};
    shape.coordinate_width = descriptor.coordinate_width;
    shape.coordinate_height = descriptor.coordinate_height;
    const auto &geometry = detection.geometry;
    shape.body_frame_valid = geometry.body_frame_valid;
    shape.body_origin = point(geometry.body_origin);
    shape.body_forward_axis = point(geometry.body_forward_axis);
    shape.body_left_axis = point(geometry.body_left_axis);
    shape.snout_tip_valid = geometry.snout_tip_valid;
    shape.snout_tip = point(geometry.snout_tip);
    shape.tail_base_valid = geometry.tail_base_valid;
    shape.tail_base = point(geometry.tail_base);
    shape.tail_tip = point(geometry.tail_tip);
    shape.caudal_anchor_valid = geometry.caudal_anchor_valid;
    shape.caudal_anchor = point(geometry.caudal_anchor);
    shape.centerline_valid = geometry.centerline_valid;
    shape.centerline_reaches_snout = geometry.centerline_reaches_snout;
    shape.centerline = points(geometry.centerline);
    shape.bspline_valid = geometry.bspline_valid;
    shape.bspline_sample = points(geometry.bspline_sample);
    shape.bspline_control_points = points(geometry.bspline_control_points);
    shape.tail_sample_valid = geometry.tail_sample_valid;
    shape.tail_samples = points(geometry.tail_samples);
    shape.tail_normals = points(geometry.tail_normals);
    input->subject_shapes.push_back(std::move(shape));
  }
  return input->subject_shapes.size() > initial_size;
}

overlay::ReadOnlyOverlayInput makeSubjectShapeOverlaySceneInput(
    const SubjectShapeOverlayDescriptor &descriptor,
    const SubjectShapeOverlayResolution &resolution, int surface_view,
    int64_t surface_frame, int overlay_view, int full_frame_width,
    int full_frame_height) {
  overlay::ReadOnlyOverlayInput input;
  input.identity = {surface_view, surface_frame, overlay_view,
                    resolution.camera_frame};
  input.source_width = full_frame_width;
  input.source_height = full_frame_height;
  input.show_boxes = false;
  input.show_headings = false;
  input.show_keypoints = false;
  appendSubjectShapeOverlaySceneInput(descriptor, resolution, surface_frame,
                                      &input);
  return input;
}

} // namespace crimson::zarr
