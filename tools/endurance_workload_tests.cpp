#include "endurance_workload.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testInvalidConfigurations() {
  crimson::diagnostics::EnduranceWorkloadConfig config;
  std::string error;
  CHECK(!crimson::diagnostics::validateEnduranceWorkloadConfig(config, &error));
  CHECK(!error.empty());

  config.frame_count = 1000;
  config.traversal_span_frames = 100;
  config.page_frames = 10;
  config.cycle_count = 2;
  config.simultaneous_probes_per_cycle = 1;
  config.seek_requests_per_cycle = 4;
  CHECK(crimson::diagnostics::validateEnduranceWorkloadConfig(config, &error));
  return true;
}

bool testDeterministicCoveragePlan() {
  crimson::diagnostics::EnduranceWorkloadConfig config;
  config.frame_count = 10000;
  config.traversal_span_frames = 700;
  config.page_frames = 70;
  config.cycle_count = 5;
  config.simultaneous_probes_per_cycle = 3;
  config.seek_requests_per_cycle = 4;
  config.seed = 17;
  const auto first = crimson::diagnostics::buildEnduranceWorkload(config);
  const auto second = crimson::diagnostics::buildEnduranceWorkload(config);
  CHECK(first.size() == config.cycle_count);
  CHECK(second.size() == first.size());
  CHECK(first.front().traversal_first_frame == 0);
  CHECK(first.back().traversal_last_frame_exclusive <= config.frame_count);
  for (size_t index = 0; index < first.size(); ++index) {
    CHECK(first[index].cycle_index == index);
    CHECK(first[index].reverse == (index % 2 != 0));
    CHECK(first[index].traversal_first_frame %
              static_cast<int64_t>(config.page_frames) ==
          0);
    CHECK(first[index].simultaneous_probe_frames ==
          second[index].simultaneous_probe_frames);
    CHECK(first[index].rapid_seek_frames == second[index].rapid_seek_frames);
    CHECK(first[index].simultaneous_probe_frames.size() == 3);
    CHECK(first[index].rapid_seek_frames.size() == 4);
    for (const int64_t frame : first[index].simultaneous_probe_frames) {
      CHECK(frame >= 0 && frame < config.frame_count);
    }
  }
  return true;
}

bool testStablePlateauPasses() {
  using crimson::diagnostics::MemoryPlateauSample;
  const uint64_t mib = 1024ULL * 1024ULL;
  std::vector<MemoryPlateauSample> samples;
  for (size_t index = 0; index < 10; ++index) {
    samples.push_back(
        {static_cast<double>(index) * 10000.0, 100 * mib + (index % 2) * mib});
  }
  crimson::diagnostics::MemoryPlateauPolicy policy;
  policy.warmup_samples = 2;
  policy.minimum_analysis_samples = 6;
  policy.endpoint_window_samples = 2;
  policy.maximum_final_growth_bytes = 8 * mib;
  policy.maximum_peak_growth_bytes = 8 * mib;
  policy.maximum_slope_bytes_per_minute = 8.0 * mib;
  const auto result =
      crimson::diagnostics::analyzeMemoryPlateau(samples, policy);
  CHECK(result.enough_data);
  CHECK(result.pass);
  CHECK(result.analyzed_samples == 8);
  return true;
}

bool testGrowingMemoryFails() {
  using crimson::diagnostics::MemoryPlateauSample;
  const uint64_t mib = 1024ULL * 1024ULL;
  std::vector<MemoryPlateauSample> samples;
  for (size_t index = 0; index < 10; ++index) {
    samples.push_back(
        {static_cast<double>(index) * 10000.0, (100 + index * 16) * mib});
  }
  crimson::diagnostics::MemoryPlateauPolicy policy;
  policy.warmup_samples = 2;
  policy.minimum_analysis_samples = 6;
  policy.endpoint_window_samples = 2;
  policy.maximum_final_growth_bytes = 64 * mib;
  policy.maximum_peak_growth_bytes = 128 * mib;
  policy.maximum_slope_bytes_per_minute = 64.0 * mib;
  const auto result =
      crimson::diagnostics::analyzeMemoryPlateau(samples, policy);
  CHECK(result.enough_data);
  CHECK(!result.pass);
  CHECK(result.final_growth_bytes > 64 * static_cast<int64_t>(mib));
  CHECK(result.slope_bytes_per_minute > 64.0 * mib);
  return true;
}

bool testInsufficientPlateauEvidence() {
  crimson::diagnostics::MemoryPlateauPolicy policy;
  const auto result = crimson::diagnostics::analyzeMemoryPlateau(
      {{0.0, 10}, {1.0, 10}, {2.0, 10}}, policy);
  CHECK(!result.enough_data);
  CHECK(!result.pass);
  CHECK(!result.reason.empty());
  return true;
}

} // namespace

int main() {
  if (!testInvalidConfigurations() || !testDeterministicCoveragePlan() ||
      !testStablePlateauPasses() || !testGrowingMemoryFails() ||
      !testInsufficientPlateauEvidence()) {
    return EXIT_FAILURE;
  }
  std::cout << "endurance_workload_tests: PASS\n";
  return EXIT_SUCCESS;
}
