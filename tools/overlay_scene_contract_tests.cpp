#include "crop_source_contract.h"
#include "overlay_scene_contract.h"
#include "tests/fixtures/overlay_scene_fixture.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__    \
                      << ": " #condition << '\n';                             \
            return false;                                                      \
        }                                                                      \
    } while (false)

bool near(double actual, double expected, double tolerance = 1e-9) {
    return std::abs(actual - expected) <= tolerance;
}

bool pointNear(crimson::overlay::Point actual,
               crimson::overlay::Point expected,
               double tolerance = 1e-9) {
    return near(actual.x, expected.x, tolerance) &&
           near(actual.y, expected.y, tolerance);
}

bool testFrameIdentity() {
    using namespace crimson::overlay;
    FrameIdentity identity{fixture::kView,
                           fixture::kFrame,
                           fixture::kView,
                           fixture::kFrame};
    CHECK(evaluateFrameIdentity(identity) == FrameIdentityStatus::Exact);
    CHECK(canComposite(identity));

    identity.overlay_frame--;
    CHECK(evaluateFrameIdentity(identity) == FrameIdentityStatus::FrameMismatch);
    CHECK(!canComposite(identity));
    identity.overlay_frame = fixture::kFrame;
    identity.overlay_view++;
    CHECK(evaluateFrameIdentity(identity) == FrameIdentityStatus::ViewMismatch);
    identity.overlay_view = fixture::kView;
    identity.surface_frame = -1;
    CHECK(evaluateFrameIdentity(identity) == FrameIdentityStatus::InvalidSurface);
    identity.surface_frame = fixture::kFrame;
    identity.overlay_frame = -1;
    CHECK(evaluateFrameIdentity(identity) == FrameIdentityStatus::InvalidOverlay);
    return true;
}

bool testViewportTransformAndClipping() {
    using namespace crimson::overlay;
    const SourceViewportTransform transform{fixture::kVisibleSource,
                                            fixture::kDisplay};
    CHECK(transform.valid());
    const auto display_point = transform.sourceToDisplay(fixture::kSourcePoint);
    CHECK(display_point.has_value());
    CHECK(pointNear(*display_point, fixture::kExpectedDisplayPoint));
    const auto round_trip = transform.displayToSource(*display_point);
    CHECK(round_trip.has_value());
    CHECK(pointNear(*round_trip, fixture::kSourcePoint));

    const auto clipped =
        intersectRects(fixture::kPartiallyVisibleRect, fixture::kVisibleSource);
    CHECK(clipped.has_value());
    CHECK(near(clipped->x, fixture::kExpectedClippedRect.x));
    CHECK(near(clipped->y, fixture::kExpectedClippedRect.y));
    CHECK(near(clipped->width, fixture::kExpectedClippedRect.width));
    CHECK(near(clipped->height, fixture::kExpectedClippedRect.height));
    const auto display_rect = transform.sourceToDisplay(*clipped);
    CHECK(display_rect.has_value());
    CHECK(near(display_rect->x, fixture::kExpectedDisplayRect.x));
    CHECK(near(display_rect->y, fixture::kExpectedDisplayRect.y));
    CHECK(near(display_rect->width, fixture::kExpectedDisplayRect.width));
    CHECK(near(display_rect->height, fixture::kExpectedDisplayRect.height));

    const Rect disjoint{1400.0, 900.0, 20.0, 20.0};
    CHECK(!intersectRects(disjoint, fixture::kVisibleSource).has_value());
    SourceViewportTransform invalid = transform;
    invalid.display.width = 0.0;
    CHECK(!invalid.valid());
    CHECK(!invalid.sourceToDisplay(fixture::kSourcePoint).has_value());
    CHECK(!transform
               .sourceToDisplay(Point{
                   std::numeric_limits<double>::quiet_NaN(), 1.0})
               .has_value());
    return true;
}

bool testCropTransforms() {
    using namespace crimson::overlay;
    const auto geometry = fixture::makeCropGeometry();
    CHECK(geometry.valid());
    const auto crop_point = geometry.fullFrameToCrop(fixture::kFullFramePoint);
    CHECK(crop_point.has_value());
    CHECK(near(crop_point->x, fixture::kExpectedCropPoint.x));
    CHECK(near(crop_point->y, fixture::kExpectedCropPoint.y));
    const auto crop_detection =
        geometry.fullFrameToCrop(*geometry.full_frame_detection);
    CHECK(crop_detection.has_value());
    CHECK(near(crop_detection->x, fixture::kExpectedCropDetection.x));
    CHECK(near(crop_detection->y, fixture::kExpectedCropDetection.y));
    CHECK(near(crop_detection->width, fixture::kExpectedCropDetection.width));
    CHECK(near(crop_detection->height, fixture::kExpectedCropDetection.height));

    const auto rotated = fixture::kRotation.apply(fixture::kCropRotationPoint);
    CHECK(rotated.has_value());
    CHECK(pointNear(*rotated, fixture::kExpectedRotatedPoint));
    HeadingNormalizedCropTransform invalid = fixture::kRotation;
    invalid.output_side = 0.0;
    CHECK(!invalid.apply(fixture::kCropRotationPoint).has_value());
    return true;
}

bool testCanonicalLayerOrder() {
    using namespace crimson::overlay;
    constexpr std::array<std::string_view, 11> expected_camera_names = {
        "bounding_boxes",      "bounding_box_draft", "chaser",
        "movement_trail",      "keypoint_heading",   "movement_label",
        "subject_masks",       "subject_shape",      "tail_kinematics",
        "subject_mask_picking", "keypoints",
    };
    for (size_t i = 0; i < kCameraOverlayLayerOrder.size(); ++i) {
        CHECK(cameraOverlayLayerName(kCameraOverlayLayerOrder[i]) ==
              expected_camera_names[i]);
    }

    constexpr std::array<std::string_view, 4> expected_crop_names = {
        "skeleton_edges", "keypoint_markers", "stored_heading",
        "candidate_heading"};
    for (size_t i = 0; i < kCropOverlayLayerOrder.size(); ++i) {
        CHECK(cropOverlayLayerName(kCropOverlayLayerOrder[i]) ==
              expected_crop_names[i]);
    }

    CHECK(near(kPhase5ParityThresholds.source_geometry_px, 0.25));
    CHECK(near(kPhase5ParityThresholds.display_anchor_px, 0.5));
    CHECK(near(kPhase5ParityThresholds.inverse_hit_test_source_px, 0.5));
    CHECK(kPhase5ParityThresholds.raster_channel_delta == 3);
    CHECK(near(kPhase5ParityThresholds.mask_iou, 0.995));
    CHECK(near(kPhase5ParityThresholds.vector_coverage, 0.995));
    CHECK(near(kPhase5ParityThresholds.vector_coverage_radius_px, 1.0));
    CHECK(near(kPhase5ParityThresholds.vector_max_outlier_px, 2.0));
    return true;
}

}  // namespace

int main() {
    if (!testFrameIdentity() || !testViewportTransformAndClipping() ||
        !testCropTransforms() || !testCanonicalLayerOrder()) {
        return 1;
    }
    std::cout << "overlay_scene_contract_tests: PASS\n";
    return 0;
}
