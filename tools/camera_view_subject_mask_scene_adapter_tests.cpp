#include "gui/camera_view_subject_mask_scene_adapter.h"

#include <iostream>
#include <memory>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

crimson::zarr::SubjectMaskOverlayDetection detection(int64_t crop_row,
                                                     const std::string &label,
                                                     size_t channel,
                                                     double offset) {
  crimson::zarr::SubjectMaskOverlayDetection value;
  value.source_crop_row_id = crop_row;
  value.roi_x = offset;
  value.roi_y = 5.0;
  value.roi_width = 20.0;
  value.roi_height = 20.0;
  crimson::zarr::SubjectMaskOverlayComponent component;
  component.label = label;
  component.channel_index = channel;
  component.present = true;
  component.mask_width = 2;
  component.mask_height = 2;
  component.mask = std::make_shared<const std::vector<uint8_t>>(
      std::vector<uint8_t>{1, 0, 0, 1});
  component.contour = {
      {offset + 1.0, 6.0}, {offset + 10.0, 6.0}, {offset + 10.0, 15.0}};
  value.components.push_back(std::move(component));
  return value;
}

bool testCompleteFrameAndPresentationModes() {
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  descriptor.cache_namespace = "fixture";
  descriptor.source_group = "refined_subject_masks_runs";
  descriptor.run_name = "mask_v1";
  descriptor.component_labels = {"subject_body", "eye_left"};

  crimson::zarr::SubjectMaskOverlayResolution frame;
  frame.status = crimson::zarr::SubjectMaskOverlayStatus::Mapped;
  frame.camera_frame = 9;
  frame.detections = {detection(41, "subject_body", 0, 0.0),
                      detection(42, "eye_left", 1, 30.0)};

  crimson::gui::CameraViewSubjectMaskSceneOptions review;
  const auto review_scene = crimson::gui::makeCameraViewSubjectMaskScene(
      descriptor, frame, review, 0, 9, 100, 80);
  CHECK(review_scene.ready());
  CHECK(review_scene.rasterCount(
            crimson::overlay::CameraOverlayLayer::SubjectMasks) == 2);
  CHECK(review_scene.count(
            crimson::overlay::CameraOverlayLayer::SubjectMasks) == 2);
  CHECK(review_scene.raster_masks[0].cache_key.rfind("fixture:", 0) == 0);
  CHECK(review_scene.raster_masks[0].cache_key !=
        review_scene.raster_masks[1].cache_key);

  auto realtime = review;
  realtime.mode = crimson::overlay::ReadOnlyMaskOverlayMode::Realtime;
  const auto realtime_scene = crimson::gui::makeCameraViewSubjectMaskScene(
      descriptor, frame, realtime, 0, 9, 100, 80);
  CHECK(realtime_scene.raster_masks.size() == 2);
  CHECK(realtime_scene.primitives.empty());
  return true;
}

bool testEditSuppressionAndStaleFrame() {
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  descriptor.cache_namespace = "fixture";
  descriptor.source_group = "refined_subject_masks_runs";
  descriptor.run_name = "mask_v1";
  descriptor.component_labels = {"subject_body", "eye_left"};
  crimson::zarr::SubjectMaskOverlayResolution frame;
  frame.status = crimson::zarr::SubjectMaskOverlayStatus::Mapped;
  frame.camera_frame = 11;
  frame.detections = {detection(51, "subject_body", 0, 0.0),
                      detection(52, "eye_left", 1, 30.0)};

  crimson::gui::CameraViewSubjectMaskSceneOptions options;
  options.suppressed_source_crop_row_id = 52;
  options.suppressed_component = "eye_left";
  const auto suppressed = crimson::gui::makeCameraViewSubjectMaskScene(
      descriptor, frame, options, 0, 11, 100, 80);
  CHECK(suppressed.ready());
  CHECK(suppressed.raster_masks.size() == 1);
  CHECK(suppressed.primitives.size() == 1);

  const auto stale = crimson::gui::makeCameraViewSubjectMaskScene(
      descriptor, frame, options, 0, 12, 100, 80);
  CHECK(!stale.ready());
  CHECK(stale.raster_masks.empty());
  CHECK(stale.primitives.empty());
  return true;
}

} // namespace

int main() {
  if (!testCompleteFrameAndPresentationModes() ||
      !testEditSuppressionAndStaleFrame()) {
    return 1;
  }
  std::cout << "camera_view_subject_mask_scene_adapter_tests: PASS\n";
  return 0;
}
