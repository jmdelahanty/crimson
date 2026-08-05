#include "clipped_media_handoff.h"

namespace crimson::playback {
namespace {

bool bindingContainsParentFrame(const ClippedFrameBinding &binding,
                                int64_t parent_frame) {
  return isValidClippedFrameBinding(binding) && parent_frame >= 0 &&
         parent_frame >= binding.first_parent_frame &&
         parent_frame <= binding.last_parent_frame;
}

bool commandMatches(const ClippedMediaHandoffCommand &command,
                    const ClippedFrameBinding &binding) {
  return command.request_load_and_seek &&
         bindingContainsParentFrame(binding, command.parent_frame) &&
         binding.selected_run_index == command.expected_selected_run_index &&
         binding.clip_id == command.expected_clip_id &&
         binding.clip_local_frame_index ==
             command.expected_clip_local_frame_index;
}

void clearPendingSwitch(ClippedMediaHandoffState &state) {
  state.switch_in_progress = false;
  state.pending_switch_parent_frame = -1;
}

} // namespace

bool isValidClippedFrameBinding(const ClippedFrameBinding &binding) {
  return binding.mapped &&
         binding.selected_run_index != kNoClippedSelectedRun &&
         !binding.clip_id.empty() && binding.clip_local_frame_index >= 0 &&
         binding.first_parent_frame >= 0 &&
         binding.last_parent_frame >= binding.first_parent_frame;
}

bool isValidClippedMediaHandoffState(const ClippedMediaHandoffState &state) {
  const bool current_clip_valid =
      state.selected_run_index != kNoClippedSelectedRun &&
      !state.clip_id.empty() && state.first_parent_frame >= 0 &&
      state.last_parent_frame >= state.first_parent_frame;
  const bool pending_valid = !state.switch_in_progress
                                 ? state.pending_switch_parent_frame == -1
                                 : state.pending_switch_parent_frame >= 0;
  return current_clip_valid && pending_valid;
}

ClippedMediaHandoffResult
updateClippedMediaHandoff(ClippedMediaHandoffState &state, bool playback_active,
                          int64_t presented_parent_frame,
                          int64_t total_parent_frames,
                          const ClippedFrameBinding &presented_binding,
                          const ClippedFrameBinding &next_binding) {
  ClippedMediaHandoffResult result;
  if (!playback_active) {
    result.outcome = ClippedMediaHandoffOutcome::Ignored;
    return result;
  }
  if (!isValidClippedMediaHandoffState(state) || presented_parent_frame < 0 ||
      total_parent_frames <= 0 ||
      presented_parent_frame >= total_parent_frames ||
      !bindingContainsParentFrame(presented_binding, presented_parent_frame)) {
    result.outcome = ClippedMediaHandoffOutcome::InvalidInput;
    return result;
  }

  if (state.switch_in_progress) {
    if (presented_parent_frame >= state.pending_switch_parent_frame &&
        presented_binding.selected_run_index == state.selected_run_index) {
      state.last_presented_parent_frame = presented_parent_frame;
      clearPendingSwitch(state);
      result.outcome = ClippedMediaHandoffOutcome::SwitchSettled;
      return result;
    }
    if (presented_binding.selected_run_index == state.selected_run_index) {
      state.last_presented_parent_frame = presented_parent_frame;
    }
    result.outcome = ClippedMediaHandoffOutcome::Ignored;
    return result;
  }

  if (presented_binding.selected_run_index != state.selected_run_index) {
    result.outcome = ClippedMediaHandoffOutcome::InvalidInput;
    return result;
  }
  state.last_presented_parent_frame = presented_parent_frame;
  if (presented_parent_frame < state.last_parent_frame) {
    result.outcome = ClippedMediaHandoffOutcome::Ignored;
    return result;
  }

  const int64_t next_parent_frame = presented_parent_frame + 1;
  if (next_parent_frame < 0 || next_parent_frame >= total_parent_frames) {
    result.outcome = ClippedMediaHandoffOutcome::Ignored;
    return result;
  }
  if (!bindingContainsParentFrame(next_binding, next_parent_frame)) {
    result.outcome = ClippedMediaHandoffOutcome::InvalidInput;
    return result;
  }
  if (next_binding.selected_run_index == state.selected_run_index) {
    result.outcome = ClippedMediaHandoffOutcome::Ignored;
    return result;
  }

  state.switch_in_progress = true;
  state.pending_switch_parent_frame = next_parent_frame;
  result.outcome = ClippedMediaHandoffOutcome::SwitchRequested;
  result.command.request_load_and_seek = true;
  result.command.parent_frame = next_parent_frame;
  result.command.expected_selected_run_index = next_binding.selected_run_index;
  result.command.expected_clip_id = next_binding.clip_id;
  result.command.expected_clip_local_frame_index =
      next_binding.clip_local_frame_index;
  return result;
}

ClippedMediaHandoffResult completeClippedMediaHandoffLoad(
    ClippedMediaHandoffState &state, const ClippedMediaHandoffCommand &command,
    bool load_succeeded, const ClippedFrameBinding &loaded_binding) {
  ClippedMediaHandoffResult result;
  if (!isValidClippedMediaHandoffState(state) || !state.switch_in_progress ||
      state.pending_switch_parent_frame != command.parent_frame ||
      !command.request_load_and_seek) {
    result.outcome = ClippedMediaHandoffOutcome::InvalidInput;
    return result;
  }
  if (!load_succeeded || !commandMatches(command, loaded_binding)) {
    clearPendingSwitch(state);
    result.outcome = ClippedMediaHandoffOutcome::SwitchFailed;
    return result;
  }

  state.selected_run_index = loaded_binding.selected_run_index;
  state.clip_id = loaded_binding.clip_id;
  state.first_parent_frame = loaded_binding.first_parent_frame;
  state.last_parent_frame = loaded_binding.last_parent_frame;
  result.outcome = ClippedMediaHandoffOutcome::SwitchLoaded;
  return result;
}

ClippedMediaHandoffResult
resetClippedMediaHandoff(ClippedMediaHandoffState &state) {
  clearPendingSwitch(state);
  ClippedMediaHandoffResult result;
  result.outcome = ClippedMediaHandoffOutcome::Reset;
  return result;
}

} // namespace crimson::playback
