#include "subject_mask_presentation_coordinator.h"

#include "data_access_scheduler.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

class FixtureRepository final
    : public crimson::zarr::SubjectMaskOverlayRepository {
public:
  FixtureRepository() {
    descriptor_.source_group = "refined_subject_masks_runs";
    descriptor_.run_name = "presentation_fixture";
    descriptor_.component_labels = {"subject_body"};
    descriptor_.camera_frame_count = 10;
    descriptor_.mask_width = 2;
    descriptor_.mask_height = 2;
  }

  const crimson::zarr::SubjectMaskOverlayDescriptor &
  descriptor() const override {
    return descriptor_;
  }

  crimson::zarr::SubjectMaskOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int, int) const override {
    crimson::zarr::SubjectMaskOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame != 4) {
      result.status = crimson::zarr::SubjectMaskOverlayStatus::Missing;
      return result;
    }

    result.status = crimson::zarr::SubjectMaskOverlayStatus::Mapped;
    crimson::zarr::SubjectMaskOverlayDetection detection;
    detection.instance_key = 42;
    detection.source_crop_row_id = 7;
    detection.roi_x = 10.0;
    detection.roi_y = 20.0;
    detection.roi_width = 2.0;
    detection.roi_height = 2.0;
    crimson::zarr::SubjectMaskOverlayComponent component;
    component.label = "subject_body";
    component.channel_index = 0;
    component.present = true;
    component.mask_width = 2;
    component.mask_height = 2;
    component.mask = std::make_shared<const std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{255, 0, 0, 255});
    detection.components.push_back(std::move(component));
    result.detections.push_back(std::move(detection));
    return result;
  }

private:
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor_;
};

crimson::overlay::ReadOnlyOverlayInput sceneForFrame(int64_t frame) {
  crimson::overlay::ReadOnlyOverlayInput scene;
  scene.identity = {0, frame, 0, frame};
  scene.source_width = 100;
  scene.source_height = 80;
  return scene;
}

bool testBufferedPresentationAndMissingFrame() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 1, 0);
  SubjectMaskOverlayBuffer buffer(scheduler, "presentation_fixture");
  std::string error;
  CHECK(buffer.open(std::make_unique<FixtureRepository>(), 0, 2, &error));

  crimson::overlay::SubjectMaskPresentationCoordinator coordinator;
  auto scene = sceneForFrame(4);
  auto result = coordinator.update({4, 100, 80, true, true, true, false},
                                   buffer, buffer.descriptor(), scene, &error);
  CHECK(result.action == crimson::overlay::ReadOnlyOverlayFrameAction::Wait ||
        result.action == crimson::overlay::ReadOnlyOverlayFrameAction::Present);
  CHECK(buffer.waitForFrame(4, std::chrono::seconds(2)));
  scene.subject_masks.clear();
  result = coordinator.update({4, 100, 80, true, true, true, false}, buffer,
                              buffer.descriptor(), scene, &error);
  CHECK(result.action == crimson::overlay::ReadOnlyOverlayFrameAction::Present);
  CHECK(result.overlay_ready);
  CHECK(result.detection_count == 1);
  CHECK(result.component_count == 1);
  CHECK(scene.subject_masks.size() == 1);
  CHECK(scene.subject_masks[0].source_rect.x == 10.0);
  CHECK(scene.subject_masks[0].source_rect.y == 20.0);

  scene = sceneForFrame(5);
  result = coordinator.update({5, 100, 80, true, true, true, true}, buffer,
                              buffer.descriptor(), scene, &error);
  CHECK(buffer.waitForFrame(5, std::chrono::seconds(2)));
  result = coordinator.update({5, 100, 80, true, true, true, false}, buffer,
                              buffer.descriptor(), scene, &error);
  CHECK(result.action == crimson::overlay::ReadOnlyOverlayFrameAction::Clear);
  CHECK(!result.overlay_ready);
  CHECK(coordinator.metrics().discontinuity_requests == 1);
  CHECK(coordinator.metrics().exact_presentations >= 1);

  buffer.close();
  scheduler->shutdown();
  return true;
}

bool testClosedBufferRejectsRequest() {
  SubjectMaskOverlayBuffer buffer;
  crimson::overlay::SubjectMaskPresentationCoordinator coordinator;
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  auto scene = sceneForFrame(2);
  std::string error;
  const auto result = coordinator.update({2, 100, 80, true, true, true, false},
                                         buffer, descriptor, scene, &error);
  CHECK(result.action ==
        crimson::overlay::ReadOnlyOverlayFrameAction::RequestRejected);
  CHECK(!error.empty());
  return true;
}

} // namespace

int main() {
  if (!testBufferedPresentationAndMissingFrame() ||
      !testClosedBufferRejectsRequest()) {
    return EXIT_FAILURE;
  }
  std::cout << "subject_mask_presentation_coordinator_tests: PASS\n";
  return EXIT_SUCCESS;
}
