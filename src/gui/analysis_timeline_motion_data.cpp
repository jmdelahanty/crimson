#include "gui/analysis_timeline_motion_data.h"

#include "ui_path_config.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

namespace {

bool isFiniteFloatValue(float value) {
    return std::isfinite(static_cast<double>(value));
}

std::optional<double> frameToTime(const std::vector<int32_t>& frame_indices,
                                  const std::vector<float>& time_data,
                                  double video_fps,
                                  int32_t frame) {
    if (frame < 0) {
        return std::nullopt;
    }
    if (!frame_indices.empty()) {
        auto upper = std::lower_bound(frame_indices.begin(),
                                      frame_indices.end(),
                                      frame);
        if (upper != frame_indices.end() && *upper == frame) {
            const size_t idx =
                static_cast<size_t>(std::distance(frame_indices.begin(), upper));
            if (idx < time_data.size()) {
                return static_cast<double>(time_data[idx]);
            }
        }
        if (upper != frame_indices.end() && upper != frame_indices.begin()) {
            auto lower = upper - 1;
            const size_t lower_idx =
                static_cast<size_t>(std::distance(frame_indices.begin(), lower));
            const size_t upper_idx =
                static_cast<size_t>(std::distance(frame_indices.begin(), upper));
            if (lower_idx < time_data.size() && upper_idx < time_data.size()) {
                const int32_t f0 = *lower;
                const int32_t f1 = *upper;
                const double t0 = static_cast<double>(time_data[lower_idx]);
                const double t1 = static_cast<double>(time_data[upper_idx]);
                const int32_t delta_f = f1 - f0;
                if (delta_f != 0) {
                    const double alpha = static_cast<double>(frame - f0) /
                                         static_cast<double>(delta_f);
                    return t0 + alpha * (t1 - t0);
                }
            }
        } else if (upper == frame_indices.begin() && !time_data.empty()) {
            return static_cast<double>(time_data.front());
        } else if (upper == frame_indices.end() && !time_data.empty()) {
            return static_cast<double>(time_data.back());
        }
    }
    if (video_fps > 0.0) {
        return static_cast<double>(frame) / video_fps;
    }
    return std::nullopt;
}

void extendHeadingRange(double value,
                        double& heading_y_min,
                        double& heading_y_max) {
    heading_y_min = std::min(heading_y_min, value);
    heading_y_max = std::max(heading_y_max, value);
}

void finalizeHeadingAxis(double heading_y_min,
                         double heading_y_max,
                         double& min_out,
                         double& max_out) {
    if (heading_y_min == std::numeric_limits<double>::infinity() ||
        heading_y_max == -std::numeric_limits<double>::infinity()) {
        min_out = -180.0;
        max_out = 180.0;
        return;
    }
    min_out = heading_y_min;
    max_out = heading_y_max;
    const double span = std::max(10.0, max_out - min_out);
    const double padding = std::max(5.0, span * 0.1);
    min_out -= padding;
    max_out += padding;
    if (min_out >= max_out) {
        min_out -= 1.0;
        max_out += 1.0;
    }
}

}  // namespace

AnalysisTimelineMotionPreparedData prepareAnalysisTimelineMotionData(
    const AnalysisTimelineMotionDataInput& input) {
    AnalysisTimelineMotionPreparedData prepared;

    prepared.time_plot.reserve(input.time_seconds.size());
    prepared.smoothed_plot.reserve(input.smoothed_speed.size());
    if (!input.instant_speed.empty()) {
        prepared.instant_plot.reserve(input.instant_speed.size());
    }

    for (size_t i = 0; i < input.time_seconds.size(); ++i) {
        prepared.time_plot.push_back(static_cast<double>(input.time_seconds[i]));
        if (input.smoothed_available && i < input.smoothed_speed.size()) {
            prepared.smoothed_plot.push_back(
                static_cast<double>(input.smoothed_speed[i]));
        }
        if (input.instant_available && i < input.instant_speed.size()) {
            prepared.instant_plot.push_back(
                static_cast<double>(input.instant_speed[i]));
        }
    }

    if (input.selected_swim_bouts != nullptr &&
        input.show_detector_response &&
        input.selected_swim_bouts->has_detector_trace &&
        !input.selected_swim_bouts->detector_trace_values.empty()) {
        const size_t detector_count =
            input.selected_swim_bouts->detector_trace_values.size();
        prepared.detector_time_plot.reserve(detector_count);
        prepared.detector_value_plot.reserve(detector_count);
        for (size_t i = 0; i < detector_count; ++i) {
            const float value =
                input.selected_swim_bouts->detector_trace_values[i];
            if (!isFiniteFloatValue(value)) {
                continue;
            }
            std::optional<double> t;
            if (i < input.selected_swim_bouts
                        ->detector_trace_frame_indices.size()) {
                t = frameToTime(
                    input.frame_indices,
                    input.time_seconds,
                    input.video_fps,
                    input.selected_swim_bouts->detector_trace_frame_indices[i]);
            } else if (i < input.time_seconds.size()) {
                t = static_cast<double>(input.time_seconds[i]);
            }
            if (!t.has_value()) {
                continue;
            }
            prepared.detector_time_plot.push_back(*t);
            prepared.detector_value_plot.push_back(static_cast<double>(value));
        }
    }

    prepared.distance_samples =
        std::min(input.time_seconds.size(), input.distance_mm.size());
    prepared.distance_time.reserve(prepared.distance_samples);
    prepared.distance_units.reserve(prepared.distance_samples);
    for (size_t i = 0; i < prepared.distance_samples; ++i) {
        const float raw_distance = input.distance_mm[i];
        if (!IsFiniteFloat(raw_distance)) {
            continue;
        }
        const double t = static_cast<double>(input.time_seconds[i]);
        const double value_mm = static_cast<double>(raw_distance);
        prepared.distance_time.push_back(t);
        prepared.distance_units.push_back(
            value_mm / AnalysisTimelineMotionPreparedData::kMmPerPlotUnit);
        prepared.sum_distance_mm += value_mm;
        prepared.max_distance_mm =
            std::max(prepared.max_distance_mm, value_mm);
        prepared.min_distance_mm =
            std::min(prepared.min_distance_mm, value_mm);
        ++prepared.valid_distance_count;
    }

    prepared.heading_time_raw.reserve(
        std::min(input.time_seconds.size(), input.heading_degrees.size()));
    prepared.heading_raw_plot.reserve(
        std::min(input.time_seconds.size(), input.heading_degrees.size()));
    prepared.heading_time_smoothed.reserve(std::min(
        input.time_seconds.size(), input.smoothed_heading_degrees.size()));
    prepared.heading_smoothed_plot.reserve(std::min(
        input.time_seconds.size(), input.smoothed_heading_degrees.size()));

    auto heading_sample_allowed = [&](size_t index) {
        if (!input.heading_keypoint_success.empty() &&
            index < input.heading_keypoint_success.size()) {
            return input.heading_keypoint_success[index] != 0;
        }
        return true;
    };

    double heading_y_min = std::numeric_limits<double>::infinity();
    double heading_y_max = -std::numeric_limits<double>::infinity();

    const size_t raw_samples =
        std::min(input.time_seconds.size(), input.heading_degrees.size());
    for (size_t i = 0; i < raw_samples; ++i) {
        if (!heading_sample_allowed(i)) {
            continue;
        }
        const float heading_raw_val = input.heading_degrees[i];
        if (!IsFiniteFloat(heading_raw_val)) {
            continue;
        }
        const double t = static_cast<double>(input.time_seconds[i]);
        const double v = static_cast<double>(heading_raw_val);
        prepared.heading_time_raw.push_back(t);
        prepared.heading_raw_plot.push_back(v);
        extendHeadingRange(v, heading_y_min, heading_y_max);
    }

    const size_t smoothed_samples = std::min(
        input.time_seconds.size(), input.smoothed_heading_degrees.size());
    for (size_t i = 0; i < smoothed_samples; ++i) {
        if (!heading_sample_allowed(i)) {
            continue;
        }
        const float heading_smooth_val = input.smoothed_heading_degrees[i];
        if (!IsFiniteFloat(heading_smooth_val)) {
            continue;
        }
        const double t = static_cast<double>(input.time_seconds[i]);
        const double v = static_cast<double>(heading_smooth_val);
        prepared.heading_time_smoothed.push_back(t);
        prepared.heading_smoothed_plot.push_back(v);
        extendHeadingRange(v, heading_y_min, heading_y_max);
    }

    if (input.heading_per_second_available) {
        const size_t per_samples = std::min(
            input.heading_per_second_degrees.size(),
            input.heading_per_second_time.size());
        prepared.heading_per_second_time_plot.reserve(per_samples);
        prepared.heading_per_second_plot.reserve(per_samples);
        if (!input.heading_per_second_resultant.empty()) {
            prepared.heading_per_second_resultant_plot.reserve(per_samples);
        }
        for (size_t i = 0; i < per_samples; ++i) {
            const float heading_val = input.heading_per_second_degrees[i];
            const float heading_time_val = input.heading_per_second_time[i];
            if (!IsFiniteFloat(heading_val) ||
                !IsFiniteFloat(heading_time_val)) {
                continue;
            }
            const double t = static_cast<double>(heading_time_val);
            const double v = static_cast<double>(heading_val);
            prepared.heading_per_second_time_plot.push_back(t);
            prepared.heading_per_second_plot.push_back(v);
            extendHeadingRange(v, heading_y_min, heading_y_max);
            if (!input.heading_per_second_resultant.empty() &&
                i < input.heading_per_second_resultant.size()) {
                const float resultant = input.heading_per_second_resultant[i];
                if (IsFiniteFloat(resultant)) {
                    prepared.heading_per_second_resultant_plot.push_back(
                        static_cast<double>(resultant));
                } else {
                    prepared.heading_per_second_resultant_plot.push_back(
                        std::numeric_limits<double>::quiet_NaN());
                }
            }
        }
        if (!input.heading_per_second_resultant.empty() &&
            prepared.heading_per_second_resultant_plot.size() !=
                prepared.heading_per_second_plot.size()) {
            prepared.heading_per_second_resultant_plot.resize(
                prepared.heading_per_second_plot.size(),
                std::numeric_limits<double>::quiet_NaN());
        }
    }

    finalizeHeadingAxis(heading_y_min,
                        heading_y_max,
                        prepared.heading_axis_min,
                        prepared.heading_axis_max);
    return prepared;
}

AnalysisTimelineMotionSpeedStats computePrimarySpeedStats(
    const std::vector<float>& values) {
    AnalysisTimelineMotionSpeedStats stats;
    stats.has_primary_speed_data = !values.empty();
    stats.average = 0.0;
    for (float value : values) {
        if (!IsFiniteFloat(value)) {
            continue;
        }
        stats.average += static_cast<double>(value);
        stats.max = std::max(stats.max, static_cast<double>(value));
        ++stats.finite_count;
    }
    if (stats.finite_count > 0) {
        stats.average /= static_cast<double>(stats.finite_count);
    } else {
        stats.average = std::numeric_limits<double>::quiet_NaN();
    }
    return stats;
}

AnalysisTimelineMotionHeadingStats computeHeadingStats(
    const AnalysisTimelineMotionPreparedData& prepared) {
    AnalysisTimelineMotionHeadingStats stats;
    stats.average_per_second_resultant = 0.0;
    const std::vector<double>* heading_for_stats = nullptr;
    if (!prepared.heading_smoothed_plot.empty()) {
        heading_for_stats = &prepared.heading_smoothed_plot;
    } else if (!prepared.heading_raw_plot.empty()) {
        heading_for_stats = &prepared.heading_raw_plot;
    }
    if (heading_for_stats && !heading_for_stats->empty()) {
        double sum_cos = 0.0;
        double sum_sin = 0.0;
        for (double deg : *heading_for_stats) {
            const double rad = deg * static_cast<double>(M_PI) / 180.0;
            sum_cos += std::cos(rad);
            sum_sin += std::sin(rad);
        }
        stats.valid = true;
        stats.count = heading_for_stats->size();
        const double mean_rad = std::atan2(sum_sin, sum_cos);
        stats.circular_mean_deg =
            mean_rad * 180.0 / static_cast<double>(M_PI);
        stats.mean_resultant_length =
            std::sqrt(sum_cos * sum_cos + sum_sin * sum_sin) /
            static_cast<double>(stats.count);
    }

    for (double value : prepared.heading_per_second_resultant_plot) {
        if (std::isfinite(value)) {
            stats.average_per_second_resultant += value;
            ++stats.per_second_resultant_count;
        }
    }
    if (stats.per_second_resultant_count > 0) {
        stats.average_per_second_resultant /=
            static_cast<double>(stats.per_second_resultant_count);
    } else {
        stats.average_per_second_resultant =
            std::numeric_limits<double>::quiet_NaN();
    }
    return stats;
}

double computeFiniteMean(const std::vector<float>& values,
                         const std::vector<uint8_t>* valid_mask) {
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        if (valid_mask && i < valid_mask->size() && (*valid_mask)[i] == 0) {
            continue;
        }
        if (!isFiniteFloatValue(values[i])) {
            continue;
        }
        sum += static_cast<double>(values[i]);
        ++count;
    }
    return count == 0 ? std::numeric_limits<double>::quiet_NaN()
                      : sum / static_cast<double>(count);
}

size_t countValidMaskValues(const std::vector<uint8_t>& values) {
    return static_cast<size_t>(
        std::count_if(values.begin(), values.end(), [](uint8_t value) {
            return value != 0;
        }));
}
