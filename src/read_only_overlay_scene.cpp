#include "read_only_overlay_scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace crimson::overlay {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool finite(double value) {
    return std::isfinite(value);
}

bool finite(Point point) {
    return finite(point.x) && finite(point.y);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

size_t findKeypoint(const std::vector<std::string>& labels,
                    bool swim_bladder,
                    bool left_eye,
                    bool right_eye) {
    for (size_t index = 0; index < labels.size(); ++index) {
        const std::string label = lowerCopy(labels[index]);
        const bool has_eye = label.find("eye") != std::string::npos;
        const bool has_left = label.find("left") != std::string::npos;
        const bool has_right = label.find("right") != std::string::npos;
        const bool has_swim = label.find("swim") != std::string::npos;
        const bool has_bladder = label.find("bladder") != std::string::npos;
        if (swim_bladder && has_swim && has_bladder) {
            return index;
        }
        if (left_eye && has_eye && has_left) {
            return index;
        }
        if (right_eye && has_eye && has_right) {
            return index;
        }
    }
    return std::numeric_limits<size_t>::max();
}

Color withAlpha(Color color, float alpha) {
    color.alpha = std::clamp(alpha, 0.0f, 1.0f);
    return color;
}

void appendBoxPrimitive(const DetectionBoxInput& box,
                        std::vector<Primitive>& primitives) {
    if (!box.rect.valid()) {
        return;
    }
    Color color{0.2f, 0.6f, 1.0f, 1.0f};
    double line_width = 2.0;
    if (box.provenance == BoxProvenance::Interpolated) {
        color = {1.0f, 0.7f, 0.0f, 0.9f};
        line_width = 2.5;
    } else if (box.provenance == BoxProvenance::Manual) {
        color = {0.0f, 0.85f, 0.65f, 1.0f};
        line_width = 2.75;
    }
    if (box.selected) {
        color = {1.0f, 0.25f, 0.95f, 1.0f};
        line_width = 3.5;
    } else if (box.added) {
        color = {0.95f, 0.35f, 0.15f, 1.0f};
        line_width = std::max(line_width, 3.0);
    } else if (box.frame_modified) {
        line_width = std::max(line_width, 2.5);
    }

    Primitive primitive;
    primitive.type = PrimitiveType::Polyline;
    primitive.layer = CameraOverlayLayer::BoundingBoxes;
    primitive.points = {
        {box.rect.x, box.rect.y},
        {box.rect.x + box.rect.width, box.rect.y},
        {box.rect.x + box.rect.width, box.rect.y + box.rect.height},
        {box.rect.x, box.rect.y + box.rect.height},
        {box.rect.x, box.rect.y},
    };
    primitive.stroke = color;
    primitive.stroke_width_px = line_width;
    primitive.label = "Zarr_" + std::to_string(box.class_id);
    if (box.provenance == BoxProvenance::Interpolated) {
        primitive.label += " [I]";
    } else if (box.provenance == BoxProvenance::Manual) {
        primitive.label += " [MAN]";
    }
    if (box.added) {
        primitive.label += " [A]";
    }
    if (box.selected) {
        primitive.label += " [S]";
    } else if (box.frame_modified) {
        primitive.label += " [M]";
    }
    primitives.push_back(std::move(primitive));
}

void appendHeadingPrimitive(const ReadOnlyOverlayInput& input,
                            const DetectionOverlayInput& detection,
                            size_t detection_index,
                            std::vector<Primitive>& primitives) {
    if (!detection.heading_valid || detection.detection_interpolated ||
        !detection.box || !detection.box->rect.valid() ||
        !detection.heading_origin) {
        return;
    }
    const Rect& box = detection.box->rect;
    Point start = *detection.heading_origin;
    if (!finite(start)) {
        start = {box.x + box.width * 0.5, box.y + box.height * 0.5};
    }

    Point end;
    bool used_keypoints = false;
    const size_t swim = findKeypoint(input.keypoint_labels, true, false, false);
    const size_t left = findKeypoint(input.keypoint_labels, false, true, false);
    const size_t right = findKeypoint(input.keypoint_labels, false, false, true);
    if (swim != std::numeric_limits<size_t>::max() &&
        left != std::numeric_limits<size_t>::max() &&
        right != std::numeric_limits<size_t>::max() &&
        swim < detection.keypoints.size() &&
        left < detection.keypoints.size() &&
        right < detection.keypoints.size() && finite(detection.keypoints[swim]) &&
        finite(detection.keypoints[left]) && finite(detection.keypoints[right])) {
        start = detection.keypoints[swim];
        const Point eye_midpoint{
            (detection.keypoints[left].x + detection.keypoints[right].x) * 0.5,
            (detection.keypoints[left].y + detection.keypoints[right].y) * 0.5};
        const double dx = eye_midpoint.x - start.x;
        const double dy = eye_midpoint.y - start.y;
        const double direction_length = std::hypot(dx, dy);
        if (direction_length > 1e-3) {
            const double box_scale = std::max(box.width, box.height) * 1.25;
            const double frame_scale = input.source_height * 0.02;
            const double extension = std::max(20.0, direction_length * 0.35);
            const double arrow_length =
                std::max(direction_length + extension,
                         std::max(60.0, std::max(box_scale, frame_scale)));
            const double shortened =
                std::max(direction_length + 8.0, arrow_length * (2.0 / 3.0));
            end = {start.x + dx / direction_length * shortened,
                   start.y + dy / direction_length * shortened};
            used_keypoints = true;
        }
    }

    if (!used_keypoints) {
        if (!detection.heading_degrees ||
            !finite(*detection.heading_degrees)) {
            return;
        }
        const double radians = *detection.heading_degrees * kPi / 180.0;
        const double arrow_length =
            std::max(60.0,
                     std::max(std::max(box.width, box.height) * 1.25,
                              input.source_height * 0.02)) *
            (2.0 / 3.0);
        end = {start.x + std::cos(radians) * arrow_length,
               start.y - std::sin(radians) * arrow_length};
    }

    Primitive primitive;
    primitive.type = PrimitiveType::Arrow;
    primitive.layer = CameraOverlayLayer::KeypointHeading;
    primitive.points = {start, end};
    primitive.stroke = {1.0f, 0.25f, 0.1f, 0.95f};
    primitive.fill = primitive.stroke;
    primitive.stroke_width_px = 2.0;
    primitive.arrow_head_size_px = 8.0;
    primitive.label = "##heading_" + std::to_string(detection_index);
    primitives.push_back(std::move(primitive));
}

void appendKeypointPrimitives(const ReadOnlyOverlayInput& input,
                              const DetectionOverlayInput& detection,
                              size_t detection_index,
                              std::vector<Primitive>& primitives) {
    for (const auto& edge : input.skeleton_edges) {
        if (edge[0] >= detection.keypoints.size() ||
            edge[1] >= detection.keypoints.size()) {
            continue;
        }
        const Point a = detection.keypoints[edge[0]];
        const Point b = detection.keypoints[edge[1]];
        if (!finite(a) || !finite(b)) {
            continue;
        }
        Primitive primitive;
        primitive.type = PrimitiveType::Polyline;
        primitive.layer = CameraOverlayLayer::Keypoints;
        primitive.points = {a, b};
        primitive.stroke = {1.0f, 1.0f, 1.0f, 0.63f};
        primitive.stroke_width_px = 1.0;
        primitive.label = "##edge_" + std::to_string(detection_index) + "_" +
                          std::to_string(edge[0]) + "_" +
                          std::to_string(edge[1]);
        primitives.push_back(std::move(primitive));
    }

    if (detection.suppress_keypoint_markers) {
        return;
    }
    for (size_t index = 0; index < detection.keypoints.size(); ++index) {
        const Point point = detection.keypoints[index];
        if (!finite(point)) {
            continue;
        }
        const std::string label =
            index < input.keypoint_labels.size() ? input.keypoint_labels[index]
                                                 : std::string{};
        float alpha = 1.0f;
        if (!detection.heading_valid) {
            alpha *= 0.4f;
        }
        if (detection.detection_interpolated) {
            alpha *= 0.65f;
        }
        bool unusable = false;
        if (detection.refined_keypoints) {
            if (!detection.keypoint_usable) {
                alpha *= 0.35f;
                unusable = true;
            }
            if (detection.keypoint_detection_interpolated) {
                alpha *= 0.65f;
            }
        }
        alpha = std::clamp(alpha, 0.25f, 1.0f);
        const Color base = keypointColor(label, index);

        Primitive primitive;
        primitive.type = PrimitiveType::Marker;
        primitive.layer = CameraOverlayLayer::Keypoints;
        primitive.points = {point};
        primitive.marker_shape = keypointMarkerShape(label, index);
        primitive.marker_size_px = keypointMarkerSizePx(label);
        primitive.fill = withAlpha(base, base.alpha * alpha);
        primitive.outline = withAlpha(base, std::max(alpha, 0.6f));
        if (detection.keypoint_flip_corrected) {
            primitive.outline = {0.0f, 0.9f, 0.9f, primitive.outline.alpha};
        }
        if (unusable) {
            primitive.outline = {0.95f, 0.3f, 0.3f, primitive.outline.alpha};
        }
        primitive.outline_width_px = 2.0;
        primitive.label = "##kp_" + std::to_string(detection_index) + "_" +
                          std::to_string(index);
        primitives.push_back(std::move(primitive));
    }
}

void appendSubjectMasks(const ReadOnlyOverlayInput& input,
                        ReadOnlyOverlayScene& scene) {
    std::vector<const SubjectMaskComponentInput*> components;
    components.reserve(input.subject_masks.size());
    for (const auto& component : input.subject_masks) {
        components.push_back(&component);
    }
    std::stable_sort(
        components.begin(), components.end(),
        [](const auto* first, const auto* second) {
            return subjectMaskComponentRank(first->label) <
                   subjectMaskComponentRank(second->label);
        });

    for (const auto* component : components) {
        const Color color = subjectMaskColor(component->label);
        if (input.show_subject_mask_fills && component->source_rect.valid() &&
            component->mask && component->mask_width > 0 &&
            component->mask_height > 0 &&
            component->mask_width <=
                std::numeric_limits<size_t>::max() /
                    component->mask_height &&
            component->mask->size() ==
                component->mask_width * component->mask_height) {
            RasterMask raster;
            raster.source_rect = component->source_rect;
            raster.width = component->mask_width;
            raster.height = component->mask_height;
            raster.alpha = component->mask;
            raster.color = color;
            raster.label = component->label;
            raster.cache_key = component->cache_namespace + ":" +
                component->label + ":" +
                std::to_string(component->source_crop_row_id) + ":" +
                std::to_string(component->channel_index);
            scene.raster_masks.push_back(std::move(raster));
        }
        if (input.show_subject_mask_contours &&
            component->contour.size() > 1) {
            Primitive contour;
            contour.type = PrimitiveType::Polyline;
            contour.layer = CameraOverlayLayer::SubjectMasks;
            contour.points = component->contour;
            if (contour.points.size() > 2) {
                const Point first = contour.points.front();
                const Point last = contour.points.back();
                if (std::hypot(first.x - last.x, first.y - last.y) > 1e-3) {
                    contour.points.push_back(first);
                }
            }
            contour.stroke = withAlpha(color, 0.95f);
            contour.stroke_width_px =
                component->label == "subject_body" ? 1.5 : 1.75;
            contour.label = "##mask_contour_" + component->label + "_" +
                std::to_string(component->source_crop_row_id);
            scene.primitives.push_back(std::move(contour));
        }
    }
}

Point subjectShapePoint(const SubjectShapeInput& shape, Point point) {
    if (!shape.source_rect.valid() || !finite(point) ||
        !finite(shape.coordinate_width) || !finite(shape.coordinate_height) ||
        shape.coordinate_width <= 0.0 || shape.coordinate_height <= 0.0) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan};
    }
    return {shape.source_rect.x + point.x / shape.coordinate_width *
                                      shape.source_rect.width,
            shape.source_rect.y + point.y / shape.coordinate_height *
                                      shape.source_rect.height};
}

std::vector<Point> subjectShapePoints(const SubjectShapeInput& shape,
                                      const std::vector<Point>& points) {
    std::vector<Point> converted;
    converted.reserve(points.size());
    for (const Point point : points) {
        const Point scene = subjectShapePoint(shape, point);
        if (finite(scene)) {
            converted.push_back(scene);
        }
    }
    return converted;
}

void appendShapePolyline(const SubjectShapeInput& shape,
                         const std::vector<Point>& points,
                         const std::string& label,
                         Color color,
                         double width,
                         std::vector<Primitive>& primitives) {
    auto converted = subjectShapePoints(shape, points);
    if (converted.size() < 2) {
        return;
    }
    Primitive primitive;
    primitive.type = PrimitiveType::Polyline;
    primitive.layer = CameraOverlayLayer::SubjectShape;
    primitive.points = std::move(converted);
    primitive.stroke = color;
    primitive.stroke_width_px = width;
    primitive.label = label;
    primitives.push_back(std::move(primitive));
}

void appendShapeMarker(const SubjectShapeInput& shape,
                       Point point,
                       const std::string& label,
                       MarkerShape marker,
                       double size,
                       Color fill,
                       std::vector<Primitive>& primitives) {
    const Point converted = subjectShapePoint(shape, point);
    if (!finite(converted)) {
        return;
    }
    Primitive primitive;
    primitive.type = PrimitiveType::Marker;
    primitive.layer = CameraOverlayLayer::SubjectShape;
    primitive.points = {converted};
    primitive.marker_shape = marker;
    primitive.marker_size_px = size;
    primitive.fill = fill;
    primitive.outline = marker == MarkerShape::Cross
                            ? fill
                            : Color{0.0f, 0.0f, 0.0f, 0.9f};
    primitive.outline_width_px = marker == MarkerShape::Cross ? 2.0 : 1.5;
    primitive.label = label;
    primitives.push_back(std::move(primitive));
}

void appendBodyAxis(const SubjectShapeInput& shape,
                    Point axis,
                    const std::string& label,
                    Color color,
                    std::vector<Primitive>& primitives) {
    if (!shape.body_frame_valid || !finite(shape.body_origin) || !finite(axis)) {
        return;
    }
    const double length = std::hypot(axis.x, axis.y);
    if (length <= 1e-5) {
        return;
    }
    const double axis_length =
        std::max(shape.coordinate_width, shape.coordinate_height) * 0.12;
    appendShapePolyline(
        shape,
        {shape.body_origin,
         {shape.body_origin.x + axis.x / length * axis_length,
          shape.body_origin.y + axis.y / length * axis_length}},
        label, color, 2.0, primitives);
}

void appendSubjectShapes(const ReadOnlyOverlayInput& input,
                         std::vector<Primitive>& primitives) {
    if (!input.show_subject_shape) {
        return;
    }
    std::vector<const SubjectShapeInput*> shapes;
    shapes.reserve(input.subject_shapes.size());
    for (const auto& shape : input.subject_shapes) {
        shapes.push_back(&shape);
    }
    std::stable_sort(shapes.begin(), shapes.end(), [](const auto* first,
                                                      const auto* second) {
        if (first->detection_index != second->detection_index) {
            return first->detection_index < second->detection_index;
        }
        return first->shape_row < second->shape_row;
    });

    for (const SubjectShapeInput* shape : shapes) {
        if (!shape->source_rect.valid() || shape->coordinate_width <= 0.0 ||
            shape->coordinate_height <= 0.0) {
            continue;
        }
        const std::string suffix = std::to_string(shape->shape_row);
        if (input.show_subject_shape_body_axes) {
            appendBodyAxis(*shape, shape->body_forward_axis,
                           "##shape_body_forward_" + suffix,
                           {1.0f, 0.55f, 0.15f, 0.9f}, primitives);
            appendBodyAxis(*shape, shape->body_left_axis,
                           "##shape_body_left_" + suffix,
                           {0.1f, 0.9f, 0.95f, 0.85f}, primitives);
        }
        if (input.show_subject_shape_centerline && shape->centerline_valid) {
            appendShapePolyline(*shape, shape->centerline,
                                "##shape_centerline_" + suffix,
                                {1.0f, 0.92f, 0.25f, 0.95f}, 2.2,
                                primitives);
        }
        if (input.show_subject_shape_bspline && shape->bspline_valid) {
            appendShapePolyline(*shape, shape->bspline_sample,
                                "##shape_bspline_" + suffix,
                                {0.2f, 1.0f, 0.7f, 0.95f}, 2.0, primitives);
        }
        if (input.show_subject_shape_bspline_debug_points &&
            shape->bspline_valid) {
            for (size_t index = 0; index < shape->bspline_sample.size(); ++index) {
                appendShapeMarker(*shape, shape->bspline_sample[index],
                                  "##shape_bspline_debug_" + suffix + "_" +
                                      std::to_string(index),
                                  MarkerShape::Circle, 2.4,
                                  {0.2f, 1.0f, 0.7f, 0.45f}, primitives);
            }
        }
        if (input.show_subject_shape_bspline_control_points) {
            appendShapePolyline(*shape, shape->bspline_control_points,
                                "##shape_bspline_controls_" + suffix,
                                {0.2f, 0.9f, 0.7f, 0.35f}, 1.0, primitives);
            for (size_t index = 0; index < shape->bspline_control_points.size();
                 ++index) {
                appendShapeMarker(*shape, shape->bspline_control_points[index],
                                  "##shape_bspline_control_" + suffix + "_" +
                                      std::to_string(index),
                                  MarkerShape::Square, 4.0,
                                  {0.2f, 1.0f, 0.7f, 0.65f}, primitives);
            }
        }
        if (input.show_subject_shape_tail_samples &&
            shape->tail_sample_valid) {
            for (size_t index = 0; index < shape->tail_samples.size(); ++index) {
                appendShapeMarker(*shape, shape->tail_samples[index],
                                  "##shape_tail_sample_" + suffix + "_" +
                                      std::to_string(index),
                                  MarkerShape::Circle, 3.0,
                                  {0.85f, 0.6f, 1.0f, 0.7f}, primitives);
            }
        }
        if (input.show_subject_shape_tail_normals &&
            shape->tail_sample_valid &&
            shape->tail_samples.size() == shape->tail_normals.size()) {
            const double normal_length =
                std::max(shape->coordinate_width, shape->coordinate_height) *
                0.035;
            for (size_t index = 0; index < shape->tail_samples.size(); ++index) {
                const Point sample = shape->tail_samples[index];
                const Point normal = shape->tail_normals[index];
                const double length = std::hypot(normal.x, normal.y);
                if (!finite(sample) || !finite(normal) || length <= 1e-5) {
                    continue;
                }
                const Point delta{normal.x / length * normal_length,
                                  normal.y / length * normal_length};
                appendShapePolyline(
                    *shape,
                    {{sample.x - delta.x, sample.y - delta.y},
                     {sample.x + delta.x, sample.y + delta.y}},
                    "##shape_tail_normal_" + suffix + "_" +
                        std::to_string(index),
                    {0.65f, 0.95f, 1.0f, 0.65f}, 1.0, primitives);
            }
        }
        if (input.show_subject_shape_snout_tip && shape->snout_tip_valid) {
            appendShapeMarker(*shape, shape->snout_tip,
                              "##shape_snout_" + suffix, MarkerShape::Circle,
                              7.0, {1.0f, 0.45f, 0.1f, 0.95f}, primitives);
        }
        if (input.show_subject_shape_tail_base && shape->tail_base_valid) {
            appendShapeMarker(*shape, shape->tail_base,
                              "##shape_tail_base_" + suffix,
                              MarkerShape::Diamond, 6.5,
                              {0.95f, 0.55f, 1.0f, 0.9f}, primitives);
        }
        if (input.show_subject_shape_tail_tip) {
            appendShapeMarker(*shape, shape->tail_tip,
                              "##shape_tail_tip_" + suffix,
                              MarkerShape::Cross, 6.5,
                              {0.75f, 0.45f, 1.0f, 0.9f}, primitives);
        }
        if (input.show_subject_shape_caudal_anchor &&
            shape->caudal_anchor_valid) {
            appendShapeMarker(*shape, shape->caudal_anchor,
                              "##shape_caudal_anchor_" + suffix,
                              MarkerShape::TriangleUp, 6.5,
                              {0.25f, 0.75f, 1.0f, 0.95f}, primitives);
        }
    }
}

void appendTriangle(ScreenMesh& mesh,
                    Point a,
                    Point b,
                    Point c,
                    Color color) {
    mesh.triangle_vertices.push_back(
        {static_cast<float>(a.x), static_cast<float>(a.y), color});
    mesh.triangle_vertices.push_back(
        {static_cast<float>(b.x), static_cast<float>(b.y), color});
    mesh.triangle_vertices.push_back(
        {static_cast<float>(c.x), static_cast<float>(c.y), color});
}

void appendSegment(ScreenMesh& mesh,
                   Point a,
                   Point b,
                   double width,
                   Color color) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length = std::hypot(dx, dy);
    if (!finite(a) || !finite(b) || !finite(width) || width <= 0.0 ||
        length <= 1e-6) {
        return;
    }
    const double half_width = width * 0.5;
    const double nx = -dy / length * half_width;
    const double ny = dx / length * half_width;
    const Point a0{a.x + nx, a.y + ny};
    const Point a1{a.x - nx, a.y - ny};
    const Point b0{b.x + nx, b.y + ny};
    const Point b1{b.x - nx, b.y - ny};
    appendTriangle(mesh, a0, a1, b0, color);
    appendTriangle(mesh, b0, a1, b1, color);
}

std::vector<Point> markerPolygon(MarkerShape shape,
                                 Point center,
                                 double size) {
    switch (shape) {
        case MarkerShape::Square:
            return {{center.x - size, center.y - size},
                    {center.x + size, center.y - size},
                    {center.x + size, center.y + size},
                    {center.x - size, center.y + size}};
        case MarkerShape::Diamond:
            return {{center.x, center.y - size},
                    {center.x + size, center.y},
                    {center.x, center.y + size},
                    {center.x - size, center.y}};
        case MarkerShape::TriangleUp:
            return {{center.x, center.y - size},
                    {center.x + size, center.y + size},
                    {center.x - size, center.y + size}};
        case MarkerShape::TriangleDown:
            return {{center.x - size, center.y - size},
                    {center.x + size, center.y - size},
                    {center.x, center.y + size}};
        default:
            return {};
    }
}

void appendPolygon(ScreenMesh& mesh,
                   const std::vector<Point>& polygon,
                   Color fill,
                   Color outline,
                   double outline_width) {
    if (polygon.size() < 3) {
        return;
    }
    for (size_t index = 1; index + 1 < polygon.size(); ++index) {
        appendTriangle(mesh, polygon[0], polygon[index], polygon[index + 1],
                       fill);
    }
    for (size_t index = 0; index < polygon.size(); ++index) {
        appendSegment(mesh, polygon[index], polygon[(index + 1) % polygon.size()],
                      outline_width, outline);
    }
}

void appendCircle(ScreenMesh& mesh,
                  Point center,
                  double radius,
                  Color fill,
                  Color outline,
                  double outline_width,
                  size_t segments) {
    segments = std::max<size_t>(segments, 8);
    const double inner_radius = std::max(0.0, radius - outline_width * 0.5);
    const double outer_radius = radius + outline_width * 0.5;
    for (size_t index = 0; index < segments; ++index) {
        const double a0 = 2.0 * kPi * static_cast<double>(index) / segments;
        const double a1 =
            2.0 * kPi * static_cast<double>(index + 1) / segments;
        const Point inner0{center.x + std::cos(a0) * inner_radius,
                           center.y + std::sin(a0) * inner_radius};
        const Point inner1{center.x + std::cos(a1) * inner_radius,
                           center.y + std::sin(a1) * inner_radius};
        const Point outer0{center.x + std::cos(a0) * outer_radius,
                           center.y + std::sin(a0) * outer_radius};
        const Point outer1{center.x + std::cos(a1) * outer_radius,
                           center.y + std::sin(a1) * outer_radius};
        appendTriangle(mesh, center, inner0, inner1, fill);
        appendTriangle(mesh, inner0, outer0, outer1, outline);
        appendTriangle(mesh, inner0, outer1, inner1, outline);
    }
}

void appendMarker(ScreenMesh& mesh,
                  const Primitive& primitive,
                  Point center,
                  size_t circle_segments) {
    if (primitive.marker_shape == MarkerShape::Circle) {
        appendCircle(mesh, center, primitive.marker_size_px, primitive.fill,
                     primitive.outline, primitive.outline_width_px,
                     circle_segments);
        return;
    }
    if (primitive.marker_shape == MarkerShape::Cross) {
        const double size = primitive.marker_size_px;
        appendSegment(mesh, {center.x - size, center.y - size},
                      {center.x + size, center.y + size},
                      primitive.outline_width_px, primitive.outline);
        appendSegment(mesh, {center.x + size, center.y - size},
                      {center.x - size, center.y + size},
                      primitive.outline_width_px, primitive.outline);
        return;
    }
    if (primitive.marker_shape == MarkerShape::Plus) {
        const double size = primitive.marker_size_px;
        appendSegment(mesh, {center.x - size, center.y},
                      {center.x + size, center.y},
                      primitive.outline_width_px, primitive.outline);
        appendSegment(mesh, {center.x, center.y - size},
                      {center.x, center.y + size},
                      primitive.outline_width_px, primitive.outline);
        return;
    }
    appendPolygon(mesh,
                  markerPolygon(primitive.marker_shape, center,
                                primitive.marker_size_px),
                  primitive.fill, primitive.outline,
                  primitive.outline_width_px);
}

}  // namespace

bool Color::valid() const {
    return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) &&
           std::isfinite(alpha) && red >= 0.0f && red <= 1.0f &&
           green >= 0.0f && green <= 1.0f && blue >= 0.0f && blue <= 1.0f &&
           alpha >= 0.0f && alpha <= 1.0f;
}

bool ReadOnlyOverlayScene::ready() const {
    return status == ReadOnlyOverlayBuildStatus::Ready &&
           canComposite(identity) && source_width > 0.0 && source_height > 0.0;
}

size_t ReadOnlyOverlayScene::count(PrimitiveType type) const {
    return static_cast<size_t>(std::count_if(
        primitives.begin(), primitives.end(),
        [type](const Primitive& primitive) { return primitive.type == type; }));
}

size_t ReadOnlyOverlayScene::count(CameraOverlayLayer layer) const {
    return static_cast<size_t>(std::count_if(
        primitives.begin(), primitives.end(),
        [layer](const Primitive& primitive) { return primitive.layer == layer; }));
}

size_t ReadOnlyOverlayScene::rasterCount(CameraOverlayLayer layer) const {
    return static_cast<size_t>(std::count_if(
        raster_masks.begin(), raster_masks.end(),
        [layer](const RasterMask& raster) { return raster.layer == layer; }));
}

ReadOnlyOverlayScene buildReadOnlyOverlayScene(
    const ReadOnlyOverlayInput& input) {
    ReadOnlyOverlayScene scene;
    scene.identity = input.identity;
    scene.source_width = input.source_width;
    scene.source_height = input.source_height;
    if (!canComposite(input.identity)) {
        scene.status = ReadOnlyOverlayBuildStatus::InvalidIdentity;
        return scene;
    }
    if (!finite(input.source_width) || !finite(input.source_height) ||
        input.source_width <= 0.0 || input.source_height <= 0.0) {
        scene.status = ReadOnlyOverlayBuildStatus::InvalidDimensions;
        return scene;
    }
    scene.status = ReadOnlyOverlayBuildStatus::Ready;

    if (input.show_boxes) {
        for (const auto& detection : input.detections) {
            if (detection.box) {
                appendBoxPrimitive(*detection.box, scene.primitives);
            }
        }
    }
    if (input.show_headings) {
        for (size_t index = 0; index < input.detections.size(); ++index) {
            appendHeadingPrimitive(input, input.detections[index], index,
                                   scene.primitives);
        }
    }
    appendSubjectMasks(input, scene);
    appendSubjectShapes(input, scene.primitives);
    if (input.show_keypoints) {
        for (size_t index = 0; index < input.detections.size(); ++index) {
            appendKeypointPrimitives(input, input.detections[index], index,
                                     scene.primitives);
        }
    }
    return scene;
}

Color keypointColor(const std::string& label, size_t keypoint_index) {
    const std::string lowered = lowerCopy(label);
    if (lowered.find("swim") != std::string::npos ||
        lowered.find("bladder") != std::string::npos) {
        return {1.0f, 0.85f, 0.15f, 1.0f};
    }
    if (lowered.find("left") != std::string::npos) {
        return {0.3f, 0.95f, 0.4f, 1.0f};
    }
    if (lowered.find("right") != std::string::npos) {
        return {0.75f, 0.4f, 0.95f, 1.0f};
    }
    constexpr std::array<Color, 5> kFallbackColors = {
        Color{0.95f, 0.6f, 0.2f, 1.0f},
        Color{0.35f, 0.85f, 0.55f, 1.0f},
        Color{0.6f, 0.5f, 0.95f, 1.0f},
        Color{0.95f, 0.4f, 0.4f, 1.0f},
        Color{0.4f, 0.75f, 0.95f, 1.0f},
    };
    return kFallbackColors[keypoint_index % kFallbackColors.size()];
}

MarkerShape keypointMarkerShape(const std::string& label,
                                size_t keypoint_index) {
    const std::string lowered = lowerCopy(label);
    if (lowered.find("swim") != std::string::npos ||
        lowered.find("bladder") != std::string::npos) {
        return MarkerShape::Circle;
    }
    if (lowered.find("left") != std::string::npos) {
        return MarkerShape::Square;
    }
    if (lowered.find("right") != std::string::npos) {
        return MarkerShape::Diamond;
    }
    constexpr std::array<MarkerShape, 7> kFallbackShapes = {
        MarkerShape::Circle,       MarkerShape::Square,
        MarkerShape::Diamond,      MarkerShape::Cross,
        MarkerShape::Plus,         MarkerShape::TriangleUp,
        MarkerShape::TriangleDown,
    };
    return kFallbackShapes[keypoint_index % kFallbackShapes.size()];
}

double keypointMarkerSizePx(const std::string& label) {
    const std::string lowered = lowerCopy(label);
    if (lowered.find("swim") != std::string::npos ||
        lowered.find("bladder") != std::string::npos ||
        lowered.find("left") != std::string::npos ||
        lowered.find("right") != std::string::npos) {
        return 4.5;
    }
    return 7.0;
}

Color subjectMaskColor(const std::string& label) {
    const std::string lowered = lowerCopy(label);
    if (lowered == "subject_body") {
        return {0.15f, 0.75f, 0.95f, 0.20f};
    }
    if (lowered == "swim_bladder") {
        return {1.0f, 0.72f, 0.12f, 0.34f};
    }
    if (lowered == "eye_left") {
        return {0.25f, 0.95f, 0.35f, 0.42f};
    }
    if (lowered == "eye_right") {
        return {0.82f, 0.35f, 0.95f, 0.42f};
    }
    return {0.95f, 0.55f, 0.25f, 0.30f};
}

int subjectMaskComponentRank(const std::string& label) {
    const std::string lowered = lowerCopy(label);
    if (lowered == "subject_body") {
        return 10;
    }
    if (lowered == "swim_bladder") {
        return 20;
    }
    if (lowered == "eye_left") {
        return 30;
    }
    if (lowered == "eye_right") {
        return 31;
    }
    return 100;
}

ScreenMesh tessellateReadOnlyOverlayScene(
    const ReadOnlyOverlayScene& scene,
    const SourceViewportTransform& transform,
    size_t circle_segment_count) {
    ScreenMesh mesh;
    if (!scene.ready() || !transform.valid()) {
        return mesh;
    }
    for (const auto& primitive : scene.primitives) {
        if (primitive.type == PrimitiveType::Polyline) {
            if (primitive.points.size() < 2 || !primitive.stroke.valid()) {
                continue;
            }
            bool emitted = false;
            for (size_t index = 1; index < primitive.points.size(); ++index) {
                const auto a = transform.sourceToDisplay(
                    primitive.points[index - 1]);
                const auto b = transform.sourceToDisplay(primitive.points[index]);
                if (a && b) {
                    const size_t before = mesh.triangle_vertices.size();
                    appendSegment(mesh, *a, *b, primitive.stroke_width_px,
                                  primitive.stroke);
                    emitted = emitted || mesh.triangle_vertices.size() > before;
                }
            }
            mesh.primitive_count += emitted ? 1 : 0;
        } else if (primitive.type == PrimitiveType::Marker) {
            if (primitive.points.size() != 1 || !primitive.fill.valid() ||
                !primitive.outline.valid()) {
                continue;
            }
            const auto center = transform.sourceToDisplay(primitive.points[0]);
            if (center) {
                const size_t before = mesh.triangle_vertices.size();
                appendMarker(mesh, primitive, *center, circle_segment_count);
                mesh.primitive_count +=
                    mesh.triangle_vertices.size() > before ? 1 : 0;
            }
        } else if (primitive.type == PrimitiveType::Arrow) {
            if (primitive.points.size() != 2 || !primitive.stroke.valid() ||
                !primitive.fill.valid()) {
                continue;
            }
            const auto start = transform.sourceToDisplay(primitive.points[0]);
            const auto end = transform.sourceToDisplay(primitive.points[1]);
            if (!start || !end) {
                continue;
            }
            const size_t before = mesh.triangle_vertices.size();
            appendSegment(mesh, *start, *end, primitive.stroke_width_px,
                          primitive.stroke);
            const double dx = start->x - end->x;
            const double dy = start->y - end->y;
            const double length = std::hypot(dx, dy);
            if (length > 1e-6) {
                const double ux = dx / length;
                const double uy = dy / length;
                const double size = primitive.arrow_head_size_px;
                const Point left{end->x + ux * size + uy * size * 0.5,
                                 end->y + uy * size - ux * size * 0.5};
                const Point right{end->x + ux * size - uy * size * 0.5,
                                  end->y + uy * size + ux * size * 0.5};
                appendTriangle(mesh, *end, left, right, primitive.fill);
            }
            mesh.primitive_count +=
                mesh.triangle_vertices.size() > before ? 1 : 0;
        }
    }
    return mesh;
}

ScreenMesh tessellateReadOnlyOverlaySceneLayer(
    const ReadOnlyOverlayScene& scene,
    const SourceViewportTransform& transform,
    CameraOverlayLayer layer,
    size_t circle_segment_count) {
    ReadOnlyOverlayScene filtered = scene;
    filtered.primitives.erase(
        std::remove_if(filtered.primitives.begin(), filtered.primitives.end(),
                       [layer](const Primitive& primitive) {
                           return primitive.layer != layer;
                       }),
        filtered.primitives.end());
    filtered.raster_masks.clear();
    return tessellateReadOnlyOverlayScene(filtered, transform,
                                          circle_segment_count);
}

}  // namespace crimson::overlay
