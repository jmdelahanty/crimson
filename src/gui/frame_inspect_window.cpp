#include "gui/frame_inspect_window.h"

#include "imgui.h"

namespace crimson::gui {

FrameInspectWindowResult
drawFrameInspectWindow(const FrameInspectWindowOptions &options,
                       workspace::FrameInspectView &selected_view,
                       workspace::FrameInspectViewSyncState &selection_sync,
                       const FrameInspectWindowComposition &composition) {
  FrameInspectWindowResult result;
  if (!ImGui::Begin(options.title)) {
    ImGui::End();
    return result;
  }

  result.window_visible = true;
  if (!options.interactive) {
    ImGui::BeginDisabled();
  }

  if (composition.draw_header) {
    composition.draw_header();
  }

  bool has_visible_module = false;
  for (const FrameInspectModule &module : composition.modules) {
    has_visible_module |= module.visible;
  }

  if (has_visible_module && ImGui::BeginTabBar(options.tab_bar_id)) {
    const workspace::FrameInspectView requested_view = selected_view;
    bool requested_view_visible = false;
    for (const FrameInspectModule &module : composition.modules) {
      requested_view_visible |= module.visible && module.view == requested_view;
    }
    const bool apply_requested_view =
        requested_view_visible && selection_sync.shouldApply(requested_view);
    for (const FrameInspectModule &module : composition.modules) {
      if (!module.visible) {
        continue;
      }
      const ImGuiTabItemFlags flags =
          apply_requested_view && requested_view == module.view
              ? ImGuiTabItemFlags_SetSelected
              : ImGuiTabItemFlags_None;
      if (!ImGui::BeginTabItem(module.label.c_str(), nullptr, flags)) {
        continue;
      }
      result.rendered_module = true;
      result.active_module = module.view;
      if (!apply_requested_view || module.view == requested_view) {
        selected_view = module.view;
        selection_sync.observe(module.view);
      }
      if (module.draw) {
        module.draw();
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  } else if (!has_visible_module && options.empty_message != nullptr &&
             options.empty_message[0] != '\0') {
    ImGui::TextDisabled("%s", options.empty_message);
  }

  if (composition.draw_footer) {
    composition.draw_footer();
  }

  if (!options.interactive) {
    ImGui::EndDisabled();
  }
  ImGui::End();
  return result;
}

} // namespace crimson::gui
