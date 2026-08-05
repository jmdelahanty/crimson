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
