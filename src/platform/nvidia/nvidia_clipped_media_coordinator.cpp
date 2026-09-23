#include "platform/nvidia/nvidia_clipped_media_coordinator.h"

#include <utility>

namespace crimson::platform::nvidia {

ClippedMediaCoordinator::ClippedMediaCoordinator(
    ClippedMediaCoordinatorContext context)
    : context_(std::move(context)) {}

void ClippedMediaCoordinator::publish(
    const ClippedMediaCoordinatorEvent &event) const {
  if (context_.publish_event) {
    context_.publish_event(event);
  }
}

playback::ClippedMediaHandoffOutcome
ClippedMediaCoordinator::onPresentedFrame(int64_t presented_parent_frame,
                                          bool playback_active) const {
  if (context_.state == nullptr || !context_.total_parent_frames ||
      !context_.resolve_binding || presented_parent_frame < 0) {
    return playback::ClippedMediaHandoffOutcome::InvalidInput;
  }
  auto &state = *context_.state;
  if (!playback::isValidClippedMediaHandoffState(state)) {
    return playback::ClippedMediaHandoffOutcome::InvalidInput;
  }

  const int64_t total_parent_frames = context_.total_parent_frames();
  const auto presented_binding =
      context_.resolve_binding(presented_parent_frame);
  playback::ClippedFrameBinding next_binding;
  const int64_t next_parent_frame = presented_parent_frame + 1;
  if (presented_parent_frame >= state.last_parent_frame &&
      next_parent_frame >= 0 && next_parent_frame < total_parent_frames) {
    next_binding = context_.resolve_binding(next_parent_frame);
  }

  const int64_t pending_switch_before = state.pending_switch_parent_frame;
  const std::string old_clip = state.clip_id;
  const size_t old_selected_run = state.selected_run_index;
  const auto handoff = playback::updateClippedMediaHandoff(
      state, playback_active, presented_parent_frame, total_parent_frames,
      presented_binding, next_binding);

  if (handoff.outcome == playback::ClippedMediaHandoffOutcome::SwitchSettled) {
    publish({ClippedMediaCoordinatorEventKind::SwitchPresented,
             presented_parent_frame, pending_switch_before, old_clip,
             state.clip_id, old_selected_run, state.selected_run_index,
             presented_binding.clip_local_frame_index, state.first_parent_frame,
             state.last_parent_frame});
    return handoff.outcome;
  }
  if (handoff.outcome !=
      playback::ClippedMediaHandoffOutcome::SwitchRequested) {
    return handoff.outcome;
  }

  const auto &command = handoff.command;
  publish({ClippedMediaCoordinatorEventKind::SwitchRequested,
           command.parent_frame, state.pending_switch_parent_frame, old_clip,
           command.expected_clip_id, old_selected_run,
           command.expected_selected_run_index,
           command.expected_clip_local_frame_index});

  const bool load_succeeded =
      context_.load_and_seek && context_.load_and_seek(command.parent_frame);
  const auto loaded_binding = context_.resolve_binding(command.parent_frame);
  const auto completion = playback::completeClippedMediaHandoffLoad(
      state, command, load_succeeded, loaded_binding);

  if (completion.outcome !=
      playback::ClippedMediaHandoffOutcome::SwitchLoaded) {
    (void)playback::resetClippedMediaHandoff(state);
    publish({ClippedMediaCoordinatorEventKind::SwitchFailed,
             command.parent_frame, -1, old_clip, command.expected_clip_id,
             old_selected_run, command.expected_selected_run_index,
             command.expected_clip_local_frame_index});
    return completion.outcome;
  }

  publish({ClippedMediaCoordinatorEventKind::SwitchLoaded, command.parent_frame,
           state.pending_switch_parent_frame, old_clip, state.clip_id,
           old_selected_run, state.selected_run_index,
           command.expected_clip_local_frame_index, state.first_parent_frame,
           state.last_parent_frame});
  return completion.outcome;
}

} // namespace crimson::platform::nvidia
