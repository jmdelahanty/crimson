#include "platform/nvidia/nvidia_performance_probe.h"
#include <iostream>
#include <unistd.h>

int main() {
  using Probe = crimson::platform::nvidia::PerformanceProbe;
  if (Probe::load(nullptr).enabled) return 1;
  char name[] = "/tmp/crimson-performance-case-test.XXXXXX";
  const int fd = mkstemp(name);
  if (fd < 0) return 1;
  close(fd);
  const auto check = [&](const std::string& text, bool valid) {
    std::ofstream(name) << text;
    try {
      const auto result = Probe::load(name);
      return valid && result.enabled;
    } catch (const std::exception&) {
      return !valid;
    }
  };
  const nlohmann::json base = {{"schema", "crimson.august_performance_case.v1"},
      {"shading", true}, {"overlays", true}, {"seek_frames", {0, 54010}}};
  bool passed = check(base.dump(), true);
  for (const auto& bad : {nlohmann::json(-1), nlohmann::json(2147483648LL),
                          nlohmann::json(1.5), nlohmann::json("1")}) {
    auto j = base;
    j["seek_frames"] = {bad};
    passed &= check(j.dump(), false);
  }
  auto j = base;
  j["seek_frames"] = std::vector<int>(65, 0);
  passed &= check(j.dump(), false);
  j = base;
  j["schema"] = "wrong";
  passed &= check(j.dump(), false);
  passed &= check("{", false);
  passed &= check(std::string(65537, ' '), false);
  std::filesystem::remove(name);
  if (!passed) std::cerr << "performance case parsing failed\n";
  return passed ? 0 : 1;
}
