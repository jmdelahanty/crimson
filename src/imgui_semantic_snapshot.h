#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

struct ImGuiContext;

namespace crimson::ui {

struct SemanticRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

struct SemanticWindow {
  std::string name;
  std::string visible_name;
  std::string parent_name;
  SemanticRect bounds;
  int begin_order = -1;
  bool collapsed = false;
};

struct SemanticItem {
  uint32_t id = 0;
  std::string window_name;
  std::string label;
  std::string visible_label;
  SemanticRect bounds;
  SemanticRect window_relative_bounds;
  uint32_t status_flags = 0;
  uint32_t item_flags = 0;
  int submission_order = -1;
};

struct SemanticSnapshot {
  int frame = -1;
  float display_width = 0.0f;
  float display_height = 0.0f;
  float framebuffer_scale_x = 1.0f;
  float framebuffer_scale_y = 1.0f;
  std::vector<SemanticWindow> windows;
  std::vector<SemanticItem> items;
};

void setSemanticCaptureEnabled(ImGuiContext *context, bool enabled);
void beginSemanticFrame(ImGuiContext *context);
SemanticSnapshot finishSemanticFrame(ImGuiContext *context);
nlohmann::json semanticSnapshotJson(const SemanticSnapshot &snapshot);

} // namespace crimson::ui
