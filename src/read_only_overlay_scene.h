#pragma once

#include "overlay_scene_contract.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crimson::overlay {

struct Color {
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    float alpha = 1.0f;

    bool valid() const;
};

enum class BoxProvenance : uint8_t {
    Clean,
    Interpolated,
    Manual,
};

struct DetectionBoxInput {
    Rect rect;
    int class_id = 0;
    BoxProvenance provenance = BoxProvenance::Clean;
    bool selected = false;
    bool added = false;
    bool frame_modified = false;
};

struct DetectionOverlayInput {
    std::optional<DetectionBoxInput> box;
    std::vector<Point> keypoints;
    std::optional<Point> heading_origin;
    std::optional<double> heading_degrees;
    bool heading_valid = false;
    bool detection_interpolated = false;
    bool suppress_keypoint_markers = false;
    bool refined_keypoints = false;
    bool keypoint_usable = true;
    bool keypoint_detection_interpolated = false;
    bool keypoint_flip_corrected = false;
};

struct ReadOnlyOverlayInput {
    FrameIdentity identity;
    double source_width = 0.0;
    double source_height = 0.0;
    std::vector<std::string> keypoint_labels;
    std::vector<std::array<size_t, 2>> skeleton_edges;
    std::vector<DetectionOverlayInput> detections;
    bool show_boxes = true;
    bool show_headings = true;
    bool show_keypoints = true;
};

enum class MarkerShape : uint8_t {
    Circle,
    Square,
    Diamond,
    Cross,
    Plus,
    TriangleUp,
    TriangleDown,
};

enum class PrimitiveType : uint8_t {
    Polyline,
    Marker,
    Arrow,
};

struct Primitive {
    PrimitiveType type = PrimitiveType::Polyline;
    CameraOverlayLayer layer = CameraOverlayLayer::BoundingBoxes;
    std::vector<Point> points;
    Color stroke;
    Color fill;
    Color outline;
    double stroke_width_px = 1.0;
    double marker_size_px = 0.0;
    double outline_width_px = 0.0;
    double arrow_head_size_px = 0.0;
    MarkerShape marker_shape = MarkerShape::Circle;
    std::string label;
};

enum class ReadOnlyOverlayBuildStatus : uint8_t {
    Ready,
    InvalidIdentity,
    InvalidDimensions,
};

struct ReadOnlyOverlayScene {
    ReadOnlyOverlayBuildStatus status =
        ReadOnlyOverlayBuildStatus::InvalidIdentity;
    FrameIdentity identity;
    double source_width = 0.0;
    double source_height = 0.0;
    std::vector<Primitive> primitives;

    bool ready() const;
    size_t count(PrimitiveType type) const;
    size_t count(CameraOverlayLayer layer) const;
};

ReadOnlyOverlayScene buildReadOnlyOverlayScene(
    const ReadOnlyOverlayInput& input);

Color keypointColor(const std::string& label, size_t keypoint_index);
MarkerShape keypointMarkerShape(const std::string& label,
                                size_t keypoint_index);
double keypointMarkerSizePx(const std::string& label);

struct ScreenVertex {
    float x = 0.0f;
    float y = 0.0f;
    Color color;
};

struct ScreenMesh {
    std::vector<ScreenVertex> triangle_vertices;
    size_t primitive_count = 0;

    size_t triangleCount() const { return triangle_vertices.size() / 3; }
};

ScreenMesh tessellateReadOnlyOverlayScene(
    const ReadOnlyOverlayScene& scene,
    const SourceViewportTransform& transform,
    size_t circle_segment_count = 24);

}  // namespace crimson::overlay
