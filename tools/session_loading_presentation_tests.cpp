#include "session_loading_presentation.h"

#include <cmath>
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

bool near(double lhs, double rhs) { return std::abs(lhs - rhs) < 1e-9; }

bool testRunningPresentation() {
  LoadingProgressTracker progress;
  progress.start(3, "Loading keypoints");
  progress.startProduct("archive", "Opening archive");
  progress.completeProduct("archive", "Archive ready", true, 4.0);
  progress.startProduct("keypoints", "Loading keypoints");
  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto readiness =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  const auto presentation = crimson::session::makeSessionLoadingPresentation(
      progress.snapshot(), readiness);

  CHECK(presentation.visible);
  CHECK(!presentation.dismissible);
  CHECK(presentation.phase == "Loading keypoints");
  CHECK(presentation.completed_products == 1);
  CHECK(presentation.total_products == 3);
  CHECK(near(presentation.fraction, 1.0 / 3.0));
  CHECK(presentation.products.size() == 2);
  CHECK(presentation.products[0].status == "Ready");
  CHECK(presentation.products[1].status == "Loading");
  return true;
}

bool testOptionalUnavailablePresentation() {
  LoadingProgressTracker progress;
  progress.start(2, "Loading analysis");
  progress.completeProduct("archive", "Archive ready", true, 1.0);
  progress.completeProduct("stimulus", "Stimulus unavailable", false, 2.0,
                           "not published");
  progress.finish();
  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto readiness =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  const auto presentation = crimson::session::makeSessionLoadingPresentation(
      progress.snapshot(), readiness);

  CHECK(!presentation.visible);
  CHECK(!presentation.error_visible);
  CHECK(near(presentation.fraction, 1.0));
  CHECK(presentation.products[1].status == "Unavailable");
  CHECK(presentation.products[1].error == "not published");
  return true;
}

bool testBlockedPresentation() {
  LoadingProgressTracker progress;
  progress.start(1, "Opening archive");
  progress.completeProduct("archive", "Archive unavailable", false, 1.0,
                           "archive failed");
  progress.finish();
  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto readiness =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  const auto presentation = crimson::session::makeSessionLoadingPresentation(
      progress.snapshot(), readiness);

  CHECK(!presentation.visible);
  CHECK(presentation.error_visible);
  CHECK(presentation.detail == "archive failed");
  CHECK(presentation.readiness_state ==
        crimson::session::SessionReadinessState::Blocked);
  return true;
}

bool testDismissibleWaitingPresentation() {
  LoadingProgressTracker progress;
  progress.start(1, "Opening archive");
  progress.startProduct("archive", "Opening archive");
  SessionReadinessPolicy policy;
  policy.allow_background_continue = true;
  const std::vector<SessionReadinessProductRule> rules = {
      {"archive", ProductAvailabilityRequirement::Required}};
  const auto readiness = crimson::session::evaluateSessionReadiness(
      progress.snapshot(), rules, policy);
  const auto presentation = crimson::session::makeSessionLoadingPresentation(
      progress.snapshot(), readiness, "Opening recording");

  CHECK(presentation.visible);
  CHECK(presentation.dismissible);
  CHECK(presentation.title == "Opening recording");
  CHECK(presentation.detail == "Waiting for archive");
  return true;
}

} // namespace

int main() {
  if (!testRunningPresentation() || !testOptionalUnavailablePresentation() ||
      !testBlockedPresentation() || !testDismissibleWaitingPresentation()) {
    return EXIT_FAILURE;
  }
  std::cout << "session_loading_presentation_tests: PASS\n";
  return EXIT_SUCCESS;
}
