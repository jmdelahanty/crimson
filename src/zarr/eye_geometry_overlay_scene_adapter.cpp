#include "zarr/eye_geometry_overlay_scene_adapter.h"

#include <utility>

namespace crimson::zarr {
namespace {

overlay::Point Point(EyeGeometryPoint value) { return {value.x, value.y}; }

overlay::EyeAxisInput Axis(EyeGeometryAxis value) {
  return {value.valid, Point(value.start), Point(value.end)};
}

} // namespace

bool appendEyeGeometryOverlaySceneInput(
    const EyeGeometryOverlayDescriptor &descriptor,
    const EyeGeometryOverlayResolution &resolution, int64_t surface_frame,
    overlay::ReadOnlyOverlayInput *input) {
  if (input == nullptr ||
      resolution.status != EyeGeometryOverlayStatus::Mapped ||
      resolution.camera_frame != surface_frame ||
      input->identity.surface_frame != surface_frame ||
      input->identity.overlay_frame != surface_frame ||
      descriptor.coordinate_width == 0 || descriptor.coordinate_height == 0) {
    return false;
  }
  const size_t initial_size = input->eye_geometry.size();
  for (const auto &detection : resolution.detections) {
    overlay::EyeGeometryInput geometry;
    geometry.eye_row = detection.eye_row;
    geometry.detection_index = detection.detection_index;
    geometry.source_crop_row_id = detection.source_crop_row_id;
    geometry.source_rect = {detection.roi_x, detection.roi_y,
                            detection.roi_width, detection.roi_height};
    geometry.coordinate_width = descriptor.coordinate_width;
    geometry.coordinate_height = descriptor.coordinate_height;
    geometry.frame_valid = detection.frame_valid;
    geometry.body_frame_valid = detection.body_frame_valid;
    geometry.body_origin = Point(detection.body_origin);
    geometry.body_forward_axis = Point(detection.body_forward_axis);
    geometry.body_left_axis = Point(detection.body_left_axis);
    for (size_t eye = 0; eye < geometry.eyes.size(); ++eye) {
      geometry.eyes[eye].valid = detection.eyes[eye].valid;
      geometry.eyes[eye].major_axis = Axis(detection.eyes[eye].major_axis);
      geometry.eyes[eye].minor_axis = Axis(detection.eyes[eye].minor_axis);
      geometry.eyes[eye].gaze_valid = detection.eyes[eye].gaze_valid;
      geometry.eyes[eye].gaze = Point(detection.eyes[eye].gaze);
      geometry.eyes[eye].signed_angle_valid =
          detection.eyes[eye].signed_angle_valid;
      geometry.eyes[eye].signed_angle_degrees =
          detection.eyes[eye].signed_angle_degrees;
      geometry.eyes[eye].eye_frame_angle_valid =
          detection.eyes[eye].eye_frame_angle_valid;
      geometry.eyes[eye].eye_frame_angle_degrees =
          detection.eyes[eye].eye_frame_angle_degrees;
    }
    geometry.vergence_valid = detection.vergence_valid;
    geometry.vergence_degrees = detection.vergence_degrees;
    input->eye_geometry.push_back(std::move(geometry));
  }
  return input->eye_geometry.size() > initial_size;
}

overlay::ReadOnlyOverlayInput
makeEyeGeometryOverlaySceneInput(const EyeGeometryOverlayDescriptor &descriptor,
                                 const EyeGeometryOverlayResolution &resolution,
                                 int surface_view, int64_t surface_frame,
                                 int overlay_view, int full_frame_width,
                                 int full_frame_height) {
  overlay::ReadOnlyOverlayInput input;
  input.identity = {surface_view, surface_frame, overlay_view,
                    resolution.camera_frame};
  input.source_width = full_frame_width;
  input.source_height = full_frame_height;
  input.show_boxes = false;
  input.show_headings = false;
  input.show_keypoints = false;
  appendEyeGeometryOverlaySceneInput(descriptor, resolution, surface_frame,
                                     &input);
  return input;
}

} // namespace crimson::zarr
