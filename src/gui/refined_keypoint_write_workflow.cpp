#include "gui/refined_keypoint_write_workflow.h"

#include <sstream>

namespace {

std::string buildEditStatusPrefix(CropKeypointEditorActionType action_type) {
    switch (action_type) {
    case CropKeypointEditorActionType::Save:
        return "Keypoint edit";
    case CropKeypointEditorActionType::MarkNoKeypoints:
        return "Marked fish_present_no_keypoints";
    case CropKeypointEditorActionType::MarkDetectionIssue:
        return "Marked detection_issue";
    case CropKeypointEditorActionType::Reset:
    case CropKeypointEditorActionType::None:
    default:
        return "Keypoint write";
    }
}

std::string buildEditFailurePrefix(CropKeypointEditorActionType action_type) {
    switch (action_type) {
    case CropKeypointEditorActionType::Save:
        return "Keypoint edit failed: ";
    case CropKeypointEditorActionType::MarkNoKeypoints:
        return "Mark no keypoints failed: ";
    case CropKeypointEditorActionType::MarkDetectionIssue:
        return "Mark detection issue failed: ";
    case CropKeypointEditorActionType::Reset:
    case CropKeypointEditorActionType::None:
    default:
        return "Keypoint write failed: ";
    }
}

std::string buildReloadFailurePrefix(CropKeypointEditorActionType action_type) {
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

}  // namespace

namespace {

void applyRefinedKeypointEditAction(
    RefinedKeypointRepository& refined_keypoint_repo,
    const CropKeypointEditorAction& action,
    const std::optional<RefinedKeypointSelection>& selection,
    const std::function<void()>& reset_editor_state,
    std::string& status_out,
    const std::function<bool(std::string&)>& reload_active_zarr) {
    if (!selection.has_value()) {
        return;
    }

    RefinedKeypointEditResult edit_result;
    std::string write_error;
    bool write_ok = false;

    switch (action.type) {
    case CropKeypointEditorActionType::Save:
        write_ok = refined_keypoint_repo.writeManualCorrection(
            *selection, action.keypoints_roi, write_error, &edit_result);
        break;
    case CropKeypointEditorActionType::MarkNoKeypoints:
        write_ok = refined_keypoint_repo.markFishPresentNoKeypoints(
            *selection, write_error, &edit_result);
        break;
    case CropKeypointEditorActionType::MarkDetectionIssue:
        write_ok = refined_keypoint_repo.markDetectionIssue(
            *selection, write_error, &edit_result);
        break;
    case CropKeypointEditorActionType::Reset:
    case CropKeypointEditorActionType::None:
    default:
        return;
    }

    if (!write_ok) {
        status_out = buildEditFailurePrefix(action.type) + write_error;
        return;
    }

    std::string reload_error;
    reset_editor_state();
    if (!reload_active_zarr(reload_error)) {
        status_out = buildReloadFailurePrefix(action.type) + reload_error;
        return;
    }

    std::ostringstream status;
    if (action.type == CropKeypointEditorActionType::Save) {
        status << (edit_result.changed ? "Keypoint edit saved"
                                       : "Keypoint edit was a no-op");
    } else {
        status << buildEditStatusPrefix(action.type);
    }
    status << ": roi=" << selection->roi_index;
    if (edit_result.summary_updated) {
        status << " summary=updated";
    }
    if (edit_result.stale_eye_mask_runs > 0) {
        status << " stale_eye_masks=" << edit_result.stale_eye_mask_runs;
    }
    status_out = status.str();
}

}  // namespace

void applyCropPreviewKeypointWriteAction(
    RefinedKeypointRepository& refined_keypoint_repo,
    const CropKeypointEditorAction& action,
    const std::optional<RefinedKeypointSelection>& selection,
    CropKeypointEditorState& editor_state,
    FullFrameKeypointEditState* synced_full_frame_state,
    std::string& status_out,
    const std::function<bool(std::string&)>& reload_active_zarr) {
    applyRefinedKeypointEditAction(
        refined_keypoint_repo,
        action,
        selection,
        [&]() {
            resetCropKeypointEditorState(editor_state);
            if (synced_full_frame_state != nullptr) {
                resetFullFrameKeypointEditState(*synced_full_frame_state);
            }
        },
        status_out,
        reload_active_zarr);
}

void applyFullFrameKeypointWriteAction(
    RefinedKeypointRepository& refined_keypoint_repo,
    const CropKeypointEditorAction& action,
    const std::optional<RefinedKeypointSelection>& selection,
    FullFrameKeypointEditState& editor_state,
    CropKeypointEditorState* synced_crop_state,
    std::string& status_out,
    const std::function<bool(std::string&)>& reload_active_zarr) {
    applyRefinedKeypointEditAction(
        refined_keypoint_repo,
        action,
        selection,
        [&]() {
            resetFullFrameKeypointEditState(editor_state);
            if (synced_crop_state != nullptr) {
                resetCropKeypointEditorState(*synced_crop_state);
            }
        },
        status_out,
        reload_active_zarr);
}

RefinedKeypointReviewWriteWorkflowResult applyRefinedKeypointReviewWrite(
    RefinedKeypointRepository& refined_keypoint_repo,
    const RefinedKeypointReviewPanelResult& review_result,
    std::string& status_out,
    const std::function<bool(std::string&)>& reload_active_zarr) {
    RefinedKeypointReviewWriteWorkflowResult result;
    if (!review_result.request_review_write) {
        return result;
    }

    std::string write_error;
    std::string resolved_run_name;
    if (!refined_keypoint_repo.writeReviewStatus(review_result.review_options,
                                                 write_error,
                                                 &resolved_run_name)) {
        status_out = "Keypoint review write failed: " + write_error;
        return result;
    }

    std::string reload_error;
    if (!reload_active_zarr(reload_error)) {
        status_out =
            "Keypoint review write succeeded but reload failed: " + reload_error;
        result.should_clear_zarr_loaded = true;
        return result;
    }

    std::ostringstream status;
    status << "Keypoint review status updated: run="
           << (resolved_run_name.empty() ? "<latest>" : resolved_run_name)
           << " state=" << review_result.review_options.state
           << " use=" << review_result.review_options.intended_use
           << " method=" << review_result.review_options.method;
    status_out = status.str();
    return result;
}
