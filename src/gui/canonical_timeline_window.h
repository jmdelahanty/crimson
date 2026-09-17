#pragma once

#include "gui/canonical_timeline_session.h"
#include "gui/stimulus_event_timeline_window.h"

#include <cstdint>

namespace crimson::gui {

struct CanonicalTimelineWindowState {
  TimelineScrollState scroll{true, 8.0f, false};
};

void drawCanonicalTimelineWindow(const CanonicalTimelineSnapshot& snapshot,
                                 int64_t current_frame,
                                 double frames_per_second,
                                 CanonicalTimelineWindowState* state,
                                 bool* open = nullptr);

}  // namespace crimson::gui
