#include "platform/nvidia/nvidia_clipped_media_coordinator.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using crimson::platform::nvidia::ClippedMediaCoordinator;
using crimson::platform::nvidia::ClippedMediaCoordinatorContext;
using crimson::platform::nvidia::ClippedMediaCoordinatorEvent;
using crimson::platform::nvidia::ClippedMediaCoordinatorEventKind;
using crimson::playback::ClippedFrameBinding;
using crimson::playback::ClippedMediaHandoffOutcome;
using crimson::playback::ClippedMediaHandoffState;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

ClippedFrameBinding bindingForFrame(int64_t parent_frame) {
  if (parent_frame < 0 || parent_frame >= 5) {
    return {};
  }
  if (parent_frame <= 2) {
    return {true, 0, "clip-a", parent_frame, 0, 2};
  }
  return {true, 1, "clip-b", parent_frame - 3, 3, 4};
}

ClippedMediaHandoffState initialState() {
  return {0, "clip-a", 0, 2, -1, false, -1};
}

void testCurrentClipProgressDoesNotSeek() {
  auto state = initialState();
  int seek_count = 0;
  std::vector<ClippedMediaCoordinatorEvent> events;
  const ClippedMediaCoordinator coordinator(ClippedMediaCoordinatorContext{
      &state, []() { return 5; }, bindingForFrame,
      [&](int64_t) {
        ++seek_count;
        return true;
      },
      [&](const auto &event) { events.push_back(event); }});

  const auto outcome = coordinator.onPresentedFrame(1, true);
  require(outcome == ClippedMediaHandoffOutcome::Ignored,
          "an interior frame should not request a handoff");
  require(seek_count == 0, "an interior frame must not seek");
  require(events.empty(), "an interior frame must not publish handoff events");
  require(state.last_presented_parent_frame == 1,
          "the last settled frame should advance");
}

void testBoundaryLoadsOnceAndSettlesOnPresentation() {
  auto state = initialState();
  int seek_count = 0;
  int64_t seek_target = -1;
  std::vector<ClippedMediaCoordinatorEvent> events;
  const ClippedMediaCoordinator coordinator(ClippedMediaCoordinatorContext{
      &state, []() { return 5; }, bindingForFrame,
      [&](int64_t parent_frame) {
        ++seek_count;
        seek_target = parent_frame;
        return true;
      },
      [&](const auto &event) { events.push_back(event); }});

  require(coordinator.onPresentedFrame(2, true) ==
              ClippedMediaHandoffOutcome::SwitchLoaded,
          "the last frame in clip A should load clip B");
  require(seek_count == 1 && seek_target == 3,
          "the coordinator should seek the first frame of clip B once");
  require(events.size() == 2,
          "a successful load should publish request and loaded events");
  require(events[0].kind == ClippedMediaCoordinatorEventKind::SwitchRequested &&
              events[1].kind == ClippedMediaCoordinatorEventKind::SwitchLoaded,
          "handoff event order should be request then loaded");
  require(state.selected_run_index == 1 && state.clip_id == "clip-b",
          "the loaded clip identity should be installed");
  require(state.switch_in_progress && state.pending_switch_parent_frame == 3,
          "the switch should remain pending until clip B presents");

  require(coordinator.onPresentedFrame(2, true) ==
              ClippedMediaHandoffOutcome::Ignored,
          "a repeated old-clip frame should not submit duplicate work");
  require(seek_count == 1, "a pending switch must suppress duplicate seeks");

  require(coordinator.onPresentedFrame(3, true) ==
              ClippedMediaHandoffOutcome::SwitchSettled,
          "presentation of clip B should settle the handoff");
  require(!state.switch_in_progress && state.pending_switch_parent_frame == -1,
          "a settled switch should clear pending state");
  require(state.last_presented_parent_frame == 3,
          "the first presented frame in clip B should become settled");
  require(events.size() == 3 &&
              events.back().kind ==
                  ClippedMediaCoordinatorEventKind::SwitchPresented,
          "settlement should publish one presented event");
  require(events.back().pending_switch_parent_frame == 3,
          "the presented event should retain the completed pending target");
}

void testFailedLoadClearsPendingState() {
  auto state = initialState();
  std::vector<ClippedMediaCoordinatorEvent> events;
  const ClippedMediaCoordinator coordinator(ClippedMediaCoordinatorContext{
      &state, []() { return 5; }, bindingForFrame,
      [](int64_t) { return false; },
      [&](const auto &event) { events.push_back(event); }});

  require(coordinator.onPresentedFrame(2, true) ==
              ClippedMediaHandoffOutcome::SwitchFailed,
          "a failed seek should fail the handoff");
  require(!state.switch_in_progress && state.pending_switch_parent_frame == -1,
          "a failed handoff should clear its pending request");
  require(state.selected_run_index == 0 && state.clip_id == "clip-a",
          "a failed handoff should retain the previous clip identity");
  require(events.size() == 2 &&
              events.back().kind ==
                  ClippedMediaCoordinatorEventKind::SwitchFailed,
          "a failed load should publish request and failure events");
}

void testInactiveAndInvalidInputsDoNoWork() {
  auto state = initialState();
  int seek_count = 0;
  const ClippedMediaCoordinator coordinator(
      ClippedMediaCoordinatorContext{&state,
                                     []() { return 5; },
                                     bindingForFrame,
                                     [&](int64_t) {
                                       ++seek_count;
                                       return true;
                                     },
                                     {}});

  require(coordinator.onPresentedFrame(2, false) ==
              ClippedMediaHandoffOutcome::Ignored,
          "paused playback should not initiate a handoff");
  require(coordinator.onPresentedFrame(-1, true) ==
              ClippedMediaHandoffOutcome::InvalidInput,
          "a negative presented frame should fail closed");
  require(seek_count == 0, "inactive and invalid calls must not seek");

  state.clip_id.clear();
  require(coordinator.onPresentedFrame(2, true) ==
              ClippedMediaHandoffOutcome::InvalidInput,
          "an invalid active state should fail closed");
  require(seek_count == 0, "invalid state must not seek");
}

void testUnmappedNextFrameFailsClosed() {
  auto state = initialState();
  int seek_count = 0;
  const ClippedMediaCoordinator coordinator(ClippedMediaCoordinatorContext{
      &state,
      []() { return 5; },
      [](int64_t parent_frame) {
        return parent_frame == 3 ? ClippedFrameBinding{}
                                 : bindingForFrame(parent_frame);
      },
      [&](int64_t) {
        ++seek_count;
        return true;
      },
      {}});

  require(coordinator.onPresentedFrame(2, true) ==
              ClippedMediaHandoffOutcome::InvalidInput,
          "an unmapped next frame should fail closed");
  require(seek_count == 0, "an unmapped next frame must not seek");
  require(!state.switch_in_progress,
          "an invalid boundary must not leave a pending switch");
}

} // namespace

int main() {
  testCurrentClipProgressDoesNotSeek();
  testBoundaryLoadsOnceAndSettlesOnPresentation();
  testFailedLoadClearsPendingState();
  testInactiveAndInvalidInputsDoNoWork();
  testUnmappedNextFrameFailsClosed();
  std::cout << "nvidia_clipped_media_coordinator_tests: PASS\n";
  return 0;
}
