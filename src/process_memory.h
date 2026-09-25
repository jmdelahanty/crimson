#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::diagnostics {

struct ProcessMemorySnapshot {
  uint64_t current_rss_bytes = 0;
  uint64_t peak_rss_bytes = 0;
  bool current_rss_supported = false;
  bool peak_rss_supported = false;
};

ProcessMemorySnapshot sampleProcessMemory();

struct ProcessMemorySample {
  double elapsed_ms = 0.0;
  std::string phase;
  std::string event;
  ProcessMemorySnapshot process;
};

class ProcessMemorySampler {
public:
  explicit ProcessMemorySampler(
      std::chrono::milliseconds interval = std::chrono::milliseconds(25));
  ~ProcessMemorySampler();

  ProcessMemorySampler(const ProcessMemorySampler &) = delete;
  ProcessMemorySampler &operator=(const ProcessMemorySampler &) = delete;

  bool start(std::string initial_phase = "process_start");
  void setPhase(std::string phase);
  void mark(std::string event);
  void stop();

  bool running() const;
  std::vector<ProcessMemorySample> samples() const;
  ProcessMemorySnapshot maximumObserved() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct RetainedMemoryOwner {
  std::string owner;
  std::string category;
  uint64_t retained_bytes = 0;
  bool complete = false;
  std::string note;
};

struct MemoryAttributionSnapshot {
  ProcessMemorySnapshot process;
  std::vector<RetainedMemoryOwner> owners;
  uint64_t reported_retained_bytes = 0;
  uint64_t unattributed_rss_bytes = 0;
  uint64_t reported_over_rss_bytes = 0;
  bool all_owners_complete = false;
};

MemoryAttributionSnapshot
attributeProcessMemory(ProcessMemorySnapshot process,
                       std::vector<RetainedMemoryOwner> owners);

} // namespace crimson::diagnostics
