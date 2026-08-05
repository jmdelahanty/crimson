#include "app/frame_inspect_controller.h"

#include <limits>

namespace crimson::app {
namespace {

bool fitsCommandValue(size_t value) {
  return value <= static_cast<size_t>(std::numeric_limits<int64_t>::max());
}

void append(std::vector<FrameInspectCommand> *commands,
            FrameInspectCommandKind kind, int64_t value = -1) {
  commands->push_back({kind, value});
}

} // namespace

bool prepareFrameInspectPresentation(
    const FrameInspectTabState &tab_state,
    const FrameInspectPresentationInput &input) {
  if (!tab_state.view_sync.shouldApply(input.requested_view)) {
    return false;
  }
  tab_state.active_view = input.requested_view;
  return true;
}

void observeFrameInspectActiveView(const FrameInspectTabState &tab_state,
                                   workspace::FrameInspectView active_view) {
  tab_state.active_view = active_view;
  tab_state.view_sync.observe(active_view);
}

void applyFrameInspectPresentation(FrameInspectControllerState *state,
                                   const FrameInspectTabState &tab_state,
                                   const FrameInspectPresentationOutput &output,
                                   workspace::WorkspaceState *workspace_state) {
  if (state == nullptr || workspace_state == nullptr) {
    return;
  }

  state->keypoint_selection = output.keypoint_selection;
  state->keypoint_full_frame_edit_enabled =
      tab_state.active_view == workspace::FrameInspectView::Keypoints &&
      output.keypoint_full_frame_edit_requested &&
      output.keypoint_selection.has_value();

  workspace_state->selections().frame_inspect_view = tab_state.active_view;
  workspace_state->overlayControls() = output.overlay_controls;
  workspace_state->setWindowRequested(workspace::Window::Stimulus,
                                      output.stimulus_debug_requested);
}

std::vector<FrameInspectCommand>
makeFrameInspectCommands(const FrameInspectCommandRequests &requests) {
  std::vector<FrameInspectCommand> commands;
  if (requests.requested_detection_dataset_index >= 0) {
    append(&commands, FrameInspectCommandKind::SelectDetectionDataset,
           requests.requested_detection_dataset_index);
  }
  if (requests.previous_review_frame) {
    append(&commands, FrameInspectCommandKind::PreviousReviewFrame);
  }
  if (requests.next_review_frame) {
    append(&commands, FrameInspectCommandKind::NextReviewFrame);
  }
  if (requests.previous_subject_shape_qc_frame) {
    append(&commands, FrameInspectCommandKind::PreviousSubjectShapeQcFrame);
  }
  if (requests.next_subject_shape_qc_frame) {
    append(&commands, FrameInspectCommandKind::NextSubjectShapeQcFrame);
  }
  if (requests.previous_tail_kinematics_qc_frame) {
    append(&commands, FrameInspectCommandKind::PreviousTailKinematicsQcFrame);
  }
  if (requests.next_tail_kinematics_qc_frame) {
    append(&commands, FrameInspectCommandKind::NextTailKinematicsQcFrame);
  }
  if (requests.tail_kinematics_row.has_value() &&
      fitsCommandValue(*requests.tail_kinematics_row)) {
    append(&commands, FrameInspectCommandKind::SelectTailKinematicsRow,
           static_cast<int64_t>(*requests.tail_kinematics_row));
  }
  if (requests.previous_eye_angle_qc_frame) {
    append(&commands, FrameInspectCommandKind::PreviousEyeAngleQcFrame);
  }
  if (requests.next_eye_angle_qc_frame) {
    append(&commands, FrameInspectCommandKind::NextEyeAngleQcFrame);
  }
  if (requests.eye_angle_row.has_value() &&
      fitsCommandValue(*requests.eye_angle_row)) {
    append(&commands, FrameInspectCommandKind::SelectEyeAngleRow,
           static_cast<int64_t>(*requests.eye_angle_row));
  }
  if (requests.reset_frame_bbox_edits) {
    append(&commands, FrameInspectCommandKind::ResetFrameBboxEdits);
  }
  if (requests.clear_bbox_selection) {
    append(&commands, FrameInspectCommandKind::ClearBboxSelection);
  }
  if (requests.build_manual_payload_preview) {
    append(&commands, FrameInspectCommandKind::BuildManualPayloadPreview);
  }
  if (requests.write_manual_payload) {
    append(&commands, FrameInspectCommandKind::WriteManualPayload);
  }
  if (requests.write_keypoint_review) {
    append(&commands, FrameInspectCommandKind::WriteKeypointReview);
  }
  return commands;
}

} // namespace crimson::app
