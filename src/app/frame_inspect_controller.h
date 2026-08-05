#pragma once

#include "read_only_overlay_controls.h"
#include "workspace_state.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace crimson::app {

// Portable identity for the observation selected in the Frame Inspect keypoint
// module. Backend adapters translate this to their repository-specific type.
struct FrameInspectKeypointSelection {
  bool valid = false;
  bool editable = false;
  int64_t frame = -1;
  int64_t observation_index = -1;
  int64_t roi_index = -1;
};

// Tab state remains owned by each presentation shell. Linux currently keeps it
// in FrameDebugWindowState; macOS keeps the selected view in workspace state
// and the synchronization marker in AppleFrameInspectPresentationState.
struct FrameInspectTabState {
  workspace::FrameInspectView &active_view;
  workspace::FrameInspectViewSyncState &view_sync;
};

// State owned by the portable controller rather than storage or rendering.
struct FrameInspectControllerState {
  std::optional<FrameInspectKeypointSelection> keypoint_selection;
  bool keypoint_full_frame_edit_enabled = false;
};

struct FrameInspectPresentationInput {
  workspace::FrameInspectView requested_view =
      workspace::FrameInspectView::Detect;
};

struct FrameInspectPresentationOutput {
  overlay::ReadOnlyOverlayControlState overlay_controls;
  bool stimulus_debug_requested = false;
  std::optional<FrameInspectKeypointSelection> keypoint_selection;
  bool keypoint_full_frame_edit_requested = false;
};

// Commands express a user intention. Their execution belongs in a backend
// adapter because rows, review caches, and write repositories are backend data.
enum class FrameInspectCommandKind : uint8_t {
  SelectDetectionDataset,
  PreviousReviewFrame,
  NextReviewFrame,
  ResetFrameBboxEdits,
  ClearBboxSelection,
  BuildManualPayloadPreview,
  WriteManualPayload,
  WriteKeypointReview,
  PreviousSubjectShapeQcFrame,
  NextSubjectShapeQcFrame,
  PreviousTailKinematicsQcFrame,
  NextTailKinematicsQcFrame,
  SelectTailKinematicsRow,
  PreviousEyeAngleQcFrame,
  NextEyeAngleQcFrame,
  SelectEyeAngleRow,
};

struct FrameInspectCommand {
  FrameInspectCommandKind kind = FrameInspectCommandKind::PreviousReviewFrame;
  int64_t value = -1;
};

// This is deliberately a primitive-only representation of requests from the
// UI. Backend adapters retain complex review filters and write options.
struct FrameInspectCommandRequests {
  int requested_detection_dataset_index = -1;
  bool previous_review_frame = false;
  bool next_review_frame = false;
  bool reset_frame_bbox_edits = false;
  bool clear_bbox_selection = false;
  bool build_manual_payload_preview = false;
  bool write_manual_payload = false;
  bool write_keypoint_review = false;
  bool previous_subject_shape_qc_frame = false;
  bool next_subject_shape_qc_frame = false;
  bool previous_tail_kinematics_qc_frame = false;
  bool next_tail_kinematics_qc_frame = false;
  std::optional<size_t> tail_kinematics_row;
  bool previous_eye_angle_qc_frame = false;
  bool next_eye_angle_qc_frame = false;
  std::optional<size_t> eye_angle_row;
};

// Applies a workspace tab request before drawing. The view-sync state prevents
// a persistent requested tab from resetting an ImGui tab selected by the user.
bool prepareFrameInspectPresentation(
    const FrameInspectTabState &tab_state,
    const FrameInspectPresentationInput &input);

// Records the tab selected by the window implementation. Call this after the
// presentation layer has drawn its tab bar.
void observeFrameInspectActiveView(const FrameInspectTabState &tab_state,
                                   workspace::FrameInspectView active_view);

// Commits portable output after drawing and updates the workspace snapshot
// surface. This function does not issue storage operations or renderer calls.
void applyFrameInspectPresentation(FrameInspectControllerState *state,
                                   const FrameInspectTabState &tab_state,
                                   const FrameInspectPresentationOutput &output,
                                   workspace::WorkspaceState *workspace_state);

// Produces the deterministic command order consumed by a platform adapter.
std::vector<FrameInspectCommand>
makeFrameInspectCommands(const FrameInspectCommandRequests &requests);

} // namespace crimson::app
