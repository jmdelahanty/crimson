#include "read_only_overlay_frame_coordinator.h"

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

using crimson::overlay::ReadOnlyOverlayFrameAction;
using crimson::overlay::ReadOnlyOverlayFrameCandidate;
using crimson::overlay::ReadOnlyOverlayFrameCandidateStatus;
using crimson::overlay::ReadOnlyOverlayFrameCoordinator;
using crimson::overlay::ReadOnlyOverlayFrameInput;
using crimson::overlay::ReadOnlyOverlayFramePlanStatus;

ReadOnlyOverlayFrameInput enabledFrame(int64_t frame,
                                       bool discontinuity = false) {
  return {frame, true, true, true, discontinuity};
}

ReadOnlyOverlayFrameCandidate
candidate(ReadOnlyOverlayFrameCandidateStatus status,
          std::optional<int64_t> frame = std::nullopt) {
  return {true, status, frame};
}

bool testInactiveAndUnavailableFramesDoNotRequest() {
  ReadOnlyOverlayFrameCoordinator coordinator;
  auto plan = coordinator.beginFrame({4, false, true, true, false});
  CHECK(plan.status == ReadOnlyOverlayFramePlanStatus::Inactive);
  CHECK(!plan.issue_request);

  plan = coordinator.beginFrame({5, true, false, true, false});
  CHECK(plan.status == ReadOnlyOverlayFramePlanStatus::Inactive);
  CHECK(!plan.issue_request);

  plan = coordinator.beginFrame({6, true, true, false, false});
  CHECK(plan.status == ReadOnlyOverlayFramePlanStatus::Unavailable);
  CHECK(!plan.issue_request);

  plan = coordinator.beginFrame({-1, true, true, true, false});
  CHECK(plan.status == ReadOnlyOverlayFramePlanStatus::Inactive);
  CHECK(!plan.issue_request);

  const auto &metrics = coordinator.metrics();
  CHECK(metrics.frame_updates == 4);
  CHECK(metrics.inactive_frames == 2);
  CHECK(metrics.unavailable_frames == 1);
  CHECK(metrics.invalid_frames == 1);
  CHECK(metrics.request_plans == 0);
  return true;
}

bool testExactPendingMissingAndFailedResults() {
  ReadOnlyOverlayFrameCoordinator coordinator;

  auto plan = coordinator.beginFrame(enabledFrame(10));
  CHECK(plan.issue_request);
  CHECK(!plan.request_discontinuity);
  auto decision = coordinator.finishFrame(
      plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Pending));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::Wait);

  plan = coordinator.beginFrame(enabledFrame(10));
  decision = coordinator.finishFrame(
      plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Mapped, 10));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::Present);
  CHECK(decision.exact);

  plan = coordinator.beginFrame(enabledFrame(11));
  decision = coordinator.finishFrame(
      plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Missing, 11));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::Clear);

  plan = coordinator.beginFrame(enabledFrame(12));
  decision = coordinator.finishFrame(
      plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Failed, 12));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::Failed);

  const auto &metrics = coordinator.metrics();
  CHECK(metrics.accepted_requests == 4);
  CHECK(metrics.pending_frames == 1);
  CHECK(metrics.exact_presentations == 1);
  CHECK(metrics.missing_frames == 1);
  CHECK(metrics.failed_frames == 1);
  CHECK(metrics.last_requested_frame == 12);
  CHECK(metrics.last_presented_frame == 10);
  return true;
}

bool testDiscontinuityRequiresPriorAcceptedRequest() {
  ReadOnlyOverlayFrameCoordinator coordinator;
  auto plan = coordinator.beginFrame(enabledFrame(100, true));
  CHECK(!plan.request_discontinuity);
  auto decision = coordinator.finishFrame(
      plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Pending));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::Wait);

  plan = coordinator.beginFrame(enabledFrame(800, true));
  CHECK(plan.request_discontinuity);
  decision = coordinator.finishFrame(
      plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Mapped, 800));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::Present);
  CHECK(coordinator.metrics().discontinuity_requests == 1);
  return true;
}

bool testRejectedAndStaleResultsNeverPresent() {
  ReadOnlyOverlayFrameCoordinator coordinator;
  auto old_plan = coordinator.beginFrame(enabledFrame(20));
  auto current_plan = coordinator.beginFrame(enabledFrame(21));
  auto decision = coordinator.finishFrame(
      old_plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Mapped, 20));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::IgnoreStale);

  decision = coordinator.finishFrame(
      current_plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Mapped, 20));
  CHECK(decision.action == ReadOnlyOverlayFrameAction::IgnoreStale);

  auto rejected_plan = coordinator.beginFrame(enabledFrame(22));
  decision = coordinator.finishFrame(
      rejected_plan,
      {false, ReadOnlyOverlayFrameCandidateStatus::Pending, std::nullopt});
  CHECK(decision.action == ReadOnlyOverlayFrameAction::RequestRejected);

  auto duplicate_plan = coordinator.beginFrame(enabledFrame(23));
  const auto exact = candidate(ReadOnlyOverlayFrameCandidateStatus::Mapped, 23);
  decision = coordinator.finishFrame(duplicate_plan, exact);
  CHECK(decision.action == ReadOnlyOverlayFrameAction::Present);
  decision = coordinator.finishFrame(duplicate_plan, exact);
  CHECK(decision.action == ReadOnlyOverlayFrameAction::IgnoreStale);

  const auto &metrics = coordinator.metrics();
  CHECK(metrics.superseded_plans == 1);
  CHECK(metrics.mismatched_candidate_frames == 1);
  CHECK(metrics.stale_completions == 3);
  CHECK(metrics.rejected_requests == 1);
  CHECK(metrics.exact_presentations == 1);
  return true;
}

bool testResetStartsANewSession() {
  ReadOnlyOverlayFrameCoordinator coordinator;
  auto plan = coordinator.beginFrame(enabledFrame(1));
  CHECK(coordinator
            .finishFrame(
                plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Mapped, 1))
            .exact);
  coordinator.reset();
  plan = coordinator.beginFrame(enabledFrame(50, true));
  CHECK(plan.sequence == 1);
  CHECK(!plan.request_discontinuity);
  CHECK(coordinator.metrics().frame_updates == 1);
  return true;
}

bool testPortableDiagnostics() {
  ReadOnlyOverlayFrameCoordinator coordinator;
  const auto plan = coordinator.beginFrame(enabledFrame(7));
  CHECK(coordinator
            .finishFrame(
                plan, candidate(ReadOnlyOverlayFrameCandidateStatus::Mapped, 7))
            .exact);
  std::ostringstream output;
  crimson::overlay::writeReadOnlyOverlayFrameDiagnostics(
      output, "Test", "subject_masks", coordinator.metrics());
  CHECK(output.str().find("[TestOverlayPresentation] source=subject_masks") !=
        std::string::npos);
  CHECK(output.str().find("exact=1") != std::string::npos);
  return true;
}

} // namespace

int main() {
  if (!testInactiveAndUnavailableFramesDoNotRequest() ||
      !testExactPendingMissingAndFailedResults() ||
      !testDiscontinuityRequiresPriorAcceptedRequest() ||
      !testRejectedAndStaleResultsNeverPresent() ||
      !testResetStartsANewSession() || !testPortableDiagnostics()) {
    return EXIT_FAILURE;
  }
  std::cout << "read_only_overlay_frame_coordinator_tests: PASS\n";
  return EXIT_SUCCESS;
}
