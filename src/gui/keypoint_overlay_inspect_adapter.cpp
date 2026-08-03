#include "gui/keypoint_overlay_inspect_adapter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace crimson::gui {

KeypointInspectPresentation makeKeypointOverlayInspectPresentation(
    const zarr::KeypointOverlayDescriptor *descriptor,
    const zarr::KeypointOverlayResolution *frame, int64_t presented_frame) {
  KeypointInspectPresentation presentation;
  presentation.available =
      descriptor != nullptr && !descriptor->run_name.empty();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label =
      descriptor->refined ? "Refined snapshot" : "Raw observations";
  presentation.run_name = descriptor->run_name;
  presentation.frame_ready =
      frame != nullptr && frame->camera_frame == presented_frame;
  if (!presentation.frame_ready) {
    return presentation;
  }

  presentation.camera_frame = frame->camera_frame;
  presentation.observations.reserve(frame->detections.size());
  for (const zarr::KeypointOverlayDetection &detection : frame->detections) {
    KeypointInspectObservation observation;
    observation.instance_key = detection.instance_key;
    observation.selectable = detection.instance_key != 0;
    observation.pose_confidence = static_cast<float>(detection.pose_confidence);
    observation.pose_confidence_valid =
        std::isfinite(detection.pose_confidence) &&
        (!detection.refined_keypoints || detection.confidence_valid);
    observation.landmark_count = descriptor->keypoint_labels.size();
    observation.valid_landmark_count = static_cast<size_t>(
        std::count(detection.keypoint_valid.begin(),
                   detection.keypoint_valid.begin() +
                       std::min(detection.keypoint_valid.size(),
                                descriptor->keypoint_labels.size()),
                   uint8_t{1}));
    observation.state_label =
        detection.refined_keypoints
            ? detection.keypoint_usable ? "Usable" : "Rejected"
        : detection.source_success ? "Succeeded"
                                   : "Failed";
    observation.landmarks.reserve(descriptor->keypoint_labels.size());
    for (size_t point = 0; point < descriptor->keypoint_labels.size();
         ++point) {
      KeypointInspectLandmark landmark;
      landmark.label = descriptor->keypoint_labels[point];
      if (point < detection.keypoint_confidences.size() &&
          std::isfinite(detection.keypoint_confidences[point]) &&
          (!detection.refined_keypoints || detection.confidence_valid)) {
        landmark.confidence =
            static_cast<float>(detection.keypoint_confidences[point]);
        landmark.confidence_valid = true;
      }
      landmark.valid_known = true;
      landmark.valid = point < detection.keypoint_valid.size() &&
                       detection.keypoint_valid[point] != 0;
      landmark.edited_known = true;
      landmark.edited = point < detection.keypoint_edit_flags.size() &&
                        detection.keypoint_edit_flags[point] != 0;
      observation.landmarks.push_back(std::move(landmark));
    }
    presentation.observations.push_back(std::move(observation));
  }
  return presentation;
}

} // namespace crimson::gui
