#pragma once

#include "clipped_media_handoff.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace crimson::platform::nvidia {

enum class ClippedMediaCoordinatorEventKind {
  SwitchRequested,
  SwitchLoaded,
  SwitchPresented,
  SwitchFailed,
};

struct ClippedMediaCoordinatorEvent {
  ClippedMediaCoordinatorEventKind kind =
      ClippedMediaCoordinatorEventKind::SwitchRequested;
  int64_t parent_frame = -1;
  int64_t pending_switch_parent_frame = -1;
  std::string old_clip_id;
  std::string clip_id;
  size_t old_selected_run_index = playback::kNoClippedSelectedRun;
  size_t selected_run_index = playback::kNoClippedSelectedRun;
  int64_t clip_local_frame_index = -1;
  int64_t first_parent_frame = -1;
  int64_t last_parent_frame = -1;
};

struct ClippedMediaCoordinatorContext {
  playback::ClippedMediaHandoffState *state = nullptr;
  std::function<int64_t()> total_parent_frames;
  std::function<playback::ClippedFrameBinding(int64_t)> resolve_binding;
  std::function<bool(int64_t)> load_and_seek;
  std::function<void(const ClippedMediaCoordinatorEvent &)> publish_event;
};

// Executes the platform handoff command emitted by the backend-neutral policy.
// Media resolution, decoder seeking, and telemetry sinks remain injected by
// the NVIDIA composition root.
class ClippedMediaCoordinator {
public:
  explicit ClippedMediaCoordinator(ClippedMediaCoordinatorContext context);

  playback::ClippedMediaHandoffOutcome
  onPresentedFrame(int64_t presented_parent_frame, bool playback_active) const;

private:
  void publish(const ClippedMediaCoordinatorEvent &event) const;

  ClippedMediaCoordinatorContext context_;
};

} // namespace crimson::platform::nvidia
