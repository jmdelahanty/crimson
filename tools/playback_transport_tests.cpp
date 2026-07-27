#include "playback_clock.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

using crimson::playback::PlaybackTransportCommand;
using crimson::playback::PlaybackTransportController;
using crimson::playback::PlaybackTransportRejection;

bool testCommandsAndClamping() {
  using namespace std::chrono_literals;
  const PlaybackTransportController::TimePoint start{};
  PlaybackTransportController transport;
  transport.configure(100.0, 1000, start, 20);

  CHECK(transport.configured());
  CHECK(!transport.isPlaying());
  CHECK(transport.requestedFrame(start) == 20);

  auto transition = transport.apply(PlaybackTransportCommand::play(), start);
  CHECK(transition.accepted);
  CHECK(transition.state_changed);
  CHECK(transport.requestedFrame(start + 250ms) == 45);

  transition =
      transport.apply(PlaybackTransportCommand::pause(), start + 250ms);
  CHECK(transition.accepted);
  CHECK(!transition.playing);
  CHECK(transport.requestedFrame(start + 5s) == 45);

  transition =
      transport.apply(PlaybackTransportCommand::step(-100), start + 5s);
  CHECK(transition.accepted);
  CHECK(transition.seek_requested);
  CHECK(transition.target_frame == 0);

  transition =
      transport.apply(PlaybackTransportCommand::seek(5000), start + 5s);
  CHECK(transition.accepted);
  CHECK(transition.target_frame == 999);

  transition = transport.apply(PlaybackTransportCommand::toggle(), start + 5s);
  CHECK(transition.accepted);
  CHECK(transition.playing);
  transition = transport.apply(PlaybackTransportCommand::toggle(), start + 5s);
  CHECK(transition.accepted);
  CHECK(!transition.playing);
  return true;
}

bool testReadinessGatePausesAndRejectsCommands() {
  using namespace std::chrono_literals;
  const PlaybackTransportController::TimePoint start{};
  PlaybackTransportController transport;
  transport.configure(30.0, 300, start, 0, false);

  auto transition = transport.apply(PlaybackTransportCommand::play(), start);
  CHECK(!transition.accepted);
  CHECK(transition.rejection == PlaybackTransportRejection::ControlsDisabled);
  CHECK(!transport.isPlaying());

  CHECK(!transport.setControlsEnabled(true, start));
  CHECK(transport.play(start));
  CHECK(transport.requestedFrame(start + 1s) == 30);
  CHECK(transport.setControlsEnabled(false, start + 1s));
  CHECK(!transport.isPlaying());
  CHECK(transport.requestedFrame(start + 2s) == 30);

  transition = transport.apply(PlaybackTransportCommand::seek(90), start + 2s);
  CHECK(!transition.accepted);
  CHECK(transition.rejection == PlaybackTransportRejection::ControlsDisabled);

  // Platform-controlled initialization and recovery may still place the
  // logical cursor while interactive transport commands are gated.
  transport.seek(90, start + 2s);
  CHECK(transport.requestedFrame(start + 2s) == 90);
  return true;
}

bool testRateContinuityAndEndOfStream() {
  using namespace std::chrono_literals;
  const PlaybackTransportController::TimePoint start{};
  PlaybackTransportController transport;
  transport.configure(100.0, 101, start);
  CHECK(transport.play(start));
  CHECK(transport.setPlaybackRate(0.5, start + 500ms));
  CHECK(transport.requestedFrame(start + 500ms) == 50);
  CHECK(transport.requestedFrame(start + 1s) == 75);

  const auto tick = transport.update(start + 2s);
  CHECK(tick.reached_end);
  CHECK(tick.requested_frame == 100);
  CHECK(!tick.playing);
  CHECK(!transport.isPlaying());
  return true;
}

bool testTimelineUpdatePreservesPositionAndState() {
  using namespace std::chrono_literals;
  const PlaybackTransportController::TimePoint start{};
  PlaybackTransportController transport;
  transport.configure(20.0, 1000, start, 100);
  CHECK(transport.play(start));
  transport.updateTimeline(40.0, 150, start + 1s);
  CHECK(transport.isPlaying());
  CHECK(transport.requestedFrame(start + 1s) == 120);
  CHECK(transport.requestedFrame(start + 1500ms) == 140);

  transport.updateTimeline(40.0, 110, start + 1500ms);
  CHECK(transport.requestedFrame(start + 1500ms) == 109);
  return true;
}

bool testInvalidConfigurationAndRate() {
  const PlaybackTransportController::TimePoint start{};
  PlaybackTransportController transport;
  transport.configure(0.0, 0, start);
  auto transition = transport.apply(PlaybackTransportCommand::play(), start);
  CHECK(!transition.accepted);
  CHECK(transition.rejection == PlaybackTransportRejection::NotConfigured);

  transport.configure(30.0, 10, start);
  transition = transport.apply(PlaybackTransportCommand::setRate(0.0), start);
  CHECK(!transition.accepted);
  CHECK(transition.rejection == PlaybackTransportRejection::InvalidRate);
  return true;
}

} // namespace

int main() {
  if (!testCommandsAndClamping() ||
      !testReadinessGatePausesAndRejectsCommands() ||
      !testRateContinuityAndEndOfStream() ||
      !testTimelineUpdatePreservesPositionAndState() ||
      !testInvalidConfigurationAndRate()) {
    return EXIT_FAILURE;
  }
  std::cout << "playback_transport_tests: PASS\n";
  return EXIT_SUCCESS;
}
