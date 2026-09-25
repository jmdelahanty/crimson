#include "stimulus_camera_overlay_scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace crimson::stimulus {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool finite(double value) { return std::isfinite(value); }

StimulusCameraOverlayColor color(double red,
                                 double green,
                                 double blue,
                                 double alpha) {
  return {red, green, blue, alpha};
}

void appendTriangle(StimulusCameraOverlayMesh* mesh,
                    StimulusCameraOverlayPoint a,
                    StimulusCameraOverlayPoint b,
                    StimulusCameraOverlayPoint c,
                    const StimulusCameraOverlayColor& value,
                    StimulusCameraOverlayPoint origin,
                    double scale_x,
                    double scale_y) {
  auto append = [&](StimulusCameraOverlayPoint point) {
    mesh->triangle_vertices.push_back(
        {static_cast<float>(origin.x + point.x * scale_x),
         static_cast<float>(origin.y + point.y * scale_y), value});
  };
  append(a);
  append(b);
  append(c);
}

void appendLine(StimulusCameraOverlayMesh* mesh,
                StimulusCameraOverlayPoint first,
                StimulusCameraOverlayPoint second,
                double width,
                const StimulusCameraOverlayColor& value,
                StimulusCameraOverlayPoint origin,
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
  const StimulusCameraOverlayPoint a{first.x + nx, first.y + ny};
  const StimulusCameraOverlayPoint b{second.x + nx, second.y + ny};
  const StimulusCameraOverlayPoint c{second.x - nx, second.y - ny};
  const StimulusCameraOverlayPoint d{first.x - nx, first.y - ny};
  appendTriangle(mesh, a, b, c, value, origin, scale_x, scale_y);
  appendTriangle(mesh, a, c, d, value, origin, scale_x, scale_y);
}

std::vector<StimulusCameraOverlayPoint> roundedRectPoints(
    StimulusCameraOverlayPoint minimum,
    StimulusCameraOverlayPoint maximum,
    double requested_radius,
    size_t segment_count) {
  const double width = maximum.x - minimum.x;
  const double height = maximum.y - minimum.y;
  if (!(finite(width) && finite(height) && width > 0.0 && height > 0.0)) {
    return {};
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
  const std::array<StimulusCameraOverlayPoint, 4> centers = {{
      {minimum.x + radius, minimum.y + radius},
      {maximum.x - radius, minimum.y + radius},
      {maximum.x - radius, maximum.y - radius},
      {minimum.x + radius, maximum.y - radius},
  }};
  const std::array<double, 4> starts = {kPi, -0.5 * kPi, 0.0, 0.5 * kPi};
  std::vector<StimulusCameraOverlayPoint> points;
  points.reserve(corner_segments * 4);
  for (size_t corner = 0; corner < centers.size(); ++corner) {
    for (size_t index = 0; index < corner_segments; ++index) {
      const double t = static_cast<double>(index) /
                       static_cast<double>(corner_segments - 1);
      const double angle = starts[corner] + t * 0.5 * kPi;
      points.push_back({centers[corner].x + radius * std::cos(angle),
                        centers[corner].y + radius * std::sin(angle)});
    }
  }
  return points;
}

void appendConvexFill(StimulusCameraOverlayMesh* mesh,
                      const std::vector<StimulusCameraOverlayPoint>& points,
                      const StimulusCameraOverlayColor& value,
                      StimulusCameraOverlayPoint origin,
                      double scale_x,
                      double scale_y) {
  for (size_t index = 2; index < points.size(); ++index) {
    appendTriangle(mesh, points[0], points[index - 1], points[index], value,
                   origin, scale_x, scale_y);
  }
}

void appendPolyline(StimulusCameraOverlayMesh* mesh,
                    const std::vector<StimulusCameraOverlayPoint>& points,
                    double width,
                    const StimulusCameraOverlayColor& value,
                    StimulusCameraOverlayPoint origin,
                    double scale_x,
                    double scale_y) {
  if (points.size() < 2) {
    return;
  }
  for (size_t index = 1; index < points.size(); ++index) {
    appendLine(mesh, points[index - 1], points[index], width, value, origin,
               scale_x, scale_y);
  }
  appendLine(mesh, points.back(), points.front(), width, value, origin, scale_x,
             scale_y);
}

std::vector<timeline::StimulusContextEvent> eventsAtFrame(
    const timeline::StimulusContextTimelineSnapshot& snapshot,
    int64_t camera_frame) {
  std::vector<timeline::StimulusContextEvent> result;
  for (const auto& event : snapshot.events) {
    if (event.camera_frame == camera_frame) {
      result.push_back(event);
    }
  }
  return result;
}

std::optional<int64_t> latestEventFrame(
    const timeline::StimulusContextTimelineSnapshot& snapshot,
    int64_t camera_frame) {
  std::optional<int64_t> result;
  for (const auto& event : snapshot.events) {
    if (event.camera_frame >= 0 && event.camera_frame <= camera_frame &&
        (!result || event.camera_frame > *result)) {
      result = event.camera_frame;
    }
  }
  return result;
}

StimulusCameraOverlayPoint localPoint(
    const StimulusCameraOverlayScene& scene,
    StimulusCameraOverlaySceneLayer layer,
    StimulusCameraOverlayPoint point) {
  const bool event = layer == StimulusCameraOverlaySceneLayer::EventPanel ||
                     layer == StimulusCameraOverlaySceneLayer::EventText;
  const auto& box = event ? scene.event_box : scene.step_box;
  return box.valid() ? StimulusCameraOverlayPoint{point.x - box.x,
                                                  point.y - box.y}
                     : point;
}

void writePoint(std::ostringstream& output,
                StimulusCameraOverlayPoint point) {
  output << point.x << ',' << point.y;
}

void writeColor(std::ostringstream& output,
                const StimulusCameraOverlayColor& value) {
  output << value.red << ',' << value.green << ',' << value.blue << ','
         << value.alpha;
}

}  // namespace

bool StimulusCameraOverlayRect::valid() const {
  return finite(x) && finite(y) && finite(width) && finite(height) &&
         width > 0.0 && height > 0.0;
}

bool StimulusCameraOverlayViewport::valid() const {
  return finite(width_px) && finite(height_px) && width_px > 0.0 &&
         height_px > 0.0;
}

bool StimulusCameraOverlayColor::valid() const {
  return finite(red) && finite(green) && finite(blue) && finite(alpha) &&
         red >= 0.0 && red <= 1.0 && green >= 0.0 && green <= 1.0 &&
         blue >= 0.0 && blue <= 1.0 && alpha >= 0.0 && alpha <= 1.0;
}

bool StimulusCameraOverlayTextMetrics::valid() const {
  return finite(width_px) && finite(height_px) && width_px >= 0.0 &&
         height_px > 0.0;
}

StimulusCameraOverlayFrameSample resolveStimulusCameraOverlayFrame(
    const timeline::StimulusContextTimelineSnapshot* snapshot,
    int64_t requested_camera_frame) {
  StimulusCameraOverlayFrameSample result;
  result.requested_camera_frame = requested_camera_frame;
  if (snapshot == nullptr ||
      (snapshot->events.empty() && snapshot->steps.empty())) {
    return result;
  }
  if (requested_camera_frame < 0 ||
      (snapshot->descriptor.frame_count > 0 &&
       requested_camera_frame >=
           static_cast<int64_t>(snapshot->descriptor.frame_count))) {
    result.availability = StimulusCameraOverlayFrameAvailability::FrameOutOfRange;
    return result;
  }

  result.source_camera_frame = requested_camera_frame;
  result.exact_events = eventsAtFrame(*snapshot, requested_camera_frame);
  result.latest_event_camera_frame =
      latestEventFrame(*snapshot, requested_camera_frame);
  if (result.latest_event_camera_frame) {
    result.latest_events =
        eventsAtFrame(*snapshot, *result.latest_event_camera_frame);
  }
  if (const auto* step = timeline::findStimulusContextStepForFrame(
          *snapshot, requested_camera_frame)) {
    result.step = *step;
  }
  result.availability =
      result.latest_events.empty() && !result.step
          ? StimulusCameraOverlayFrameAvailability::ValidFrameEmpty
          : StimulusCameraOverlayFrameAvailability::Ready;
  return result;
}

size_t StimulusCameraOverlayScene::primitiveCount(
    StimulusCameraOverlaySceneLayer layer) const {
  return static_cast<size_t>(std::count_if(
      primitives.begin(), primitives.end(),
      [&](const auto& primitive) { return primitive.layer == layer; }));
}

size_t StimulusCameraOverlayScene::textCount(
    StimulusCameraOverlaySceneLayer layer) const {
  return static_cast<size_t>(std::count_if(
      text.begin(), text.end(),
      [&](const auto& annotation) { return annotation.layer == layer; }));
}

std::string_view stimulusCameraOverlayFrameAvailabilityName(
    StimulusCameraOverlayFrameAvailability availability) {
  switch (availability) {
    case StimulusCameraOverlayFrameAvailability::Ready:
      return "ready";
    case StimulusCameraOverlayFrameAvailability::ValidFrameEmpty:
      return "valid_frame_empty";
    case StimulusCameraOverlayFrameAvailability::TimelineUnavailable:
      return "timeline_unavailable";
    case StimulusCameraOverlayFrameAvailability::FrameOutOfRange:
      return "frame_out_of_range";
  }
  return "timeline_unavailable";
}

std::string_view stimulusCameraOverlaySceneStatusName(
    StimulusCameraOverlaySceneStatus status) {
  switch (status) {
    case StimulusCameraOverlaySceneStatus::Ready:
      return "ready";
    case StimulusCameraOverlaySceneStatus::Disabled:
      return "disabled";
    case StimulusCameraOverlaySceneStatus::FrameUnavailable:
      return "frame_unavailable";
    case StimulusCameraOverlaySceneStatus::NonExactFrame:
      return "non_exact_frame";
    case StimulusCameraOverlaySceneStatus::InvalidViewport:
      return "invalid_viewport";
    case StimulusCameraOverlaySceneStatus::InvalidTextMetrics:
      return "invalid_text_metrics";
  }
  return "frame_unavailable";
}

std::string_view stimulusCameraOverlaySceneLayerName(
    StimulusCameraOverlaySceneLayer layer) {
  switch (layer) {
    case StimulusCameraOverlaySceneLayer::EventPanel:
      return "event_panel";
    case StimulusCameraOverlaySceneLayer::EventText:
      return "event_text";
    case StimulusCameraOverlaySceneLayer::StepPanel:
      return "step_panel";
    case StimulusCameraOverlaySceneLayer::StepText:
      return "step_text";
    case StimulusCameraOverlaySceneLayer::StepArrow:
      return "step_arrow";
  }
  return "event_panel";
}

std::string stimulusCameraOverlayEventText(
    const StimulusCameraOverlayFrameSample& frame,
    const StimulusCameraOverlayControls& controls) {
  const auto& events = controls.persist_latest_event ? frame.latest_events
                                                     : frame.exact_events;
  std::string result;
  for (const auto& event : events) {
    if (!result.empty()) {
      result += '\n';
    }
    result += event.label;
  }
  return result;
}

StimulusCameraOverlayScene buildStimulusCameraOverlayScene(
    const StimulusCameraOverlayFrameSample& frame,
    const StimulusCameraOverlayViewport& viewport,
    const StimulusCameraOverlayTextMetrics& event_text_metrics,
    const StimulusCameraOverlayControls& controls) {
  StimulusCameraOverlayScene scene;
  scene.frame_availability = frame.availability;
  scene.requested_camera_frame = frame.requested_camera_frame;
  scene.source_camera_frame = frame.source_camera_frame;
  scene.viewport = viewport;
  if (!controls.show_events && !controls.show_step_direction) {
    scene.status = StimulusCameraOverlaySceneStatus::Disabled;
    return scene;
  }
  if (frame.availability ==
          StimulusCameraOverlayFrameAvailability::TimelineUnavailable ||
      frame.availability ==
          StimulusCameraOverlayFrameAvailability::FrameOutOfRange) {
    scene.status = StimulusCameraOverlaySceneStatus::FrameUnavailable;
    return scene;
  }
  if (!frame.exactFrame()) {
    scene.status = StimulusCameraOverlaySceneStatus::NonExactFrame;
    return scene;
  }
  if (!viewport.valid()) {
    scene.status = StimulusCameraOverlaySceneStatus::InvalidViewport;
    return scene;
  }

  const std::string event_text =
      controls.show_events ? stimulusCameraOverlayEventText(frame, controls)
                           : std::string{};
  if (!event_text.empty()) {
    if (!event_text_metrics.valid()) {
      scene.status = StimulusCameraOverlaySceneStatus::InvalidTextMetrics;
      return scene;
    }
    scene.event_source_camera_frame = controls.persist_latest_event
                                          ? frame.latest_event_camera_frame
                                          : frame.source_camera_frame;
    scene.event_box = {12.0, 12.0, event_text_metrics.width_px + 12.0,
                       event_text_metrics.height_px + 8.0};
    StimulusCameraOverlayPrimitive panel;
    panel.type = StimulusCameraOverlayPrimitiveType::RoundedRectangle;
    panel.layer = StimulusCameraOverlaySceneLayer::EventPanel;
    panel.first = {scene.event_box.x, scene.event_box.y};
    panel.second = {scene.event_box.x + scene.event_box.width,
                    scene.event_box.y + scene.event_box.height};
    panel.corner_radius_px = 4.0;
    panel.stroke_width_px = 1.0;
    panel.segment_count = 20;
    panel.fill = color(0.0, 0.0, 0.0, 180.0 / 255.0);
    panel.stroke = color(80.0 / 255.0, 180.0 / 255.0, 1.0,
                         220.0 / 255.0);
    panel.has_fill = true;
    panel.has_stroke = true;
    scene.primitives.push_back(std::move(panel));
    scene.text.push_back({StimulusCameraOverlaySceneLayer::EventText,
                          {scene.event_box.x + 6.0, scene.event_box.y + 4.0},
                          color(200.0 / 255.0, 220.0 / 255.0, 1.0, 1.0),
                          event_text});
  }

  if (controls.show_step_direction && frame.step &&
      frame.step->kind == timeline::StimulusStepKind::MovingGrating &&
      frame.step->moving_grating.present &&
      finite(frame.step->moving_grating.grating_direction_camera_deg) &&
      viewport.width_px >= 80.0 && viewport.height_px >= 60.0) {
    constexpr double kPanelHeight = 76.0;
    const double panel_width = std::min(204.0, viewport.width_px - 20.0);
    scene.step_box = {viewport.width_px - panel_width - 12.0, 12.0,
                      panel_width, kPanelHeight};
    scene.step_index = frame.step->step_index;
    scene.grating_direction_camera_deg =
        frame.step->moving_grating.grating_direction_camera_deg;

    StimulusCameraOverlayPrimitive panel;
    panel.type = StimulusCameraOverlayPrimitiveType::RoundedRectangle;
    panel.layer = StimulusCameraOverlaySceneLayer::StepPanel;
    panel.first = {scene.step_box.x, scene.step_box.y};
    panel.second = {scene.step_box.x + scene.step_box.width,
                    scene.step_box.y + scene.step_box.height};
    panel.corner_radius_px = 6.0;
    panel.stroke_width_px = 1.0;
    panel.segment_count = 24;
    panel.fill = color(8.0 / 255.0, 14.0 / 255.0, 24.0 / 255.0,
                       190.0 / 255.0);
    panel.stroke = color(120.0 / 255.0, 190.0 / 255.0, 1.0,
                         220.0 / 255.0);
    panel.has_fill = true;
    panel.has_stroke = true;
    scene.primitives.push_back(std::move(panel));

    std::ostringstream label;
    label.imbue(std::locale::classic());
    label << "Grating motion " << std::fixed << std::setprecision(0)
          << *scene.grating_direction_camera_deg << " deg";
    scene.text.push_back({StimulusCameraOverlaySceneLayer::StepText,
                          {scene.step_box.x + 10.0, scene.step_box.y + 8.0},
                          color(225.0 / 255.0, 238.0 / 255.0, 1.0, 1.0),
                          label.str()});

    const StimulusCameraOverlayPoint center{
        scene.step_box.x + panel_width * 0.5, scene.step_box.y + 49.0};
    const double arrow_half_length = std::max(28.0, panel_width * 0.27);
    const double direction_radians =
        *scene.grating_direction_camera_deg * kPi / 180.0;
    const StimulusCameraOverlayPoint direction{std::cos(direction_radians),
                                               -std::sin(direction_radians)};
    const StimulusCameraOverlayPoint p0{
        center.x - direction.x * arrow_half_length,
        center.y - direction.y * arrow_half_length};
    const StimulusCameraOverlayPoint p1{
        center.x + direction.x * arrow_half_length,
        center.y + direction.y * arrow_half_length};
    const auto arrow_color = color(1.0, 215.0 / 255.0, 75.0 / 255.0, 1.0);
    StimulusCameraOverlayPrimitive shaft;
    shaft.type = StimulusCameraOverlayPrimitiveType::Line;
    shaft.layer = StimulusCameraOverlaySceneLayer::StepArrow;
    shaft.first = p0;
    shaft.second = p1;
    shaft.stroke_width_px = 3.0;
    shaft.stroke = arrow_color;
    shaft.has_stroke = true;
    scene.primitives.push_back(std::move(shaft));

    const StimulusCameraOverlayPoint back{p0.x - p1.x, p0.y - p1.y};
    const double length = std::hypot(back.x, back.y);
    if (length > 1e-3) {
      const StimulusCameraOverlayPoint unit{back.x / length, back.y / length};
      constexpr double kHeadSize = 12.0;
      StimulusCameraOverlayPrimitive head;
      head.type = StimulusCameraOverlayPrimitiveType::Triangle;
      head.layer = StimulusCameraOverlaySceneLayer::StepArrow;
      head.first = p1;
      head.second = {p1.x + unit.x * kHeadSize + unit.y * kHeadSize * 0.55,
                     p1.y + unit.y * kHeadSize - unit.x * kHeadSize * 0.55};
      head.third = {p1.x + unit.x * kHeadSize - unit.y * kHeadSize * 0.55,
                    p1.y + unit.y * kHeadSize + unit.x * kHeadSize * 0.55};
      head.fill = arrow_color;
      head.has_fill = true;
      scene.primitives.push_back(std::move(head));
    }
  }

  scene.status = StimulusCameraOverlaySceneStatus::Ready;
  return scene;
}

std::string stimulusCameraOverlaySceneSemanticSignature(
    const StimulusCameraOverlayScene& scene) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(6);
  output << "crimson.stimulus-camera-overlay-semantics.v1"
         << "|status=" << stimulusCameraOverlaySceneStatusName(scene.status)
         << "|availability="
         << stimulusCameraOverlayFrameAvailabilityName(scene.frame_availability)
         << "|requested=" << scene.requested_camera_frame << "|source=";
  if (scene.source_camera_frame) {
    output << *scene.source_camera_frame;
  } else {
    output << "none";
  }
  output << "|event_source=";
  if (scene.event_source_camera_frame) {
    output << *scene.event_source_camera_frame;
  } else {
    output << "none";
  }
  output << "|step=";
  if (scene.step_index) {
    output << *scene.step_index;
  } else {
    output << "none";
  }
  output << "|direction=";
  if (scene.grating_direction_camera_deg) {
    output << *scene.grating_direction_camera_deg;
  } else {
    output << "none";
  }
  output << "|event_box=" << scene.event_box.x << ',' << scene.event_box.y
         << ',' << scene.event_box.width << ',' << scene.event_box.height;
  const double step_right_margin =
      scene.step_box.valid()
          ? scene.viewport.width_px - scene.step_box.x - scene.step_box.width
          : 0.0;
  output << "|step_box=" << step_right_margin << ',' << scene.step_box.y << ','
         << scene.step_box.width << ',' << scene.step_box.height;
  for (const auto& primitive : scene.primitives) {
    output << "|p:" << stimulusCameraOverlaySceneLayerName(primitive.layer)
           << ':' << static_cast<int>(primitive.type) << ':';
    writePoint(output, localPoint(scene, primitive.layer, primitive.first));
    output << ':';
    writePoint(output, localPoint(scene, primitive.layer, primitive.second));
    output << ':';
    writePoint(output,
               primitive.type == StimulusCameraOverlayPrimitiveType::Triangle
                   ? localPoint(scene, primitive.layer, primitive.third)
                   : StimulusCameraOverlayPoint{});
    output << ':' << primitive.corner_radius_px << ':'
           << primitive.stroke_width_px << ':' << primitive.segment_count << ':'
           << primitive.has_fill << ':';
    writeColor(output, primitive.fill);
    output << ':' << primitive.has_stroke << ':';
    writeColor(output, primitive.stroke);
  }
  for (const auto& annotation : scene.text) {
    output << "|t:" << stimulusCameraOverlaySceneLayerName(annotation.layer)
           << ':';
    writePoint(output, localPoint(scene, annotation.layer, annotation.anchor));
    output << ':';
    writeColor(output, annotation.color);
    output << ':' << annotation.content;
  }
  return output.str();
}

StimulusCameraOverlayMesh tessellateStimulusCameraOverlayScene(
    const StimulusCameraOverlayScene& scene,
    StimulusCameraOverlayPoint display_origin,
    double scale_x,
    double scale_y,
    size_t rounded_segment_count) {
  StimulusCameraOverlayMesh mesh;
  if (!scene.ready() || !finite(display_origin.x) ||
      !finite(display_origin.y) || !finite(scale_x) || !finite(scale_y) ||
      scale_x <= 0.0 || scale_y <= 0.0) {
    return mesh;
  }
  rounded_segment_count = std::max<size_t>(8, rounded_segment_count);
  for (const auto& primitive : scene.primitives) {
    const size_t before = mesh.triangle_vertices.size();
    switch (primitive.type) {
      case StimulusCameraOverlayPrimitiveType::RoundedRectangle: {
        const auto points = roundedRectPoints(
            primitive.first, primitive.second, primitive.corner_radius_px,
            primitive.segment_count > 0 ? primitive.segment_count
                                        : rounded_segment_count);
        if (primitive.has_fill) {
          appendConvexFill(&mesh, points, primitive.fill, display_origin,
                           scale_x, scale_y);
        }
        if (primitive.has_stroke) {
          appendPolyline(&mesh, points, primitive.stroke_width_px,
                         primitive.stroke, display_origin, scale_x, scale_y);
        }
        break;
      }
      case StimulusCameraOverlayPrimitiveType::Line:
        if (primitive.has_stroke) {
          appendLine(&mesh, primitive.first, primitive.second,
                     primitive.stroke_width_px, primitive.stroke,
                     display_origin, scale_x, scale_y);
        }
        break;
      case StimulusCameraOverlayPrimitiveType::Triangle:
        if (primitive.has_fill) {
          appendTriangle(&mesh, primitive.first, primitive.second,
                         primitive.third, primitive.fill, display_origin,
                         scale_x, scale_y);
        }
        break;
    }
    if (mesh.triangle_vertices.size() > before) {
      ++mesh.primitive_count;
    }
  }
  return mesh;
}

}  // namespace crimson::stimulus
