#include "clipped_media_handoff.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using crimson::playback::ClippedFrameBinding;
using crimson::playback::ClippedMediaHandoffOutcome;
using crimson::playback::ClippedMediaHandoffState;

[[noreturn]] void fail(const std::string &message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void require(bool value, const std::string &message) {
  if (!value) {
    fail(message);
  }
}

ClippedFrameBinding binding(size_t run, const char *clip, int64_t local,
                            int64_t first, int64_t last) {
  return {true, run, clip, local, first, last};
}

ClippedMediaHandoffState initialState() {
  ClippedMediaHandoffState state;
  state.selected_run_index = 4;
  state.clip_id = "clip-a";
  state.first_parent_frame = 0;
  state.last_parent_frame = 9;
  return state;
}

void testNoBoundary() {
  auto state = initialState();
  const auto result = crimson::playback::updateClippedMediaHandoff(
      state, true, 5, 20, binding(4, "clip-a", 5, 0, 9), {});
  require(result.outcome == ClippedMediaHandoffOutcome::Ignored,
          "inside a clip does not request a handoff");
  require(!state.switch_in_progress && state.last_presented_parent_frame == 5,
          "inside a clip settles current presentation");
}

void testBoundaryRequest() {
  auto state = initialState();
  const auto result = crimson::playback::updateClippedMediaHandoff(
      state, true, 9, 20, binding(4, "clip-a", 9, 0, 9),
      binding(7, "clip-b", 0, 10, 19));
  require(result.outcome == ClippedMediaHandoffOutcome::SwitchRequested,
          "boundary requests a switch");
  require(state.switch_in_progress && state.pending_switch_parent_frame == 10,
          "boundary records its pending parent frame");
  require(result.command.request_load_and_seek &&
              result.command.parent_frame == 10 &&
              result.command.expected_selected_run_index == 7 &&
              result.command.expected_clip_id == "clip-b",
          "boundary emits the target clip command");
}

void testAlreadySwitching() {
  auto state = initialState();
  state.switch_in_progress = true;
  state.pending_switch_parent_frame = 10;
  const auto result = crimson::playback::updateClippedMediaHandoff(
      state, true, 9, 20, binding(4, "clip-a", 9, 0, 9),
      binding(7, "clip-b", 0, 10, 19));
  require(result.outcome == ClippedMediaHandoffOutcome::Ignored,
          "in-flight handoff does not issue a duplicate command");
  require(state.switch_in_progress && state.pending_switch_parent_frame == 10,
          "in-flight handoff remains pending");
}

void testSuccessfulSettlement() {
  auto state = initialState();
  const auto request = crimson::playback::updateClippedMediaHandoff(
      state, true, 9, 20, binding(4, "clip-a", 9, 0, 9),
      binding(7, "clip-b", 0, 10, 19));
  const auto loaded = binding(7, "clip-b", 0, 10, 19);
  const auto complete = crimson::playback::completeClippedMediaHandoffLoad(
      state, request.command, true, loaded);
  require(complete.outcome == ClippedMediaHandoffOutcome::SwitchLoaded &&
              state.switch_in_progress && state.selected_run_index == 7,
          "successful load preserves pending switch until presentation");
  const auto settled = crimson::playback::updateClippedMediaHandoff(
      state, true, 10, 20, loaded, binding(7, "clip-b", 1, 10, 19));
  require(settled.outcome == ClippedMediaHandoffOutcome::SwitchSettled,
          "new clip presentation settles handoff");
  require(!state.switch_in_progress &&
              state.pending_switch_parent_frame == -1 &&
              state.last_presented_parent_frame == 10,
          "settlement clears pending state");
}

void testFailedSwitch() {
  auto state = initialState();
  const auto request = crimson::playback::updateClippedMediaHandoff(
      state, true, 9, 20, binding(4, "clip-a", 9, 0, 9),
      binding(7, "clip-b", 0, 10, 19));
  const auto complete = crimson::playback::completeClippedMediaHandoffLoad(
      state, request.command, false, {});
  require(complete.outcome == ClippedMediaHandoffOutcome::SwitchFailed,
          "failed media load has explicit outcome");
  require(!state.switch_in_progress &&
              state.pending_switch_parent_frame == -1 &&
              state.selected_run_index == 4,
          "failed load retains original clip state");
}

void testInvalidUnmappedNextFrame() {
  auto state = initialState();
  const auto result = crimson::playback::updateClippedMediaHandoff(
      state, true, 9, 20, binding(4, "clip-a", 9, 0, 9), {});
  require(result.outcome == ClippedMediaHandoffOutcome::InvalidInput,
          "unmapped next parent frame fails closed");
  require(!state.switch_in_progress,
          "invalid next mapping does not mutate state");
}

void testReset() {
  auto state = initialState();
  state.switch_in_progress = true;
  state.pending_switch_parent_frame = 10;
  state.last_presented_parent_frame = 9;
  const auto result = crimson::playback::resetClippedMediaHandoff(state);
  require(result.outcome == ClippedMediaHandoffOutcome::Reset,
          "reset has an explicit outcome");
  require(!state.switch_in_progress &&
              state.pending_switch_parent_frame == -1 &&
              state.selected_run_index == 4 &&
              state.last_presented_parent_frame == 9,
          "reset only cancels the pending handoff");
}

} // namespace

int main() {
  testNoBoundary();
  testBoundaryRequest();
  testAlreadySwitching();
  testSuccessfulSettlement();
  testFailedSwitch();
  testInvalidUnmappedNextFrame();
  testReset();
  std::cout << "clipped_media_handoff_tests: PASS\n";
  return 0;
}
