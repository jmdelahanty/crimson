#include "crop_presentation_coordinator.h"

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

using namespace crimson::crop;

CropFrameGeometry Geometry(int64_t camera_frame) {
  CropFrameGeometry geometry;
  geometry.camera_frame = camera_frame;
  geometry.source_width = 128;
  geometry.source_height = 96;
  geometry.output_width = 64;
  geometry.output_height = 48;
  geometry.full_frame_crop = {16.0, 12.0, 64.0, 48.0};
  geometry.geometry_available = true;
  return geometry;
}

CropSourceSelection Selected(int64_t camera_frame, CropSourceKind source,
                             int64_t source_frame) {
  CropSourceSelection selection;
  selection.status = CropSourceSelectionStatus::Selected;
  selection.source = source;
  selection.camera_frame = camera_frame;
  selection.source_frame_index = source_frame;
  selection.geometry = Geometry(camera_frame);
  return selection;
}

CropSourceSelection Unavailable(int64_t camera_frame,
                                CropSourceKind source,
                                CropSourceSelectionStatus status,
                                int64_t source_frame = -1) {
  CropSourceSelection selection;
  selection.status = status;
  selection.source = source;
  selection.camera_frame = camera_frame;
  if (source_frame >= 0) {
    selection.source_frame_index = source_frame;
  }
  return selection;
}

bool RunTest() {
  CropPresentationCoordinator coordinator;

  auto awaiting = coordinator.update(
      0, Unavailable(0, CropSourceKind::AcquisitionVideo,
                     CropSourceSelectionStatus::AwaitingExactFrame, 0),
      std::nullopt);
  CHECK(awaiting.action == CropPresentationAction::Wait);
  CHECK(!awaiting.commit_crop);

  auto acquisition = coordinator.update(
      0, Selected(0, CropSourceKind::AcquisitionVideo, 0), 0);
  CHECK(acquisition.action == CropPresentationAction::Present);
  CHECK(acquisition.commit_crop);
  CHECK(acquisition.render_current);

  auto retained = coordinator.update(
      0, Unavailable(0, CropSourceKind::AcquisitionVideo,
                     CropSourceSelectionStatus::AwaitingExactFrame, 0),
      std::nullopt);
  CHECK(retained.action == CropPresentationAction::Hold);
  CHECK(retained.render_current);

  auto wrong_mapping = coordinator.update(
      1, Selected(0, CropSourceKind::AcquisitionVideo, 0), 0);
  CHECK(wrong_mapping.action == CropPresentationAction::Wait);

  auto wrong_surface = coordinator.update(
      1, Selected(1, CropSourceKind::AcquisitionVideo, 1), 0);
  CHECK(wrong_surface.action == CropPresentationAction::Wait);

  auto live = coordinator.update(
      1, Selected(1, CropSourceKind::LiveGeometry, 1), 1);
  CHECK(live.action == CropPresentationAction::Present);
  CHECK(live.render_current);

  auto missing = coordinator.update(
      2, Unavailable(2, CropSourceKind::LiveGeometry,
                     CropSourceSelectionStatus::MissingGeometry),
      std::nullopt);
  CHECK(missing.action == CropPresentationAction::Clear);
  CHECK(missing.commit_crop);
  CHECK(!missing.render_current);

  auto backward = coordinator.update(
      0, Selected(0, CropSourceKind::AcquisitionVideo, 0), 0);
  CHECK(backward.action == CropPresentationAction::Present);

  auto invalid = coordinator.update(
      3, Unavailable(3, CropSourceKind::AcquisitionVideo,
                     CropSourceSelectionStatus::InvalidState),
      std::nullopt);
  CHECK(invalid.action == CropPresentationAction::Wait);

  const auto& metrics = coordinator.metrics();
  CHECK(metrics.candidate_updates == 9);
  CHECK(metrics.exact_presentations == 3);
  CHECK(metrics.held_presentations == 1);
  CHECK(metrics.cleared_presentations == 1);
  CHECK(metrics.deferred_presentations == 4);
  CHECK(metrics.mismatched_selection_frames == 1);
  CHECK(metrics.mismatched_surface_frames == 1);
  CHECK(metrics.invalid_presentations == 1);
  CHECK(metrics.max_abs_camera_skew_frames == 0);
  CHECK(metrics.presented_crop_camera_frame == 0);
  CHECK(metrics.presented_source_frame == 0);
  CHECK(metrics.presented_source == CropSourceKind::AcquisitionVideo);

  coordinator.resetVisibleFrame();
  CHECK(coordinator.metrics().presented_crop_camera_frame == -1);
  std::cout << "crop_presentation_coordinator_tests: PASS\n";
  return true;
}

}  // namespace

int main() { return RunTest() ? 0 : 1; }
