#pragma once

#include "app/frame_inspect_controller.h"
#include "gui/frame_debug_window.h"
#include "review_frame_state.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Applies the NVIDIA application's legacy Frame Inspect navigation commands.
//
// This boundary deliberately owns only navigation, dataset switching, and
// in-memory bbox-edit commands. Archive writes, decoder diagnostics, and UI
// rendering remain outside it because they have distinct lifecycle ownership.
namespace crimson::nvidia {

struct FrameInspectNavigationRequest {
  std::vector<app::FrameInspectCommand> commands;
  std::optional<ReviewFrameFilters> review_frame_filters;
};

struct FrameInspectNavigationResult {
  // Write and preview commands intentionally remain in the application
  // workflow until their archive transaction is extracted separately.
  std::vector<app::FrameInspectCommand> unhandled_commands;
};

struct FrameInspectNavigationContext {
  bool zarr_loaded = false;
  ZarrDetectionLoader &zarr_loader;
  zarr::DetectionRepository &detection_repository;
  int &current_frame_num;

  const std::vector<zarr::DetectionDataset> &detection_dataset_ids;
  int &detection_dataset_choice;
  ReviewFrameFilters &review_frame_filters;
  ReviewFrameCache &review_frame_cache;
  std::string &review_frame_status;
  ZarrBBoxEditState &bbox_edit_state;
  FrameDebugWindowState &window_state;

  // The application owns playback and dataset-option presentation state.
  std::function<void(int frame, bool prefer_buffer_when_paused)> seek_to_frame;
  std::function<void()> refresh_detection_dataset_options;
};

// Translates the legacy Frame Debug window surface to the portable controller
// without changing fields that the legacy window does not own.
app::FrameInspectPresentationOutput makeFrameInspectPresentationOutput(
    const FrameDebugWindowResult &result,
    const overlay::ReadOnlyOverlayControlState &current_overlay_controls,
    bool keypoint_full_frame_edit_requested);

FrameInspectNavigationRequest
makeFrameInspectNavigationRequest(const FrameDebugWindowResult &result);

bool containsFrameInspectCommand(
    const std::vector<app::FrameInspectCommand> &commands,
    app::FrameInspectCommandKind kind);

// Applies portable Frame Inspect commands through narrow repositories where
// available. Legacy QC navigation remains on ZarrDetectionLoader until its
// domain repositories are extracted. Other commands are returned to callers.
FrameInspectNavigationResult
applyFrameInspectNavigation(const FrameInspectNavigationRequest &request,
                            FrameInspectNavigationContext &context);

} // namespace crimson::nvidia
