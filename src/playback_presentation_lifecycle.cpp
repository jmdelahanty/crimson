#include "playback_presentation_lifecycle.h"

#include <algorithm>

namespace crimson::playback {

PlaybackPresentationTarget
planPlaybackPresentationTarget(const PlaybackPresentationTargetInput &input) {
  PlaybackPresentationTarget result;
  result.active = !input.just_seeked && input.decoding_active &&
                  input.playing && input.buffer_size > 0;
  result.requested_frame = input.requested_frame;
  result.minimum_decoded_frame = input.minimum_decoded_frame.value_or(-1);
  result.frame = input.previous_committed_frame;
  result.bounded_target_frame = input.previous_committed_frame;
  if (!result.active) {
    return result;
  }

  const int bounded_frame =
      input.minimum_decoded_frame.has_value()
          ? std::min(input.requested_frame, *input.minimum_decoded_frame)
          : input.previous_committed_frame;
  result.frame = std::max(input.previous_committed_frame, bounded_frame);
  result.bounded_target_frame = result.frame;
  const int requested_target = result.frame;
  if (result.frame > input.previous_committed_frame) {
    FrameSelectionRequest exact_request;
    exact_request.target_frame = result.frame;
    exact_request.preferred_slot = input.preferred_slot;
    exact_request.fallback = FrameSelectionFallback::ExactOnly;
    result.slot =
        selectBufferedFrame(input.buffered_frames, exact_request).slot_index;
    if (result.slot < 0) {
      FrameSelectionRequest buffered_request;
      buffered_request.target_frame = result.frame;
      buffered_request.minimum_frame_exclusive = input.previous_committed_frame;
      buffered_request.fallback = FrameSelectionFallback::LatestAtOrBefore;
      const FrameSelectionResult buffered =
          selectBufferedFrame(input.buffered_frames, buffered_request);
      if (buffered.frame_number > input.previous_committed_frame &&
          buffered.slot_index >= 0) {
        result.frame = buffered.frame_number;
        result.slot = buffered.slot_index;
      } else {
        result.frame = input.previous_committed_frame;
      }
    }
  }
  result.clamped_to_buffer = requested_target != result.frame;
  return result;
}

PlaybackPresentationCommit
planPlaybackPresentationCommit(const PlaybackPresentationCommitInput &input) {
  PlaybackPresentationCommit result;
  result.eligible =
      input.decoding_active && input.playing && input.buffer_size > 0;
  result.previous_committed_frame = input.previous_committed_frame;
  if (!result.eligible) {
    return result;
  }

  result.presented_from_slot =
      input.presented_slot >= 0 && input.presented_frame >= 0;
  result.committed = result.presented_from_slot &&
                     input.presented_frame >= input.previous_committed_frame;
  if (result.committed) {
    result.frame = input.presented_frame;
    result.slot = input.presented_slot;
  } else {
    result.release_deferred =
        input.presenter_target_frame > input.previous_committed_frame;
  }
  return result;
}

PlaybackPresentationAdapterPlan planPlaybackPresentationAdapter(
    const PlaybackPresentationAdapterPlanInput &input) {
  PlaybackPresentationAdapterPlan result;
  result.discontinuity = input.just_seeked;
  result.requested_frame = input.requested_frame;
  result.minimum_decoded_frame = input.minimum_decoded_frame.value_or(-1);
  result.frame = input.previous_committed_frame;
  result.bounded_target_frame = input.previous_committed_frame;
  result.selection.preferred_slot = input.preferred_slot;
  result.selection.target_frame = input.previous_committed_frame;
  result.selection.minimum_frame_exclusive = input.previous_committed_frame;
  result.active = !result.discontinuity && input.decoding_active &&
                  input.playing && input.buffer_size > 0;
  if (!result.active) {
    return result;
  }

  const int64_t bounded_frame =
      input.minimum_decoded_frame.has_value()
          ? std::min(input.requested_frame, *input.minimum_decoded_frame)
          : input.previous_committed_frame;
  const int64_t target_frame =
      std::max(input.previous_committed_frame, bounded_frame);
  result.bounded_target_frame = target_frame;
  result.selection.target_frame = target_frame;
  if (target_frame <= input.previous_committed_frame) {
    return result;
  }

  bool found_exact = false;
  std::optional<int> exact_slot;
  int64_t latest_frame = -1;
  std::optional<int> latest_slot;
  for (const PlaybackPresentationCandidate &candidate : input.buffered_frames) {
    if (candidate.frame_number == target_frame) {
      if (!found_exact || candidate.slot == input.preferred_slot) {
        found_exact = true;
        exact_slot = candidate.slot;
      }
      continue;
    }
    if (candidate.frame_number > input.previous_committed_frame &&
        candidate.frame_number <= target_frame &&
        candidate.frame_number > latest_frame) {
      latest_frame = candidate.frame_number;
      latest_slot = candidate.slot;
    }
  }
  if (found_exact) {
    result.frame = target_frame;
    result.slot = exact_slot;
    result.selection.mode = PlaybackPresentationSelectionMode::Exact;
  } else if (latest_frame >= 0) {
    result.frame = latest_frame;
    result.slot = latest_slot;
    result.clamped_to_buffer = latest_frame != target_frame;
    result.selection.mode = PlaybackPresentationSelectionMode::LatestAtOrBefore;
  }
  return result;
}

PlaybackPresentationAdapterCommit planPlaybackPresentationAdapterCommit(
    const PlaybackPresentationAdapterCommitInput &input) {
  PlaybackPresentationAdapterCommit result;
  result.discontinuity = input.just_seeked;
  result.previous_committed_frame = input.previous_committed_frame;
  result.eligible = !result.discontinuity && input.decoding_active &&
                    input.playing && input.buffer_size > 0;
  if (!result.eligible) {
    return result;
  }

  result.observed_slot =
      input.observation.slot.has_value() && *input.observation.slot >= 0;
  if (input.observation.presented_frame >= 0 &&
      input.observation.presented_frame >= input.previous_committed_frame) {
    result.committed = true;
    result.frame = input.observation.presented_frame;
    result.slot = result.observed_slot ? *input.observation.slot : -1;
    if (input.release_history_explicitly) {
      result.release_policy =
          PlaybackPresentationReleasePolicy::ReleaseHistoryBeforeCommittedFrame;
      result.release_before_frame = result.frame;
    }
  } else if (input.release_history_explicitly &&
             input.presenter_target_frame > input.previous_committed_frame) {
    result.release_policy =
        PlaybackPresentationReleasePolicy::DeferUntilPresentation;
  }
  return result;
}

std::vector<PlaybackHistoryReleaseCandidate>
selectPlaybackHistoryReleaseCandidates(
    const std::vector<PlaybackHistoryReleaseCandidate> &candidates,
    int committed_frame) {
  std::vector<PlaybackHistoryReleaseCandidate> result;
  result.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    if (candidate.camera_index >= 0 && candidate.slot_index >= 0 &&
        candidate.frame_number >= 0 &&
        candidate.frame_number < committed_frame) {
      result.push_back(candidate);
    }
  }
  return result;
}

} // namespace crimson::playback
