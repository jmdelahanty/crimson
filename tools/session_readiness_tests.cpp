#include "session_readiness.h"

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

using crimson::loading::LoadingProgressTracker;
using crimson::session::ProductAvailabilityRequirement;
using crimson::session::SessionReadinessPolicy;
using crimson::session::SessionReadinessProductRule;
using crimson::session::SessionReadinessState;

bool testIdleSessionNeedsNoLoading() {
  LoadingProgressTracker progress;
  const auto decision =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), {});
  CHECK(decision.ready());
  CHECK(decision.session_interaction_enabled);
  CHECK(decision.playback_controls_enabled);
  CHECK(!decision.autoplay_allowed);
  return true;
}

bool testStrictPolicyWaitsForAllProducts() {
  LoadingProgressTracker progress;
  progress.start(2, "Loading analysis");
  progress.startProduct("archive", "Opening archive");
  progress.completeProduct("archive", "Archive ready", true, 1.0);

  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto waiting =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  CHECK(waiting.state == SessionReadinessState::Waiting);
  CHECK(waiting.show_loading_ui);
  CHECK(!waiting.loading_ui_dismissible);
  CHECK(!waiting.playback_controls_enabled);
  CHECK(waiting.pause_playback);

  progress.startProduct("optional_trace", "Opening optional trace");
  progress.completeProduct("optional_trace", "Trace unavailable", false, 2.0,
                           "not published");
  progress.finish();
  const auto ready =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  CHECK(ready.ready());
  CHECK(!ready.show_loading_ui);
  CHECK(ready.playback_controls_enabled);
  CHECK(!ready.autoplay_allowed);
  return true;
}

bool testRequiredProductBlocksReadiness() {
  LoadingProgressTracker progress;
  progress.start(1, "Loading archive");
  progress.startProduct("archive", "Opening archive");
  progress.completeProduct("archive", "Archive unavailable", false, 1.0,
                           "archive failed");
  progress.finish();
  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto blocked =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  CHECK(blocked.state == SessionReadinessState::Blocked);
  CHECK(blocked.show_error_ui);
  CHECK(!blocked.playback_controls_enabled);
  CHECK(blocked.reason == "archive failed");
  return true;
}

bool testBackgroundPolicyCanReleaseRequiredSubset() {
  LoadingProgressTracker progress;
  progress.start(2, "Loading optional products");
  progress.startProduct("archive", "Opening archive");
  progress.completeProduct("archive", "Archive ready", true, 1.0);
  SessionReadinessPolicy policy;
  policy.wait_for_all_products = false;
  policy.allow_background_continue = true;
  policy.autoplay_when_ready = true;
  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto ready = crimson::session::evaluateSessionReadiness(
      progress.snapshot(), rules, policy);
  CHECK(ready.ready());
  CHECK(ready.background_loading);
  CHECK(ready.autoplay_allowed);
  return true;
}

bool testTerminalMissingProductIsInconsistent() {
  LoadingProgressTracker progress;
  progress.start(1, "Loading archive");
  progress.finish();
  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto blocked =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  CHECK(blocked.state == SessionReadinessState::Blocked);
  CHECK(blocked.reason.find("without reporting") != std::string::npos);
  return true;
}

} // namespace

int main() {
  if (!testIdleSessionNeedsNoLoading() ||
      !testStrictPolicyWaitsForAllProducts() ||
      !testRequiredProductBlocksReadiness() ||
      !testBackgroundPolicyCanReleaseRequiredSubset() ||
      !testTerminalMissingProductIsInconsistent()) {
    return EXIT_FAILURE;
  }
  std::cout << "session_readiness_tests: PASS\n";
  return EXIT_SUCCESS;
}
