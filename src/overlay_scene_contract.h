#pragma once

#include "coordinate_contract.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace crimson::overlay {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool valid() const;
    bool contains(Point point) const;
};

enum class FrameIdentityStatus : uint8_t {
    Exact,
    InvalidSurface,
    InvalidOverlay,
    ViewMismatch,
    FrameMismatch,
};

struct FrameIdentity {
    int surface_view = -1;
    int64_t surface_frame = -1;
    int overlay_view = -1;
    int64_t overlay_frame = -1;
};

FrameIdentityStatus evaluateFrameIdentity(const FrameIdentity& identity);
bool canComposite(const FrameIdentity& identity);

// All overlay geometry uses source_camera_continuous_pixels. Display
// coordinates use the same top-left, +x-right, +y-down orientation.
struct SourceViewportTransform {
    Rect visible_source;
    Rect display;

    bool valid() const;
    std::optional<Point> sourceToDisplay(Point point) const;
    std::optional<Point> displayToSource(Point point) const;
    std::optional<Rect> sourceToDisplay(const Rect& rect) const;
};

std::optional<Rect> intersectRects(const Rect& a, const Rect& b);

struct HeadingNormalizedCropTransform {
    double input_width = 0.0;
    double input_height = 0.0;
    double output_side = 0.0;
    double rotation_degrees = 0.0;

    bool valid() const;
    std::optional<Point> apply(Point crop_point) const;
};

// This is the maintained middle camera-plot order. The base image, YOLO
// contours, full-frame keypoint editor, and post-plot inset overlays bracket
// this ordered sequence in camera_view_window.cpp.
enum class CameraOverlayLayer : uint8_t {
    BoundingBoxes,
    BoundingBoxDraft,
    Chaser,
    MovementTrail,
    KeypointHeading,
    MovementLabel,
    SubjectMasks,
    SubjectShape,
    TailKinematics,
    SubjectMaskPicking,
    Keypoints,
};

inline constexpr std::array<CameraOverlayLayer, 11>
    kCameraOverlayLayerOrder = {
        CameraOverlayLayer::BoundingBoxes,
        CameraOverlayLayer::BoundingBoxDraft,
        CameraOverlayLayer::Chaser,
        CameraOverlayLayer::MovementTrail,
        CameraOverlayLayer::KeypointHeading,
        CameraOverlayLayer::MovementLabel,
        CameraOverlayLayer::SubjectMasks,
        CameraOverlayLayer::SubjectShape,
        CameraOverlayLayer::TailKinematics,
        CameraOverlayLayer::SubjectMaskPicking,
        CameraOverlayLayer::Keypoints,
};

std::string_view cameraOverlayLayerName(CameraOverlayLayer layer);

enum class CropOverlayLayer : uint8_t {
    SkeletonEdges,
    KeypointMarkers,
    StoredHeading,
    CandidateHeading,
};

inline constexpr std::array<CropOverlayLayer, 4> kCropOverlayLayerOrder = {
    CropOverlayLayer::SkeletonEdges,
    CropOverlayLayer::KeypointMarkers,
    CropOverlayLayer::StoredHeading,
    CropOverlayLayer::CandidateHeading,
};

std::string_view cropOverlayLayerName(CropOverlayLayer layer);

struct ParityThresholds {
    double source_geometry_px = 0.25;
    double display_anchor_px = 0.5;
    double inverse_hit_test_source_px = 0.5;
    uint8_t raster_channel_delta = 3;
    double mask_iou = 0.995;
    double vector_coverage = 0.995;
    double vector_coverage_radius_px = 1.0;
    double vector_max_outlier_px = 2.0;
};

inline constexpr ParityThresholds kPhase5ParityThresholds{};

}  // namespace crimson::overlay
