#pragma once

#include "gui/analysis_timeline_trace_plot.h"
#include "gui/analysis_timeline_window.h"

#include <optional>

struct AnalysisTimelineEyeAngleContext {
    ZarrDetectionLoader& zarr_loader;
    TimelineScrollState& scroll_state;
    int current_frame_num = 0;
    double video_fps = 0.0;
    double fallback_current_time = -1.0;
};

void drawAnalysisTimelineEyeAngleControls(
    const AnalysisTimelineEyeAngleContext& context,
    AnalysisTimelineWindowState& state);

std::optional<AnalysisTimelineTracePlotRow> buildAnalysisTimelineEyeAngleRow(
    const AnalysisTimelineEyeAngleContext& context,
    AnalysisTimelineWindowState& state);

void drawAnalysisTimelineEyeAngleSection(
    const AnalysisTimelineEyeAngleContext& context,
    AnalysisTimelineWindowState& state);
