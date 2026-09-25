#pragma once

#include "read_only_overlay_scene.h"

#include <limits>

namespace crimson::overlay::fixture {

inline ReadOnlyOverlayInput makeReadOnlyOverlayInput() {
    ReadOnlyOverlayInput input;
    input.identity = {1, 137, 1, 137};
    input.source_width = 640.0;
    input.source_height = 360.0;
    input.keypoint_labels = {"swim_bladder", "eye_left", "eye_right"};
    input.skeleton_edges = {{{0, 1}}, {{0, 2}}};

    DetectionOverlayInput clean;
    clean.box = DetectionBoxInput{{100.0, 80.0, 160.0, 120.0},
                                  2,
                                  BoxProvenance::Clean};
    clean.keypoints = {{160.0, 160.0}, {200.0, 130.0}, {200.0, 190.0}};
    clean.heading_origin = Point{160.0, 160.0};
    clean.heading_degrees = 15.0;
    clean.heading_valid = true;
    input.detections.push_back(clean);

    DetectionOverlayInput interpolated;
    interpolated.box = DetectionBoxInput{{360.0, 90.0, 120.0, 90.0},
                                         4,
                                         BoxProvenance::Interpolated};
    interpolated.keypoints = {
        {400.0, 140.0},
        {430.0, 120.0},
        {430.0, std::numeric_limits<double>::quiet_NaN()},
    };
    interpolated.heading_origin = Point{400.0, 140.0};
    interpolated.heading_degrees = -25.0;
    interpolated.heading_valid = true;
    interpolated.detection_interpolated = true;
    interpolated.refined_keypoints = true;
    interpolated.keypoint_usable = false;
    interpolated.keypoint_detection_interpolated = true;
    interpolated.keypoint_flip_corrected = true;
    input.detections.push_back(interpolated);
    return input;
}

inline constexpr size_t kExpectedBoxCount = 2;
inline constexpr size_t kExpectedHeadingCount = 1;
inline constexpr size_t kExpectedSkeletonCount = 3;
inline constexpr size_t kExpectedMarkerCount = 5;
inline constexpr size_t kExpectedPrimitiveCount = 11;
inline constexpr Point kExpectedHeadingStart{160.0, 160.0};
inline constexpr Point kExpectedHeadingEnd{293.33333333333331, 160.0};

}  // namespace crimson::overlay::fixture
