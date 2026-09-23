#pragma once

#include <cstdint>
#include <vector>

namespace crimson::playback {

struct DecoderSeekBookkeeping {
  uint64_t settled_local_frame = 0;
  int next_local_frame = 0;
};

// The decoder's forward-scan cursor identifies the first frame still queued
// for publication. Keep settlement and the sequential fallback counter on the
// same identity, including for externally mapped clips where timestamps are
// deliberately not used for frame numbering.
DecoderSeekBookkeeping
finalizeDecoderSeekBookkeeping(uint64_t decode_frame_cursor);

bool queuedDecoderFrameReachedTarget(uint64_t decode_frame_cursor,
                                     uint64_t target_frame);
uint64_t advanceDecoderSeekCursor(uint64_t decode_frame_cursor,
                                  int64_t mapped_frame);

int64_t publishDecoderFrameNumber(uint64_t local_frame,
                                  const std::vector<int64_t> *frame_number_map);

// Approximate seeks may intentionally settle on a neighboring frame. Exact
// seeks must not be reported as successful when the backend returns another
// frame.
bool cameraSeekSettlementMatches(bool accurate, int requested_frame,
                                 int settled_frame);

bool shouldAttemptSeekFramePresentation(bool rejected_ring_quarantined,
                                        bool otherwise_eligible);

} // namespace crimson::playback
