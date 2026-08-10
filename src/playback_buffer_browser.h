#pragma once

#include "playback_presentation_lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace crimson::playback {

struct PlaybackBufferBrowserSpan {
  size_t begin_index = 0;
  size_t end_index = 0;
};

// Immutable, backend-neutral view of decoded frames that can be browsed while
// playback is paused. Platform slot identities are optional because retained
// Apple decode surfaces do not use NVIDIA-style ring slots.
struct PlaybackBufferBrowserModel {
  int64_t selected_frame = -1;
  std::vector<PlaybackPresentationCandidate> items;
  std::vector<PlaybackBufferBrowserSpan> spans;
  std::optional<size_t> selected_index;
  std::optional<int> preferred_slot;
  int64_t oldest_frame = -1;
  int64_t newest_frame = -1;
  int64_t largest_gap = 0;
};

PlaybackBufferBrowserModel buildPlaybackBufferBrowserModel(
    std::vector<PlaybackPresentationCandidate> candidates,
    int64_t selected_frame);

} // namespace crimson::playback
