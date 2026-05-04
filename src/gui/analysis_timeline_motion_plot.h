#pragma once

#include "gui/analysis_timeline_window.h"
#include "zarr_loader.h"

#include <string>
#include <vector>

struct AnalysisTimelineMotionPlotContext {
    const AnalysisTimelineWindowState& state;
    const TimelineScrollState& scroll_state;
    int current_frame_num = 0;
    double video_fps = 0.0;

    const std::vector<float>& source_time_seconds;
    const std::vector<int32_t>& frame_indices;
    const std::vector<double>& time_plot;
    const std::vector<double>& smoothed_plot;
    const std::vector<double>& instant_plot;
    const std::vector<double>& detector_time_plot;
    const std::vector<double>& detector_value_plot;
    const std::vector<double>& heading_time_raw;
    const std::vector<double>& heading_raw_plot;
    const std::vector<double>& heading_time_smoothed;
    const std::vector<double>& heading_smoothed_plot;
    const std::vector<double>& heading_per_second_time_plot;
    const std::vector<double>& heading_per_second_plot;
    const std::vector<double>& heading_per_second_resultant_plot;
    const std::vector<double>& distance_time;
    const std::vector<double>& distance_units;

    const ZarrDetectionData::SwimBoutSeries* selected_swim_bouts = nullptr;
    std::string primary_speed_label;
    std::string primary_speed_units;
    std::string secondary_speed_label;
    std::string secondary_speed_units;
    double heading_axis_min = -180.0;
    double heading_axis_max = 180.0;
    double max_distance_mm = 0.0;
    const ZarrDetectionLoader* stimulus_loader = nullptr;
    bool show_stimulus_context = false;
};

double drawAnalysisTimelineMotionPlots(
    const AnalysisTimelineMotionPlotContext& context);
