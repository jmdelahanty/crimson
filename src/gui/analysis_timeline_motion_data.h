#pragma once

#include "zarr_loader.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct AnalysisTimelineMotionPreparedData {
    static constexpr double kMmPerPlotUnit = 10.0;

    std::vector<double> time_plot;
    std::vector<double> smoothed_plot;
    std::vector<double> instant_plot;
    std::vector<double> detector_time_plot;
    std::vector<double> detector_value_plot;

    std::vector<double> distance_time;
    std::vector<double> distance_units;
    double sum_distance_mm = 0.0;
    double max_distance_mm = 0.0;
    double min_distance_mm = std::numeric_limits<double>::infinity();
    size_t distance_samples = 0;
    size_t valid_distance_count = 0;

    std::vector<double> heading_time_raw;
    std::vector<double> heading_raw_plot;
    std::vector<double> heading_time_smoothed;
    std::vector<double> heading_smoothed_plot;
    std::vector<double> heading_per_second_time_plot;
    std::vector<double> heading_per_second_plot;
    std::vector<double> heading_per_second_resultant_plot;
    double heading_axis_min = -180.0;
    double heading_axis_max = 180.0;
};

struct AnalysisTimelineMotionDataInput {
    const std::vector<float>& time_seconds;
    const std::vector<int32_t>& frame_indices;
    const std::vector<float>& smoothed_speed;
    const std::vector<float>& instant_speed;
    const std::vector<float>& distance_mm;
    const std::vector<float>& heading_degrees;
    const std::vector<float>& smoothed_heading_degrees;
    const std::vector<uint8_t>& heading_keypoint_success;
    const std::vector<float>& heading_per_second_degrees;
    const std::vector<float>& heading_per_second_resultant;
    const std::vector<float>& heading_per_second_time;
    const ZarrDetectionData::SwimBoutSeries* selected_swim_bouts = nullptr;
    bool smoothed_available = false;
    bool instant_available = false;
    bool heading_per_second_available = false;
    bool show_detector_response = false;
    double video_fps = 0.0;
};

struct AnalysisTimelineMotionSpeedStats {
    bool has_primary_speed_data = false;
    size_t finite_count = 0;
    double average = std::numeric_limits<double>::quiet_NaN();
    double max = 0.0;
};

struct AnalysisTimelineMotionHeadingStats {
    bool valid = false;
    size_t count = 0;
    double circular_mean_deg = std::numeric_limits<double>::quiet_NaN();
    double mean_resultant_length = std::numeric_limits<double>::quiet_NaN();
    size_t per_second_resultant_count = 0;
    double average_per_second_resultant =
        std::numeric_limits<double>::quiet_NaN();
};

AnalysisTimelineMotionPreparedData prepareAnalysisTimelineMotionData(
    const AnalysisTimelineMotionDataInput& input);

AnalysisTimelineMotionSpeedStats computePrimarySpeedStats(
    const std::vector<float>& values);

AnalysisTimelineMotionHeadingStats computeHeadingStats(
    const AnalysisTimelineMotionPreparedData& prepared);

double computeFiniteMean(const std::vector<float>& values,
                         const std::vector<uint8_t>* valid_mask = nullptr);

size_t countValidMaskValues(const std::vector<uint8_t>& values);
