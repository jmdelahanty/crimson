#pragma once

#include "playback_buffer_browser.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace crimson::gui {

struct FrameBufferResumeSummary {
  std::string_view path;
  int64_t target_frame = -1;
};

struct FrameBufferWindowContext {
  const playback::PlaybackBufferBrowserModel &model;
  size_t buffer_capacity = 0;
  std::optional<int64_t> pause_origin_frame;
  bool buffer_browsed_since_pause = false;
  std::optional<FrameBufferResumeSummary> last_resume;
};

struct FrameBufferWindowResult {
  std::optional<playback::PlaybackPresentationCandidate> selection;
  double draw_ms = 0.0;
};

FrameBufferWindowResult
drawFrameBufferWindow(const FrameBufferWindowContext &context);

} // namespace crimson::gui
