#pragma once

#include "gui/analysis_timeline_window.h"

struct AnalysisTimelineStimulusContextInput {
    const ZarrDetectionLoader& zarr_loader;
    const TimelineScrollState& scroll_state;
    int current_frame_num = 0;
    double video_fps = 0.0;
    bool embedded_in_subplots = false;
    bool use_external_x_limits = false;
    double external_x_min = 0.0;
    double external_x_max = 0.0;
    bool external_x_limits_always = false;
};

void drawAnalysisTimelineStimulusContext(
    const AnalysisTimelineStimulusContextInput& context);
