#include "recording_open_workflow.h"

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
using crimson::session::RecordingOpenWorkflowController;
using crimson::session::SessionLifecycle;
using crimson::session::SessionOpenPlan;
using crimson::session::SessionOpenReadinessBinding;
using crimson::session::SessionOpenSettlementState;
using crimson::session::SessionPhase;

SessionOpenPlan
makePlan(std::string video_path, std::string zarr_path, std::string phase,
         std::vector<crimson::session::SessionReadinessProductRule> products) {
  SessionOpenPlan plan;
  plan.requested.video_path = std::move(video_path);
  plan.requested.zarr_path = std::move(zarr_path);
  plan.phase = std::move(phase);
  plan.products = std::move(products);
  return plan;
}

bool testDirectRecordingOpen() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  CHECK(workflow.begin(
      makePlan("camera.mp4", {}, "Opening recording",
               {{"archive", ProductAvailabilityRequirement::Optional},
                {"media", ProductAvailabilityRequirement::Required}})));
  CHECK(workflow.startProduct("archive", "Resolving archive"));
  CHECK(workflow.completeProduct("archive", "No archive", false,
                                 "not published"));
  CHECK(workflow.startProduct("media", "Opening media"));
  CHECK(workflow.completeProduct("media", "Media ready", true));
  CHECK(workflow.commit());
  CHECK(lifecycle.snapshot().phase == SessionPhase::Ready);
  CHECK(lifecycle.snapshot().active.video_path == "camera.mp4");
  CHECK(progress.snapshot().terminal());
  return true;
}

bool testFailedCommitBecomesFailedSession() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  CHECK(workflow.begin(
      makePlan("camera.mp4", {}, "Opening recording",
               {{"media", ProductAvailabilityRequirement::Required}})));
  std::string error;
  CHECK(
      !workflow.commit(std::nullopt, "Ready", "Recording unavailable", &error));
  CHECK(error.find("has not completed") != std::string::npos);
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  CHECK(progress.snapshot().state == crimson::loading::LoadingState::Failed);
  return true;
}

bool testChildLoaderSettlement() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  SessionOpenReadinessBinding binding;
  binding.transaction_product = "analysis";
  binding.readiness_products = {
      {"archive", ProductAvailabilityRequirement::Required}};
  binding.resolved = {{"camera.mp4", "analysis.zarr", {}}};
  CHECK(workflow.begin(
      makePlan("camera.mp4", "analysis.zarr", "Opening recording",
               {{"media", ProductAvailabilityRequirement::Required},
                {"analysis", ProductAvailabilityRequirement::Required}}),
      binding));
  CHECK(workflow.startProduct("media", "Opening media"));
  CHECK(workflow.completeProduct("media", "Media ready", true));
  CHECK(workflow.startProduct("analysis", "Loading analysis"));
  const uint64_t generation = workflow.generation();

  crimson::loading::LoadingProgressTracker child;
  child.start(1, "Loading analysis");
  child.startProduct("archive", "Opening archive");
  CHECK(workflow.settle(generation, child.snapshot()).state ==
        SessionOpenSettlementState::Waiting);
  child.completeProduct("archive", "Archive ready", true, 1.0);
  child.finish();
  CHECK(workflow.settle(generation, child.snapshot()).state ==
        SessionOpenSettlementState::Committed);
  CHECK(lifecycle.snapshot().phase == SessionPhase::Ready);
  CHECK(!workflow.active());
  return true;
}

bool testFatalProductFailureEndsWorkflow() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  CHECK(workflow.begin(
      makePlan("camera.mp4", {}, "Opening recording",
               {{"media", ProductAvailabilityRequirement::Required}})));
  CHECK(workflow.startProduct("media", "Opening media"));
  CHECK(workflow.failProduct("media", "Media unavailable", "decode failed"));
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  CHECK(!workflow.active());
  return true;
}

bool testNewOpenInvalidatesOldGeneration() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  CHECK(workflow.begin(makePlan("first.mp4", {}, "First", {{"media", {}}})));
  const uint64_t first_generation = workflow.generation();
  CHECK(workflow.begin(makePlan("second.mp4", {}, "Second", {{"media", {}}})));
  CHECK(workflow.generation() > first_generation);
  crimson::loading::LoadingProgressTracker stale_child;
  CHECK(workflow.settle(first_generation, stale_child.snapshot()).state ==
        SessionOpenSettlementState::Inactive);
  CHECK(workflow.active());
  CHECK(workflow.completeProduct("media", "Ready", true));
  CHECK(workflow.commit());
  CHECK(lifecycle.snapshot().active.video_path == "second.mp4");
  return true;
}

} // namespace

int main() {
  if (!testDirectRecordingOpen() || !testFailedCommitBecomesFailedSession() ||
      !testChildLoaderSettlement() || !testFatalProductFailureEndsWorkflow() ||
      !testNewOpenInvalidatesOldGeneration()) {
    return EXIT_FAILURE;
  }
  std::cout << "recording_open_workflow_tests: PASS\n";
  return EXIT_SUCCESS;
}
