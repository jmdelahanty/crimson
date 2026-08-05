#pragma once

#include "frame_selection.h"

#include <optional>
#include <vector>

namespace crimson::playback {

// Backend-neutral policy for advancing the logical playback cursor from frames
// that a renderer has already presented. Slot inspection and release remain in
// the platform session controller.
struct PlaybackPresentationTargetInput {
  bool just_seeked = false;
  bool decoding_active = false;
  bool playing = false;
  int buffer_size = 0;
  int previous_committed_frame = 0;
  int preferred_slot = -1;
  int requested_frame = 0;
  std::optional<int> minimum_decoded_frame;
  std::vector<BufferedFrameCandidate> buffered_frames;
};

struct PlaybackPresentationTarget {
  bool active = false;
  // Raw transport request is retained for diagnostics. The legacy clipped
  // trace calls the decode-bounded value its requested frame.
  int requested_frame = -1;
  int bounded_target_frame = -1;
  int minimum_decoded_frame = -1;
  int frame = -1;
  int slot = -1;
  bool clamped_to_buffer = false;
};

PlaybackPresentationTarget
planPlaybackPresentationTarget(const PlaybackPresentationTargetInput &input);

struct PlaybackPresentationCommitInput {
  bool decoding_active = false;
  bool playing = false;
  int buffer_size = 0;
  int previous_committed_frame = -1;
  int presenter_target_frame = -1;
  int presented_frame = -1;
  int presented_slot = -1;
};

struct PlaybackPresentationCommit {
  bool eligible = false;
  bool presented_from_slot = false;
  bool committed = false;
  int previous_committed_frame = -1;
  int frame = -1;
  int slot = -1;
  bool release_deferred = false;
  int release_attempts = 0;
  int release_count = 0;
  int release_skip_count = 0;
};

PlaybackPresentationCommit
planPlaybackPresentationCommit(const PlaybackPresentationCommitInput &input);

struct PlaybackHistoryReleaseCandidate {
  int camera_index = -1;
  int slot_index = -1;
  int frame_number = -1;
};

std::vector<PlaybackHistoryReleaseCandidate>
selectPlaybackHistoryReleaseCandidates(
    const std::vector<PlaybackHistoryReleaseCandidate> &candidates,
    int committed_frame);

} // namespace crimson::playback
