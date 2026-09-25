#include "platform/macos/apple_playback_presentation_adapter.h"

#include <algorithm>
#include <limits>

ApplePlaybackPresentationResult updateApplePlaybackPresentation(
    AppleVideoPlaybackBuffer &playback,
    const ApplePlaybackPresentationRequest &request) {
  playback.setPlaybackState(request.requested_frame, request.playing,
                            request.effective_frames_per_second);

  crimson::playback::PlaybackPresentationAdapterPlanInput plan_input;
  plan_input.just_seeked = request.discontinuity;
  plan_input.decoding_active = playback.isOpen();
  plan_input.playing = request.playing;
  plan_input.buffer_size = static_cast<int>(
      std::min<size_t>(playback.capacity(),
                       static_cast<size_t>(std::numeric_limits<int>::max())));
  plan_input.previous_committed_frame = request.visible_frame.value_or(-1);
  plan_input.requested_frame = request.requested_frame;
  const auto buffered_frames = playback.bufferedFrameNumbers();
  plan_input.buffered_frames.reserve(buffered_frames.size());
  for (const int64_t frame_number : buffered_frames) {
    plan_input.buffered_frames.push_back({frame_number, std::nullopt});
  }
  const AppleVideoPlaybackBufferMetrics metrics = playback.metrics();
  if (metrics.last_decoded_frame >= 0) {
    plan_input.minimum_decoded_frame = metrics.last_decoded_frame;
  }

  ApplePlaybackPresentationResult result;
  result.plan = crimson::playback::planPlaybackPresentationAdapter(plan_input);
  if (result.plan.active &&
      result.plan.selection.mode !=
          crimson::playback::PlaybackPresentationSelectionMode::
              HoldCommittedFrame) {
    // The portable plan already chose a concrete buffered frame. If that frame
    // is evicted before this lookup, hold the visible frame instead of running
    // a second fallback search against a changed deque.
    result.selected_frame = playback.frameForTarget(result.plan.frame, true);
  } else if (!result.plan.active) {
    // Paused and discontinuous requests retain the existing exact-frame
    // semantics; the portable adapter advances only steady playback.
    result.selected_frame =
        playback.frameForTarget(request.requested_frame, !request.playing);
  }

  const auto selected_frame_number =
      result.selected_frame
          ? std::optional<int64_t>(result.selected_frame->metadata.frame_number)
          : std::nullopt;
  result.decision = crimson::playback::resolveFramePresentation(
      {request.requested_frame, selected_frame_number, request.visible_frame,
       request.playing ? crimson::playback::MissingFramePolicy::HoldVisible
                       : crimson::playback::MissingFramePolicy::ClearVisible,
       request.discontinuity});

  if (result.plan.active && result.selected_frame &&
      result.decision.action ==
          crimson::playback::FramePresentationAction::Present) {
    crimson::playback::PlaybackPresentationAdapterCommitInput commit_input;
    commit_input.decoding_active = plan_input.decoding_active;
    commit_input.playing = plan_input.playing;
    commit_input.release_history_explicitly = false;
    commit_input.buffer_size = plan_input.buffer_size;
    commit_input.previous_committed_frame = plan_input.previous_committed_frame;
    commit_input.presenter_target_frame = result.plan.frame;
    commit_input.observation = {result.selected_frame->metadata.frame_number,
                                std::nullopt};
    result.commit =
        crimson::playback::planPlaybackPresentationAdapterCommit(commit_input);
    result.candidate_committed = result.commit.committed;
  } else if (!result.plan.active && result.selected_frame &&
             result.decision.action ==
                 crimson::playback::FramePresentationAction::Present) {
    // Paused and discontinuous exact-frame presentation is deliberately
    // outside the steady-playback commit gate.
    result.candidate_committed = true;
  }
  result.present_selected =
      result.candidate_committed && result.selected_frame &&
      result.decision.action ==
          crimson::playback::FramePresentationAction::Present;
  return result;
}
