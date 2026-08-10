#include "gui/camera_view_subject_mask_scene_adapter.h"

#include "zarr/subject_mask_overlay_scene_adapter.h"

#include <algorithm>

namespace crimson::gui {

overlay::ReadOnlyOverlayScene makeCameraViewSubjectMaskScene(
    const zarr::SubjectMaskOverlayDescriptor &descriptor,
    const zarr::SubjectMaskOverlayResolution &frame,
    const CameraViewSubjectMaskSceneOptions &options, int view_index,
    int64_t presented_frame, int full_frame_width, int full_frame_height) {
  auto input = zarr::makeSubjectMaskOverlaySceneInput(
      descriptor, frame, view_index, presented_frame, view_index,
      full_frame_width, full_frame_height);
  overlay::ReadOnlyOverlayControlState controls;
  controls.show_keypoints = false;
  controls.show_headings = false;
  controls.show_subject_masks = true;
  controls.show_subject_body_mask = options.show_subject_body;
  controls.show_eye_left_mask = options.show_eye_left;
  controls.show_eye_right_mask = options.show_eye_right;
  controls.show_swim_bladder_mask = options.show_swim_bladder;
  controls.mask_mode = options.mode;
  overlay::applyReadOnlyOverlayControls(controls, &input);

  if (options.suppressed_source_crop_row_id >= 0 &&
      !options.suppressed_component.empty()) {
    input.subject_masks.erase(
        std::remove_if(
            input.subject_masks.begin(), input.subject_masks.end(),
            [&](const overlay::SubjectMaskComponentInput &component) {
              return component.source_crop_row_id ==
                         options.suppressed_source_crop_row_id &&
                     component.label == options.suppressed_component;
            }),
        input.subject_masks.end());
  }
  return overlay::buildReadOnlyOverlayScene(input);
}

} // namespace crimson::gui
