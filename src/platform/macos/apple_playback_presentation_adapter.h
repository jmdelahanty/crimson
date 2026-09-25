#pragma once

#include "frame_presentation.h"
#include "platform/macos/apple_video_playback_buffer.h"
#include "playback_presentation_lifecycle.h"

#include <cstdint>
#include <optional>

struct ApplePlaybackPresentationRequest {
  int64_t requested_frame = -1;
  bool playing = false;
  double effective_frames_per_second = 0.0;
  bool discontinuity = false;
  std::optional<int64_t> visible_frame;
};

struct ApplePlaybackPresentationResult {
  std::optional<AppleDecodedVideoFrame> selected_frame;
  crimson::playback::FramePresentationDecision decision;
  crimson::playback::PlaybackPresentationAdapterPlan plan;
  crimson::playback::PlaybackPresentationAdapterCommit commit;
  bool candidate_committed = false;
  bool present_selected = false;
};

// Translates the slotless Apple decode buffer into the portable presentation
// contract. The buffer retains ownership of decoded surfaces and eviction.
ApplePlaybackPresentationResult updateApplePlaybackPresentation(
    AppleVideoPlaybackBuffer &playback,
    const ApplePlaybackPresentationRequest &request);
