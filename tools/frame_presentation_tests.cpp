#include "frame_presentation.h"

#include <cstdlib>
#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition   \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testPresentationDecisions() {
  using namespace crimson::playback;
  auto decision = resolveFramePresentation({10, 10, 9});
  CHECK(decision.action == FramePresentationAction::Present);
  CHECK(decision.commit_candidate);
  CHECK(decision.render_current);
  CHECK(decision.exact);

  FramePresentationRequest hold;
  hold.requested_frame = 11;
  hold.visible_frame = 10;
  hold.missing_policy = MissingFramePolicy::HoldVisible;
  decision = resolveFramePresentation(hold);
  CHECK(decision.action == FramePresentationAction::Hold);
  CHECK(decision.presented_frame == 10);
  CHECK(!decision.commit_candidate);

  hold.missing_policy = MissingFramePolicy::ClearVisible;
  decision = resolveFramePresentation(hold);
  CHECK(decision.action == FramePresentationAction::Clear);
  CHECK(!decision.render_current);
  return true;
}

bool testMetricsRespectDiscontinuities() {
  crimson::playback::FramePresentationTracker tracker;
  tracker.record(1, 1, true);
  tracker.record(2, 1, false, 1.0);
  tracker.record(4, 4, false, 0.75);
  tracker.record(20, 20, true, 8.0);
  const auto &metrics = tracker.metrics();
  CHECK(metrics.presentation_count == 4);
  CHECK(metrics.exact_presentations == 3);
  CHECK(metrics.repeated_presentations == 1);
  CHECK(metrics.skipped_source_frames == 2);
  CHECK(metrics.late_presentations == 2);
  CHECK(metrics.discontinuities == 2);
  CHECK(metrics.max_lag_frames == 1.0);
  return true;
}

} // namespace

int main() {
  if (!testPresentationDecisions() || !testMetricsRespectDiscontinuities()) {
    return EXIT_FAILURE;
  }
  std::cout << "frame_presentation_tests: PASS\n";
  return EXIT_SUCCESS;
}
