#include "endurance_workload.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace crimson::diagnostics {
namespace {

void assignError(std::string *error, std::string value) {
  if (error) {
    *error = std::move(value);
  }
}

uint64_t splitMix64(uint64_t *state) {
  uint64_t value = (*state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

uint64_t median(std::vector<uint64_t> values) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  if (values.size() % 2 != 0) {
    return values[middle];
  }
  const uint64_t lower = values[middle - 1];
  const uint64_t upper = values[middle];
  return lower + (upper - lower) / 2;
}

int64_t signedDifference(uint64_t value, uint64_t baseline) {
  if (value >= baseline) {
    const uint64_t difference = value - baseline;
    return difference >
                   static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? std::numeric_limits<int64_t>::max()
               : static_cast<int64_t>(difference);
  }
  const uint64_t difference = baseline - value;
  return difference > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
             ? std::numeric_limits<int64_t>::min()
             : -static_cast<int64_t>(difference);
}

} // namespace

bool validateEnduranceWorkloadConfig(const EnduranceWorkloadConfig &config,
                                     std::string *error) {
  if (config.frame_count <= 0) {
    assignError(error, "Endurance frame count must be positive");
    return false;
  }
  if (config.traversal_span_frames <= 0 ||
      config.traversal_span_frames > config.frame_count) {
    assignError(error,
                "Endurance traversal span must fit the camera frame domain");
    return false;
  }
  if (config.page_frames == 0) {
    assignError(error, "Endurance page size must be positive");
    return false;
  }
  if (config.cycle_count == 0) {
    assignError(error, "Endurance cycle count must be positive");
    return false;
  }
  if (config.simultaneous_probes_per_cycle == 0) {
    assignError(error, "Endurance requires at least one simultaneous probe");
    return false;
  }
  if (config.seek_requests_per_cycle < 2) {
    assignError(error, "Endurance requires at least two rapid-seek requests");
    return false;
  }
  const uint64_t page_count =
      (static_cast<uint64_t>(config.frame_count) + config.page_frames - 1) /
      config.page_frames;
  if (page_count < config.seek_requests_per_cycle * 3) {
    assignError(error,
                "Endurance frame domain is too small for separated seeks");
    return false;
  }
  return true;
}

std::vector<EnduranceCyclePlan>
buildEnduranceWorkload(const EnduranceWorkloadConfig &config) {
  std::string error;
  if (!validateEnduranceWorkloadConfig(config, &error)) {
    return {};
  }

  const int64_t page_frames = static_cast<int64_t>(config.page_frames);
  const int64_t maximum_start =
      ((config.frame_count - config.traversal_span_frames) / page_frames) *
      page_frames;
  const uint64_t page_count =
      (static_cast<uint64_t>(config.frame_count) + config.page_frames - 1) /
      config.page_frames;
  const uint64_t seek_stride =
      std::max<uint64_t>(3, page_count / config.seek_requests_per_cycle);
  uint64_t random_state = config.seed;

  std::vector<EnduranceCyclePlan> plans;
  plans.reserve(config.cycle_count);
  for (size_t cycle = 0; cycle < config.cycle_count; ++cycle) {
    EnduranceCyclePlan plan;
    plan.cycle_index = cycle;
    if (config.cycle_count == 1) {
      plan.traversal_first_frame = maximum_start / 2;
    } else {
      plan.traversal_first_frame = static_cast<int64_t>(
          (static_cast<long double>(maximum_start) * cycle) /
          (config.cycle_count - 1));
    }
    plan.traversal_first_frame =
        (plan.traversal_first_frame / page_frames) * page_frames;
    plan.traversal_last_frame_exclusive =
        plan.traversal_first_frame + config.traversal_span_frames;
    plan.reverse = cycle % 2 != 0;

    plan.simultaneous_probe_frames.reserve(
        config.simultaneous_probes_per_cycle);
    for (size_t probe = 0; probe < config.simultaneous_probes_per_cycle;
         ++probe) {
      plan.simultaneous_probe_frames.push_back(
          static_cast<int64_t>(splitMix64(&random_state) %
                               static_cast<uint64_t>(config.frame_count)));
    }

    plan.rapid_seek_frames.reserve(config.seek_requests_per_cycle);
    const uint64_t seek_offset = splitMix64(&random_state) % page_count;
    for (size_t seek = 0; seek < config.seek_requests_per_cycle; ++seek) {
      const uint64_t page = (seek_offset + seek * seek_stride) % page_count;
      const uint64_t frame = std::min<uint64_t>(
          static_cast<uint64_t>(config.frame_count - 1),
          page * config.page_frames + config.page_frames / 2);
      plan.rapid_seek_frames.push_back(static_cast<int64_t>(frame));
    }
    plans.push_back(std::move(plan));
  }
  return plans;
}

MemoryPlateauResult
analyzeMemoryPlateau(const std::vector<MemoryPlateauSample> &samples,
                     const MemoryPlateauPolicy &policy) {
  MemoryPlateauResult result;
  result.total_samples = samples.size();
  if (policy.endpoint_window_samples == 0) {
    result.reason = "Endpoint window must contain at least one sample";
    return result;
  }
  if (samples.size() <= policy.warmup_samples) {
    result.reason = "No samples remain after the warmup interval";
    return result;
  }

  std::vector<MemoryPlateauSample> analyzed(
      samples.begin() + static_cast<std::ptrdiff_t>(policy.warmup_samples),
      samples.end());
  std::sort(
      analyzed.begin(), analyzed.end(),
      [](const MemoryPlateauSample &left, const MemoryPlateauSample &right) {
        return left.elapsed_ms < right.elapsed_ms;
      });
  result.analyzed_samples = analyzed.size();
  if (analyzed.size() < policy.minimum_analysis_samples ||
      analyzed.size() < policy.endpoint_window_samples * 2) {
    result.reason = "Too few post-warmup samples for a plateau verdict";
    return result;
  }
  if (!std::isfinite(analyzed.front().elapsed_ms) ||
      !std::isfinite(analyzed.back().elapsed_ms) ||
      analyzed.back().elapsed_ms <= analyzed.front().elapsed_ms) {
    result.reason = "Memory sample times do not span a positive interval";
    return result;
  }

  result.analysis_first_elapsed_ms = analyzed.front().elapsed_ms;
  result.analysis_last_elapsed_ms = analyzed.back().elapsed_ms;
  std::vector<uint64_t> initial_values;
  std::vector<uint64_t> final_values;
  initial_values.reserve(policy.endpoint_window_samples);
  final_values.reserve(policy.endpoint_window_samples);
  for (size_t index = 0; index < policy.endpoint_window_samples; ++index) {
    initial_values.push_back(analyzed[index].bytes);
    final_values.push_back(
        analyzed[analyzed.size() - policy.endpoint_window_samples + index]
            .bytes);
  }
  result.initial_window_median_bytes = median(std::move(initial_values));
  result.final_window_median_bytes = median(std::move(final_values));
  result.peak_bytes = std::max_element(analyzed.begin(), analyzed.end(),
                                       [](const MemoryPlateauSample &left,
                                          const MemoryPlateauSample &right) {
                                         return left.bytes < right.bytes;
                                       })
                          ->bytes;
  result.final_growth_bytes = signedDifference(
      result.final_window_median_bytes, result.initial_window_median_bytes);
  result.peak_growth_bytes =
      signedDifference(result.peak_bytes, result.initial_window_median_bytes);

  const double origin_ms = analyzed.front().elapsed_ms;
  double mean_minutes = 0.0;
  long double mean_bytes = 0.0;
  for (const auto &sample : analyzed) {
    mean_minutes += (sample.elapsed_ms - origin_ms) / 60000.0;
    mean_bytes += static_cast<long double>(sample.bytes);
  }
  mean_minutes /= static_cast<double>(analyzed.size());
  mean_bytes /= static_cast<long double>(analyzed.size());
  long double numerator = 0.0;
  long double denominator = 0.0;
  for (const auto &sample : analyzed) {
    const long double x =
        (sample.elapsed_ms - origin_ms) / 60000.0 - mean_minutes;
    const long double y = static_cast<long double>(sample.bytes) - mean_bytes;
    numerator += x * y;
    denominator += x * x;
  }
  if (denominator <= 0.0) {
    result.reason = "Memory sample times are not distinct";
    return result;
  }
  result.slope_bytes_per_minute = static_cast<double>(numerator / denominator);
  result.enough_data = true;

  const bool final_growth_pass =
      result.final_growth_bytes <= 0 ||
      static_cast<uint64_t>(result.final_growth_bytes) <=
          policy.maximum_final_growth_bytes;
  const bool peak_growth_pass =
      result.peak_growth_bytes <= 0 ||
      static_cast<uint64_t>(result.peak_growth_bytes) <=
          policy.maximum_peak_growth_bytes;
  const bool slope_pass =
      result.slope_bytes_per_minute <= policy.maximum_slope_bytes_per_minute;
  result.pass = final_growth_pass && peak_growth_pass && slope_pass;
  result.reason = result.pass
                      ? "Post-warmup memory stayed within the plateau policy"
                      : "Post-warmup memory exceeded a plateau policy limit";
  return result;
}

} // namespace crimson::diagnostics
