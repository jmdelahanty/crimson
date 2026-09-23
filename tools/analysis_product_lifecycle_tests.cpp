#include "analysis_product_lifecycle.h"

#include <iostream>
#include <string>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

using crimson::analysis::AnalysisProductLifecycleController;
using crimson::analysis::ProductLifecyclePlan;
using crimson::analysis::ProductLifecycleState;
using crimson::analysis::ProductRouteDisposition;

bool testInitialFrameDemand() {
  CHECK(!crimson::analysis::shouldRequestInitialFrame(false, true));
  CHECK(crimson::analysis::shouldRequestInitialFrame(false, false));
  CHECK(crimson::analysis::shouldRequestInitialFrame(true, true));
  return true;
}

bool testDependencyReleaseAfterAvailablePrerequisite() {
  AnalysisProductLifecycleController controller;
  CHECK(controller.begin({7, {{"swim_bouts", "motion"}}}));

  const auto deferred = controller.route(7, "swim_bouts");
  CHECK(deferred.deferred());
  CHECK(controller.deferredProductCount() == 1);
  CHECK(controller.route(7, "swim_bouts").deferred());

  CHECK(controller.route(7, "motion").adopt());
  const auto motion = controller.complete(7, "motion", true);
  CHECK(motion.accepted);
  CHECK(motion.released_products == std::vector<std::string>{"swim_bouts"});
  CHECK(controller.state("motion") == ProductLifecycleState::Available);

  CHECK(controller.route(7, "swim_bouts").adopt());
  CHECK(controller.complete(7, "swim_bouts", true).accepted);
  CHECK(controller.state("swim_bouts") == ProductLifecycleState::Available);
  CHECK(controller.deferredProductCount() == 0);
  return true;
}

bool testUnavailablePrerequisiteStillReleasesDependent() {
  AnalysisProductLifecycleController controller;
  CHECK(controller.begin({11, {{"swim_bouts", "motion"}}}));
  CHECK(controller.route(11, "swim_bouts").deferred());
  CHECK(controller.route(11, "motion").adopt());
  const auto motion = controller.complete(11, "motion", false);
  CHECK(motion.accepted);
  CHECK(motion.released_products == std::vector<std::string>{"swim_bouts"});
  CHECK(controller.state("motion") == ProductLifecycleState::Unavailable);
  CHECK(controller.route(11, "swim_bouts").adopt());
  CHECK(controller.complete(11, "swim_bouts", false).accepted);
  return true;
}

bool testMultiplePrerequisitesAndDeterministicRelease() {
  AnalysisProductLifecycleController controller;
  CHECK(controller.begin({3,
                          {{"derived_b", "base"},
                           {"derived_a", "base"},
                           {"combined", "derived_a"},
                           {"combined", "derived_b"}}}));
  CHECK(controller.route(3, "combined").deferred());
  CHECK(controller.route(3, "derived_b").deferred());
  CHECK(controller.route(3, "derived_a").deferred());
  CHECK(controller.route(3, "base").adopt());
  const auto base = controller.complete(3, "base", true);
  CHECK(base.accepted);
  CHECK(base.released_products ==
        (std::vector<std::string>{"derived_a", "derived_b"}));
  CHECK(controller.route(3, "derived_a").adopt());
  CHECK(controller.complete(3, "derived_a", true).accepted);
  CHECK(controller.deferredProductCount() == 1);
  CHECK(controller.route(3, "derived_b").adopt());
  const auto derived_b = controller.complete(3, "derived_b", true);
  CHECK(derived_b.released_products == std::vector<std::string>{"combined"});
  return true;
}

bool testStaleDuplicateAndCancellationRejection() {
  AnalysisProductLifecycleController controller;
  CHECK(controller.begin({21, {}}));
  CHECK(controller.route(20, "motion").disposition ==
        ProductRouteDisposition::RejectStale);
  CHECK(controller.route(21, "motion").adopt());
  CHECK(controller.route(21, "motion").disposition ==
        ProductRouteDisposition::RejectDuplicate);
  CHECK(!controller.complete(20, "motion", true).accepted);
  CHECK(controller.complete(21, "motion", true).accepted);
  CHECK(controller.route(21, "motion").disposition ==
        ProductRouteDisposition::RejectDuplicate);

  CHECK(controller.route(21, "keypoints").adopt());
  CHECK(controller.cancel(21));
  CHECK(controller.cancelled());
  CHECK(!controller.active());
  CHECK(controller.route(21, "subject_masks").disposition ==
        ProductRouteDisposition::RejectCancelled);
  CHECK(!controller.complete(21, "keypoints", true).accepted);
  CHECK(!controller.cancel(20));
  return true;
}

bool testBeginValidationAndGenerationReset() {
  AnalysisProductLifecycleController controller;
  std::string error;
  CHECK(!controller.begin({0, {}}, &error));
  CHECK(!error.empty());
  CHECK(!controller.begin({1, {{"", "motion"}}}, &error));
  CHECK(!controller.begin({1, {{"motion", "motion"}}}, &error));
  CHECK(!controller.begin(
      {1, {{"swim_bouts", "motion"}, {"swim_bouts", "motion"}}}, &error));
  CHECK(!controller.begin(
      {1, {{"motion", "swim_bouts"}, {"swim_bouts", "motion"}}}, &error));

  CHECK(controller.begin({1, {{"swim_bouts", "motion"}}}, &error));
  CHECK(error.empty());
  CHECK(controller.route(1, "swim_bouts").deferred());
  CHECK(controller.begin({2, {}}, &error));
  CHECK(controller.generation() == 2);
  CHECK(controller.deferredProductCount() == 0);
  CHECK(!controller.state("motion"));
  CHECK(controller.route(1, "motion").disposition ==
        ProductRouteDisposition::RejectStale);
  CHECK(controller.route(2, "motion").adopt());
  return true;
}

} // namespace

int main() {
  if (!testInitialFrameDemand() ||
      !testDependencyReleaseAfterAvailablePrerequisite() ||
      !testUnavailablePrerequisiteStillReleasesDependent() ||
      !testMultiplePrerequisitesAndDeterministicRelease() ||
      !testStaleDuplicateAndCancellationRejection() ||
      !testBeginValidationAndGenerationReset()) {
    return 1;
  }
  std::cout << "analysis_product_lifecycle_tests: PASS\n";
  return 0;
}
