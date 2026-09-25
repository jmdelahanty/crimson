#include "imgui_semantic_snapshot.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdarg>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace crimson::ui {
namespace {

struct Recorder {
  bool enabled = false;
  bool frame_active = false;
  SemanticSnapshot snapshot;
  std::unordered_map<ImGuiID, size_t> latest_items;
  std::unordered_map<ImGuiID, std::string> debug_labels;
};

Recorder &recorder() {
  static Recorder instance;
  return instance;
}

std::string visibleLabel(const std::string &label) {
  const size_t hidden_suffix = label.find("##");
  return label.substr(0, hidden_suffix);
}

SemanticRect semanticRect(const ImRect &rect) {
  return {rect.Min.x, rect.Min.y, rect.GetWidth(), rect.GetHeight()};
}

ImGuiWindow *rootWindow(ImGuiWindow *window) {
  if (window == nullptr) {
    return nullptr;
  }
  return window->RootWindow != nullptr ? window->RootWindow : window;
}

void recordItemAdd(ImGuiContext *context, ImGuiID id, const ImRect &bounds,
                   const ImGuiLastItemData *item_data) {
  Recorder &state = recorder();
  if (!state.enabled || !state.frame_active || context == nullptr || id == 0) {
    return;
  }
  ImGuiWindow *current = context->CurrentWindow;
  ImGuiWindow *root = rootWindow(current);
  if (current == nullptr || root == nullptr || id == root->ID) {
    return;
  }

  SemanticItem item;
  item.id = id;
  item.window_name = root->Name != nullptr ? root->Name : "";
  item.bounds = semanticRect(bounds);
  item.window_relative_bounds = {
      bounds.Min.x - root->Pos.x, bounds.Min.y - root->Pos.y,
      bounds.GetWidth(), bounds.GetHeight()};
  if (item_data != nullptr) {
    item.status_flags = static_cast<uint32_t>(item_data->StatusFlags);
    item.item_flags = static_cast<uint32_t>(item_data->ItemFlags);
  }
  item.submission_order = static_cast<int>(state.snapshot.items.size());
  state.snapshot.items.push_back(std::move(item));
  state.latest_items[id] = state.snapshot.items.size() - 1;
}

void recordItemInfo(ImGuiContext *, ImGuiID id, const char *label,
                    ImGuiItemStatusFlags flags) {
  Recorder &state = recorder();
  if (!state.enabled || !state.frame_active || id == 0 || label == nullptr) {
    return;
  }
  state.debug_labels[id] = label;
  const auto found = state.latest_items.find(id);
  if (found == state.latest_items.end()) {
    return;
  }
  SemanticItem &item = state.snapshot.items[found->second];
  item.label = label;
  item.visible_label = visibleLabel(item.label);
  item.status_flags = static_cast<uint32_t>(flags);
}

} // namespace

void setSemanticCaptureEnabled(ImGuiContext *context, bool enabled) {
  Recorder &state = recorder();
  state.enabled = enabled;
  state.frame_active = false;
  if (context != nullptr) {
    context->TestEngineHookItems = enabled;
  }
}

void beginSemanticFrame(ImGuiContext *context) {
  Recorder &state = recorder();
  if (!state.enabled || context == nullptr) {
    return;
  }
  context->TestEngineHookItems = true;
  state.snapshot = {};
  state.snapshot.frame = context->FrameCount + 1;
  state.snapshot.display_width = context->IO.DisplaySize.x;
  state.snapshot.display_height = context->IO.DisplaySize.y;
  state.snapshot.framebuffer_scale_x = context->IO.DisplayFramebufferScale.x;
  state.snapshot.framebuffer_scale_y = context->IO.DisplayFramebufferScale.y;
  state.latest_items.clear();
  state.debug_labels.clear();
  state.frame_active = true;
}

SemanticSnapshot finishSemanticFrame(ImGuiContext *context) {
  Recorder &state = recorder();
  if (!state.enabled || !state.frame_active || context == nullptr) {
    return {};
  }

  for (ImGuiWindow *window : context->Windows) {
    if (window == nullptr || !window->Active || window->Hidden ||
        window->IsFallbackWindow ||
        (window->Flags & ImGuiWindowFlags_ChildWindow) != 0) {
      continue;
    }
    SemanticWindow output;
    output.name = window->Name != nullptr ? window->Name : "";
    output.visible_name = visibleLabel(output.name);
    output.parent_name =
        window->ParentWindow != nullptr && window->ParentWindow->Name != nullptr
            ? window->ParentWindow->Name
            : "";
    output.bounds = semanticRect(window->Rect());
    output.begin_order = window->BeginOrderWithinContext;
    output.collapsed = window->Collapsed;
    state.snapshot.windows.push_back(std::move(output));
  }
  std::sort(state.snapshot.windows.begin(), state.snapshot.windows.end(),
            [](const SemanticWindow &lhs, const SemanticWindow &rhs) {
              return lhs.begin_order < rhs.begin_order;
            });
  state.frame_active = false;
  return state.snapshot;
}

nlohmann::json semanticSnapshotJson(const SemanticSnapshot &snapshot) {
  auto rect_json = [](const SemanticRect &rect) {
    return nlohmann::json{{"x", rect.x},
                          {"y", rect.y},
                          {"width", rect.width},
                          {"height", rect.height}};
  };

  nlohmann::json windows = nlohmann::json::array();
  for (const SemanticWindow &window : snapshot.windows) {
    windows.push_back({{"name", window.name},
                       {"visible_name", window.visible_name},
                       {"parent_name", window.parent_name},
                       {"bounds", rect_json(window.bounds)},
                       {"begin_order", window.begin_order},
                       {"collapsed", window.collapsed}});
  }

  nlohmann::json items = nlohmann::json::array();
  for (const SemanticItem &item : snapshot.items) {
    if (item.label.empty()) {
      continue;
    }
    items.push_back({{"id", item.id},
                     {"window_name", item.window_name},
                     {"label", item.label},
                     {"visible_label", item.visible_label},
                     {"bounds", rect_json(item.bounds)},
                     {"window_relative_bounds",
                      rect_json(item.window_relative_bounds)},
                     {"status_flags", item.status_flags},
                     {"item_flags", item.item_flags},
                     {"submission_order", item.submission_order}});
  }

  return {{"schema", "crimson.imgui_semantic_snapshot.v1"},
          {"frame", snapshot.frame},
          {"display_size",
           {{"width", snapshot.display_width},
            {"height", snapshot.display_height}}},
          {"framebuffer_scale",
           {{"x", snapshot.framebuffer_scale_x},
            {"y", snapshot.framebuffer_scale_y}}},
          {"windows", std::move(windows)},
          {"items", std::move(items)}};
}

} // namespace crimson::ui

#if defined(IMGUI_ENABLE_TEST_ENGINE)
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *context, ImGuiID id,
                                 const ImRect &bounds,
                                 const ImGuiLastItemData *item_data) {
  crimson::ui::recordItemAdd(context, id, bounds, item_data);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext *context, ImGuiID id,
                                  const char *label,
                                  ImGuiItemStatusFlags flags) {
  crimson::ui::recordItemInfo(context, id, label, flags);
}

void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}

const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID id) {
  const auto found = crimson::ui::recorder().debug_labels.find(id);
  return found == crimson::ui::recorder().debug_labels.end()
             ? ""
             : found->second.c_str();
}
#endif
