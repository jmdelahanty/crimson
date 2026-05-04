#pragma once

#include "gui/analysis_timeline_trace_plot.h"
#include "gui/analysis_timeline_window.h"

#include <vector>

struct AnalysisTimelineTailKinematicsContext {
    ZarrDetectionLoader& zarr_loader;
    TimelineScrollState& scroll_state;
    int current_frame_num = 0;
    double video_fps = 0.0;
    double fallback_current_time = -1.0;
    AnalysisTimelinePerfStats* perf_stats = nullptr;
};

void drawAnalysisTimelineTailKinematicsControls(
    const AnalysisTimelineTailKinematicsContext& context,
    AnalysisTimelineWindowState& state);

std::vector<AnalysisTimelineTracePlotRow>
buildAnalysisTimelineTailKinematicsRows(
    const AnalysisTimelineTailKinematicsContext& context,
    AnalysisTimelineWindowState& state);

void drawAnalysisTimelineTailKinematicsSection(
    const AnalysisTimelineTailKinematicsContext& context,
    AnalysisTimelineWindowState& state);
