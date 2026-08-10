#include "platform/nvidia/nvidia_refined_keypoint_write_session.h"

namespace crimson::platform::nvidia {

namespace {

std::string reloadFailurePrefix(CropKeypointEditorActionType action_type) {
  switch (action_type) {
  case CropKeypointEditorActionType::Save:
    return "Keypoint edit saved but reload failed: ";
  case CropKeypointEditorActionType::MarkNoKeypoints:
    return "Marked fish_present_no_keypoints but reload failed: ";
  case CropKeypointEditorActionType::MarkDetectionIssue:
    return "Marked detection_issue but reload failed: ";
  case CropKeypointEditorActionType::Reset:
  case CropKeypointEditorActionType::None:
  default:
    return "Keypoint write succeeded but reload failed: ";
  }
}

} // namespace

RefinedKeypointWriteWorkerResult executeRefinedKeypointWrite(
    const RefinedKeypointWriteRequest &request,
    const crimson::zarr::ReviewWriteRepositoryFactory &repository_factory) {
  RefinedKeypointWriteWorkerResult result;
  if (!request.selection.has_value()) {
    result.error = "No keypoint selection.";
    return result;
  }
  crimson::zarr::ReviewWriteOperation operation;
  operation.selection = *request.selection;
  switch (request.action.type) {
  case CropKeypointEditorActionType::Save:
    operation.kind =
        crimson::zarr::ReviewWriteOperationKind::ManualKeypointCorrection;
    operation.keypoints_roi = request.action.keypoints_roi;
    break;
  case CropKeypointEditorActionType::MarkNoKeypoints:
    operation.kind =
        crimson::zarr::ReviewWriteOperationKind::FishPresentNoKeypoints;
    break;
  case CropKeypointEditorActionType::MarkDetectionIssue:
    operation.kind = crimson::zarr::ReviewWriteOperationKind::DetectionIssue;
    break;
  case CropKeypointEditorActionType::Reset:
  case CropKeypointEditorActionType::None:
  default:
    result.error = "No keypoint write action.";
    return result;
  }
  return crimson::zarr::ExecuteReviewWrite(repository_factory,
                                           request.archive_path, operation);
}

bool pollAndApplyRefinedKeypointWrite(
    RefinedKeypointWriteSession &session, uint64_t active_session_generation,
    const std::string &active_archive_path, std::string &status_out,
    const RefinedKeypointWriteSettlementCallbacks &callbacks) {
  auto completion =
      session.takeReady(active_session_generation, active_archive_path);
  if (!completion.has_value()) {
    return false;
  }
  status_out = completion->status_message;
  if (!completion->worker_result.ok || completion->stale_session) {
    return true;
  }

  if (completion->request.reset_crop_editor && callbacks.reset_crop_editor) {
    callbacks.reset_crop_editor();
  }
  if (completion->request.reset_full_frame_editor &&
      callbacks.reset_full_frame_editor) {
    callbacks.reset_full_frame_editor();
  }

  std::string cache_error;
  if (!callbacks.apply_cache_update ||
      !callbacks.apply_cache_update(
          completion->worker_result.edit_result.cache_update, &cache_error)) {
    std::string reload_error;
    if (!callbacks.reload_active_zarr ||
        !callbacks.reload_active_zarr(reload_error)) {
      status_out = reloadFailurePrefix(completion->request.action.type) +
                   reload_error + " (cache update failed: " + cache_error + ")";
      return true;
    }
    if (callbacks.invalidate_after_write) {
      callbacks.invalidate_after_write();
    }
    status_out = completion->status_message +
                 " (reloaded; targeted cache update failed: " + cache_error +
                 ")";
    return true;
  }

  if (callbacks.invalidate_after_write) {
    callbacks.invalidate_after_write();
  }
  return true;
}

} // namespace crimson::platform::nvidia
