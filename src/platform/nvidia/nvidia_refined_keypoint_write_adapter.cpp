#include "platform/nvidia/nvidia_refined_keypoint_write_session.h"

#include "zarr_loader.h"

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

RefinedKeypointWriteWorkerResult
executeLegacyRefinedKeypointWrite(const RefinedKeypointWriteRequest &request) {
  RefinedKeypointWriteWorkerResult result;
  if (!request.selection.has_value()) {
    result.error = "No keypoint selection.";
    return result;
  }
  if (request.archive_path.empty()) {
    result.error = "No loaded Zarr archive.";
    return result;
  }

  ZarrDetectionLoader worker_loader;
  std::string load_error;
  if (!worker_loader.loadZarrFile(request.archive_path, load_error)) {
    result.error = "Worker failed to load active Zarr: " + load_error;
    return result;
  }

  RefinedKeypointRepository refined_keypoint_repo(worker_loader);
  switch (request.action.type) {
  case CropKeypointEditorActionType::Save:
    result.ok = refined_keypoint_repo.writeManualCorrection(
        *request.selection, request.action.keypoints_roi, result.error,
        &result.edit_result);
    break;
  case CropKeypointEditorActionType::MarkNoKeypoints:
    result.ok = refined_keypoint_repo.markFishPresentNoKeypoints(
        *request.selection, result.error, &result.edit_result);
    break;
  case CropKeypointEditorActionType::MarkDetectionIssue:
    result.ok = refined_keypoint_repo.markDetectionIssue(
        *request.selection, result.error, &result.edit_result);
    break;
  case CropKeypointEditorActionType::Reset:
  case CropKeypointEditorActionType::None:
  default:
    result.error = "No keypoint write action.";
    break;
  }
  return result;
}

bool pollAndApplyLegacyRefinedKeypointWrite(
    RefinedKeypointWriteSession &session, uint64_t active_session_generation,
    ZarrDetectionLoader &loader, CropKeypointEditorState &crop_editor_state,
    FullFrameKeypointEditState &full_frame_editor_state,
    std::string &status_out,
    const std::function<bool(std::string &)> &reload_active_zarr,
    const std::function<void()> &invalidate_after_write) {
  auto completion =
      session.takeReady(active_session_generation, loader.getArchivePath());
  if (!completion.has_value()) {
    return false;
  }
  status_out = completion->status_message;
  if (!completion->worker_result.ok || completion->stale_session) {
    return true;
  }

  if (completion->request.reset_crop_editor) {
    resetCropKeypointEditorState(crop_editor_state);
  }
  if (completion->request.reset_full_frame_editor) {
    resetFullFrameKeypointEditState(full_frame_editor_state);
  }

  std::string cache_error;
  if (!loader.applyRefinedKeypointCacheUpdate(
          completion->worker_result.edit_result.cache_update, &cache_error)) {
    std::string reload_error;
    if (!reload_active_zarr(reload_error)) {
      status_out = reloadFailurePrefix(completion->request.action.type) +
                   reload_error + " (cache update failed: " + cache_error + ")";
      return true;
    }
    if (invalidate_after_write) {
      invalidate_after_write();
    }
    status_out = completion->status_message +
                 " (reloaded; targeted cache update failed: " + cache_error +
                 ")";
    return true;
  }

  if (invalidate_after_write) {
    invalidate_after_write();
  }
  return true;
}

} // namespace crimson::platform::nvidia
