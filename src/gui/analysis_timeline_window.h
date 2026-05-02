#pragma once

#include "stimulus_event_timeline_window.h"

#include <string>

class ZarrDetectionLoader;

struct AnalysisTimelineWindowState {
    bool show_smoothed = true;
    bool show_instantaneous = false;
    bool show_heading_raw = false;
    bool show_heading_smoothed = true;
    bool show_heading_per_second = false;
    bool show_swim_bouts = true;
    bool show_detector_response = true;
    bool show_eye_angle_traces = true;
    bool show_eye_left_trace = true;
    bool show_eye_right_trace = true;
    bool show_eye_vergence_trace = true;
    bool show_tail_tip_angle = true;
    bool show_tail_tip_lateral_deflection = true;
    bool show_tail_curvature = false;
    int eye_angle_representation_index = -1;
    std::string selected_swim_bout_run;
    std::string selected_swim_bout_speed_level;
    std::string selected_bout_kinematics_run;
};

struct AnalysisTimelineWindowContext {
    ZarrDetectionLoader& zarr_loader;
    TimelineScrollState& scroll_state;
    int current_frame_num = 0;
    double video_fps = 0.0;
};

void drawAnalysisTimelineWindow(const AnalysisTimelineWindowContext& context,
                                AnalysisTimelineWindowState& state);
