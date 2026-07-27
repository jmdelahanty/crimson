#include "session_lifecycle.h"

#include <cstdlib>
#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition   \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testOpenAndClose() {
  crimson::session::SessionLifecycle lifecycle;
  CHECK(lifecycle.snapshot().phase == crimson::session::SessionPhase::Empty);
  const auto generation = lifecycle.beginOpen(
      {"camera.mp4", "analysis.zarr", "stimulus.mp4"});
  CHECK(generation == 1);
  CHECK(lifecycle.snapshot().phase == crimson::session::SessionPhase::Opening);
  CHECK(lifecycle.completeOpen(generation));
  const auto ready = lifecycle.snapshot();
  CHECK(ready.ready());
  CHECK(ready.active.video_path == "camera.mp4");
  CHECK(lifecycle.beginClose());
  CHECK(lifecycle.snapshot().phase == crimson::session::SessionPhase::Closing);
  lifecycle.completeClose();
  CHECK(lifecycle.snapshot().phase == crimson::session::SessionPhase::Closed);
  CHECK(lifecycle.snapshot().active.empty());
  return true;
}

bool testStaleCompletionAndFailedReplacement() {
  crimson::session::SessionLifecycle lifecycle;
  const auto first = lifecycle.beginOpen({"first.mp4", {}, {}});
  const auto second = lifecycle.beginOpen({"second.mp4", {}, {}});
  CHECK(!lifecycle.completeOpen(first));
  CHECK(lifecycle.completeOpen(second));
  CHECK(lifecycle.snapshot().active.video_path == "second.mp4");

  const auto replacement = lifecycle.beginOpen({"third.mp4", {}, {}});
  CHECK(lifecycle.failOpen(replacement, "network timeout"));
  const auto failed = lifecycle.snapshot();
  CHECK(failed.phase == crimson::session::SessionPhase::Failed);
  CHECK(failed.active.video_path == "second.mp4");
  CHECK(failed.error == "network timeout");
  return true;
}

bool testRelaunchRequestSurvivesClose() {
  crimson::session::SessionLifecycle lifecycle;
  const auto initial = lifecycle.beginOpen({"first.mp4", "one.zarr", {}});
  CHECK(lifecycle.completeOpen(initial));
  crimson::session::SessionReplacementRequest request{
      true, "second.mp4", "two.zarr", "stim.mp4", 8, 4};
  CHECK(lifecycle.requestReplacement(request));
  CHECK(lifecycle.snapshot().phase ==
        crimson::session::SessionPhase::ReplacementPending);
  CHECK(lifecycle.beginClose());
  lifecycle.completeClose();
  const auto retained = lifecycle.replacementRequest();
  CHECK(retained.has_value());
  CHECK(retained->video_path == "second.mp4");
  CHECK(retained->video_buffer_capacity == 8);

  std::string error;
  CHECK(!lifecycle.requestReplacement({}, &error));
  CHECK(!error.empty());
  return true;
}

} // namespace

int main() {
  if (!testOpenAndClose() || !testStaleCompletionAndFailedReplacement() ||
      !testRelaunchRequestSurvivesClose()) {
    return EXIT_FAILURE;
  }
  std::cout << "session_lifecycle_tests: PASS\n";
  return EXIT_SUCCESS;
}
