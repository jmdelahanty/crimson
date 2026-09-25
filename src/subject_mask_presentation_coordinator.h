#pragma once

#include "read_only_overlay_frame_coordinator.h"
#include "read_only_overlay_scene.h"
#include "subject_mask_overlay_buffer.h"
#include "zarr/subject_mask_overlay_repository.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace crimson::overlay {

struct SubjectMaskPresentationInput {
  int64_t camera_frame = -1;
  int full_frame_width = 0;
  int full_frame_height = 0;
  bool demand_enabled = false;
  bool layer_enabled = false;
  bool source_available = false;
  bool presentation_discontinuity = false;
};

struct SubjectMaskPresentationResult {
  ReadOnlyOverlayFrameAction action = ReadOnlyOverlayFrameAction::None;
  bool overlay_ready = false;
  size_t detection_count = 0;
  size_t component_count = 0;
};

class SubjectMaskPresentationCoordinator {
public:
  SubjectMaskPresentationResult
  update(const SubjectMaskPresentationInput &input,
         SubjectMaskOverlayBuffer &buffer,
         const zarr::SubjectMaskOverlayDescriptor &descriptor,
         ReadOnlyOverlayInput &scene, std::string *error_message = nullptr);

  void reset();
  const ReadOnlyOverlayFrameMetrics &metrics() const;

private:
  ReadOnlyOverlayFrameCoordinator frame_coordinator_;
};

} // namespace crimson::overlay
