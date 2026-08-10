#include "ui_reference_capture.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

using crimson::ui_reference::CaptureCoordinator;
using crimson::ui_reference::CapturePhase;

bool testStableFrameGateAndOneShotPublication() {
  CaptureCoordinator coordinator({3, 10.0});
  const auto start = CaptureCoordinator::TimePoint{};
  CHECK(coordinator.start(start));
  CHECK(coordinator.waitingForStableFrame());
  CHECK(!coordinator.observeFrame(true, true,
                                  start + std::chrono::milliseconds(1)));
  CHECK(coordinator.stableFrameCount() == 1);
  CHECK(!coordinator.observeFrame(true, false,
                                  start + std::chrono::milliseconds(2)));
  CHECK(coordinator.stableFrameCount() == 0);
  CHECK(!coordinator.observeFrame(true, true,
                                  start + std::chrono::milliseconds(3)));
  CHECK(!coordinator.observeFrame(true, true,
                                  start + std::chrono::milliseconds(4)));
  CHECK(coordinator.observeFrame(true, true,
                                 start + std::chrono::milliseconds(5)));
  CHECK(coordinator.captureRequested());
  CHECK(!coordinator.observeFrame(true, true,
                                  start + std::chrono::milliseconds(6)));
  CHECK(coordinator.stableFrameCount() == 3);
  CHECK(coordinator.markCaptureComplete());
  CHECK(coordinator.readyToPublish());
  CHECK(coordinator.markPublished());
  CHECK(coordinator.published());
  CHECK(coordinator.terminal());
  CHECK(!coordinator.markPublished());
  return true;
}

bool testExactPresentationIsRequired() {
  CaptureCoordinator coordinator({2, 10.0});
  const auto start = CaptureCoordinator::Clock::now();
  CHECK(coordinator.start(start));
  CHECK(!coordinator.observeFrame(false, true,
                                  start + std::chrono::milliseconds(1)));
  CHECK(coordinator.stableFrameCount() == 0);
  CHECK(!coordinator.observeFrame(true, true,
                                  start + std::chrono::milliseconds(2)));
  CHECK(coordinator.observeFrame(true, true,
                                 start + std::chrono::milliseconds(3)));
  return true;
}

bool testTimeoutAndFailureAreTerminal() {
  const auto start = CaptureCoordinator::Clock::now();
  CaptureCoordinator timeout({60, 2.0});
  CHECK(timeout.start(start));
  CHECK(!timeout.pollTimeout(start + std::chrono::seconds(2)));
  CHECK(timeout.pollTimeout(start + std::chrono::milliseconds(2001)));
  CHECK(timeout.phase() == CapturePhase::TimedOut);
  CHECK(timeout.snapshot(start + std::chrono::seconds(3)).failure_reason ==
        "UI-reference capture timed out");
  timeout.fail("replacement");
  CHECK(timeout.phase() == CapturePhase::TimedOut);

  CaptureCoordinator capture_pending({1, 2.0});
  CHECK(capture_pending.start(start));
  CHECK(capture_pending.observeFrame(true, true, start));
  CHECK(capture_pending.pollTimeout(start + std::chrono::milliseconds(2001)));
  CHECK(capture_pending.phase() == CapturePhase::TimedOut);

  CaptureCoordinator failed({1, 2.0});
  CHECK(failed.start(start));
  CHECK(failed.observeFrame(true, true, start));
  failed.fail("capture failed");
  CHECK(failed.phase() == CapturePhase::Failed);
  CHECK(failed.snapshot(start).failure_reason == "capture failed");
  CHECK(!failed.markCaptureComplete());
  return true;
}

bool testInvalidPolicyFailsClosed() {
  CaptureCoordinator bad_frames({0, 1.0});
  CHECK(!bad_frames.start());
  CHECK(bad_frames.phase() == CapturePhase::Failed);

  CaptureCoordinator bad_timeout({1, 0.0});
  CHECK(!bad_timeout.start());
  CHECK(bad_timeout.phase() == CapturePhase::Failed);
  return true;
}

bool testAtomicMarkerPublicationAndCleanup() {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("crimson_ui_reference_capture_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto marker = root / "reference.json";
  const auto image = crimson::ui_reference::uiReferenceImagePath(marker);
  std::filesystem::create_directories(root);
  {
    std::ofstream(marker) << "stale";
    std::ofstream(image) << "stale-image";
    std::ofstream(marker.string() + ".tmp") << "stale-temporary";
  }
  std::string error;
  CHECK(crimson::ui_reference::prepareUiReferenceOutput(marker, &error));
  CHECK(!std::filesystem::exists(marker));
  CHECK(!std::filesystem::exists(image));
  CHECK(!std::filesystem::exists(marker.string() + ".tmp"));

  const nlohmann::json expected = {{"format", "crimson_ui_reference_v1"},
                                   {"stable_frames", 60}};
  CHECK(crimson::ui_reference::writeUiReferenceMarkerAtomically(
      marker, expected, &error));
  CHECK(std::filesystem::is_regular_file(marker));
  CHECK(!std::filesystem::exists(marker.string() + ".tmp"));
  std::ifstream stream(marker);
  const auto actual = nlohmann::json::parse(stream);
  CHECK(actual == expected);

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return true;
}

} // namespace

int main() {
  if (!testStableFrameGateAndOneShotPublication() ||
      !testExactPresentationIsRequired() ||
      !testTimeoutAndFailureAreTerminal() || !testInvalidPolicyFailsClosed() ||
      !testAtomicMarkerPublicationAndCleanup()) {
    return EXIT_FAILURE;
  }
  std::cout << "ui_reference_capture_tests: PASS\n";
  return EXIT_SUCCESS;
}
