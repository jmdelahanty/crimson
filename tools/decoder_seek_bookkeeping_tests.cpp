#include "decoder_seek_bookkeeping.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition    \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testSuccessfulForwardScanUsesTargetIdentity() {
  uint64_t cursor = 0;
  int queued_frames = 1;
  int discarded_frames = 0;
  while (queued_frames > 0 &&
         !crimson::playback::queuedDecoderFrameReachedTarget(cursor, 10)) {
    --queued_frames;
    ++discarded_frames;
    cursor = crimson::playback::advanceDecoderSeekCursor(
        cursor, static_cast<int64_t>(cursor));
    queued_frames = 1;
  }
  CHECK(cursor == 10);
  CHECK(discarded_frames == 10);
  CHECK(queued_frames == 1);
  const auto bookkeeping =
      crimson::playback::finalizeDecoderSeekBookkeeping(cursor);
  CHECK(bookkeeping.settled_local_frame == 10);
  CHECK(bookkeeping.next_local_frame == 10);

  // The first queued frame after discarding 0..9 is local frame 10. External
  // clip maps must publish its parent identity, not the keyframe's identity.
  std::vector<int64_t> frame_map(20);
  for (size_t i = 0; i < frame_map.size(); ++i) {
    frame_map[i] = 54000 + static_cast<int64_t>(i);
  }
  CHECK(crimson::playback::publishDecoderFrameNumber(
            bookkeeping.settled_local_frame, &frame_map) == 54010);
  CHECK(crimson::playback::publishDecoderFrameNumber(
            static_cast<uint64_t>(bookkeeping.next_local_frame), &frame_map) ==
        54010);
  return true;
}

bool testOrdinaryNonresidentSeekFrom60To100() {
  constexpr uint64_t target = 100;
  uint64_t cursor = 60;
  int queued_frames = 1;
  int discarded_frames = 0;
  while (queued_frames > 0 &&
         !crimson::playback::queuedDecoderFrameReachedTarget(cursor, target)) {
    --queued_frames;
    ++discarded_frames;
    cursor = crimson::playback::advanceDecoderSeekCursor(
        cursor, static_cast<int64_t>(cursor));
    queued_frames = 1;
  }

  CHECK(cursor == target);
  CHECK(discarded_frames == 40);
  CHECK(queued_frames == 1);
  const auto bookkeeping =
      crimson::playback::finalizeDecoderSeekBookkeeping(cursor);
  CHECK(bookkeeping.settled_local_frame == target);
  CHECK(bookkeeping.next_local_frame == static_cast<int>(target));

  const int first_published_local = bookkeeping.next_local_frame;
  CHECK(crimson::playback::publishDecoderFrameNumber(
            static_cast<uint64_t>(first_published_local), nullptr) == 100);
  const int subsequent_fallback_local = first_published_local + 1;
  CHECK(crimson::playback::publishDecoderFrameNumber(
            static_cast<uint64_t>(subsequent_fallback_local), nullptr) == 101);
  return true;
}

bool testTargetZeroPreservesFirstQueuedFrame() {
  const auto zero = crimson::playback::finalizeDecoderSeekBookkeeping(0);
  CHECK(zero.settled_local_frame == 0);
  CHECK(zero.next_local_frame == 0);

  CHECK(crimson::playback::queuedDecoderFrameReachedTarget(0, 0));
  return true;
}

bool testExternalMapFallbacks() {
  const std::vector<int64_t> frame_map = {54000, -1};
  CHECK(crimson::playback::publishDecoderFrameNumber(0, &frame_map) == 54000);
  CHECK(crimson::playback::publishDecoderFrameNumber(1, &frame_map) == 1);
  CHECK(crimson::playback::publishDecoderFrameNumber(2, &frame_map) == 2);
  CHECK(crimson::playback::publishDecoderFrameNumber(10, nullptr) == 10);
  return true;
}

bool testExactMismatchRejectedButApproximatePreserved() {
  using crimson::playback::cameraSeekSettlementMatches;
  CHECK(cameraSeekSettlementMatches(true, 54010, 54010));
  CHECK(!cameraSeekSettlementMatches(true, 54010, 54000));
  CHECK(!cameraSeekSettlementMatches(true, 10, 9));
  CHECK(!cameraSeekSettlementMatches(true, 100, 99));
  CHECK(!cameraSeekSettlementMatches(true, 100, 101));
  CHECK(cameraSeekSettlementMatches(false, 54010, 54000));
  CHECK(cameraSeekSettlementMatches(false, 100, 99));
  CHECK(cameraSeekSettlementMatches(false, 100, 101));
  CHECK(crimson::playback::shouldAttemptSeekFramePresentation(false, true));
  CHECK(!crimson::playback::shouldAttemptSeekFramePresentation(true, true));
  CHECK(!crimson::playback::shouldAttemptSeekFramePresentation(false, false));
  return true;
}

} // namespace

int main() {
  if (!testSuccessfulForwardScanUsesTargetIdentity() ||
      !testOrdinaryNonresidentSeekFrom60To100() ||
      !testTargetZeroPreservesFirstQueuedFrame() ||
      !testExternalMapFallbacks() ||
      !testExactMismatchRejectedButApproximatePreserved()) {
    return 1;
  }
  std::cout << "decoder seek bookkeeping tests passed\n";
  return 0;
}
