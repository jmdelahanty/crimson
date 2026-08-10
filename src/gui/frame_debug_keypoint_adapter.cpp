#include "gui/frame_debug_keypoint_adapter.h"

#include "gui/keypoint_overlay_inspect_adapter.h"

crimson::gui::KeypointInspectPresentation
makeFrameDebugKeypointInspectPresentation(
    const FrameDebugWindowContext &context) {
  auto presentation = crimson::gui::makeKeypointOverlayInspectPresentation(
      context.keypoint_descriptor, context.keypoint_frame,
      context.current_frame_num);
  presentation.presentation_label = "Current-frame presentation";
  presentation.unavailable_message =
      "Keypoint arrays unavailable for current dataset.";
  if (context.keypoint_descriptor != nullptr &&
      !context.keypoint_descriptor->keypoint_labels.empty()) {
    presentation.detail_lines.push_back(
        "Keypoint labels: " +
        std::to_string(context.keypoint_descriptor->keypoint_labels.size()));
  }
  return presentation;
}
