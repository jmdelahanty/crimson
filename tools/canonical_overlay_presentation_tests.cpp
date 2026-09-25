#include "gui/canonical_overlay_presentation.h"
#include "zarr/keypoint_overlay_scene_adapter.h"
#include <iostream>
#include <cmath>
#include <limits>

using namespace crimson;
#define CHECK(c) do { if (!(c)) { std::cerr << "CHECK failed: " #c << " line " << __LINE__ << '\n'; return false; } } while (false)

gui::CanonicalOverlaySnapshot fixture() {
  gui::CanonicalOverlaySnapshot value;
  value.state = gui::CanonicalOverlayState::Ready;
  value.requested_frame = 54000;
  auto selection = std::make_shared<zarr::CanonicalOverlaySelection>();
  selection->keypoints.valid = selection->mask.valid = selection->shape.valid = true;
  selection->keypoints.run_id = "keypoints";
  selection->mask.run_id = "masks";
  selection->mask.manifest_payload_digest = "mask-digest";
  selection->shape.run_id = "shapes";
  selection->eye.valid = true;
  selection->eye.run_id = "eyes";
  selection->eye.identity_digest = "eye-publication";
  value.selection = selection;
  value.keypoints.state = value.masks.state = value.shapes.state = gui::CanonicalOverlayState::Ready;
  value.keypoints.descriptor.run_name = "keypoints";
  value.keypoints.descriptor.keypoint_labels = {"left_eye", "right_eye"};
  value.masks.descriptor.run_name = "masks";
  value.masks.descriptor.strict_v1 = true;
  value.masks.descriptor.component_labels = {"subject_body"};
  value.masks.descriptor.cache_namespace = "archive:masks:digest";
  value.masks.descriptor.run_manifest_payload_digest = "mask-digest";
  value.shapes.descriptor.run_name = "shapes";
  value.shapes.descriptor.geometry_in_source_camera_coordinates = true;
  value.shapes.descriptor.coordinate_width = value.shapes.descriptor.coordinate_height = 4512;
  auto kp = std::make_shared<zarr::KeypointOverlayResolution>();
  auto mask = std::make_shared<zarr::SubjectMaskOverlayResolution>();
  auto shape = std::make_shared<zarr::SubjectShapeOverlayResolution>();
  kp->camera_frame = mask->camera_frame = shape->camera_frame = 54000;
  kp->status = zarr::KeypointOverlayStatus::Mapped;
  mask->status = zarr::SubjectMaskOverlayStatus::Mapped;
  shape->status = zarr::SubjectShapeOverlayStatus::Mapped;
  for (uint64_t key : {uint64_t{0}, std::numeric_limits<uint64_t>::max()}) {
    zarr::KeypointOverlayDetection point;
    point.instance_key = key;
    point.instance_key_valid = true;
    point.keypoints = {{100,200}, {999,999}};
    point.keypoint_valid = {1,0};
    kp->detections.push_back(point);
    zarr::SubjectMaskOverlayDetection m;
    m.instance_key = key;
    m.roi_x = 90; m.roi_y = 190; m.roi_width = m.roi_height = 2;
    zarr::SubjectMaskOverlayComponent component;
    component.label = "subject_body";
    component.present = true;
    component.mask_width = component.mask_height = 2;
    component.mask = std::make_shared<const std::vector<uint8_t>>(4,1);
    m.components.push_back(component);
    mask->detections.insert(mask->detections.begin(), m); // Deliberately reordered.
    zarr::SubjectShapeOverlayDetection s;
    s.instance_key = key;
    s.instance_key_valid = true;
    s.roi_x = 90; s.roi_y = 190; s.roi_width = s.roi_height = 384;
    s.geometry.body_frame_valid = true;
    s.geometry.body_axis_valid = true;
    s.geometry.body_origin = {100,200};
    s.geometry.body_forward_axis = {1,0};
    s.geometry.body_left_axis = {0,-1};
    s.geometry.heading_degrees = key == 0 ? 30.0 : 60.0;
    s.geometry.centerline_valid = true;
    s.geometry.centerline = {{100,200}, {110,210}};
    shape->detections.insert(shape->detections.begin(), s);
  }
  value.keypoints.frame = kp; value.masks.frame = mask; value.shapes.frame = shape;
  return value;
}
auto present(gui::CanonicalOverlaySnapshot value, overlay::ReadOnlyOverlayControlState controls = {}) {
  return gui::makeCanonicalOverlayPresentation(std::move(value),0,54000,4512,4512,controls);
}
bool joinsByKeyAndPreservesCoordinates() {
  auto output = present(fixture());
  CHECK(output.keypoints_ready);
  CHECK(output.keypoints.detections.size() == 2);
  CHECK(output.keypoints.detections[0].heading_degrees == 30.0);
  CHECK(output.keypoints.detections[1].heading_degrees == 60.0);
  CHECK(output.keypoints.detections[0].heading_origin->x == 100);
  CHECK(output.keypoints.detections[0].keypoints[1].x == 999); // Source data retained.
  CHECK(output.masks.raster_masks.size() == 2);
  CHECK(!output.shapes.primitives.empty());
  bool exact_point = false;
  for (const auto& primitive : output.shapes.primitives)
    for (const auto& p : primitive.points)
      if (p.x == 100 && p.y == 200) exact_point = true;
  CHECK(exact_point); // No second application of ROI origin.
  auto input = zarr::makeKeypointOverlaySceneInput(output.snapshot.keypoints.descriptor,
      output.keypoints,0,54000,0,4512,4512);
  CHECK(input.detections.size() == 2);
  CHECK(std::isnan(input.detections[0].keypoints[1].x));
  return true;
}
bool rejectsStaleOrUnboundProducts() {
  auto value = fixture();
  auto stale = std::make_shared<zarr::SubjectMaskOverlayResolution>(*value.masks.frame);
  stale->camera_frame = 53999;
  value.masks.frame = stale;
  auto output = present(value);
  CHECK(output.snapshot.masks.state == gui::CanonicalOverlayState::Failed);
  CHECK(output.masks.raster_masks.empty());
  CHECK(output.keypoints_ready);
  value = fixture(); value.shapes.descriptor.run_name = "unrelated-latest";
  output = present(value);
  CHECK(output.snapshot.shapes.state == gui::CanonicalOverlayState::Failed);
  CHECK(!output.keypoints.detections[0].heading_degrees);
  value = fixture();
  auto invalid_axis = std::make_shared<zarr::SubjectShapeOverlayResolution>(*value.shapes.frame);
  for (auto& row : invalid_axis->detections) row.geometry.body_axis_valid = false;
  value.shapes.frame = invalid_axis;
  CHECK(!present(value).keypoints.detections.front().heading_degrees);
  for (auto& row : invalid_axis->detections) {
    row.geometry.body_axis_valid = true;
    row.geometry.body_origin.x = std::numeric_limits<double>::quiet_NaN();
  }
  CHECK(!present(value).keypoints.detections.front().heading_degrees);
  return true;
}
bool mismatchesDuplicatesAndEmptyFrames() {
  auto value = fixture();
  auto masks = std::make_shared<zarr::SubjectMaskOverlayResolution>(*value.masks.frame);
  masks->detections[0].instance_key = 17;
  value.masks.frame = masks;
  CHECK(present(value).snapshot.masks.state == gui::CanonicalOverlayState::Failed);
  value = fixture();
  auto kp = std::make_shared<zarr::KeypointOverlayResolution>(*value.keypoints.frame);
  kp->detections[1].instance_key = 0;
  value.keypoints.frame = kp;
  CHECK(!present(std::move(value)).keypoints_ready);
  value = fixture();
  kp = std::make_shared<zarr::KeypointOverlayResolution>(*value.keypoints.frame);
  kp->detections.clear(); kp->status = zarr::KeypointOverlayStatus::Missing;
  value.keypoints.frame = kp; value.keypoints.state = gui::CanonicalOverlayState::Empty;
  masks = std::make_shared<zarr::SubjectMaskOverlayResolution>(*value.masks.frame);
  masks->detections.clear(); masks->status = zarr::SubjectMaskOverlayStatus::Missing;
  value.masks.frame = masks; value.masks.state = gui::CanonicalOverlayState::Empty;
  auto shapes = std::make_shared<zarr::SubjectShapeOverlayResolution>(*value.shapes.frame);
  shapes->detections.clear(); shapes->status = zarr::SubjectShapeOverlayStatus::Missing;
  value.shapes.frame = shapes; value.shapes.state = gui::CanonicalOverlayState::Empty;
  auto output = present(value);
  CHECK(output.keypoints_ready && output.keypoints.detections.empty());
  CHECK(output.snapshot.masks.state == gui::CanonicalOverlayState::Empty);
  CHECK(output.snapshot.shapes.state == gui::CanonicalOverlayState::Empty);
  overlay::ReadOnlyOverlayControlState controls;
  controls.show_subject_masks = controls.show_subject_shape = false;
  output = present(fixture(),controls);
  CHECK(output.masks.raster_masks.empty() && output.shapes.primitives.empty());
  return true;
}
bool invalidInferenceGeometryDoesNotSuppressIndependentProducts() {
  auto value = fixture();
  auto masks = std::make_shared<zarr::SubjectMaskOverlayResolution>(*value.masks.frame);
  for (auto& row : masks->detections) for (auto& component : row.components)
    component.present = false;
  value.masks.frame = masks;
  auto shapes = std::make_shared<zarr::SubjectShapeOverlayResolution>(*value.shapes.frame);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (auto& row : shapes->detections) {
    row.geometry = {};
    row.geometry.tail_tip = {nan,nan};
  }
  value.shapes.frame = shapes;
  auto output = present(value);
  CHECK(output.keypoints_ready && output.keypoints.detections.size() == 2);
  CHECK(output.snapshot.masks.state == gui::CanonicalOverlayState::Ready);
  CHECK(output.snapshot.shapes.state == gui::CanonicalOverlayState::Ready);
  CHECK(output.masks.raster_masks.empty() && output.shapes.primitives.empty());
  CHECK(!output.keypoints.detections.front().heading_degrees);
  value.masks.state = gui::CanonicalOverlayState::Failed;
  value.masks.frame.reset();
  value.masks.error = "storage failure (distinct from absent inference)";
  output = present(value);
  CHECK(output.keypoints_ready);
  CHECK(output.snapshot.masks.state == gui::CanonicalOverlayState::Failed);
  CHECK(output.snapshot.shapes.state == gui::CanonicalOverlayState::Ready);
  return true;
}
bool contoursJoinOnlyExactBoundRowsAndDoNotBlockFills() {
  auto with_contours = [] {
    auto value = fixture();
    value.mask_contours.state = gui::CanonicalOverlayState::Ready;
    value.mask_contours.descriptor = value.masks.descriptor;
    value.mask_contours.descriptor.contour_only = true;
    value.mask_contours.descriptor.presentation_cache_run = "sampled-cache";
    auto frame = std::make_shared<zarr::SubjectMaskOverlayResolution>(*value.masks.frame);
    for (auto& row : frame->detections) {
      for (auto& component : row.components) {
        component.mask.reset();
        component.present = false;
        component.contour = {{row.roi_x,row.roi_y},
                             {row.roi_x + 2,row.roi_y},
                             {row.roi_x + 2,row.roi_y + 2}};
      }
    }
    value.mask_contours.frame = frame;
    return value;
  };
  overlay::ReadOnlyOverlayControlState controls;
  controls.independent_mask_contours = true;
  controls.show_subject_body_contour = true;
  controls.show_subject_masks = false;
  auto output = present(with_contours(), controls);
  CHECK(output.masks.ready());
  CHECK(output.masks.raster_masks.empty());
  CHECK(output.masks.primitives.size() == 2);
  CHECK(output.masks.primitives[0].instance_key == std::numeric_limits<uint64_t>::max());
  CHECK(output.masks.primitives[1].instance_key == 0);

  controls.show_subject_masks = true;
  auto pending = with_contours();
  pending.mask_contours.state = gui::CanonicalOverlayState::Pending;
  pending.mask_contours.frame.reset();
  output = present(pending, controls);
  CHECK(output.masks.raster_masks.size() == 2);
  CHECK(output.masks.primitives.empty());
  CHECK(output.snapshot.mask_contours.state == gui::CanonicalOverlayState::Pending);

  auto reject = [&](auto mutate) {
    auto value = with_contours();
    auto frame = std::make_shared<zarr::SubjectMaskOverlayResolution>(
        *value.mask_contours.frame);
    value.mask_contours.frame = frame;
    mutate(value, *frame);
    auto rejected = present(value, controls);
    CHECK(rejected.snapshot.mask_contours.state == gui::CanonicalOverlayState::Failed);
    CHECK(rejected.masks.raster_masks.size() == 2);
    CHECK(rejected.masks.primitives.empty());
    return true;
  };
  CHECK(reject([](auto&, auto& frame) { frame.camera_frame = 53999; }));
  CHECK(reject([](auto&, auto& frame) {
    frame.detections[1].instance_key = frame.detections[0].instance_key;
  }));
  CHECK(reject([](auto&, auto& frame) {
    frame.detections[0].source_crop_row_id = 77;
  }));
  CHECK(reject([](auto&, auto& frame) { frame.detections[0].roi_x += 1; }));
  CHECK(reject([](auto&, auto& frame) {
    frame.detections[0].components[0].label = "eye_left";
  }));
  CHECK(reject([](auto& value, auto&) {
    value.mask_contours.descriptor.run_name = "other-mask-run";
  }));
  CHECK(reject([](auto& value, auto&) {
    value.mask_contours.descriptor.run_manifest_payload_digest = "other-digest";
  }));
  CHECK(reject([](auto& value, auto&) {
    value.mask_contours.descriptor.presentation_cache_run.clear();
  }));
  return true;
}
bool eyesRequireExactSourceFrameAndKeys() {
  auto makeEyes = [] {
    auto value = fixture();
    value.eyes.state = gui::CanonicalOverlayState::Ready;
    value.eyes.descriptor.run_name = "eyes";
    value.eyes.descriptor.source_subject_shape_run = "shapes";
    value.eyes.descriptor.publication_identity_digest = "eye-publication";
    value.eyes.descriptor.validated_instance_keys = true;
    value.eyes.descriptor.coordinate_width = 384;
    value.eyes.descriptor.coordinate_height = 384;
    auto frame = std::make_shared<zarr::EyeGeometryOverlayResolution>();
    frame->camera_frame = 54000;
    frame->status = zarr::EyeGeometryOverlayStatus::Mapped;
    for (uint64_t key : {std::numeric_limits<uint64_t>::max(), uint64_t{0}}) {
      zarr::EyeGeometryOverlayDetection eye;
      eye.instance_key_valid = true;
      eye.instance_key = key;
      eye.frame_valid = true;
      eye.body_frame_valid = true;
      eye.roi_x = 90; eye.roi_y = 190;
      eye.roi_width = eye.roi_height = 384;
      eye.body_origin = {20, 20};
      eye.body_forward_axis = {1, 0};
      eye.body_left_axis = {0, -1};
      eye.eyes[0].valid = true;
      eye.eyes[0].major_axis = {true, {10, 10}, {20, 10}};
      eye.eyes[0].minor_axis = {true, {15, 5}, {15, 15}};
      frame->detections.push_back(eye);
    }
    value.eyes.frame = frame;
    return value;
  };
  auto output = present(makeEyes());
  CHECK(output.snapshot.eyes.state == gui::CanonicalOverlayState::Ready);
  CHECK(!output.eyes.primitives.empty());
  auto reject = [&](auto mutate) {
    auto value = makeEyes();
    auto frame = std::make_shared<zarr::EyeGeometryOverlayResolution>(
        *value.eyes.frame);
    value.eyes.frame = frame;
    mutate(value, *frame);
    const auto rejected = present(value);
    CHECK(rejected.snapshot.eyes.state == gui::CanonicalOverlayState::Failed);
    CHECK(rejected.eyes.primitives.empty());
    CHECK(rejected.keypoints_ready && rejected.shapes.ready());
    return true;
  };
  CHECK(reject([](auto&, auto& frame) { frame.camera_frame = 53999; }));
  CHECK(reject([](auto&, auto& frame) { frame.detections[0].instance_key = 99; }));
  CHECK(reject([](auto&, auto& frame) {
    frame.detections[0].instance_key = frame.detections[1].instance_key;
  }));
  CHECK(reject([](auto&, auto& frame) {
    frame.detections[0].instance_key_valid = false;
  }));
  CHECK(reject([](auto& value, auto&) {
    value.eyes.descriptor.publication_identity_digest = "wrong";
  }));
  CHECK(reject([](auto& value, auto&) {
    value.eyes.descriptor.source_subject_shape_run = "wrong";
  }));
  auto only = makeEyes();
  only.keypoints.frame.reset(); only.keypoints.state = gui::CanonicalOverlayState::Unavailable;
  only.masks.frame.reset(); only.masks.state = gui::CanonicalOverlayState::Unavailable;
  only.shapes.frame.reset(); only.shapes.state = gui::CanonicalOverlayState::Unavailable;
  CHECK(!present(only).eyes.primitives.empty());
  auto partial = makeEyes();
  auto partial_frame = std::make_shared<zarr::EyeGeometryOverlayResolution>(*partial.eyes.frame);
  partial_frame->loaded_fields = zarr::EyeGeometryFields::LeftGeometry |
                                 zarr::EyeGeometryFields::RightGeometry;
  partial.eyes.frame = partial_frame;
  // The complete scene may not invent a minor-axis fallback for unloaded gaze.
  CHECK(present(partial).eyes.primitives.empty());
  CHECK(present(partial).snapshot.eyes.state == gui::CanonicalOverlayState::Ready);
  overlay::ReadOnlyOverlayControlState axes;
  axes.show_eye_direction_beams = axes.show_eye_gaze_rays = false;
  axes.show_eye_angle_arcs = axes.show_eye_angle_labels = false;
  const auto axes_only = present(partial, axes);
  CHECK(!axes_only.eyes.primitives.empty());
  CHECK(axes_only.eyes.text_annotations.empty());
  for (const auto& primitive : axes_only.eyes.primitives)
    CHECK(primitive.type != overlay::PrimitiveType::Polygon);
  return true;
}
int main() {
  if (!joinsByKeyAndPreservesCoordinates() || !rejectsStaleOrUnboundProducts() ||
      !mismatchesDuplicatesAndEmptyFrames() ||
      !invalidInferenceGeometryDoesNotSuppressIndependentProducts() ||
      !contoursJoinOnlyExactBoundRowsAndDoNotBlockFills() ||
      !eyesRequireExactSourceFrameAndKeys()) return 1;
  std::cout << "canonical_overlay_presentation_tests passed\n";
}
