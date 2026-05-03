#pragma once

#include "gui/analysis_timeline_window.h"
#include "zarr_loader.h"

#include <cstddef>
#include <string>

struct AnalysisTimelineMotionSelection {
    const ZarrDetectionData::MovementSeries* selected_series = nullptr;
    const ZarrDetectionData::SwimBoutSeries* selected_swim_bouts = nullptr;
    const ZarrDetectionData::BoutKinematicsSeries* selected_bout_kinematics =
        nullptr;
};

struct AnalysisTimelineMotionControlsContext {
    ZarrDetectionLoader& zarr_loader;
    TimelineScrollState& scroll_state;
    const ZarrDetectionData::MovementSeries* selected_series = nullptr;
    size_t data_point_count = 0;
    bool smoothed_available = false;
    bool instant_available = false;
    bool heading_sample_available = false;
    bool heading_per_second_available = false;
    std::string primary_speed_label;
    std::string primary_speed_units;
    std::string secondary_speed_label;
};

const ZarrDetectionData::MovementSeries* drawAnalysisTimelineMotionSourceSelector(
    ZarrDetectionLoader& zarr_loader);

AnalysisTimelineMotionSelection drawAnalysisTimelineMotionControls(
    const AnalysisTimelineMotionControlsContext& context,
    AnalysisTimelineWindowState& state);
