#include "platform/nvidia/nvidia_frame_inspect_adapter.h"

#include "review_frame_index.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace crimson::nvidia {
namespace {

void seekToFrame(FrameInspectNavigationContext &context, int frame) {
  if (context.seek_to_frame) {
    context.seek_to_frame(frame, true);
  }
}

void applyReviewJump(FrameInspectNavigationContext &context, bool forward) {
  auto jump_result = computeReviewFrameJump(
      context.zarr_loaded, context.zarr_loader, context.review_frame_filters,
      context.review_frame_cache, context.current_frame_num, forward);
  context.review_frame_status = std::move(jump_result.status);
  if (jump_result.target_frame.has_value()) {
    seekToFrame(context, *jump_result.target_frame);
  }
}

void applySubjectShapeQcJump(FrameInspectNavigationContext &context,
                             bool forward) {
  auto jump_result = context.zarr_loader.computeSubjectShapeQcJump(
      context.window_state.subject_shape_qc_filters, context.current_frame_num,
      forward);
  context.window_state.subject_shape_qc_status = std::move(jump_result.status);
  if (jump_result.target_frame.has_value()) {
    seekToFrame(context, *jump_result.target_frame);
  }
}

void applyTailKinematicsQcJump(FrameInspectNavigationContext &context,
                               bool forward) {
  auto jump_result = context.zarr_loader.computeTailKinematicsQcJump(
      context.window_state.tail_kinematics_qc_filters,
      context.current_frame_num, forward);
  context.window_state.tail_kinematics_qc_status =
      std::move(jump_result.status);
  if (jump_result.target_row.has_value()) {
    context.window_state.tail_kinematics_selected_row =
        static_cast<int>(*jump_result.target_row);
  }
  if (jump_result.target_frame.has_value()) {
    seekToFrame(context, *jump_result.target_frame);
  }
}

void applyEyeAngleQcJump(FrameInspectNavigationContext &context, bool forward) {
  auto jump_result = context.zarr_loader.computeEyeAngleQcJump(
      context.window_state.eye_angle_qc_filters, context.current_frame_num,
      forward);
  context.window_state.eye_angle_qc_status = std::move(jump_result.status);
  if (jump_result.target_row.has_value()) {
    context.window_state.eye_angle_selected_row =
        static_cast<int>(*jump_result.target_row);
  }
  if (jump_result.target_frame.has_value()) {
    seekToFrame(context, *jump_result.target_frame);
  }
}

} // namespace

app::FrameInspectPresentationOutput makeFrameInspectPresentationOutput(
    const FrameDebugWindowResult &result,
    const overlay::ReadOnlyOverlayControlState &current_overlay_controls,
    bool keypoint_full_frame_edit_requested) {
  app::FrameInspectPresentationOutput output;
  output.overlay_controls = current_overlay_controls;
  output.overlay_controls.show_keypoints = result.show_keypoint_markers;
  output.overlay_controls.show_headings = result.show_heading_arrows;
  output.overlay_controls.show_subject_masks = result.show_eye_masks;
  output.overlay_controls.show_subject_body_mask =
      result.show_subject_body_mask;
  output.overlay_controls.show_eye_left_mask = result.show_eye_left_mask;
  output.overlay_controls.show_eye_right_mask = result.show_eye_right_mask;
  output.overlay_controls.show_swim_bladder_mask =
      result.show_swim_bladder_mask;
  output.overlay_controls.show_eye_direction_beams =
      result.show_eye_direction_beams;
  output.overlay_controls.show_eye_gaze_rays = result.show_eye_gaze_rays;
  output.overlay_controls.show_eye_angle_arcs = result.show_eye_angle_arcs;
  output.overlay_controls.show_eye_angle_labels = result.show_eye_angle_labels;
  output.stimulus_debug_requested = result.show_stimulus_debug_windows;
  output.keypoint_full_frame_edit_requested =
      keypoint_full_frame_edit_requested;

  if (result.selected_keypoint_selection.has_value()) {
    const RefinedKeypointSelection &selection =
        *result.selected_keypoint_selection;
    constexpr size_t kMaxInt64 =
        static_cast<size_t>(std::numeric_limits<int64_t>::max());
    if (selection.frame_id <= kMaxInt64 &&
        selection.detection_index <= kMaxInt64) {
      output.keypoint_selection = app::FrameInspectKeypointSelection{
          selection.valid, selection.editable,
          static_cast<int64_t>(selection.frame_id),
          static_cast<int64_t>(selection.detection_index), selection.roi_index};
    }
  }
  return output;
}

FrameInspectNavigationRequest
makeFrameInspectNavigationRequest(const FrameDebugWindowResult &result) {
  app::FrameInspectCommandRequests requests;
  requests.requested_detection_dataset_index =
      result.requested_detection_dataset_index;
  requests.previous_review_frame = result.request_prev_review_frame;
  requests.next_review_frame = result.request_next_review_frame;
  requests.reset_frame_bbox_edits = result.request_reset_frame_bbox_edits;
  requests.clear_bbox_selection = result.request_clear_bbox_selection;
  requests.build_manual_payload_preview =
      result.request_build_manual_payload_preview;
  requests.write_manual_payload = result.request_write_manual_payload;
  requests.write_keypoint_review = result.request_keypoint_review_write;
  requests.previous_subject_shape_qc_frame =
      result.request_prev_subject_shape_qc_frame;
  requests.next_subject_shape_qc_frame =
      result.request_next_subject_shape_qc_frame;
  requests.previous_tail_kinematics_qc_frame =
      result.request_prev_tail_kinematics_qc_frame;
  requests.next_tail_kinematics_qc_frame =
      result.request_next_tail_kinematics_qc_frame;
  if (result.request_seek_tail_kinematics_row) {
    requests.tail_kinematics_row = result.requested_tail_kinematics_row;
  }
  requests.previous_eye_angle_qc_frame = result.request_prev_eye_angle_qc_frame;
  requests.next_eye_angle_qc_frame = result.request_next_eye_angle_qc_frame;
  if (result.request_seek_eye_angle_row) {
    requests.eye_angle_row = result.requested_eye_angle_row;
  }

  FrameInspectNavigationRequest request;
  request.commands = app::makeFrameInspectCommands(requests);
  if (result.review_filters_changed) {
    request.review_frame_filters = result.review_frame_filters;
  }
  return request;
}

bool containsFrameInspectCommand(
    const std::vector<app::FrameInspectCommand> &commands,
    app::FrameInspectCommandKind kind) {
  return std::any_of(commands.begin(), commands.end(),
                     [kind](const app::FrameInspectCommand &command) {
                       return command.kind == kind;
                     });
}

FrameInspectNavigationResult
applyFrameInspectNavigation(const FrameInspectNavigationRequest &request,
                            FrameInspectNavigationContext &context) {
  FrameInspectNavigationResult navigation_result;
  bool filters_applied = false;
  const auto apply_review_filters = [&]() {
    if (filters_applied || !request.review_frame_filters.has_value()) {
      return;
    }
    context.review_frame_filters = *request.review_frame_filters;
    invalidateReviewFrameCache(context.review_frame_cache);
    context.review_frame_status.clear();
    filters_applied = true;
  };

  for (const app::FrameInspectCommand &command : request.commands) {
    if (command.kind != app::FrameInspectCommandKind::SelectDetectionDataset) {
      apply_review_filters();
    }
    switch (command.kind) {
    case app::FrameInspectCommandKind::SelectDetectionDataset:
      if (command.value < 0 ||
          command.value >=
              static_cast<int64_t>(context.detection_dataset_ids.size()) ||
          !context.zarr_loader.setActiveDetectionDataset(
              context
                  .detection_dataset_ids[static_cast<size_t>(command.value)])) {
        break;
      }
      context.detection_dataset_choice = static_cast<int>(command.value);
      if (context.refresh_detection_dataset_options) {
        context.refresh_detection_dataset_options();
      }
      context.bbox_edit_state.clearAll();
      invalidateReviewFrameCache(context.review_frame_cache);
      context.review_frame_status.clear();
      if (context.zarr_loader.getTotalFrames() > 0 &&
          context.current_frame_num >=
              static_cast<int>(context.zarr_loader.getTotalFrames())) {
        context.current_frame_num =
            static_cast<int>(context.zarr_loader.getTotalFrames()) - 1;
      }
      break;
    case app::FrameInspectCommandKind::PreviousReviewFrame:
      applyReviewJump(context, false);
      break;
    case app::FrameInspectCommandKind::NextReviewFrame:
      applyReviewJump(context, true);
      break;
    case app::FrameInspectCommandKind::ResetFrameBboxEdits:
      context.bbox_edit_state.clearFrameEdits(context.current_frame_num);
      break;
    case app::FrameInspectCommandKind::ClearBboxSelection:
      context.bbox_edit_state.clearSelection();
      break;
    case app::FrameInspectCommandKind::PreviousSubjectShapeQcFrame:
      applySubjectShapeQcJump(context, false);
      break;
    case app::FrameInspectCommandKind::NextSubjectShapeQcFrame:
      applySubjectShapeQcJump(context, true);
      break;
    case app::FrameInspectCommandKind::PreviousTailKinematicsQcFrame:
      applyTailKinematicsQcJump(context, false);
      break;
    case app::FrameInspectCommandKind::NextTailKinematicsQcFrame:
      applyTailKinematicsQcJump(context, true);
      break;
    case app::FrameInspectCommandKind::SelectTailKinematicsRow: {
      if (command.value < 0) {
        break;
      }
      const size_t row = static_cast<size_t>(command.value);
      context.window_state.tail_kinematics_selected_row = static_cast<int>(row);
      const auto frame = context.zarr_loader.getTailKinematicsFrameForRow(row);
      if (frame.has_value()) {
        seekToFrame(context, *frame);
        context.window_state.tail_kinematics_qc_status =
            "Selected tail row " + std::to_string(row) + " mapped to frame " +
            std::to_string(*frame) + ".";
      } else {
        context.window_state.tail_kinematics_qc_status =
            "Selected tail row has no frame mapping.";
      }
      break;
    }
    case app::FrameInspectCommandKind::PreviousEyeAngleQcFrame:
      applyEyeAngleQcJump(context, false);
      break;
    case app::FrameInspectCommandKind::NextEyeAngleQcFrame:
      applyEyeAngleQcJump(context, true);
      break;
    case app::FrameInspectCommandKind::SelectEyeAngleRow: {
      if (command.value < 0) {
        break;
      }
      const size_t row = static_cast<size_t>(command.value);
      context.window_state.eye_angle_selected_row = static_cast<int>(row);
      const auto frame = context.zarr_loader.getEyeAngleFrameForRow(row);
      if (frame.has_value()) {
        seekToFrame(context, *frame);
        context.window_state.eye_angle_qc_status =
            "Selected eye-angle row " + std::to_string(row) +
            " mapped to frame " + std::to_string(*frame) + ".";
      } else {
        context.window_state.eye_angle_qc_status =
            "Selected eye-angle row has no frame mapping.";
      }
      break;
    }
    case app::FrameInspectCommandKind::BuildManualPayloadPreview:
    case app::FrameInspectCommandKind::WriteManualPayload:
    case app::FrameInspectCommandKind::WriteKeypointReview:
      navigation_result.unhandled_commands.push_back(command);
      break;
    }
  }
  apply_review_filters();
  return navigation_result;
}

} // namespace crimson::nvidia
