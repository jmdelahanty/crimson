#include "playback_buffer_browser.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void testSortingSpansAndExactSelection() {
  const auto model = crimson::playback::buildPlaybackBufferBrowserModel(
      {{20, 7}, {11, 3}, {10, 2}, {12, 4}}, 11);
  require(model.items.size() == 4, "all valid candidates should remain");
  require(model.items.front().frame_number == 10 &&
              model.items.back().frame_number == 20,
          "items should be sorted by frame");
  require(model.spans.size() == 2, "a gap should create a second span");
  require(model.spans[0].begin_index == 0 && model.spans[0].end_index == 2,
          "the first contiguous span should cover frames 10 through 12");
  require(model.largest_gap == 7, "the missing-frame count should be exact");
  require(model.selected_index && *model.selected_index == 1,
          "an exact frame should be selected");
  require(model.preferred_slot && *model.preferred_slot == 3,
          "an exact slotted frame should become the preferred slot");
}

void testNearestSelectionPrefersLaterFrameOnTie() {
  const auto model = crimson::playback::buildPlaybackBufferBrowserModel(
      {{12, 4}, {20, 7}}, 16);
  require(model.selected_index &&
              model.items[*model.selected_index].frame_number == 20,
          "nearest-frame ties should prefer the later frame");
  require(!model.preferred_slot,
          "a nearest highlight must not masquerade as an exact slot");
}

void testDuplicatesSlotlessCandidatesAndInvalidFrames() {
  const auto model = crimson::playback::buildPlaybackBufferBrowserModel(
      {{-1, 9}, {5, std::nullopt}, {5, 3}, {6, std::nullopt}}, 5);
  require(model.items.size() == 3, "negative frames should be discarded");
  require(model.spans.size() == 1,
          "duplicate and adjacent frames should share one span");
  require(model.selected_index && model.items[*model.selected_index].slot &&
              *model.items[*model.selected_index].slot == 3,
          "an exact concrete slot should be preferred over a slotless copy");
  require(model.preferred_slot && *model.preferred_slot == 3,
          "the concrete exact slot should be retained");
}

void testSlotlessExactSelectionAndEmptyBuffer() {
  const auto slotless = crimson::playback::buildPlaybackBufferBrowserModel(
      {{3'500'000'000LL, std::nullopt}}, 3'500'000'000LL);
  require(slotless.selected_index && *slotless.selected_index == 0,
          "64-bit slotless frames should be selectable");
  require(!slotless.preferred_slot,
          "slotless backends should not invent a ring slot");

  const auto empty = crimson::playback::buildPlaybackBufferBrowserModel({}, 42);
  require(empty.items.empty() && empty.spans.empty() && !empty.selected_index &&
              !empty.preferred_slot,
          "an empty buffer should produce an empty model");
  require(empty.oldest_frame == -1 && empty.newest_frame == -1,
          "empty buffer bounds should remain unavailable");
}

} // namespace

int main() {
  testSortingSpansAndExactSelection();
  testNearestSelectionPrefersLaterFrameOnTie();
  testDuplicatesSlotlessCandidatesAndInvalidFrames();
  testSlotlessExactSelectionAndEmptyBuffer();
  std::cout << "playback_buffer_browser_tests: PASS\n";
  return 0;
}
