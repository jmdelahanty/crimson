#include "playback_clock.h"
#include "playback_presentation_lifecycle.h"

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

using crimson::playback::PlaybackPresentationCommitInput;
using crimson::playback::PlaybackPresentationTargetInput;
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

BufferedFrameCandidate candidate(int slot, int frame) {
  BufferedFrameCandidate result;
  result.slot_index = slot;
  result.metadata.frame_number = frame;
  return result;
}

bool testPresentationTargetUsesDecodeBoundAndReadableSlots() {
  PlaybackPresentationTargetInput input;
  input.decoding_active = true;
  input.playing = true;
  input.buffer_size = 4;
  input.previous_committed_frame = 100;
  input.preferred_slot = 2;
  input.requested_frame = 120;
  input.minimum_decoded_frame = 115;
  input.buffered_frames = {candidate(0, 101), candidate(2, 115),
                           candidate(3, 114)};

  const auto target = crimson::playback::planPlaybackPresentationTarget(input);
  CHECK(target.active);
  CHECK(target.requested_frame == 120);
  CHECK(target.bounded_target_frame == 115);
  CHECK(target.minimum_decoded_frame == 115);
  CHECK(target.frame == 115);
  CHECK(target.slot == 2);
  CHECK(!target.clamped_to_buffer);
  return true;
}

bool testPresentationTargetFallsBackOrHoldsCommittedFrame() {
  PlaybackPresentationTargetInput input;
  input.decoding_active = true;
  input.playing = true;
  input.buffer_size = 4;
  input.previous_committed_frame = 100;
  input.requested_frame = 120;
  input.minimum_decoded_frame = 118;
  input.buffered_frames = {candidate(0, 99), candidate(1, 105),
                           candidate(2, 111), candidate(3, 119)};

  auto target = crimson::playback::planPlaybackPresentationTarget(input);
  CHECK(target.active);
  CHECK(target.frame == 111);
  CHECK(target.bounded_target_frame == 118);
  CHECK(target.slot == 2);
  CHECK(target.clamped_to_buffer);

  input.minimum_decoded_frame.reset();
  target = crimson::playback::planPlaybackPresentationTarget(input);
  CHECK(target.frame == 100);
  CHECK(target.bounded_target_frame == 100);
  CHECK(target.slot == -1);
  CHECK(!target.clamped_to_buffer);

  input.just_seeked = true;
  target = crimson::playback::planPlaybackPresentationTarget(input);
  CHECK(!target.active);
  CHECK(target.frame == 100);
  return true;
}

bool testPresentationCommitRequiresCurrentSlotAndDefersRelease() {
  PlaybackPresentationCommitInput input;
  input.decoding_active = true;
  input.playing = true;
  input.buffer_size = 4;
  input.previous_committed_frame = 100;
  input.presenter_target_frame = 110;
  input.presented_frame = 110;
  input.presented_slot = 1;

  auto commit = crimson::playback::planPlaybackPresentationCommit(input);
  CHECK(commit.eligible);
  CHECK(commit.presented_from_slot);
  CHECK(commit.committed);
  CHECK(commit.frame == 110);
  CHECK(commit.slot == 1);
  CHECK(!commit.release_deferred);

  input.presented_slot = -1;
  commit = crimson::playback::planPlaybackPresentationCommit(input);
  CHECK(commit.eligible);
  CHECK(!commit.presented_from_slot);
  CHECK(!commit.committed);
  CHECK(commit.release_deferred);

  input.presented_slot = 1;
  input.presented_frame = 99;
  commit = crimson::playback::planPlaybackPresentationCommit(input);
  CHECK(!commit.committed);
  CHECK(commit.release_deferred);

  input.playing = false;
  commit = crimson::playback::planPlaybackPresentationCommit(input);
  CHECK(!commit.eligible);
  CHECK(!commit.committed);
  CHECK(!commit.release_deferred);
  return true;
}

bool testPortablePresentationAdapterContract() {
  using namespace crimson::playback;
  PlaybackPresentationAdapterPlanInput plan_input;
  plan_input.decoding_active = true;
  plan_input.playing = true;
  plan_input.buffer_size = 4;
  plan_input.previous_committed_frame = 10000000000LL;
  plan_input.preferred_slot = 2;
  plan_input.requested_frame = 10000000020LL;
  plan_input.minimum_decoded_frame = 10000000018LL;
  plan_input.buffered_frames = {
      {10000000001LL, 0}, {10000000018LL, std::nullopt}, {10000000015LL, 1}};

  auto plan = planPlaybackPresentationAdapter(plan_input);
  CHECK(plan.active);
  CHECK(!plan.discontinuity);
  CHECK(plan.frame == 10000000018LL);
  CHECK(!plan.slot.has_value());
  CHECK(plan.selection.mode == PlaybackPresentationSelectionMode::Exact);
  CHECK(plan.selection.target_frame == 10000000018LL);
  CHECK(plan.minimum_decoded_frame == 10000000018LL);
  CHECK(plan.selection.preferred_slot == 2);

  // Multiple exact NVIDIA candidates still choose the preferred slot without
  // requiring the backend to select the same frame a second time.
  plan_input.buffered_frames = {{10000000018LL, 1}, {10000000018LL, 2}};
  plan = planPlaybackPresentationAdapter(plan_input);
  CHECK(plan.frame == 10000000018LL);
  CHECK(plan.slot == 2);
  CHECK(plan.selection.mode == PlaybackPresentationSelectionMode::Exact);

  plan_input.buffered_frames = {{10000000001LL, 0},
                                {10000000012LL, 3},
                                {10000000015LL, 1},
                                {10000000021LL, 2}};
  plan = planPlaybackPresentationAdapter(plan_input);
  CHECK(plan.frame == 10000000015LL);
  CHECK(plan.slot == 1);
  CHECK(plan.clamped_to_buffer);
  CHECK(plan.selection.mode ==
        PlaybackPresentationSelectionMode::LatestAtOrBefore);
  CHECK(plan.selection.minimum_frame_exclusive == 10000000000LL);

  plan_input.buffered_frames = {{9999999999LL, 0}, {10000000021LL, 2}};
  plan = planPlaybackPresentationAdapter(plan_input);
  CHECK(plan.frame == 10000000000LL);
  CHECK(!plan.slot.has_value());
  CHECK(plan.selection.mode ==
        PlaybackPresentationSelectionMode::HoldCommittedFrame);

  plan_input.playing = false;
  plan = planPlaybackPresentationAdapter(plan_input);
  CHECK(!plan.active);
  plan_input.playing = true;
  plan_input.just_seeked = true;
  plan = planPlaybackPresentationAdapter(plan_input);
  CHECK(!plan.active);
  CHECK(plan.discontinuity);

  PlaybackPresentationAdapterCommitInput commit_input;
  commit_input.decoding_active = true;
  commit_input.playing = true;
  commit_input.buffer_size = 4;
  commit_input.previous_committed_frame = 10000000000LL;
  commit_input.presenter_target_frame = 10000000010LL;
  commit_input.observation.presented_frame = 10000000010LL;

  auto commit = planPlaybackPresentationAdapterCommit(commit_input);
  CHECK(commit.eligible);
  CHECK(!commit.observed_slot);
  CHECK(commit.committed);
  CHECK(commit.frame == 10000000010LL);
  CHECK(commit.slot == -1);
  CHECK(commit.release_policy == PlaybackPresentationReleasePolicy::None);
  CHECK(commit.release_before_frame == -1);

  commit_input.release_history_explicitly = true;
  commit_input.observation.presented_frame = 10000000011LL;
  commit_input.observation.slot = 3;
  commit = planPlaybackPresentationAdapterCommit(commit_input);
  CHECK(commit.committed);
  CHECK(commit.observed_slot);
  CHECK(commit.slot == 3);
  CHECK(commit.release_policy ==
        PlaybackPresentationReleasePolicy::ReleaseHistoryBeforeCommittedFrame);
  CHECK(commit.release_before_frame == 10000000011LL);

  commit_input.observation.presented_frame = 9999999999LL;
  commit_input.observation.slot.reset();
  commit = planPlaybackPresentationAdapterCommit(commit_input);
  CHECK(!commit.committed);
  CHECK(commit.release_policy ==
        PlaybackPresentationReleasePolicy::DeferUntilPresentation);

  commit_input.release_history_explicitly = false;
  commit = planPlaybackPresentationAdapterCommit(commit_input);
  CHECK(!commit.committed);
  CHECK(commit.release_policy == PlaybackPresentationReleasePolicy::None);

  commit_input.just_seeked = true;
  commit_input.observation.presented_frame = 10000000010LL;
  commit = planPlaybackPresentationAdapterCommit(commit_input);
  CHECK(!commit.eligible);
  CHECK(commit.discontinuity);
  CHECK(commit.release_policy == PlaybackPresentationReleasePolicy::None);
  return true;
}

bool testHistoryReleaseSelectionAcrossCameras() {
  using crimson::playback::PlaybackHistoryReleaseCandidate;
  const std::vector<PlaybackHistoryReleaseCandidate> candidates{
      {0, 0, 99},  {0, 1, 100}, {0, 2, 101}, {1, 0, 98},
      {1, 1, 100}, {1, 2, 102}, {2, 0, -1},  {-1, 1, 97}};
  const auto release =
      crimson::playback::selectPlaybackHistoryReleaseCandidates(candidates,
                                                                100);
  CHECK(release.size() == 2);
  CHECK(release[0].camera_index == 0);
  CHECK(release[0].slot_index == 0);
  CHECK(release[0].frame_number == 99);
  CHECK(release[1].camera_index == 1);
  CHECK(release[1].slot_index == 0);
  CHECK(release[1].frame_number == 98);
  return true;
}

} // namespace

int main() {
  if (!testCommandsAndClamping() ||
      !testReadinessGatePausesAndRejectsCommands() ||
      !testRateContinuityAndEndOfStream() ||
      !testTimelineUpdatePreservesPositionAndState() ||
      !testInvalidConfigurationAndRate() || !testSeekPlanningAcrossAdapters() ||
      !testSeekGenerationAndTelemetry() ||
      !testPresentationTargetUsesDecodeBoundAndReadableSlots() ||
      !testPresentationTargetFallsBackOrHoldsCommittedFrame() ||
      !testPresentationCommitRequiresCurrentSlotAndDefersRelease() ||
      !testPortablePresentationAdapterContract() ||
      !testHistoryReleaseSelectionAcrossCameras()) {
    return EXIT_FAILURE;
  }
  std::cout << "playback_transport_tests: PASS\n";
  return EXIT_SUCCESS;
}
