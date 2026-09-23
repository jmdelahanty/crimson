#include "gui/canonical_detection_inspect_adapter.h"

#include <utility>

namespace crimson::gui {

DetectionInspectPresentation makeCanonicalDetectionInspectPresentation(
    const zarr::CanonicalDetectionDescriptor *descriptor,
    const zarr::CanonicalDetectionFrame *frame, int64_t presented_frame) {
  DetectionInspectPresentation presentation;
  presentation.available = descriptor != nullptr && descriptor->ready();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label =
      descriptor->surface_kind == zarr::DetectionSurfaceKind::RefinedSnapshotV1
          ? "Refined snapshot"
          : "Canonical raw";
  presentation.run_name = descriptor->run_name;
  presentation.frame_ready =
      frame != nullptr && frame->camera_frame == presented_frame;
  if (!presentation.frame_ready) {
    return presentation;
  }

  presentation.camera_frame = frame->camera_frame;
  presentation.observations.reserve(frame->detections.size());
  for (const zarr::CanonicalDetection &detection : frame->detections) {
    DetectionInspectObservation observation;
    observation.instance_key = detection.instance_key;
    observation.selectable = detection.instance_key != 0;
    observation.confidence = detection.score;
    observation.confidence_valid = detection.score_valid;
    observation.class_id = detection.class_id;
    observation.class_id_valid = true;
    observation.source_label = detection.source_kind_code == 3 ? "Manual"
                               : detection.manual_edit         ? "Edited raw"
                                                               : "Raw";
    presentation.observations.push_back(std::move(observation));
  }
  return presentation;
}

} // namespace crimson::gui
