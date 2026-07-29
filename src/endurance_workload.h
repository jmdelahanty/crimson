#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crimson::diagnostics {

struct EnduranceWorkloadConfig {
  int64_t frame_count = 0;
  int64_t traversal_span_frames = 3500;
  size_t page_frames = 70;
  size_t cycle_count = 0;
  size_t simultaneous_probes_per_cycle = 2;
  size_t seek_requests_per_cycle = 8;
  uint64_t seed = 20260729;
};

struct EnduranceCyclePlan {
  size_t cycle_index = 0;
  int64_t traversal_first_frame = 0;
  int64_t traversal_last_frame_exclusive = 0;
  bool reverse = false;
  std::vector<int64_t> simultaneous_probe_frames;
  std::vector<int64_t> rapid_seek_frames;
};

bool validateEnduranceWorkloadConfig(const EnduranceWorkloadConfig &config,
                                     std::string *error = nullptr);

std::vector<EnduranceCyclePlan>
buildEnduranceWorkload(const EnduranceWorkloadConfig &config);

struct MemoryPlateauSample {
  double elapsed_ms = 0.0;
  uint64_t bytes = 0;
};

struct MemoryPlateauPolicy {
  size_t warmup_samples = 2;
  size_t minimum_analysis_samples = 4;
  size_t endpoint_window_samples = 2;
  uint64_t maximum_final_growth_bytes = 256ULL * 1024ULL * 1024ULL;
  uint64_t maximum_peak_growth_bytes = 512ULL * 1024ULL * 1024ULL;
  double maximum_slope_bytes_per_minute = 64.0 * 1024.0 * 1024.0;
};

struct MemoryPlateauResult {
  bool enough_data = false;
  bool pass = false;
  size_t total_samples = 0;
  size_t analyzed_samples = 0;
  double analysis_first_elapsed_ms = 0.0;
  double analysis_last_elapsed_ms = 0.0;
  uint64_t initial_window_median_bytes = 0;
  uint64_t final_window_median_bytes = 0;
  uint64_t peak_bytes = 0;
  int64_t final_growth_bytes = 0;
  int64_t peak_growth_bytes = 0;
  double slope_bytes_per_minute = 0.0;
  std::string reason;
};

MemoryPlateauResult
analyzeMemoryPlateau(const std::vector<MemoryPlateauSample> &samples,
                     const MemoryPlateauPolicy &policy);

} // namespace crimson::diagnostics
