#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace crimson::playback {

constexpr size_t kNoClippedSelectedRun = std::numeric_limits<size_t>::max();

// A parent-frame mapping supplied by the archive adapter.  The controller
// intentionally knows nothing about Zarr, video paths, or decoder state.
struct ClippedFrameBinding {
  bool mapped = false;
  size_t selected_run_index = kNoClippedSelectedRun;
  std::string clip_id;
  int64_t clip_local_frame_index = -1;
  int64_t first_parent_frame = -1;
  int64_t last_parent_frame = -1;
};

struct ClippedMediaHandoffState {
  size_t selected_run_index = kNoClippedSelectedRun;
  std::string clip_id;
  int64_t first_parent_frame = -1;
  int64_t last_parent_frame = -1;
  int64_t pending_switch_parent_frame = -1;
  bool switch_in_progress = false;
  int64_t last_presented_parent_frame = -1;
};

enum class ClippedMediaHandoffOutcome {
  None,
  Ignored,
  InvalidInput,
  SwitchRequested,
  SwitchLoaded,
  SwitchSettled,
  SwitchFailed,
  Reset,
};

struct ClippedMediaHandoffCommand {
  bool request_load_and_seek = false;
  int64_t parent_frame = -1;
  size_t expected_selected_run_index = kNoClippedSelectedRun;
  std::string expected_clip_id;
  int64_t expected_clip_local_frame_index = -1;
};

struct ClippedMediaHandoffResult {
  ClippedMediaHandoffOutcome outcome = ClippedMediaHandoffOutcome::None;
  ClippedMediaHandoffCommand command;
};

// Handles a newly presented parent frame.  `presented_binding` describes the
// just-presented frame and `next_binding` describes parent_frame + 1 when it
// is in range.  The caller owns resolving those bindings and executing any
// emitted command.
ClippedMediaHandoffResult
updateClippedMediaHandoff(ClippedMediaHandoffState &state, bool playback_active,
                          int64_t presented_parent_frame,
                          int64_t total_parent_frames,
                          const ClippedFrameBinding &presented_binding,
                          const ClippedFrameBinding &next_binding);

// The platform calls this only after the load/seek command has completed.
// Success installs the new clip identity but retains the pending switch until
// that clip has actually presented its requested parent frame.
ClippedMediaHandoffResult completeClippedMediaHandoffLoad(
    ClippedMediaHandoffState &state, const ClippedMediaHandoffCommand &command,
    bool load_succeeded, const ClippedFrameBinding &loaded_binding);

// Cancels an in-flight handoff while retaining the current displayed clip and
// its last settled parent frame.
ClippedMediaHandoffResult
resetClippedMediaHandoff(ClippedMediaHandoffState &state);

bool isValidClippedFrameBinding(const ClippedFrameBinding &binding);
bool isValidClippedMediaHandoffState(const ClippedMediaHandoffState &state);

} // namespace crimson::playback
