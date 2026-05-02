#pragma once

#include "stimulus_event_timeline_window.h"

#include <string>

class ZarrDetectionLoader;

struct MovementTimelineWindowState {
    bool show_smoothed = true;
    bool show_instantaneous = false;
    bool show_vergence = true;
    bool show_heading_raw = false;
    bool show_heading_smoothed = true;
    bool show_heading_per_second = false;
    bool show_swim_bouts = true;
    bool show_detector_response = true;
    std::string selected_swim_bout_run;
    std::string selected_swim_bout_speed_level;
    std::string selected_bout_kinematics_run;
};

struct MovementTimelineWindowContext {
    ZarrDetectionLoader& zarr_loader;
    TimelineScrollState& scroll_state;
    int current_frame_num = 0;
    double video_fps = 0.0;
};

void drawMovementTimelineWindow(const MovementTimelineWindowContext& context,
                                MovementTimelineWindowState& state);
