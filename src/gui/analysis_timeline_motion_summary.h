#pragma once

#include "gui/analysis_timeline_motion_data.h"
#include "gui/analysis_timeline_trace_plot.h"
#include "gui/analysis_timeline_window.h"
#include "zarr_loader.h"

#include <optional>
#include <string>
#include <vector>

struct AnalysisTimelineMotionSummaryContext {
    const AnalysisTimelineWindowState& state;
    const TimelineScrollState& scroll_state;
    const ZarrDetectionData::MovementSeries* selected_series = nullptr;
    const std::vector<float>& time_data;
    const std::vector<float>& smoothed_speed;
    const AnalysisTimelineMotionPreparedData& motion_data;
    double current_time_line = -1.0;
    std::string primary_speed_label;
    std::string primary_speed_units;
};

std::optional<AnalysisTimelineTracePlotRow> buildAnalysisTimelineTrackPositionRow(
    const AnalysisTimelineMotionSummaryContext& context);

void drawAnalysisTimelineMotionSummary(
    const AnalysisTimelineMotionSummaryContext& context);
