#include "app/frame_inspect_controller.h"

#include <iostream>
#include <limits>
#include <optional>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "   \
                << #condition << '\n';                                         \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool sameOverlays(const crimson::overlay::ReadOnlyOverlayControlState &left,
                  const crimson::overlay::ReadOnlyOverlayControlState &right) {
  return left.show_keypoints == right.show_keypoints &&
         left.show_headings == right.show_headings &&
         left.show_subject_masks == right.show_subject_masks &&
         left.show_subject_body_mask == right.show_subject_body_mask &&
         left.show_eye_left_mask == right.show_eye_left_mask &&
         left.show_eye_right_mask == right.show_eye_right_mask &&
         left.show_swim_bladder_mask == right.show_swim_bladder_mask &&
         left.mask_mode == right.mask_mode &&
         left.show_eye_geometry == right.show_eye_geometry &&
         left.show_eye_direction_beams == right.show_eye_direction_beams &&
         left.show_eye_gaze_rays == right.show_eye_gaze_rays &&
         left.show_eye_angle_arcs == right.show_eye_angle_arcs &&
         left.show_eye_angle_labels == right.show_eye_angle_labels &&
         left.show_subject_shape == right.show_subject_shape &&
         left.show_subject_shape_body_axes ==
             right.show_subject_shape_body_axes &&
         left.show_subject_shape_snout_tip ==
             right.show_subject_shape_snout_tip &&
         left.show_subject_shape_caudal_anchor ==
             right.show_subject_shape_caudal_anchor &&
         left.show_subject_shape_tail_base ==
             right.show_subject_shape_tail_base &&
         left.show_subject_shape_tail_tip ==
             right.show_subject_shape_tail_tip &&
         left.show_subject_shape_centerline ==
             right.show_subject_shape_centerline &&
         left.show_subject_shape_bspline == right.show_subject_shape_bspline &&
         left.show_subject_shape_bspline_debug_points ==
             right.show_subject_shape_bspline_debug_points &&
         left.show_subject_shape_bspline_control_points ==
             right.show_subject_shape_bspline_control_points &&
         left.show_subject_shape_tail_samples ==
             right.show_subject_shape_tail_samples &&
         left.show_subject_shape_tail_normals ==
             right.show_subject_shape_tail_normals;
}

crimson::overlay::ReadOnlyOverlayControlState allDistinctOverlayControls() {
  crimson::overlay::ReadOnlyOverlayControlState controls;
  controls.show_keypoints = false;
  controls.show_headings = false;
  controls.show_subject_masks = false;
  controls.show_subject_body_mask = false;
  controls.show_eye_left_mask = false;
  controls.show_eye_right_mask = false;
  controls.show_swim_bladder_mask = false;
  controls.mask_mode = crimson::overlay::ReadOnlyMaskOverlayMode::Debug;
  controls.show_eye_geometry = false;
  controls.show_eye_direction_beams = false;
  controls.show_eye_gaze_rays = false;
  controls.show_eye_angle_arcs = false;
  controls.show_eye_angle_labels = false;
  controls.show_subject_shape = false;
  controls.show_subject_shape_body_axes = true;
  controls.show_subject_shape_snout_tip = false;
  controls.show_subject_shape_caudal_anchor = false;
  controls.show_subject_shape_tail_base = false;
  controls.show_subject_shape_tail_tip = false;
  controls.show_subject_shape_centerline = false;
  controls.show_subject_shape_bspline = false;
  controls.show_subject_shape_bspline_debug_points = true;
  controls.show_subject_shape_bspline_control_points = true;
  controls.show_subject_shape_tail_samples = true;
  controls.show_subject_shape_tail_normals = true;
  return controls;
}

bool testPresentationStateAndWorkspaceProjection() {
  using crimson::app::FrameInspectControllerState;
  using crimson::app::FrameInspectKeypointSelection;
  using crimson::app::FrameInspectPresentationInput;
  using crimson::app::FrameInspectPresentationOutput;
  using crimson::app::FrameInspectTabState;
  using crimson::workspace::FrameInspectView;
  using crimson::workspace::Window;

  FrameInspectControllerState controller;
  FrameInspectView active_view = FrameInspectView::Detect;
  crimson::workspace::FrameInspectViewSyncState view_sync;
  const FrameInspectTabState tab{active_view, view_sync};
  FrameInspectPresentationInput input;
  input.requested_view = FrameInspectView::Keypoints;
  CHECK(crimson::app::prepareFrameInspectPresentation(tab, input));
  CHECK(active_view == FrameInspectView::Keypoints);

  // The tab selected by the UI becomes authoritative until workspace changes.
  crimson::app::observeFrameInspectActiveView(tab, FrameInspectView::EyeMasks);
  CHECK(active_view == FrameInspectView::EyeMasks);
  crimson::workspace::WorkspaceState workspace;
  FrameInspectPresentationOutput output;
  crimson::app::applyFrameInspectPresentation(&controller, tab, output,
                                              &workspace);
  input.requested_view = workspace.selections().frame_inspect_view;
  CHECK(!crimson::app::prepareFrameInspectPresentation(tab, input));
  CHECK(active_view == FrameInspectView::EyeMasks);
  input.requested_view = FrameInspectView::TailKinematics;
  CHECK(crimson::app::prepareFrameInspectPresentation(tab, input));
  CHECK(active_view == FrameInspectView::TailKinematics);

  // Full-frame keypoint editing is valid only on the Keypoints view.
  crimson::app::observeFrameInspectActiveView(tab, FrameInspectView::Keypoints);
  output.overlay_controls = allDistinctOverlayControls();
  output.stimulus_debug_requested = true;
  output.keypoint_selection =
      FrameInspectKeypointSelection{true, true, 81, 12, 7};
  output.keypoint_full_frame_edit_requested = true;
  crimson::app::applyFrameInspectPresentation(&controller, tab, output,
                                              &workspace);

  CHECK(workspace.selections().frame_inspect_view ==
        FrameInspectView::Keypoints);
  CHECK(sameOverlays(workspace.overlayControls(), output.overlay_controls));
  CHECK(workspace.windowRequested(Window::Stimulus));
  CHECK(controller.keypoint_selection.has_value());
  CHECK(controller.keypoint_selection->valid);
  CHECK(controller.keypoint_selection->editable);
  CHECK(controller.keypoint_selection->frame == 81);
  CHECK(controller.keypoint_selection->observation_index == 12);
  CHECK(controller.keypoint_selection->roi_index == 7);
  CHECK(controller.keypoint_full_frame_edit_enabled);

  // A valid selection remains available for presentation outside the keypoint
  // tab, but it cannot enable the full-frame editing affordance there.
  crimson::app::observeFrameInspectActiveView(tab, FrameInspectView::EyeMasks);
  crimson::app::applyFrameInspectPresentation(&controller, tab, output,
                                              &workspace);
  CHECK(controller.keypoint_selection.has_value());
  CHECK(!controller.keypoint_full_frame_edit_enabled);
  crimson::app::observeFrameInspectActiveView(tab, FrameInspectView::Keypoints);

  // Preserve the existing Linux policy: selection presence, not its editable
  // metadata, gates the full-frame tool while the Keypoints tab is active.
  output.keypoint_selection =
      FrameInspectKeypointSelection{false, true, 3, 4, 5};
  crimson::app::applyFrameInspectPresentation(&controller, tab, output,
                                              &workspace);
  CHECK(controller.keypoint_full_frame_edit_enabled);
  output.keypoint_selection.reset();
  output.stimulus_debug_requested = false;
  crimson::app::applyFrameInspectPresentation(&controller, tab, output,
                                              &workspace);
  CHECK(!controller.keypoint_selection.has_value());
  CHECK(!workspace.windowRequested(Window::Stimulus));
  return true;
}

bool testNoUnintendedStateMutation() {
  using crimson::app::FrameInspectControllerState;
  using crimson::app::FrameInspectPresentationOutput;
  using crimson::app::FrameInspectTabState;
  using crimson::workspace::FrameInspectView;
  using crimson::workspace::Window;

  FrameInspectControllerState controller;
  FrameInspectView active_view = FrameInspectView::EyeAngles;
  crimson::workspace::FrameInspectViewSyncState view_sync;
  const FrameInspectTabState tab{active_view, view_sync};
  crimson::workspace::WorkspaceState workspace;
  workspace.selections().motion_source_key = "motion-source";
  workspace.selections().swim_bout_candidate_key = "bout-source";
  workspace.selections().eye_angle_representation_key = "eye-source";
  workspace.selections().tail_kinematics_source_key = "tail-source";
  workspace.setWindowRequested(Window::Help, true);
  workspace.setWindowRequested(Window::Stimulus, false);
  const auto preserved = workspace.selections();

  FrameInspectPresentationOutput output;
  output.overlay_controls = allDistinctOverlayControls();
  crimson::app::applyFrameInspectPresentation(&controller, tab, output,
                                              &workspace);
  CHECK(workspace.selections().frame_inspect_view ==
        FrameInspectView::EyeAngles);
  CHECK(workspace.selections().motion_source_key ==
        preserved.motion_source_key);
  CHECK(workspace.selections().swim_bout_candidate_key ==
        preserved.swim_bout_candidate_key);
  CHECK(workspace.selections().eye_angle_representation_key ==
        preserved.eye_angle_representation_key);
  CHECK(workspace.selections().tail_kinematics_source_key ==
        preserved.tail_kinematics_source_key);
  CHECK(workspace.windowRequested(Window::Help));
  CHECK(!workspace.windowRequested(Window::Stimulus));

  const auto controls_before_null = workspace.overlayControls();
  CHECK(!controller.keypoint_selection.has_value());
  crimson::app::applyFrameInspectPresentation(nullptr, tab, output, &workspace);
  crimson::app::applyFrameInspectPresentation(&controller, tab, output,
                                              nullptr);
  CHECK(!controller.keypoint_selection.has_value());
  CHECK(sameOverlays(workspace.overlayControls(), controls_before_null));
  return true;
}

bool testDeterministicCommandRepresentation() {
  using crimson::app::FrameInspectCommandKind;
  using crimson::app::FrameInspectCommandRequests;

  FrameInspectCommandRequests requests;
  requests.requested_detection_dataset_index = 4;
  requests.previous_review_frame = true;
  requests.next_review_frame = true;
  requests.reset_frame_bbox_edits = true;
  requests.clear_bbox_selection = true;
  requests.build_manual_payload_preview = true;
  requests.write_manual_payload = true;
  requests.write_keypoint_review = true;
  requests.previous_subject_shape_qc_frame = true;
  requests.next_subject_shape_qc_frame = true;
  requests.previous_tail_kinematics_qc_frame = true;
  requests.next_tail_kinematics_qc_frame = true;
  requests.tail_kinematics_row = 73;
  requests.previous_eye_angle_qc_frame = true;
  requests.next_eye_angle_qc_frame = true;
  requests.eye_angle_row = 91;

  const auto commands = crimson::app::makeFrameInspectCommands(requests);
  const std::vector<FrameInspectCommandKind> expected = {
      FrameInspectCommandKind::SelectDetectionDataset,
      FrameInspectCommandKind::PreviousReviewFrame,
      FrameInspectCommandKind::NextReviewFrame,
      FrameInspectCommandKind::PreviousSubjectShapeQcFrame,
      FrameInspectCommandKind::NextSubjectShapeQcFrame,
      FrameInspectCommandKind::PreviousTailKinematicsQcFrame,
      FrameInspectCommandKind::NextTailKinematicsQcFrame,
      FrameInspectCommandKind::SelectTailKinematicsRow,
      FrameInspectCommandKind::PreviousEyeAngleQcFrame,
      FrameInspectCommandKind::NextEyeAngleQcFrame,
      FrameInspectCommandKind::SelectEyeAngleRow,
      FrameInspectCommandKind::ResetFrameBboxEdits,
      FrameInspectCommandKind::ClearBboxSelection,
      FrameInspectCommandKind::BuildManualPayloadPreview,
      FrameInspectCommandKind::WriteManualPayload,
      FrameInspectCommandKind::WriteKeypointReview,
  };
  CHECK(commands.size() == expected.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    CHECK(commands[index].kind == expected[index]);
  }
  CHECK(commands.front().value == 4);
  CHECK(commands[7].value == 73);
  CHECK(commands[10].value == 91);

  FrameInspectCommandRequests invalid;
  invalid.requested_detection_dataset_index = -1;
  invalid.tail_kinematics_row = std::numeric_limits<size_t>::max();
  invalid.eye_angle_row = std::numeric_limits<size_t>::max();
  CHECK(crimson::app::makeFrameInspectCommands(invalid).empty());
  return true;
}

} // namespace

int main() {
  if (!testPresentationStateAndWorkspaceProjection() ||
      !testNoUnintendedStateMutation() ||
      !testDeterministicCommandRepresentation()) {
    return 1;
  }
  std::cout << "frame inspect workflow tests passed\n";
  return 0;
}
