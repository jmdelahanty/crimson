#include "zarr/subject_mask_overlay_scene_adapter.h"

#include <utility>

namespace crimson::zarr {

bool appendSubjectMaskOverlaySceneInput(
    const SubjectMaskOverlayDescriptor &descriptor,
    const SubjectMaskOverlayResolution &resolution, int64_t surface_frame,
    overlay::ReadOnlyOverlayInput *input) {
  if (input == nullptr ||
      resolution.status != SubjectMaskOverlayStatus::Mapped ||
      resolution.camera_frame != surface_frame ||
      input->identity.surface_frame != surface_frame ||
      input->identity.overlay_frame != surface_frame) {
    return false;
  }
  for (const auto &detection : resolution.detections) {
    for (const auto &component : detection.components) {
      if (component.channel_index >= descriptor.component_labels.size() ||
          descriptor.component_labels[component.channel_index] !=
              component.label) {
        continue;
      }
      if (!component.present && component.contour.empty()) {
        continue;
      }
      overlay::SubjectMaskComponentInput mask;
      mask.instance_key = detection.instance_key;
      mask.instance_key_valid = descriptor.strict_v1;
      mask.cache_namespace = descriptor.cache_namespace.empty()
                                 ? descriptor.source_group + "/" +
                                       descriptor.run_name
                                 : descriptor.cache_namespace;
      mask.label = component.label;
      if (descriptor.strict_v1) {
        mask.cache_namespace += ":instance:" + std::to_string(detection.instance_key);
      }
      mask.source_crop_row_id = detection.source_crop_row_id;
      mask.channel_index = component.channel_index;
      mask.source_rect = {detection.roi_x, detection.roi_y, detection.roi_width,
                          detection.roi_height};
      mask.mask_width = component.mask_width;
      mask.mask_height = component.mask_height;
      mask.mask = component.mask;
      mask.contour.reserve(component.contour.size());
      for (const auto point : component.contour) {
        mask.contour.push_back({point.x, point.y});
      }
      input->subject_masks.push_back(std::move(mask));
    }
  }
  return !input->subject_masks.empty();
}

overlay::ReadOnlyOverlayInput
makeSubjectMaskOverlaySceneInput(const SubjectMaskOverlayDescriptor &descriptor,
                                 const SubjectMaskOverlayResolution &resolution,
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
  appendSubjectMaskOverlaySceneInput(descriptor, resolution, surface_frame,
                                     &input);
  return input;
}

} // namespace crimson::zarr
