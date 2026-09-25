#include "gui/camera_view_vector_scene_draw.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace crimson::gui {
namespace {

bool startsWith(const std::string &value, std::string_view prefix) {
  return value.compare(0, prefix.size(), prefix) == 0;
}

bool visible(float min_x, float min_y, float max_x, float max_y) {
  const ImVec2 pos = ImPlot::GetPlotPos();
  const ImVec2 size = ImPlot::GetPlotSize();
  return max_x >= pos.x && min_x <= pos.x + size.x &&
         max_y >= pos.y && min_y <= pos.y + size.y;
}

ImU32 color(const overlay::Color &value) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(value.red, value.green, value.blue, value.alpha));
}

} // namespace

CameraViewVectorSceneDrawCounts drawCameraViewScenePolygons(
    const overlay::ReadOnlyOverlayScene &scene, float image_height_px) {
  CameraViewVectorSceneDrawCounts counts;
  if (!scene.ready() || !std::isfinite(image_height_px) ||
      image_height_px <= 0.0f) {
    return counts;
  }
  ImDrawList *draw_list = ImPlot::GetPlotDrawList();
  ImPlot::PushPlotClipRect();
  for (const auto &primitive : scene.primitives) {
    if (primitive.type != overlay::PrimitiveType::Polygon ||
        primitive.points.size() < 3 || !primitive.fill.valid() ||
        !primitive.outline.valid() ||
        !std::isfinite(primitive.outline_width_px) ||
        primitive.outline_width_px < 0.0) {
      continue;
    }
    std::vector<ImVec2> points;
    points.reserve(primitive.points.size());
    float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (const auto &point : primitive.points) {
      if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        points.clear();
        break;
      }
      const ImVec2 pixel = ImPlot::PlotToPixels(
          ImPlotPoint(point.x, static_cast<double>(image_height_px) - point.y));
      if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y)) {
        points.clear();
        break;
      }
      points.push_back(pixel);
      min_x = std::min(min_x, pixel.x);
      min_y = std::min(min_y, pixel.y);
      max_x = std::max(max_x, pixel.x);
      max_y = std::max(max_y, pixel.y);
    }
    if (points.size() < 3 || !visible(min_x, min_y, max_x, max_y)) {
      continue;
    }
    const int before_vertices = draw_list->VtxBuffer.Size;
    if (primitive.fill.alpha > 0.0f) {
      draw_list->AddConcavePolyFilled(points.data(), static_cast<int>(points.size()),
                                      color(primitive.fill));
    }
    if (primitive.outline.alpha > 0.0f && primitive.outline_width_px > 0.0) {
      draw_list->AddPolyline(points.data(), static_cast<int>(points.size()),
                             color(primitive.outline), ImDrawFlags_Closed,
                             static_cast<float>(primitive.outline_width_px));
    }
    if (draw_list->VtxBuffer.Size == before_vertices) {
      continue;
    }
    if (startsWith(primitive.label, "##eye_beam_overlap_")) {
      ++counts.visual_cone_overlaps;
    } else if (startsWith(primitive.label, "##eye_beam_")) {
      ++counts.visual_cones;
    }
  }
  ImPlot::PopPlotClipRect();
  return counts;
}

CameraViewVectorSceneDrawCounts drawCameraViewSceneText(
    const overlay::ReadOnlyOverlayScene &scene, float image_height_px) {
  CameraViewVectorSceneDrawCounts counts;
  if (!scene.ready() || !std::isfinite(image_height_px) ||
      image_height_px <= 0.0f) {
    return counts;
  }
  ImDrawList *draw_list = ImPlot::GetPlotDrawList();
  ImPlot::PushPlotClipRect();
  for (const auto &annotation : scene.text_annotations) {
    if (annotation.content.empty() || !annotation.text.valid() ||
        !annotation.background.valid() || !annotation.border.valid() ||
        !std::isfinite(annotation.source_anchor.x) ||
        !std::isfinite(annotation.source_anchor.y) ||
        !std::isfinite(annotation.offset_px.x) ||
        !std::isfinite(annotation.offset_px.y) ||
        !std::isfinite(annotation.font_scale) || annotation.font_scale <= 0.0) {
      continue;
    }
    ImVec2 anchor = ImPlot::PlotToPixels(ImPlotPoint(
        annotation.source_anchor.x,
        static_cast<double>(image_height_px) - annotation.source_anchor.y));
    anchor.x += static_cast<float>(annotation.offset_px.x);
    anchor.y += static_cast<float>(annotation.offset_px.y);
    if (!std::isfinite(anchor.x) || !std::isfinite(anchor.y)) {
      continue;
    }
    const float font_size = ImGui::GetFontSize() *
                            static_cast<float>(annotation.font_scale);
    const ImVec2 base_size = ImGui::CalcTextSize(annotation.content.c_str());
    const ImVec2 size(base_size.x * static_cast<float>(annotation.font_scale),
                      base_size.y * static_cast<float>(annotation.font_scale));
    const ImVec2 padding(7.0f, 4.0f);
    const ImVec2 text_pos(annotation.centered ? anchor.x - size.x * 0.5f : anchor.x,
                          annotation.centered ? anchor.y - size.y * 0.5f : anchor.y);
    const ImVec2 box_min(text_pos.x - padding.x, text_pos.y - padding.y);
    const ImVec2 box_max(text_pos.x + size.x + padding.x,
                         text_pos.y + size.y + padding.y);
    if (!visible(box_min.x, box_min.y, box_max.x, box_max.y)) {
      continue;
    }
    if (annotation.background.alpha > 0.0f) {
      draw_list->AddRectFilled(box_min, box_max, color(annotation.background), 4.0f);
    }
    if (annotation.border.alpha > 0.0f) {
      draw_list->AddRect(box_min, box_max, color(annotation.border), 4.0f);
    }
    draw_list->AddText(ImGui::GetFont(), font_size, text_pos,
                       color(annotation.text), annotation.content.c_str());
    if (startsWith(annotation.label, "##eye_label_") ||
        startsWith(annotation.label, "##eye_vergence_")) {
      ++counts.angle_labels;
    }
  }
  ImPlot::PopPlotClipRect();
  return counts;
}

} // namespace crimson::gui
