#include "playback_clock.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>

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

bool testSeekPlanningAcrossAdapters() {
  using namespace crimson::playback;
  const auto preview_request =
      makePlaybackSeekRequest(PlaybackSeekPhase::Preview,
                              PlaybackSeekOrigin::CameraControls, 450, 1000);
  CHECK(preview_request.has_value());

  PlaybackSeekCoordinator coordinator;
  const auto preview = coordinator.begin(*preview_request);
  const auto apple_plan = planPlaybackSeek(
      preview, PlaybackSeekAdapterCapabilities{true, false, true, true});
  CHECK(apple_plan.valid);
  CHECK(apple_plan.mode == PlaybackSeekExecutionMode::LogicalCursorOnly);
  CHECK(apple_plan.accuracy == PlaybackSeekAccuracy::ApproximateAllowed);
  CHECK(!apple_plan.prefer_resident_frame);

  const auto nvidia_plan = planPlaybackSeek(
      preview, PlaybackSeekAdapterCapabilities{false, true, true, true});
  CHECK(nvidia_plan.valid);
  CHECK(nvidia_plan.mode == PlaybackSeekExecutionMode::BackendSeek);
  CHECK(nvidia_plan.accuracy == PlaybackSeekAccuracy::ApproximateAllowed);

  const auto commit_request =
      makePlaybackSeekRequest(PlaybackSeekPhase::Commit,
                              PlaybackSeekOrigin::CameraControls, 1200, 1000);
  CHECK(commit_request.has_value());
  CHECK(commit_request->target_frame == 999);
  const auto commit = coordinator.begin(*commit_request);
  const auto commit_plan = planPlaybackSeek(
      commit, PlaybackSeekAdapterCapabilities{true, false, true, true});
  CHECK(commit_plan.valid);
  CHECK(commit_plan.mode == PlaybackSeekExecutionMode::BackendSeek);
  CHECK(commit_plan.accuracy == PlaybackSeekAccuracy::Exact);
  CHECK(commit_plan.prefer_resident_frame);

  const auto unsupported = planPlaybackSeek(
      commit, PlaybackSeekAdapterCapabilities{true, false, false, true});
  CHECK(!unsupported.valid);
  CHECK(unsupported.rejection == PlaybackSeekRejection::UnsupportedAccuracy);
  CHECK(!makePlaybackSeekRequest(PlaybackSeekPhase::Discrete,
                                 PlaybackSeekOrigin::Programmatic, 0, 0)
             .has_value());
  return true;
}

bool testSeekGenerationAndTelemetry() {
  using namespace crimson::playback;
  PlaybackSeekCoordinator coordinator;
  const auto first_request = *makePlaybackSeekRequest(
      PlaybackSeekPhase::Preview, PlaybackSeekOrigin::CameraControls, 10, 100);
  const auto first = coordinator.begin(first_request);
  const auto first_plan = planPlaybackSeek(
      first, PlaybackSeekAdapterCapabilities{false, true, true, true});
  PlaybackSeekExecutionResult submitted;
  submitted.status = PlaybackSeekExecutionStatus::Submitted;
  submitted.path = PlaybackSeekExecutionPath::BackendDecoder;
  submitted.resolved_frame = 10;
  auto event = coordinator.record(first, first_plan, submitted);
  CHECK(event.result.status == PlaybackSeekExecutionStatus::Submitted);
  CHECK(coordinator.isCurrent(first.generation));

  const auto second_request = *makePlaybackSeekRequest(
      PlaybackSeekPhase::Commit, PlaybackSeekOrigin::CameraControls, 20, 100);
  const auto second = coordinator.begin(second_request);
  CHECK(second.generation > first.generation);
  PlaybackSeekExecutionResult late_completion;
  late_completion.status = PlaybackSeekExecutionStatus::Completed;
  late_completion.path = PlaybackSeekExecutionPath::BackendDecoder;
  late_completion.resolved_frame = 10;
  event = coordinator.record(first, first_plan, late_completion);
  CHECK(event.result.status == PlaybackSeekExecutionStatus::DiscardedStale);
  CHECK(coordinator.isCurrent(second.generation));

  const auto second_plan = planPlaybackSeek(
      second, PlaybackSeekAdapterCapabilities{true, false, true, true});
  PlaybackSeekExecutionResult resident;
  resident.status = PlaybackSeekExecutionStatus::Completed;
  resident.path = PlaybackSeekExecutionPath::ResidentBuffer;
  resident.resolved_frame = 20;
  event = coordinator.record(second, second_plan, resident);
  CHECK(event.result.status == PlaybackSeekExecutionStatus::Completed);
  CHECK(coordinator.activeGeneration() == 0);

  const auto metrics = coordinator.metrics();
  CHECK(metrics.requests == 2);
  CHECK(metrics.previews == 1);
  CHECK(metrics.commits == 1);
  CHECK(metrics.superseded == 1);
  CHECK(metrics.backend_submissions == 1);
  CHECK(metrics.resident_buffer_completions == 1);
  CHECK(metrics.discarded_stale == 1);

  std::ostringstream diagnostics;
  writePlaybackSeekDiagnostics(diagnostics, "Test", metrics);
  CHECK(diagnostics.str().find("[TestTransportSeek] requests=2") !=
        std::string::npos);
  CHECK(playbackSeekExecutionStatusName(
            PlaybackSeekExecutionStatus::DiscardedStale) == "discarded_stale");

  const auto third = coordinator.begin(first_request);
  const auto third_plan = planPlaybackSeek(
      third, PlaybackSeekAdapterCapabilities{false, true, true, true});
  event = coordinator.record(third, third_plan, submitted);
  CHECK(event.result.status == PlaybackSeekExecutionStatus::Submitted);
  const auto cancelled = coordinator.cancelActive();
  CHECK(cancelled.has_value());
  CHECK(cancelled->result.status == PlaybackSeekExecutionStatus::Cancelled);
  CHECK(coordinator.activeGeneration() == 0);
  CHECK(coordinator.metrics().cancelled == 1);
  return true;
}

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
      !testInvalidConfigurationAndRate() || !testSeekPlanningAcrossAdapters() ||
      !testSeekGenerationAndTelemetry()) {
    return EXIT_FAILURE;
  }
  std::cout << "playback_transport_tests: PASS\n";
  return EXIT_SUCCESS;
}
