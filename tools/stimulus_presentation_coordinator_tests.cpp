#include "stimulus_presentation_coordinator.h"

#include <iostream>

namespace {

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' \
                << __LINE__ << '\n';                                         \
      return false;                                                           \
    }                                                                         \
  } while (false)

using crimson::playback::StimulusPresentationAction;
using crimson::playback::StimulusPresentationCoordinator;
using crimson::zarr::StimulusFrameResolution;
using crimson::zarr::StimulusMappingStatus;

StimulusFrameResolution Mapped(int32_t camera_frame,
                               int32_t stimulus_frame,
                               bool interpolated = false) {
  StimulusFrameResolution result;
  result.camera_frame = camera_frame;
  result.status = StimulusMappingStatus::Mapped;
  result.stimulus_frame = stimulus_frame;
  result.interpolated = interpolated;
  return result;
}

StimulusFrameResolution Unmapped(int32_t camera_frame,
                                 StimulusMappingStatus status) {
  StimulusFrameResolution result;
  result.camera_frame = camera_frame;
  result.status = status;
  return result;
}

bool RunTest() {
  StimulusPresentationCoordinator coordinator;

  auto missing = coordinator.update(
      0, Unmapped(0, StimulusMappingStatus::Missing), std::nullopt);
  CHECK(missing.action == StimulusPresentationAction::Clear);
  CHECK(missing.commit_composite);
  CHECK(!missing.render_current);

  auto waiting = coordinator.update(1, Mapped(1, 0), std::nullopt);
  CHECK(waiting.action == StimulusPresentationAction::Wait);
  CHECK(!waiting.commit_composite);
  CHECK(!waiting.render_current);

  auto first = coordinator.update(1, Mapped(1, 0), 0);
  CHECK(first.action == StimulusPresentationAction::Present);
  CHECK(first.commit_composite);
  CHECK(first.render_current);
  CHECK(first.generation > waiting.generation);

  auto paused_hold = coordinator.update(1, Mapped(1, 0), std::nullopt);
  CHECK(paused_hold.action == StimulusPresentationAction::Hold);
  CHECK(paused_hold.render_current);

  auto repeated_mapping =
      coordinator.update(2, Mapped(2, 0, true), std::nullopt);
  CHECK(repeated_mapping.action == StimulusPresentationAction::Hold);
  CHECK(repeated_mapping.render_current);

  auto stale_generation = coordinator.update(3, Mapped(2, 0), 0);
  CHECK(stale_generation.action == StimulusPresentationAction::Wait);
  CHECK(!stale_generation.render_current);

  auto changed_not_ready = coordinator.update(3, Mapped(3, 2), std::nullopt);
  CHECK(changed_not_ready.action == StimulusPresentationAction::Wait);
  CHECK(!changed_not_ready.commit_composite);
  CHECK(!changed_not_ready.render_current);

  auto retained_pair = coordinator.update(4, Mapped(4, 0), std::nullopt);
  CHECK(retained_pair.action == StimulusPresentationAction::Hold);
  CHECK(retained_pair.commit_composite);
  CHECK(retained_pair.render_current);

  auto mismatched = coordinator.update(3, Mapped(3, 2), 3);
  CHECK(mismatched.action == StimulusPresentationAction::Wait);
  CHECK(!mismatched.render_current);

  auto changed_ready = coordinator.update(3, Mapped(3, 2), 2);
  CHECK(changed_ready.action == StimulusPresentationAction::Present);
  CHECK(changed_ready.render_current);

  auto mismatched_visible = coordinator.update(4, Mapped(4, 2), 3);
  CHECK(mismatched_visible.action == StimulusPresentationAction::Wait);
  CHECK(!mismatched_visible.commit_composite);
  CHECK(!mismatched_visible.render_current);

  auto backward = coordinator.update(1, Mapped(1, 0), 0);
  CHECK(backward.action == StimulusPresentationAction::Present);
  CHECK(backward.render_current);

  auto out_of_range = coordinator.update(
      9, Unmapped(9, StimulusMappingStatus::OutOfRange), std::nullopt);
  CHECK(out_of_range.action == StimulusPresentationAction::Clear);
  CHECK(!out_of_range.render_current);

  const auto& metrics = coordinator.metrics();
  CHECK(metrics.candidate_updates == 13);
  CHECK(metrics.camera_presentations == 8);
  CHECK(metrics.mapped_presentations == 10);
  CHECK(metrics.missing_presentations == 1);
  CHECK(metrics.out_of_range_presentations == 1);
  CHECK(metrics.interpolated_presentations == 1);
  CHECK(metrics.exact_presentations == 3);
  CHECK(metrics.held_presentations == 3);
  CHECK(metrics.unavailable_presentations == 5);
  CHECK(metrics.deferred_presentations == 5);
  CHECK(metrics.mismatched_mapping_frames == 1);
  CHECK(metrics.mismatched_decoded_frames == 2);
  CHECK(metrics.max_consecutive_unavailable == 2);
  CHECK(metrics.presented_stimulus_frame == -1);
  CHECK(metrics.max_abs_camera_skew_frames == 0);
  CHECK(metrics.last_rendered_generation == metrics.last_generation);

  coordinator.resetVisibleFrame();
  std::cout << "stimulus_presentation_coordinator_tests: PASS\n";
  return true;
}

}  // namespace

int main() { return RunTest() ? 0 : 1; }
