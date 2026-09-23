#include "gui/camera_view_vector_scene_draw.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool near(ImVec2 a, ImVec2 b) {
  return std::abs(a.x - b.x) < 1.5f && std::abs(a.y - b.y) < 1.5f;
}

bool hasVertex(const ImDrawList *list, int begin, ImVec2 pos, ImU32 color) {
  for (int i = begin; i < list->VtxBuffer.Size; ++i) {
    if (list->VtxBuffer[i].col == color && near(list->VtxBuffer[i].pos, pos))
      return true;
  }
  return false;
}

bool hasColor(const ImDrawList *list, int begin, ImU32 color) {
  for (int i = begin; i < list->VtxBuffer.Size; ++i) {
    if (list->VtxBuffer[i].col == color) return true;
  }
  return false;
}

bool colorBounds(const ImDrawList *list, int begin, ImU32 color,
                 ImVec2 *minimum, ImVec2 *maximum) {
  bool found = false;
  for (int i = begin; i < list->VtxBuffer.Size; ++i) {
    const ImDrawVert &vertex = list->VtxBuffer[i];
    if (vertex.col != color) continue;
    if (!found) {
      *minimum = *maximum = vertex.pos;
      found = true;
    } else {
      minimum->x = std::min(minimum->x, vertex.pos.x);
      minimum->y = std::min(minimum->y, vertex.pos.y);
      maximum->x = std::max(maximum->x, vertex.pos.x);
      maximum->y = std::max(maximum->y, vertex.pos.y);
    }
  }
  return found;
}

bool hasPlotClip(const ImDrawList *list, int begin_index, int end_index,
                 ImVec2 plot_pos, ImVec2 plot_size) {
  int index = 0;
  for (const ImDrawCmd &command : list->CmdBuffer) {
    const int command_end = index + static_cast<int>(command.ElemCount);
    if (command_end > begin_index && index < end_index) {
      const ImVec4 clip = command.ClipRect;
      if (std::abs(clip.x - plot_pos.x) < 1.0f &&
          std::abs(clip.y - plot_pos.y) < 1.0f &&
          std::abs(clip.z - (plot_pos.x + plot_size.x)) < 1.0f &&
          std::abs(clip.w - (plot_pos.y + plot_size.y)) < 1.0f) {
        return true;
      }
    }
    index = command_end;
  }
  return false;
}

crimson::overlay::ReadOnlyOverlayScene scene() {
  using namespace crimson::overlay;
  ReadOnlyOverlayScene result;
  result.status = ReadOnlyOverlayBuildStatus::Ready;
  result.identity = {0, 3, 0, 3};
  result.source_width = 100;
  result.source_height = 100;

  Primitive beam;
  beam.type = PrimitiveType::Polygon;
  beam.points = {{40, 40}, {60, 40}, {50, 60}};
  beam.fill = {0.1f, 0.7f, 0.2f, 0.25f};
  beam.outline = {0.2f, 0.8f, 0.3f, 0.7f};
  beam.outline_width_px = 2;
  beam.label = "##eye_beam_0";
  result.primitives.push_back(beam);

  Primitive overlap = beam;
  overlap.points = {{47, 43}, {53, 43}, {50, 52}};
  overlap.label = "##eye_beam_overlap_0";
  result.primitives.push_back(overlap);

  Primitive outside = beam;
  outside.points = {{110, 110}, {120, 110}, {115, 120}};
  result.primitives.push_back(outside);

  Primitive partial = beam;
  partial.points = {{10, 35}, {30, 35}, {30, 45}};
  partial.fill = {0.7f, 0.1f, 0.3f, 0.4f};
  partial.label = "##eye_beam_partial";
  result.primitives.push_back(partial);

  Primitive invisible = beam;
  invisible.fill.alpha = 0.0f;
  invisible.outline.alpha = 0.0f;
  invisible.label = "##eye_beam_invisible";
  result.primitives.push_back(invisible);

  Primitive unsupported = beam;
  unsupported.type = PrimitiveType::Polyline;
  result.primitives.push_back(unsupported);

  Primitive invalid = beam;
  invalid.points[0].x = std::numeric_limits<double>::quiet_NaN();
  result.primitives.push_back(invalid);

  Primitive empty = beam;
  empty.points.clear();
  result.primitives.push_back(empty);

  TextAnnotation text;
  text.source_anchor = {50, 50};
  text.offset_px = {7, -5};
  text.text = {0.9f, 0.8f, 0.1f, 0.95f};
  text.background = {0.05f, 0.06f, 0.07f, 0.8f};
  text.border = {0.4f, 0.5f, 0.6f, 0.9f};
  text.font_scale = 1.25;
  text.content = "Eye-frame 22 deg";
  text.label = "##eye_label_0";
  result.text_annotations.push_back(text);

  TextAnnotation vergence = text;
  vergence.label = "##eye_vergence_0";
  vergence.offset_px = {7, 15};
  vergence.background = {0.08f, 0.09f, 0.1f, 0.8f};
  result.text_annotations.push_back(vergence);

  TextAnnotation partial_text = text;
  partial_text.source_anchor = {19, 50};
  partial_text.offset_px = {0, 0};
  partial_text.centered = false;
  partial_text.content = "Edge label";
  partial_text.label = "##partial_label";
  partial_text.background = {0.35f, 0.12f, 0.42f, 0.8f};
  result.text_annotations.push_back(partial_text);

  TextAnnotation offscreen = text;
  offscreen.source_anchor = {-100, -100};
  result.text_annotations.push_back(offscreen);

  TextAnnotation empty_text = text;
  empty_text.content.clear();
  result.text_annotations.push_back(empty_text);
  return result;
}

void run() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(640, 480);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0, height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(600, 450), ImGuiCond_Always);
  ImGui::Begin("headless plot");
  check(ImPlot::BeginPlot("camera", ImVec2(500, 350)), "plot opened");
  ImPlot::SetupAxesLimits(20, 80, 20, 80, ImPlotCond_Always);
  ImDrawList *list = ImPlot::GetPlotDrawList();
  const ImVec2 plot_pos = ImPlot::GetPlotPos();
  const ImVec2 plot_size = ImPlot::GetPlotSize();
  const auto input = scene();

  const ImVec2 expected = ImPlot::PlotToPixels(ImPlotPoint(40, 60));
  const int before_polygons = list->VtxBuffer.Size;
  const int before_polygon_indices = list->IdxBuffer.Size;
  const auto polygon_counts =
      crimson::gui::drawCameraViewScenePolygons(input, 100);
  check(polygon_counts.visual_cones == 2, "full and partial cones submitted");
  check(polygon_counts.visual_cone_overlaps == 1, "one overlap submitted");
  check(list->VtxBuffer.Size > before_polygons, "polygon vertices submitted");
  const ImU32 fill_color = ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.1f, 0.7f, 0.2f, 0.25f));
  const ImU32 outline_color = ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.2f, 0.8f, 0.3f, 0.7f));
  check(hasVertex(list, before_polygons, expected, fill_color),
        "source Y flipped and zoom transform applied to fill");
  check(hasColor(list, before_polygons, outline_color), "outline submitted");
  check(hasColor(list, before_polygons, ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.7f, 0.1f, 0.3f, 0.4f))), "partially clipped polygon submitted");
  check(hasPlotClip(list, before_polygon_indices, list->IdxBuffer.Size,
                    plot_pos, plot_size), "polygon draw command uses plot clip");

  const int before_text = list->VtxBuffer.Size;
  const int before_text_indices = list->IdxBuffer.Size;
  const auto text_counts = crimson::gui::drawCameraViewSceneText(input, 100);
  check(text_counts.angle_labels == 2, "angle and vergence labels submitted");
  check(list->VtxBuffer.Size > before_text, "text vertices submitted");
  check(hasColor(list, before_text, ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.9f, 0.8f, 0.1f, 0.95f))), "text color submitted");
  check(hasColor(list, before_text, ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.05f, 0.06f, 0.07f, 0.8f))), "background submitted");
  check(hasColor(list, before_text, ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.4f, 0.5f, 0.6f, 0.9f))), "border submitted");
  check(hasColor(list, before_text, ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.35f, 0.12f, 0.42f, 0.8f))),
      "partially clipped label background submitted");
  check(hasPlotClip(list, before_text_indices, list->IdxBuffer.Size,
                    plot_pos, plot_size), "text draw command uses plot clip");

  const auto &label = input.text_annotations.front();
  ImVec2 label_anchor = ImPlot::PlotToPixels(ImPlotPoint(
      label.source_anchor.x, 100.0 - label.source_anchor.y));
  label_anchor.x += static_cast<float>(label.offset_px.x);
  label_anchor.y += static_cast<float>(label.offset_px.y);
  const ImVec2 base_text = ImGui::CalcTextSize(label.content.c_str());
  const float scale = static_cast<float>(label.font_scale);
  const ImVec2 box_min(label_anchor.x - base_text.x * scale * 0.5f - 7.0f,
                       label_anchor.y - base_text.y * scale * 0.5f - 4.0f);
  const ImVec2 box_max(label_anchor.x + base_text.x * scale * 0.5f + 7.0f,
                       label_anchor.y + base_text.y * scale * 0.5f + 4.0f);
  const ImU32 background = ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.05f, 0.06f, 0.07f, 0.8f));
  ImVec2 actual_min, actual_max;
  check(colorBounds(list, before_text, background, &actual_min, &actual_max),
        "label background has vertices");
  check(near(actual_min, box_min),
        "font scale and pixel offset place label box minimum");
  check(near(actual_max, box_max),
        "font scale and pixel offset size label box maximum");

  auto stale = input;
  stale.identity.overlay_frame = 4;
  const int before_stale = list->VtxBuffer.Size;
  check(crimson::gui::drawCameraViewScenePolygons(stale, 100).visual_cones == 0,
        "stale polygon scene rejected");
  check(crimson::gui::drawCameraViewSceneText(stale, 100).angle_labels == 0,
        "stale text scene rejected");
  check(list->VtxBuffer.Size == before_stale, "stale scene submitted no vertices");

  auto empty = input;
  empty.primitives.clear();
  empty.text_annotations.clear();
  const int before_empty = list->VtxBuffer.Size;
  check(crimson::gui::drawCameraViewScenePolygons(empty, 100).visual_cones == 0,
        "empty polygon scene has no counts");
  check(crimson::gui::drawCameraViewSceneText(empty, 100).angle_labels == 0,
        "empty text scene has no counts");
  check(list->VtxBuffer.Size == before_empty, "empty scene submits no vertices");

  auto transparent = input;
  transparent.primitives = {input.primitives[4]};
  transparent.text_annotations.clear();
  const int before_transparent = list->VtxBuffer.Size;
  check(crimson::gui::drawCameraViewScenePolygons(transparent, 100)
            .visual_cones == 0,
        "transparent polygon is not counted");
  check(list->VtxBuffer.Size == before_transparent,
        "transparent fill and outline submit no vertices");

  auto disabled = input;
  disabled.status = crimson::overlay::ReadOnlyOverlayBuildStatus::InvalidDimensions;
  const int before_disabled = list->VtxBuffer.Size;
  check(crimson::gui::drawCameraViewScenePolygons(disabled, 100).visual_cones == 0,
        "disabled polygon scene rejected");
  check(crimson::gui::drawCameraViewSceneText(disabled, 100).angle_labels == 0,
        "disabled text scene rejected");
  check(list->VtxBuffer.Size == before_disabled,
        "disabled scene submits no vertices");

  ImPlot::EndPlot();
  ImGui::End();
  ImGui::Render();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
}

} // namespace

int main() {
  try {
    run();
    std::cout << "camera_view_vector_scene_draw_tests PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
