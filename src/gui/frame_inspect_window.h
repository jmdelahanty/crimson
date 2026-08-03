#pragma once

#include "workspace_state.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace crimson::gui {

struct FrameInspectModule {
  workspace::FrameInspectView view = workspace::FrameInspectView::Detect;
  std::string label;
  bool visible = true;
  std::function<void()> draw;
};

struct FrameInspectWindowComposition {
  std::function<void()> draw_header;
  std::vector<FrameInspectModule> modules;
  std::function<void()> draw_footer;
};

struct FrameInspectWindowOptions {
  const char *title = "Frame Inspect";
  const char *tab_bar_id = "##frame-inspect-tabs";
  const char *empty_message = "No inspection data is available.";
  bool interactive = true;
};

struct FrameInspectWindowResult {
  bool window_visible = false;
  bool rendered_module = false;
  std::optional<workspace::FrameInspectView> active_module;
};

FrameInspectWindowResult
drawFrameInspectWindow(const FrameInspectWindowOptions &options,
                       workspace::FrameInspectView &selected_view,
                       workspace::FrameInspectViewSyncState &selection_sync,
                       const FrameInspectWindowComposition &composition);

} // namespace crimson::gui
