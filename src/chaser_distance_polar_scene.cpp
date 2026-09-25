#include "chaser_distance_polar_scene.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace crimson::polar {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool finite(double value) { return std::isfinite(value); }

ChaserDistancePolarRgba color(double red,
                              double green,
                              double blue,
                              double alpha) {
  return {red, green, blue, alpha};
}

void addPrimitive(ChaserDistancePolarScene* scene,
                  ChaserDistancePolarScenePrimitive primitive) {
  scene->primitives.push_back(std::move(primitive));
}

void addText(ChaserDistancePolarScene* scene,
             ChaserDistancePolarSceneLayer layer,
             ChaserDistancePolarScenePoint anchor,
             ChaserDistancePolarRgba text_color,
             bool centered,
             std::string content) {
  scene->text.push_back(
      {layer, anchor, text_color, centered, std::move(content)});
}

void appendTriangle(ChaserDistancePolarSceneMesh* mesh,
                    ChaserDistancePolarScenePoint a,
                    ChaserDistancePolarScenePoint b,
                    ChaserDistancePolarScenePoint c,
                    const ChaserDistancePolarRgba& value,
                    ChaserDistancePolarScenePoint origin,
                    double scale_x,
                    double scale_y) {
  auto append = [&](ChaserDistancePolarScenePoint point) {
    mesh->triangle_vertices.push_back(
        {static_cast<float>(origin.x + point.x * scale_x),
         static_cast<float>(origin.y + point.y * scale_y), value});
  };
  append(a);
  append(b);
  append(c);
}

void appendLine(ChaserDistancePolarSceneMesh* mesh,
                ChaserDistancePolarScenePoint first,
                ChaserDistancePolarScenePoint second,
                double width,
                const ChaserDistancePolarRgba& value,
                ChaserDistancePolarScenePoint origin,
                double scale_x,
                double scale_y) {
  const double dx = second.x - first.x;
  const double dy = second.y - first.y;
  const double length = std::hypot(dx, dy);
  if (!(finite(length) && length > 0.0 && finite(width) && width > 0.0)) {
    return;
  }
  const double half = width * 0.5;
  const double nx = -dy / length * half;
  const double ny = dx / length * half;
  const ChaserDistancePolarScenePoint a{first.x + nx, first.y + ny};
  const ChaserDistancePolarScenePoint b{second.x + nx, second.y + ny};
  const ChaserDistancePolarScenePoint c{second.x - nx, second.y - ny};
  const ChaserDistancePolarScenePoint d{first.x - nx, first.y - ny};
  appendTriangle(mesh, a, b, c, value, origin, scale_x, scale_y);
  appendTriangle(mesh, a, c, d, value, origin, scale_x, scale_y);
}

void appendPolyline(ChaserDistancePolarSceneMesh* mesh,
                    const std::vector<ChaserDistancePolarScenePoint>& points,
                    bool closed,
                    double width,
                    const ChaserDistancePolarRgba& value,
                    ChaserDistancePolarScenePoint origin,
                    double scale_x,
                    double scale_y) {
  if (points.size() < 2) {
    return;
  }
  for (size_t index = 1; index < points.size(); ++index) {
    appendLine(mesh, points[index - 1], points[index], width, value, origin,
               scale_x, scale_y);
  }
  if (closed) {
    appendLine(mesh, points.back(), points.front(), width, value, origin,
               scale_x, scale_y);
  }
}

void appendConvexFill(
    ChaserDistancePolarSceneMesh* mesh,
    const std::vector<ChaserDistancePolarScenePoint>& points,
    const ChaserDistancePolarRgba& value,
    ChaserDistancePolarScenePoint origin,
    double scale_x,
    double scale_y) {
  if (points.size() < 3) {
    return;
  }
  for (size_t index = 2; index < points.size(); ++index) {
    appendTriangle(mesh, points[0], points[index - 1], points[index], value,
                   origin, scale_x, scale_y);
  }
}

std::vector<ChaserDistancePolarScenePoint> circlePoints(
    ChaserDistancePolarScenePoint center,
    double radius,
    size_t segment_count) {
  std::vector<ChaserDistancePolarScenePoint> points;
  if (!(finite(radius) && radius > 0.0)) {
    return points;
  }
  segment_count = std::max<size_t>(8, segment_count);
  points.reserve(segment_count);
  for (size_t index = 0; index < segment_count; ++index) {
    const double angle =
        2.0 * kPi * static_cast<double>(index) /
        static_cast<double>(segment_count);
    points.push_back(
        {center.x + radius * std::cos(angle),
         center.y + radius * std::sin(angle)});
  }
  return points;
}

std::vector<ChaserDistancePolarScenePoint> roundedRectPoints(
    ChaserDistancePolarScenePoint minimum,
    ChaserDistancePolarScenePoint maximum,
    double requested_radius,
    size_t segment_count) {
  std::vector<ChaserDistancePolarScenePoint> points;
  const double width = maximum.x - minimum.x;
  const double height = maximum.y - minimum.y;
  if (!(finite(width) && finite(height) && width > 0.0 && height > 0.0)) {
    return points;
  }
  const double radius =
      std::clamp(requested_radius, 0.0, std::min(width, height) * 0.5);
  if (radius == 0.0) {
    return {{minimum.x, minimum.y},
            {maximum.x, minimum.y},
            {maximum.x, maximum.y},
            {minimum.x, maximum.y}};
  }
  const size_t corner_segments = std::max<size_t>(2, segment_count / 4);
  const std::array<ChaserDistancePolarScenePoint, 4> centers = {{
      {minimum.x + radius, minimum.y + radius},
      {maximum.x - radius, minimum.y + radius},
      {maximum.x - radius, maximum.y - radius},
      {minimum.x + radius, maximum.y - radius},
  }};
  const std::array<double, 4> start_angles = {
      kPi, -0.5 * kPi, 0.0, 0.5 * kPi};
  points.reserve(corner_segments * 4);
  for (size_t corner = 0; corner < centers.size(); ++corner) {
    for (size_t index = 0; index < corner_segments; ++index) {
      const double t = static_cast<double>(index) /
                       static_cast<double>(corner_segments - 1);
      const double angle = start_angles[corner] + t * 0.5 * kPi;
      points.push_back({centers[corner].x + radius * std::cos(angle),
                        centers[corner].y + radius * std::sin(angle)});
    }
  }
  return points;
}

}  // namespace

bool ChaserDistancePolarSceneRect::valid() const {
  return finite(x) && finite(y) && finite(width) && finite(height) &&
         width > 0.0 && height > 0.0;
}

bool ChaserDistancePolarSceneViewport::valid() const {
  return finite(width_px) && finite(height_px) && width_px > 0.0 &&
         height_px > 0.0;
}

size_t ChaserDistancePolarScene::primitiveCount(
    ChaserDistancePolarSceneLayer layer) const {
  return static_cast<size_t>(std::count_if(
      primitives.begin(), primitives.end(),
      [&](const auto& primitive) { return primitive.layer == layer; }));
}

size_t ChaserDistancePolarScene::textCount(
    ChaserDistancePolarSceneLayer layer) const {
  return static_cast<size_t>(std::count_if(
      text.begin(), text.end(),
      [&](const auto& annotation) { return annotation.layer == layer; }));
}

std::string_view chaserDistancePolarSceneStatusName(
    ChaserDistancePolarSceneStatus status) {
  switch (status) {
    case ChaserDistancePolarSceneStatus::Ready:
      return "ready";
    case ChaserDistancePolarSceneStatus::Disabled:
      return "disabled";
    case ChaserDistancePolarSceneStatus::SampleUnavailable:
      return "sample_unavailable";
    case ChaserDistancePolarSceneStatus::NonExactFrame:
      return "non_exact_frame";
    case ChaserDistancePolarSceneStatus::UnsupportedConventions:
      return "unsupported_conventions";
    case ChaserDistancePolarSceneStatus::InvalidViewport:
      return "invalid_viewport";
    case ChaserDistancePolarSceneStatus::TooSmall:
      return "too_small";
  }
  return "sample_unavailable";
}

std::string_view chaserDistancePolarSceneLayerName(
    ChaserDistancePolarSceneLayer layer) {
  switch (layer) {
    case ChaserDistancePolarSceneLayer::Background:
      return "background";
    case ChaserDistancePolarSceneLayer::Grid:
      return "grid";
    case ChaserDistancePolarSceneLayer::Axes:
      return "axes";
    case ChaserDistancePolarSceneLayer::Points:
      return "points";
    case ChaserDistancePolarSceneLayer::OrientationLabels:
      return "orientation_labels";
    case ChaserDistancePolarSceneLayer::Readout:
      return "readout";
  }
  return "background";
}

std::string chaserDistancePolarSceneSemanticSignature(
    const ChaserDistancePolarScene& scene) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(6);
  const auto relative = [&](ChaserDistancePolarScenePoint point) {
    return ChaserDistancePolarScenePoint{point.x - scene.box.x,
                                         point.y - scene.box.y};
  };
  const auto appendPoint = [&](ChaserDistancePolarScenePoint point) {
    const auto value = relative(point);
    output << value.x << ',' << value.y;
  };
  const auto appendColor = [&](const ChaserDistancePolarRgba& value) {
    output << value.red << ',' << value.green << ',' << value.blue << ','
           << value.alpha;
  };

  output << "crimson.polar-scene-semantics.v1"
         << "|status=" << static_cast<int>(scene.status)
         << "|availability=" << static_cast<int>(scene.frame_availability)
         << "|requested=" << scene.requested_camera_frame << "|source=";
  if (scene.source_camera_frame) {
    output << *scene.source_camera_frame;
  } else {
    output << "none";
  }
  output << "|box=" << scene.box.width << ',' << scene.box.height
         << "|graph=" << scene.graph.x - scene.box.x << ','
         << scene.graph.y - scene.box.y << ',' << scene.graph.width << ','
         << scene.graph.height << "|center=";
  appendPoint(scene.center);
  output << "|radius=" << scene.radius_px
         << "|display_max_mm=" << scene.display_max_distance_mm
         << "|point_count=" << scene.point_count;

  output << "|primitives=" << scene.primitives.size();
  for (const auto& primitive : scene.primitives) {
    output << "|p:" << static_cast<int>(primitive.type) << ','
           << static_cast<int>(primitive.layer) << ',';
    appendPoint(primitive.first);
    output << ',';
    if (primitive.type ==
            ChaserDistancePolarScenePrimitiveType::RoundedRectangle ||
        primitive.type == ChaserDistancePolarScenePrimitiveType::Line) {
      appendPoint(primitive.second);
    } else {
      output << "0.000000,0.000000";
    }
    output << ',' << primitive.radius_px << ',' << primitive.corner_radius_px
           << ',' << primitive.stroke_width_px << ','
           << primitive.segment_count << ',' << primitive.has_fill << ',';
    appendColor(primitive.fill);
    output << ',' << primitive.has_stroke << ',';
    appendColor(primitive.stroke);
    output << ',' << primitive.chaser_index;
  }

  output << "|text=" << scene.text.size();
  for (const auto& annotation : scene.text) {
    output << "|t:" << static_cast<int>(annotation.layer) << ',';
    appendPoint(annotation.anchor);
    output << ',' << annotation.centered << ',';
    appendColor(annotation.color);
    output << ',' << std::quoted(annotation.content);
  }
  return output.str();
}

ChaserDistancePolarScene buildChaserDistancePolarScene(
    const ChaserDistancePolarFrameSample& frame,
    const ChaserDistancePolarSceneViewport& viewport,
    const ChaserDistancePolarSceneControls& controls) {
  ChaserDistancePolarScene scene;
  scene.frame_availability = frame.availability;
  scene.requested_camera_frame = frame.requested_camera_frame;
  scene.source_camera_frame = frame.source_camera_frame;
  scene.viewport = viewport;
  if (!controls.show_inset) {
    scene.status = ChaserDistancePolarSceneStatus::Disabled;
    return scene;
  }
  if (frame.availability != ChaserDistancePolarAvailability::Ready &&
      frame.availability != ChaserDistancePolarAvailability::ValidFrameEmpty) {
    scene.status = ChaserDistancePolarSceneStatus::SampleUnavailable;
    return scene;
  }
  if (!frame.exactFrame()) {
    scene.status = ChaserDistancePolarSceneStatus::NonExactFrame;
    return scene;
  }
  if (frame.distance_unit != ChaserDistancePolarDistanceUnit::Millimeters ||
      frame.normalized_coordinate_frame !=
          ChaserDistancePolarCoordinateFrame::ArenaRelativeCanvasPixels ||
      frame.normalized_angle_convention !=
          ChaserDistancePolarAngleConvention::
              EgocentricDegreesZeroFrontPositiveAnatomicalLeft ||
      !frame.radial_scale.valid()) {
    scene.status = ChaserDistancePolarSceneStatus::UnsupportedConventions;
    return scene;
  }
  if (!viewport.valid()) {
    scene.status = ChaserDistancePolarSceneStatus::InvalidViewport;
    return scene;
  }
  if (viewport.width_px < 160.0 || viewport.height_px < 160.0 ||
      !finite(controls.width_px) || !finite(controls.opacity)) {
    scene.status = ChaserDistancePolarSceneStatus::TooSmall;
    return scene;
  }

  constexpr double kPad = 8.0;
  const double max_width =
      std::min(viewport.width_px - 24.0, viewport.width_px * 0.32);
  double graph_size = std::clamp(
      static_cast<double>(controls.width_px), 140.0,
      std::max(140.0, max_width));
  const double requested_readout_height =
      controls.show_readout
          ? std::min(72.0,
                     25.0 + 16.0 * static_cast<double>(
                                         std::max<size_t>(1, frame.points.size())))
          : 0.0;
  const double max_height = viewport.height_px - 24.0;
  if (graph_size + requested_readout_height + kPad * 2.0 > max_height) {
    graph_size = std::max(
        112.0, max_height - requested_readout_height - kPad * 2.0);
  }
  if (graph_size < 96.0) {
    scene.status = ChaserDistancePolarSceneStatus::TooSmall;
    return scene;
  }
  const double readout_height =
      controls.show_readout
          ? std::min(requested_readout_height,
                     std::max(0.0,
                              max_height - graph_size - kPad * 2.0))
          : 0.0;
  const double box_width = graph_size + kPad * 2.0;
  const double box_height = graph_size + readout_height + kPad * 2.0;
  scene.box = {viewport.width_px - box_width - 12.0, 12.0, box_width,
               box_height};
  scene.graph = {scene.box.x + kPad, scene.box.y + kPad, graph_size,
                 graph_size};
  scene.center = {scene.graph.x + graph_size * 0.5,
                  scene.graph.y + graph_size * 0.5};
  const double label_margin = controls.show_labels ? 24.0 : 12.0;
  scene.radius_px = std::max(20.0, graph_size * 0.5 - label_margin);
  scene.display_max_distance_mm =
      frame.radial_scale.display_max_distance_mm;

  ChaserDistancePolarScenePrimitive background;
  background.type =
      ChaserDistancePolarScenePrimitiveType::RoundedRectangle;
  background.layer = ChaserDistancePolarSceneLayer::Background;
  background.first = {scene.box.x, scene.box.y};
  background.second = {scene.box.x + scene.box.width,
                       scene.box.y + scene.box.height};
  background.corner_radius_px = 6.0;
  background.stroke_width_px = 1.0;
  background.segment_count = 24;
  background.has_fill = true;
  background.fill = color(
      4.0 / 255.0, 8.0 / 255.0, 14.0 / 255.0,
      std::clamp(static_cast<double>(controls.opacity), 0.15, 1.0) *
          225.0 / 255.0);
  background.has_stroke = true;
  background.stroke = color(110.0 / 255.0, 205.0 / 255.0,
                            205.0 / 255.0, 210.0 / 255.0);
  addPrimitive(&scene, std::move(background));

  for (const double radial_fraction : {0.5, 1.0}) {
    ChaserDistancePolarScenePrimitive ring;
    ring.type = ChaserDistancePolarScenePrimitiveType::Circle;
    ring.layer = ChaserDistancePolarSceneLayer::Grid;
    ring.first = scene.center;
    ring.radius_px = scene.radius_px * radial_fraction;
    ring.stroke_width_px = radial_fraction == 1.0 ? 1.2 : 1.0;
    ring.segment_count = radial_fraction == 1.0 ? 96 : 72;
    ring.has_stroke = true;
    ring.stroke = color(150.0 / 255.0, 185.0 / 255.0,
                        205.0 / 255.0, 120.0 / 255.0);
    addPrimitive(&scene, std::move(ring));
  }

  const auto axis_color = color(190.0 / 255.0, 220.0 / 255.0,
                                230.0 / 255.0, 150.0 / 255.0);
  for (const auto& endpoints :
       {std::pair{ChaserDistancePolarScenePoint{
                      scene.center.x, scene.center.y - scene.radius_px},
                  ChaserDistancePolarScenePoint{
                      scene.center.x, scene.center.y + scene.radius_px}},
        std::pair{ChaserDistancePolarScenePoint{
                      scene.center.x - scene.radius_px, scene.center.y},
                  ChaserDistancePolarScenePoint{
                      scene.center.x + scene.radius_px, scene.center.y}}}) {
    ChaserDistancePolarScenePrimitive axis;
    axis.type = ChaserDistancePolarScenePrimitiveType::Line;
    axis.layer = ChaserDistancePolarSceneLayer::Axes;
    axis.first = endpoints.first;
    axis.second = endpoints.second;
    axis.stroke_width_px = 1.0;
    axis.has_stroke = true;
    axis.stroke = axis_color;
    addPrimitive(&scene, std::move(axis));
  }

  if (controls.show_labels) {
    const auto label_color = color(225.0 / 255.0, 238.0 / 255.0, 1.0,
                                   235.0 / 255.0);
    addText(&scene, ChaserDistancePolarSceneLayer::OrientationLabels,
            {scene.center.x, scene.center.y - scene.radius_px - 12.0},
            label_color, true, "front");
    addText(&scene, ChaserDistancePolarSceneLayer::OrientationLabels,
            {scene.center.x - scene.radius_px - 15.0, scene.center.y},
            label_color, true, "left");
    addText(&scene, ChaserDistancePolarSceneLayer::OrientationLabels,
            {scene.center.x + scene.radius_px + 17.0, scene.center.y},
            label_color, true, "right");
    addText(&scene, ChaserDistancePolarSceneLayer::OrientationLabels,
            {scene.center.x, scene.center.y + scene.radius_px + 12.0},
            label_color, true, "behind");
  }

  std::vector<const ChaserDistancePolarPoint*> renderable_points;
  renderable_points.reserve(frame.points.size());
  for (const auto& point : frame.points) {
    if (!point.scientificallyUsable() || !point.color.rgba.valid()) {
      continue;
    }
    renderable_points.push_back(&point);
    const double radial_fraction =
        frame.radial_scale.normalizedRadius(point.distance_mm);
    const double theta = (-90.0 - point.bearing_degrees) * kPi / 180.0;
    ChaserDistancePolarScenePrimitive marker;
    marker.type = ChaserDistancePolarScenePrimitiveType::Marker;
    marker.layer = ChaserDistancePolarSceneLayer::Points;
    marker.first = {scene.center.x + scene.radius_px * radial_fraction *
                                         std::cos(theta),
                    scene.center.y + scene.radius_px * radial_fraction *
                                         std::sin(theta)};
    marker.radius_px = 5.0;
    marker.stroke_width_px = 1.3;
    marker.segment_count = 20;
    marker.has_fill = true;
    marker.fill = point.color.rgba;
    marker.fill.alpha = 1.0;
    marker.has_stroke = true;
    marker.stroke = color(0.0, 0.0, 0.0, 230.0 / 255.0);
    marker.chaser_index = point.chaser_index;
    addPrimitive(&scene, std::move(marker));
  }
  scene.point_count = renderable_points.size();

  if (controls.show_readout) {
    const ChaserDistancePolarScenePoint readout{
        scene.graph.x, scene.graph.y + graph_size + 2.0};
    addText(&scene, ChaserDistancePolarSceneLayer::Readout, readout,
            color(230.0 / 255.0, 245.0 / 255.0, 250.0 / 255.0,
                  245.0 / 255.0),
            false,
            "Chaser bearing f=" +
                std::to_string(*frame.source_camera_frame));
    double y = readout.y + 16.0;
    if (renderable_points.empty()) {
      addText(&scene, ChaserDistancePolarSceneLayer::Readout,
              {readout.x, y},
              color(170.0 / 255.0, 185.0 / 255.0, 195.0 / 255.0,
                    235.0 / 255.0),
              false, "No valid chaser sample");
    } else {
      for (const auto* point : renderable_points) {
        if (y > scene.box.y + scene.box.height - 16.0) {
          break;
        }
        std::ostringstream line;
        line << "c" << point->chaser_index << " " << std::fixed
             << std::setprecision(1) << point->distance_mm << " mm  "
             << std::showpos << point->bearing_degrees << std::noshowpos
             << " deg";
        auto text_color = point->color.rgba;
        text_color.alpha = 1.0;
        addText(&scene, ChaserDistancePolarSceneLayer::Readout,
                {readout.x, y}, text_color, false, line.str());
        y += 15.0;
      }
    }
  }

  scene.status = ChaserDistancePolarSceneStatus::Ready;
  return scene;
}

ChaserDistancePolarSceneMesh tessellateChaserDistancePolarScene(
    const ChaserDistancePolarScene& scene,
    ChaserDistancePolarScenePoint display_origin,
    double scale_x,
    double scale_y,
    size_t circle_segment_count) {
  ChaserDistancePolarSceneMesh mesh;
  if (!scene.ready() || !finite(display_origin.x) ||
      !finite(display_origin.y) || !finite(scale_x) || !finite(scale_y) ||
      scale_x <= 0.0 || scale_y <= 0.0) {
    return mesh;
  }
  circle_segment_count = std::max<size_t>(8, circle_segment_count);
  for (const auto& primitive : scene.primitives) {
    const size_t before = mesh.triangle_vertices.size();
    switch (primitive.type) {
      case ChaserDistancePolarScenePrimitiveType::RoundedRectangle: {
        const auto points = roundedRectPoints(
            primitive.first, primitive.second, primitive.corner_radius_px,
            primitive.segment_count > 0 ? primitive.segment_count
                                        : circle_segment_count);
        if (primitive.has_fill) {
          appendConvexFill(&mesh, points, primitive.fill, display_origin,
                           scale_x, scale_y);
        }
        if (primitive.has_stroke) {
          appendPolyline(&mesh, points, true, primitive.stroke_width_px,
                         primitive.stroke, display_origin, scale_x, scale_y);
        }
        break;
      }
      case ChaserDistancePolarScenePrimitiveType::Circle:
      case ChaserDistancePolarScenePrimitiveType::Marker: {
        const auto points = circlePoints(
            primitive.first, primitive.radius_px,
            primitive.segment_count > 0 ? primitive.segment_count
                                        : circle_segment_count);
        if (primitive.has_fill) {
          appendConvexFill(&mesh, points, primitive.fill, display_origin,
                           scale_x, scale_y);
        }
        if (primitive.has_stroke) {
          appendPolyline(&mesh, points, true, primitive.stroke_width_px,
                         primitive.stroke, display_origin, scale_x, scale_y);
        }
        break;
      }
      case ChaserDistancePolarScenePrimitiveType::Line:
        if (primitive.has_stroke) {
          appendLine(&mesh, primitive.first, primitive.second,
                     primitive.stroke_width_px, primitive.stroke,
                     display_origin, scale_x, scale_y);
        }
        break;
    }
    if (mesh.triangle_vertices.size() > before) {
      ++mesh.primitive_count;
    }
  }
  return mesh;
}

}  // namespace crimson::polar
