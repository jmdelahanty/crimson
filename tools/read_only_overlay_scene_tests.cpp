#include "read_only_overlay_scene.h"
#include "tests/fixtures/read_only_overlay_scene_fixture.h"

#include <cmath>
#include <limits>
#include <iostream>
#include <utility>

namespace {

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__    \
                      << ": " #condition << '\n';                             \
            return false;                                                      \
        }                                                                      \
    } while (false)

bool near(double actual, double expected, double tolerance = 1e-6) {
    return std::abs(actual - expected) <= tolerance;
}

bool testDeterministicScene() {
    using namespace crimson::overlay;
    const auto input = fixture::makeReadOnlyOverlayInput();
    const auto scene = buildReadOnlyOverlayScene(input);
    CHECK(scene.ready());
    CHECK(scene.primitives.size() == fixture::kExpectedPrimitiveCount);
    CHECK(scene.count(CameraOverlayLayer::BoundingBoxes) ==
          fixture::kExpectedBoxCount);
    CHECK(scene.count(CameraOverlayLayer::KeypointHeading) ==
          fixture::kExpectedHeadingCount);
    CHECK(scene.count(CameraOverlayLayer::Keypoints) ==
          fixture::kExpectedSkeletonCount + fixture::kExpectedMarkerCount);
    CHECK(scene.count(PrimitiveType::Polyline) ==
          fixture::kExpectedBoxCount + fixture::kExpectedSkeletonCount);
    CHECK(scene.count(PrimitiveType::Marker) == fixture::kExpectedMarkerCount);
    CHECK(scene.count(PrimitiveType::Arrow) == fixture::kExpectedHeadingCount);

    CHECK(scene.primitives[0].label == "Zarr_2");
    CHECK(near(scene.primitives[0].stroke.blue, 1.0));
    CHECK(near(scene.primitives[0].stroke_width_px, 2.0));
    CHECK(scene.primitives[1].label == "Zarr_4 [I]");
    CHECK(near(scene.primitives[1].stroke.red, 1.0));
    CHECK(near(scene.primitives[1].stroke.green, 0.7));
    CHECK(near(scene.primitives[1].stroke_width_px, 2.5));

    const Primitive& heading = scene.primitives[2];
    CHECK(heading.type == PrimitiveType::Arrow);
    CHECK(near(heading.points[0].x, fixture::kExpectedHeadingStart.x));
    CHECK(near(heading.points[0].y, fixture::kExpectedHeadingStart.y));
    CHECK(near(heading.points[1].x, fixture::kExpectedHeadingEnd.x));
    CHECK(near(heading.points[1].y, fixture::kExpectedHeadingEnd.y));
    CHECK(near(heading.arrow_head_size_px, 8.0));

    const Primitive& clean_swim_marker = scene.primitives[5];
    CHECK(clean_swim_marker.type == PrimitiveType::Marker);
    CHECK(clean_swim_marker.marker_shape == MarkerShape::Circle);
    CHECK(near(clean_swim_marker.marker_size_px, 4.5));
    CHECK(near(clean_swim_marker.fill.red, 1.0));
    CHECK(near(clean_swim_marker.fill.green, 0.85));

    const Primitive& interpolated_swim_marker = scene.primitives[9];
    CHECK(interpolated_swim_marker.type == PrimitiveType::Marker);
    CHECK(near(interpolated_swim_marker.fill.alpha, 0.25));
    CHECK(near(interpolated_swim_marker.outline.red, 0.95));
    CHECK(near(interpolated_swim_marker.outline.green, 0.3));
    return true;
}

bool testIdentityAndOptions() {
    using namespace crimson::overlay;
    auto input = fixture::makeReadOnlyOverlayInput();
    input.identity.overlay_frame--;
    const auto stale = buildReadOnlyOverlayScene(input);
    CHECK(!stale.ready());
    CHECK(stale.status == ReadOnlyOverlayBuildStatus::InvalidIdentity);
    CHECK(stale.primitives.empty());

    input = fixture::makeReadOnlyOverlayInput();
    input.presentation_space = crimson::coordinates::kRoiContinuousPixels;
    const auto invalid_space = buildReadOnlyOverlayScene(input);
    CHECK(invalid_space.status ==
          ReadOnlyOverlayBuildStatus::InvalidCoordinateSpace);
    CHECK(!invalid_space.ready());

    input = fixture::makeReadOnlyOverlayInput();
    input.source_width = 0.0;
    const auto invalid_size = buildReadOnlyOverlayScene(input);
    CHECK(invalid_size.status == ReadOnlyOverlayBuildStatus::InvalidDimensions);

    input = fixture::makeReadOnlyOverlayInput();
    input.show_boxes = false;
    input.show_headings = false;
    const auto keypoints_only = buildReadOnlyOverlayScene(input);
    CHECK(keypoints_only.ready());
    CHECK(keypoints_only.count(CameraOverlayLayer::BoundingBoxes) == 0);
    CHECK(keypoints_only.count(CameraOverlayLayer::KeypointHeading) == 0);
    CHECK(keypoints_only.count(CameraOverlayLayer::Keypoints) ==
          fixture::kExpectedSkeletonCount + fixture::kExpectedMarkerCount);
    return true;
}

bool testStyleVariants() {
    using namespace crimson::overlay;
    ReadOnlyOverlayInput input;
    input.identity = {0, 9, 0, 9};
    input.source_width = 200.0;
    input.source_height = 100.0;
    input.show_headings = false;
    input.show_keypoints = false;
    const auto detectionWithBox = [](DetectionBoxInput box) {
        DetectionOverlayInput detection;
        detection.box = std::move(box);
        return detection;
    };
    input.detections = {
        detectionWithBox(DetectionBoxInput{
            {10.0, 10.0, 20.0, 20.0}, 1, BoxProvenance::Manual}),
        detectionWithBox(DetectionBoxInput{
            {40.0, 10.0, 20.0, 20.0}, 2, BoxProvenance::Manual, true}),
        detectionWithBox(DetectionBoxInput{
            {70.0, 10.0, 20.0, 20.0}, 3, BoxProvenance::Clean, false, true}),
        detectionWithBox(DetectionBoxInput{{100.0, 10.0, 20.0, 20.0},
                                           4,
                                           BoxProvenance::Clean,
                                           false,
                                           false,
                                           true}),
    };
    const auto boxes = buildReadOnlyOverlayScene(input);
    CHECK(boxes.primitives.size() == 4);
    CHECK(boxes.primitives[0].label == "Zarr_1 [MAN]");
    CHECK(near(boxes.primitives[0].stroke.green, 0.85));
    CHECK(near(boxes.primitives[0].stroke_width_px, 2.75));
    CHECK(boxes.primitives[1].label == "Zarr_2 [MAN] [S]");
    CHECK(near(boxes.primitives[1].stroke.red, 1.0));
    CHECK(near(boxes.primitives[1].stroke.blue, 0.95));
    CHECK(near(boxes.primitives[1].stroke_width_px, 3.5));
    CHECK(boxes.primitives[2].label == "Zarr_3 [A]");
    CHECK(near(boxes.primitives[2].stroke.red, 0.95));
    CHECK(near(boxes.primitives[2].stroke_width_px, 3.0));
    CHECK(boxes.primitives[3].label == "Zarr_4 [M]");
    CHECK(near(boxes.primitives[3].stroke_width_px, 2.5));

    input.show_boxes = false;
    input.show_keypoints = true;
    input.keypoint_labels.assign(7, "keypoint");
    input.detections.clear();
    DetectionOverlayInput keypoints;
    keypoints.heading_valid = true;
    for (size_t index = 0; index < 7; ++index) {
        keypoints.keypoints.push_back(
            {20.0 + static_cast<double>(index) * 20.0, 60.0});
    }
    input.detections.push_back(std::move(keypoints));
    const auto markers = buildReadOnlyOverlayScene(input);
    CHECK(markers.count(PrimitiveType::Marker) == 7);
    for (size_t index = 0; index < 7; ++index) {
        CHECK(markers.primitives[index].marker_shape ==
              keypointMarkerShape("keypoint", index));
    }
    const SourceViewportTransform transform{{0.0, 0.0, 200.0, 100.0},
                                            {0.0, 0.0, 200.0, 100.0}};
    const auto marker_mesh =
        tessellateReadOnlyOverlayScene(markers, transform, 12);
    CHECK(marker_mesh.primitive_count == 7);
    CHECK(marker_mesh.triangleCount() > 7);
    return true;
}

bool testScreenMesh() {
    using namespace crimson::overlay;
    const auto scene =
        buildReadOnlyOverlayScene(fixture::makeReadOnlyOverlayInput());
    const SourceViewportTransform transform{{0.0, 0.0, 640.0, 360.0},
                                            {0.0, 0.0, 320.0, 180.0}};
    const auto mesh = tessellateReadOnlyOverlayScene(scene, transform, 16);
    CHECK(mesh.primitive_count == fixture::kExpectedPrimitiveCount);
    CHECK(mesh.triangleCount() > fixture::kExpectedPrimitiveCount);
    CHECK(!mesh.triangle_vertices.empty());
    for (const auto& vertex : mesh.triangle_vertices) {
        CHECK(std::isfinite(vertex.x));
        CHECK(std::isfinite(vertex.y));
        CHECK(vertex.color.valid());
    }

    SourceViewportTransform invalid = transform;
    invalid.display.height = 0.0;
    CHECK(tessellateReadOnlyOverlayScene(scene, invalid).triangle_vertices.empty());
    return true;
}

bool testIndependentContoursAndShapeDiagnostics() {
    using namespace crimson::overlay;
    ReadOnlyOverlayInput input;
    input.identity = {0, 9, 0, 9};
    input.source_width = 200.0;
    input.source_height = 100.0;
    input.show_boxes = input.show_keypoints = input.show_headings = false;
    input.show_subject_mask_fills = false;
    input.show_subject_mask_contours = true;
    input.independent_mask_contours = true;
    input.show_subject_body_mask = false;
    input.show_subject_body_contour = true;
    SubjectMaskComponentInput mask;
    mask.label = "subject_body";
    mask.source_rect = {10, 20, 30, 30};
    mask.mask_width = mask.mask_height = 2;
    mask.mask = std::make_shared<const std::vector<uint8_t>>(4, 1);
    mask.contour = {{10, 20}, {40, 20}, {40, 50}};
    mask.instance_key = std::numeric_limits<uint64_t>::max();
    mask.instance_key_valid = true;
    input.subject_masks.push_back(mask);
    auto scene = buildReadOnlyOverlayScene(input);
    CHECK(scene.raster_masks.empty());
    CHECK(scene.primitives.size() == 1);
    CHECK(scene.primitives.front().instance_key == mask.instance_key);
    CHECK(scene.primitives.front().label.find("##mask_contour_subject_body_") == 0);

    input.show_subject_mask_fills = true;
    input.show_subject_body_contour = false;
    input.show_subject_body_mask = true;
    scene = buildReadOnlyOverlayScene(input);
    CHECK(scene.raster_masks.size() == 1);
    CHECK(scene.primitives.empty());

    input.show_subject_mask_fills = false;
    input.show_subject_shape_snout_tip = false;
    input.show_subject_shape_tail_base = false;
    input.show_subject_shape_tail_tip = false;
    input.show_subject_shape_caudal_anchor = false;
    input.show_subject_shape_centerline = false;
    input.show_subject_shape_bspline = false;
    input.show_subject_shape_bspline_debug_points = true;
    input.show_subject_shape_bspline_control_points = true;
    input.show_subject_shape_tail_samples = true;
    input.show_subject_shape_tail_normals = true;
    SubjectShapeInput shape;
    shape.shape_row = 1;
    shape.source_rect = {10, 20, 30, 30};
    shape.coordinate_width = shape.coordinate_height = 100;
    shape.bspline_sample = {{0, 0}, {10, 10}};
    shape.bspline_control_points = {{0, 0}, {20, 20}};
    shape.tail_samples = {{10, 10}};
    shape.tail_normals = {{1, 0}};
    shape.instance_key = 0;
    shape.instance_key_valid = true;
    input.subject_shapes.push_back(shape);
    scene = buildReadOnlyOverlayScene(input);
    CHECK(scene.primitives.empty());
    input.subject_shapes.front().bspline_valid = true;
    input.subject_shapes.front().tail_sample_valid = true;
    scene = buildReadOnlyOverlayScene(input);
    CHECK(scene.primitives.size() == 7);
    for (const auto &primitive : scene.primitives) {
        CHECK(primitive.instance_key == 0);
    }
    return true;
}

}  // namespace

int main() {
    if (!testDeterministicScene() || !testIdentityAndOptions() ||
        !testStyleVariants() || !testScreenMesh() ||
        !testIndependentContoursAndShapeDiagnostics()) {
        return 1;
    }
    std::cout << "read_only_overlay_scene_tests: PASS\n";
    return 0;
}
