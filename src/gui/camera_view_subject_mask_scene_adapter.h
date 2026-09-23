#pragma once

#include "read_only_overlay_controls.h"
#include "read_only_overlay_scene.h"
#include "zarr/subject_mask_overlay_repository.h"

#include <cstdint>
#include <string>

namespace crimson::gui {

struct CameraViewSubjectMaskSceneOptions {
  overlay::ReadOnlyMaskOverlayMode mode =
      overlay::ReadOnlyMaskOverlayMode::Review;
  bool show_subject_body = true;
  bool show_eye_left = true;
  bool show_eye_right = true;
  bool show_swim_bladder = true;
  int64_t suppressed_source_crop_row_id = -1;
  std::string suppressed_component;
};

overlay::ReadOnlyOverlayScene makeCameraViewSubjectMaskScene(
    const zarr::SubjectMaskOverlayDescriptor &descriptor,
    const zarr::SubjectMaskOverlayResolution &frame,
    const CameraViewSubjectMaskSceneOptions &options, int view_index,
    int64_t presented_frame, int full_frame_width, int full_frame_height);

} // namespace crimson::gui
