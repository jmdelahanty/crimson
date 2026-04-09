#pragma once

#include "gui/crop_keypoint_editor.h"
#include "gui/refined_keypoint_review_panel.h"
#include "refined_keypoint_repository.h"

#include <functional>
#include <optional>
#include <string>

void applyCropPreviewKeypointWriteAction(
    RefinedKeypointRepository& refined_keypoint_repo,
    const CropKeypointEditorAction& action,
    const std::optional<RefinedKeypointSelection>& selection,
    CropKeypointEditorState& editor_state,
    std::string& status_out,
    const std::function<bool(std::string&)>& reload_active_zarr);

struct RefinedKeypointReviewWriteWorkflowResult {
    bool should_clear_zarr_loaded = false;
};

RefinedKeypointReviewWriteWorkflowResult applyRefinedKeypointReviewWrite(
    RefinedKeypointRepository& refined_keypoint_repo,
    const RefinedKeypointReviewPanelResult& review_result,
    std::string& status_out,
    const std::function<bool(std::string&)>& reload_active_zarr);
