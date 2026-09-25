#include "overlay_scene_contract.h"

#include <algorithm>
#include <cmath>

namespace crimson::overlay {
namespace {

bool finite(double value) {
    return std::isfinite(value);
}

}  // namespace

bool Rect::valid() const {
    return finite(x) && finite(y) && finite(width) && finite(height) &&
           width > 0.0 && height > 0.0;
}

bool Rect::contains(Point point) const {
    return valid() && finite(point.x) && finite(point.y) && point.x >= x &&
           point.y >= y && point.x <= x + width && point.y <= y + height;
}

FrameIdentityStatus evaluateFrameIdentity(const FrameIdentity& identity) {
    if (identity.surface_view < 0 || identity.surface_frame < 0) {
        return FrameIdentityStatus::InvalidSurface;
    }
    if (identity.overlay_view < 0 || identity.overlay_frame < 0) {
        return FrameIdentityStatus::InvalidOverlay;
    }
    if (identity.surface_view != identity.overlay_view) {
        return FrameIdentityStatus::ViewMismatch;
    }
    if (identity.surface_frame != identity.overlay_frame) {
        return FrameIdentityStatus::FrameMismatch;
    }
    return FrameIdentityStatus::Exact;
}

bool canComposite(const FrameIdentity& identity) {
    return evaluateFrameIdentity(identity) == FrameIdentityStatus::Exact;
}

bool SourceViewportTransform::valid() const {
    return visible_source.valid() && display.valid();
}

std::optional<Point> SourceViewportTransform::sourceToDisplay(
    Point point) const {
    if (!valid() || !finite(point.x) || !finite(point.y)) {
        return std::nullopt;
    }
    return Point{
        display.x +
            (point.x - visible_source.x) * display.width / visible_source.width,
        display.y + (point.y - visible_source.y) * display.height /
                        visible_source.height,
    };
}

std::optional<Point> SourceViewportTransform::displayToSource(
    Point point) const {
    if (!valid() || !finite(point.x) || !finite(point.y)) {
        return std::nullopt;
    }
    return Point{
        visible_source.x +
            (point.x - display.x) * visible_source.width / display.width,
        visible_source.y +
            (point.y - display.y) * visible_source.height / display.height,
    };
}

std::optional<Rect> SourceViewportTransform::sourceToDisplay(
    const Rect& rect) const {
    if (!valid() || !rect.valid()) {
        return std::nullopt;
    }
    const auto origin = sourceToDisplay(Point{rect.x, rect.y});
    if (!origin) {
        return std::nullopt;
    }
    return Rect{origin->x,
                origin->y,
                rect.width * display.width / visible_source.width,
                rect.height * display.height / visible_source.height};
}

std::optional<Rect> intersectRects(const Rect& a, const Rect& b) {
    if (!a.valid() || !b.valid()) {
        return std::nullopt;
    }
    const double x_min = std::max(a.x, b.x);
    const double y_min = std::max(a.y, b.y);
    const double x_max = std::min(a.x + a.width, b.x + b.width);
    const double y_max = std::min(a.y + a.height, b.y + b.height);
    if (x_max <= x_min || y_max <= y_min) {
        return std::nullopt;
    }
    return Rect{x_min, y_min, x_max - x_min, y_max - y_min};
}

bool HeadingNormalizedCropTransform::valid() const {
    return finite(input_width) && finite(input_height) && finite(output_side) &&
           finite(rotation_degrees) && input_width > 0.0 &&
           input_height > 0.0 && output_side > 0.0;
}

std::optional<Point> HeadingNormalizedCropTransform::apply(
    Point crop_point) const {
    if (!valid() || !finite(crop_point.x) || !finite(crop_point.y)) {
        return std::nullopt;
    }
    constexpr double kPi = 3.14159265358979323846;
    const double radians = rotation_degrees * kPi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double dx = crop_point.x - input_width * 0.5;
    const double dy = crop_point.y - input_height * 0.5;
    const double output_center = output_side * 0.5;
    return Point{c * dx + s * dy + output_center,
                 -s * dx + c * dy + output_center};
}

std::string_view cameraOverlayLayerName(CameraOverlayLayer layer) {
    switch (layer) {
        case CameraOverlayLayer::BoundingBoxes:
            return "bounding_boxes";
        case CameraOverlayLayer::BoundingBoxDraft:
            return "bounding_box_draft";
        case CameraOverlayLayer::Chaser:
            return "chaser";
        case CameraOverlayLayer::MovementTrail:
            return "movement_trail";
        case CameraOverlayLayer::KeypointHeading:
            return "keypoint_heading";
        case CameraOverlayLayer::MovementLabel:
            return "movement_label";
        case CameraOverlayLayer::SubjectMasks:
            return "subject_masks";
        case CameraOverlayLayer::SubjectShape:
            return "subject_shape";
        case CameraOverlayLayer::TailKinematics:
            return "tail_kinematics";
        case CameraOverlayLayer::SubjectMaskPicking:
            return "subject_mask_picking";
        case CameraOverlayLayer::Keypoints:
            return "keypoints";
    }
    return "unknown";
}

std::string_view cropOverlayLayerName(CropOverlayLayer layer) {
    switch (layer) {
        case CropOverlayLayer::SkeletonEdges:
            return "skeleton_edges";
        case CropOverlayLayer::KeypointMarkers:
            return "keypoint_markers";
        case CropOverlayLayer::StoredHeading:
            return "stored_heading";
        case CropOverlayLayer::CandidateHeading:
            return "candidate_heading";
    }
    return "unknown";
}

}  // namespace crimson::overlay
