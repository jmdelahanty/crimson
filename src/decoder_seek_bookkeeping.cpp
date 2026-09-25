#include "decoder_seek_bookkeeping.h"

#include <algorithm>

namespace crimson::playback {

DecoderSeekBookkeeping
finalizeDecoderSeekBookkeeping(uint64_t decode_frame_cursor) {
  return DecoderSeekBookkeeping{
      decode_frame_cursor,
      static_cast<int>(decode_frame_cursor),
  };
}

bool queuedDecoderFrameReachedTarget(uint64_t decode_frame_cursor,
                                     uint64_t target_frame) {
  return decode_frame_cursor >= target_frame;
}

uint64_t advanceDecoderSeekCursor(uint64_t decode_frame_cursor,
                                  int64_t mapped_frame) {
  const int64_t fallback_frame = static_cast<int64_t>(decode_frame_cursor);
  const int64_t next_frame =
      std::max(mapped_frame + 1, fallback_frame + 1);
  return static_cast<uint64_t>(std::max<int64_t>(0, next_frame));
}

int64_t publishDecoderFrameNumber(
    uint64_t local_frame, const std::vector<int64_t> *frame_number_map) {
  if (frame_number_map != nullptr && local_frame < frame_number_map->size()) {
    const int64_t parent_frame = (*frame_number_map)[local_frame];
    if (parent_frame >= 0) {
      return parent_frame;
    }
  }
  return static_cast<int64_t>(local_frame);
}

bool cameraSeekSettlementMatches(bool accurate, int requested_frame,
                                 int settled_frame) {
  return !accurate || requested_frame == settled_frame;
}

bool shouldAttemptSeekFramePresentation(bool rejected_ring_quarantined,
                                        bool otherwise_eligible) {
  return !rejected_ring_quarantined && otherwise_eligible;
}

} // namespace crimson::playback
