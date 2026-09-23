#include "process_memory.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
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

bool testProcessSnapshot() {
  const auto snapshot = crimson::diagnostics::sampleProcessMemory();
  CHECK(snapshot.current_rss_supported);
  CHECK(snapshot.peak_rss_supported);
  CHECK(snapshot.current_rss_bytes > 0);
  CHECK(snapshot.peak_rss_bytes > 0);
  return true;
}

bool testSampler() {
  crimson::diagnostics::ProcessMemorySampler sampler(
      std::chrono::milliseconds(2));
  CHECK(sampler.start("startup"));
  CHECK(!sampler.start("duplicate"));
  sampler.setPhase("allocation");
  std::vector<uint8_t> allocation(4 * 1024 * 1024, 1);
  for (size_t index = 0; index < allocation.size(); index += 4096) {
    allocation[index] ^= 1;
  }
  sampler.mark("allocation_ready");
  sampler.stop();
  const auto samples = sampler.samples();
  CHECK(samples.size() >= 4);
  CHECK(samples.front().event == "start");
  CHECK(samples.back().event == "stop");
  CHECK(sampler.maximumObserved().current_rss_supported);
  return true;
}

bool testAttribution() {
  crimson::diagnostics::ProcessMemorySnapshot process;
  process.current_rss_supported = true;
  process.current_rss_bytes = 1000;
  auto attribution = crimson::diagnostics::attributeProcessMemory(
      process, {{"offsets", "index", 100, true, {}},
                {"cache", "decoded_cache", 250, false, "partial"}});
  CHECK(attribution.reported_retained_bytes == 350);
  CHECK(attribution.unattributed_rss_bytes == 650);
  CHECK(attribution.reported_over_rss_bytes == 0);
  CHECK(!attribution.all_owners_complete);

  process.current_rss_bytes = 10;
  attribution = crimson::diagnostics::attributeProcessMemory(
      process, {{"a", "test", std::numeric_limits<uint64_t>::max(), true, {}},
                {"b", "test", 1, true, {}}});
  CHECK(attribution.reported_retained_bytes ==
        std::numeric_limits<uint64_t>::max());
  CHECK(attribution.reported_over_rss_bytes ==
        std::numeric_limits<uint64_t>::max() - 10);
  return true;
}

} // namespace

int main() {
  if (!testProcessSnapshot() || !testSampler() || !testAttribution()) {
    return EXIT_FAILURE;
  }
  std::cout << "process_memory_tests: PASS\n";
  return EXIT_SUCCESS;
}
