#include "session_open_transaction.h"

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

using crimson::session::ProductAvailabilityRequirement;
using crimson::session::SessionLifecycle;
using crimson::session::SessionOpenCoordinator;
using crimson::session::SessionOpenPlan;
using crimson::session::SessionOpenReadinessBinding;
using crimson::session::SessionOpenSettlementState;
using crimson::session::SessionPhase;

bool testRequiredProductCommit() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  SessionOpenCoordinator coordinator(lifecycle, progress);
  auto transaction = coordinator.begin(
      {{"camera.mp4", "analysis.zarr", {}},
       "Opening session",
       {{"archive", ProductAvailabilityRequirement::Required}}});
  CHECK(transaction);
  std::string error;
  CHECK(!transaction.commit(std::nullopt, "Ready", &error));
  CHECK(error.find("has not completed") != std::string::npos);
  CHECK(transaction.startProduct("archive", "Opening archive"));
  CHECK(transaction.completeProduct("archive", "Archive ready", true, 3.0));
  CHECK(transaction.commit(std::nullopt, "Session ready", &error));
  CHECK(error.empty());
  CHECK(lifecycle.snapshot().phase == SessionPhase::Ready);
  CHECK(progress.snapshot().terminal());
  CHECK(coordinator.readiness().ready());
  return true;
}

bool testOptionalUnavailableCanCommit() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  SessionOpenCoordinator coordinator(lifecycle, progress);
  auto transaction = coordinator.begin(
      {{"camera.mp4", {}, {}},
       "Opening media",
       {{"archive", ProductAvailabilityRequirement::Optional},
        {"media", ProductAvailabilityRequirement::Required}}});
  CHECK(transaction.completeProduct("archive", "No archive", false, 1.0,
                                    "not found"));
  CHECK(transaction.completeProduct("media", "Media ready", true, 2.0));
  CHECK(transaction.commit());
  CHECK(coordinator.readiness().ready());
  return true;
}

bool testRequiredUnavailableMustFail() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  SessionOpenCoordinator coordinator(lifecycle, progress);
  auto transaction = coordinator.begin(
      {{"camera.mp4", {}, {}},
       "Opening media",
       {{"media", ProductAvailabilityRequirement::Required}}});
  CHECK(transaction.completeProduct("media", "Media unavailable", false, 2.0,
                                    "decode failed"));
  std::string error;
  CHECK(!transaction.commit(std::nullopt, "Ready", &error));
  CHECK(error.find("unavailable") != std::string::npos);
  CHECK(transaction.fail("decode failed"));
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  CHECK(coordinator.readiness().state ==
        crimson::session::SessionReadinessState::Blocked);
  return true;
}

bool testStaleTransactionCannotPublish() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  SessionOpenCoordinator coordinator(lifecycle, progress);
  auto first =
      coordinator.begin({{"first.mp4", {}, {}}, "First", {{"media", {}}}});
  auto second =
      coordinator.begin({{"second.mp4", {}, {}}, "Second", {{"media", {}}}});
  CHECK(!first.current());
  CHECK(!first.completeProduct("media", "Stale", true, 1.0));
  CHECK(second.completeProduct("media", "Ready", true, 1.0));
  CHECK(second.commit());
  CHECK(lifecycle.snapshot().active.video_path == "second.mp4");
  return true;
}

bool testInvalidPlanIsRejected() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  SessionOpenCoordinator coordinator(lifecycle, progress);
  std::string error;
  const auto transaction = coordinator.begin(
      {{}, "Opening", {{"archive", {}}, {"archive", {}}}}, &error);
  CHECK(!transaction);
  CHECK(!error.empty());
  CHECK(lifecycle.snapshot().phase == SessionPhase::Empty);
  return true;
}

bool testCoordinatorDestructionInvalidatesHandle() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  crimson::session::SessionOpenTransaction transaction;
  {
    SessionOpenCoordinator coordinator(lifecycle, progress);
    transaction =
        coordinator.begin({{"camera.mp4", {}, {}}, "Opening", {{"media", {}}}});
    CHECK(transaction.current());
  }
  CHECK(!transaction.current());
  CHECK(!transaction.completeProduct("media", "Ready", true, 1.0));
  return true;
}

bool testProgressSettlementWaitsThenCommits() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker parent_progress;
  SessionOpenCoordinator coordinator(lifecycle, parent_progress);
  auto transaction = coordinator.begin(
      {{"camera.mp4", "analysis.zarr", {}},
       "Opening session",
       {{"media", ProductAvailabilityRequirement::Required},
        {"analysis", ProductAvailabilityRequirement::Required}}});
  CHECK(transaction.completeProduct("media", "Media ready", true, 2.0));

  crimson::loading::LoadingProgressTracker analysis_progress;
  analysis_progress.start(2, "Loading analysis");
  analysis_progress.startProduct("archive", "Opening archive");
  analysis_progress.completeProduct("archive", "Archive ready", true, 1.0);
  SessionOpenReadinessBinding binding;
  binding.transaction_product = "analysis";
  binding.readiness_products = {
      {"archive", ProductAvailabilityRequirement::Required}};
  binding.product_ready_phase = "Analysis ready";

  const auto waiting = crimson::session::settleSessionOpenFromProgress(
      transaction, analysis_progress.snapshot(), binding, 3.0);
  CHECK(waiting.state == SessionOpenSettlementState::Waiting);
  CHECK(transaction.current());

  analysis_progress.startProduct("optional", "Opening optional product");
  analysis_progress.completeProduct("optional", "Optional unavailable", false,
                                    1.0, "not published");
  analysis_progress.finish();
  const auto committed = crimson::session::settleSessionOpenFromProgress(
      transaction, analysis_progress.snapshot(), binding, 4.0);
  CHECK(committed.state == SessionOpenSettlementState::Committed);
  CHECK(committed.error.empty());
  CHECK(lifecycle.snapshot().phase == SessionPhase::Ready);
  CHECK(parent_progress.snapshot().products.size() == 2);
  return true;
}

bool testProgressSettlementPropagatesFailure() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker parent_progress;
  SessionOpenCoordinator coordinator(lifecycle, parent_progress);
  auto transaction = coordinator.begin(
      {{"camera.mp4", "analysis.zarr", {}},
       "Opening session",
       {{"analysis", ProductAvailabilityRequirement::Required}}});

  crimson::loading::LoadingProgressTracker analysis_progress;
  analysis_progress.start(1, "Loading analysis");
  analysis_progress.startProduct("archive", "Opening archive");
  analysis_progress.completeProduct("archive", "Archive unavailable", false,
                                    1.0, "archive failed");
  analysis_progress.finish();
  SessionOpenReadinessBinding binding;
  binding.transaction_product = "analysis";
  binding.readiness_products = {
      {"archive", ProductAvailabilityRequirement::Required}};

  const auto failed = crimson::session::settleSessionOpenFromProgress(
      transaction, analysis_progress.snapshot(), binding, 2.0);
  CHECK(failed.state == SessionOpenSettlementState::Failed);
  CHECK(failed.error == "archive failed");
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  CHECK(parent_progress.snapshot().state ==
        crimson::loading::LoadingState::Failed);
  return true;
}

bool testProgressSettlementCommitsWithoutChildProduct() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker parent_progress;
  SessionOpenCoordinator coordinator(lifecycle, parent_progress);
  auto transaction = coordinator.begin(
      {{"camera.mp4", {}, {}},
       "Opening session",
       {{"media", ProductAvailabilityRequirement::Required}}});
  CHECK(transaction.completeProduct("media", "Media ready", true, 1.0));

  crimson::loading::LoadingProgressTracker no_child_progress;
  const auto committed = crimson::session::settleSessionOpenFromProgress(
      transaction, no_child_progress.snapshot(), {}, 1.0);
  CHECK(committed.state == SessionOpenSettlementState::Committed);
  CHECK(lifecycle.snapshot().phase == SessionPhase::Ready);
  return true;
}

} // namespace

int main() {
  if (!testRequiredProductCommit() || !testOptionalUnavailableCanCommit() ||
      !testRequiredUnavailableMustFail() ||
      !testStaleTransactionCannotPublish() || !testInvalidPlanIsRejected() ||
      !testCoordinatorDestructionInvalidatesHandle() ||
      !testProgressSettlementWaitsThenCommits() ||
      !testProgressSettlementPropagatesFailure() ||
      !testProgressSettlementCommitsWithoutChildProduct()) {
    return EXIT_FAILURE;
  }
  std::cout << "session_open_transaction_tests: PASS\n";
  return EXIT_SUCCESS;
}
