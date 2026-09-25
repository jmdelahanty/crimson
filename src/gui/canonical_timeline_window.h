#pragma once

#include "gui/canonical_timeline_session.h"
#include "gui/stimulus_event_timeline_window.h"

#include <cstddef>
#include <cstdint>

namespace crimson::gui {

struct CanonicalTimelineWindowState {
  TimelineScrollState scroll{true, 8.0f, false};
  bool show_swim_bouts = true;
  int64_t shading_requested_frame = -1;
  size_t shading_bands_drawn = 0;
  size_t shading_core_bands_drawn = 0;
};

void drawCanonicalTimelineWindow(const CanonicalTimelineSnapshot& snapshot,
                                 int64_t current_frame,
                                 double frames_per_second,
                                 CanonicalTimelineWindowState* state,
                                 bool* open = nullptr);

}  // namespace crimson::gui
