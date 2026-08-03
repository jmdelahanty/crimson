#include "gui/frame_debug_keypoint_adapter.h"

#include <algorithm>
#include <cmath>
#include <utility>

crimson::gui::KeypointInspectPresentation
makeFrameDebugKeypointInspectPresentation(
    const FrameDebugWindowContext &context) {
  crimson::gui::KeypointInspectPresentation presentation;
  presentation.presentation_label = "Current-frame presentation";
  presentation.unavailable_message =
      "Keypoint arrays unavailable for current dataset.";
  presentation.available = context.zarr_loader.hasKeypointData();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label = context.zarr_loader.isRefinedKeypoints()
                                   ? "Legacy refined keypoints"
                                   : "Legacy raw keypoints";
  presentation.run_name = context.zarr_loader.getKeypointsRunName();
  presentation.frame_ready = true;
  presentation.camera_frame = context.current_frame_num;
  if (context.detection_details == nullptr) {
    return presentation;
  }

  const auto &details = *context.detection_details;
  presentation.observations.reserve(details.keypoints_pixels.size());
  for (size_t index = 0; index < details.keypoints_pixels.size(); ++index) {
    crimson::gui::KeypointInspectObservation observation;
    observation.landmark_count = details.keypoints_pixels[index].size();
    observation.valid_landmark_count = static_cast<size_t>(std::count_if(
        details.keypoints_pixels[index].begin(),
        details.keypoints_pixels[index].end(), [](const auto &point) {
          return std::isfinite(point[0]) && std::isfinite(point[1]);
        }));
    if (details.is_refined_keypoints) {
      observation.state_label = index < details.keypoint_usable.size() &&
                                        details.keypoint_usable[index] != 0
                                    ? "Usable"
                                    : "Rejected";
    } else {
      observation.state_label =
          observation.valid_landmark_count > 0 ? "Observed" : "Missing";
    }
    presentation.observations.push_back(std::move(observation));
  }

  if (!details.keypoint_labels.empty()) {
    presentation.detail_lines.push_back(
        "Keypoint labels: " + std::to_string(details.keypoint_labels.size()));
  }
  return presentation;
}
