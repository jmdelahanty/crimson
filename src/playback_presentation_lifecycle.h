#pragma once

#include "frame_selection.h"

#include <cstdint>
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

// Portable adapter contract for renderers that either can or cannot identify
// the decoded-buffer slot responsible for a displayed frame. It consumes only
// portable frame/slot metadata; platform code remains responsible for mapping
// a selection request to a renderer resource and for releasing that resource.
enum class PlaybackPresentationSelectionMode {
  HoldCommittedFrame,
  Exact,
  LatestAtOrBefore,
};

struct PlaybackPresentationSelectionRequest {
  PlaybackPresentationSelectionMode mode =
      PlaybackPresentationSelectionMode::HoldCommittedFrame;
  int64_t target_frame = -1;
  int64_t minimum_frame_exclusive = -1;
  int preferred_slot = -1;
};

struct PlaybackPresentationCandidate {
  int64_t frame_number = -1;
  std::optional<int> slot;
};

struct PlaybackPresentationAdapterPlanInput {
  bool just_seeked = false;
  bool decoding_active = false;
  bool playing = false;
  int buffer_size = 0;
  int64_t previous_committed_frame = -1;
  int preferred_slot = -1;
  int64_t requested_frame = -1;
  std::optional<int64_t> minimum_decoded_frame;
  std::vector<PlaybackPresentationCandidate> buffered_frames;
};

struct PlaybackPresentationAdapterPlan {
  bool active = false;
  bool discontinuity = false;
  int64_t requested_frame = -1;
  int64_t bounded_target_frame = -1;
  int64_t minimum_decoded_frame = -1;
  int64_t frame = -1;
  std::optional<int> slot;
  bool clamped_to_buffer = false;
  PlaybackPresentationSelectionRequest selection;
};

PlaybackPresentationAdapterPlan planPlaybackPresentationAdapter(
    const PlaybackPresentationAdapterPlanInput &input);

struct PlaybackPresentationObservation {
  int64_t presented_frame = -1;
  std::optional<int> slot;
};

enum class PlaybackPresentationReleasePolicy {
  None,
  DeferUntilPresentation,
  ReleaseHistoryBeforeCommittedFrame,
};

struct PlaybackPresentationAdapterCommitInput {
  bool just_seeked = false;
  bool decoding_active = false;
  bool playing = false;
  bool release_history_explicitly = false;
  int buffer_size = 0;
  int64_t previous_committed_frame = -1;
  int64_t presenter_target_frame = -1;
  PlaybackPresentationObservation observation;
};

struct PlaybackPresentationAdapterCommit {
  bool eligible = false;
  bool discontinuity = false;
  bool observed_slot = false;
  bool committed = false;
  int64_t previous_committed_frame = -1;
  int64_t frame = -1;
  int slot = -1;
  PlaybackPresentationReleasePolicy release_policy =
      PlaybackPresentationReleasePolicy::None;
  int64_t release_before_frame = -1;
};

PlaybackPresentationAdapterCommit planPlaybackPresentationAdapterCommit(
    const PlaybackPresentationAdapterCommitInput &input);

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
