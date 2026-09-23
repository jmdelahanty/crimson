#include "process_memory.h"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <psapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace crimson::diagnostics {
namespace {

uint64_t saturatingAdd(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    return std::numeric_limits<uint64_t>::max();
  }
  return left + right;
}

} // namespace

ProcessMemorySnapshot sampleProcessMemory() {
  ProcessMemorySnapshot snapshot;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) !=
      0) {
    snapshot.current_rss_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
    snapshot.peak_rss_bytes =
        static_cast<uint64_t>(counters.PeakWorkingSetSize);
    snapshot.current_rss_supported = true;
    snapshot.peak_rss_supported = true;
  }
#elif defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    snapshot.current_rss_bytes = static_cast<uint64_t>(info.resident_size);
    snapshot.current_rss_supported = true;
  }
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0) {
    snapshot.peak_rss_bytes = static_cast<uint64_t>(usage.ru_maxrss);
    snapshot.peak_rss_supported = true;
  }
#else
  std::ifstream statm("/proc/self/statm");
  uint64_t total_pages = 0;
  uint64_t resident_pages = 0;
  if (statm >> total_pages >> resident_pages) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0 &&
        resident_pages <= std::numeric_limits<uint64_t>::max() /
                              static_cast<uint64_t>(page_size)) {
      snapshot.current_rss_bytes =
          resident_pages * static_cast<uint64_t>(page_size);
      snapshot.current_rss_supported = true;
    }
  }
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0) {
    snapshot.peak_rss_bytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
    snapshot.peak_rss_supported = true;
  }
#endif
  return snapshot;
}

struct ProcessMemorySampler::Impl {
  explicit Impl(std::chrono::milliseconds sample_interval)
      : interval(std::max(sample_interval, std::chrono::milliseconds(1))) {}

  void append(std::string event) {
    ProcessMemorySample sample;
    sample.elapsed_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    sample.phase = phase;
    sample.event = std::move(event);
    sample.process = sampleProcessMemory();
    samples.push_back(std::move(sample));
  }

  using Clock = std::chrono::steady_clock;
  std::chrono::milliseconds interval;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  Clock::time_point started{};
  std::string phase;
  std::vector<ProcessMemorySample> samples;
  bool active = false;
  bool stopping = false;
};

ProcessMemorySampler::ProcessMemorySampler(std::chrono::milliseconds interval)
    : impl_(std::make_unique<Impl>(interval)) {}

ProcessMemorySampler::~ProcessMemorySampler() { stop(); }

bool ProcessMemorySampler::start(std::string initial_phase) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active || impl_->worker.joinable()) {
    return false;
  }
  impl_->samples.clear();
  impl_->phase = std::move(initial_phase);
  impl_->started = Impl::Clock::now();
  impl_->stopping = false;
  impl_->active = true;
  impl_->append("start");
  impl_->worker = std::thread([this] {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    while (!impl_->stopping) {
      if (impl_->wake.wait_for(lock, impl_->interval,
                               [this] { return impl_->stopping; })) {
        break;
      }
      impl_->append("sample");
    }
  });
  return true;
}

void ProcessMemorySampler::setPhase(std::string phase) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active) {
    return;
  }
  impl_->phase = std::move(phase);
  impl_->append("phase");
}

void ProcessMemorySampler::mark(std::string event) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) {
    impl_->append(std::move(event));
  }
}

void ProcessMemorySampler::stop() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->worker.joinable()) {
      impl_->active = false;
      return;
    }
    if (impl_->active) {
      impl_->append("stop");
    }
    impl_->active = false;
    impl_->stopping = true;
  }
  impl_->wake.notify_all();
  impl_->worker.join();
}

bool ProcessMemorySampler::running() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->active;
}

std::vector<ProcessMemorySample> ProcessMemorySampler::samples() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->samples;
}

ProcessMemorySnapshot ProcessMemorySampler::maximumObserved() const {
  const auto recorded = samples();
  ProcessMemorySnapshot maximum;
  for (const auto &sample : recorded) {
    if (sample.process.current_rss_supported) {
      maximum.current_rss_supported = true;
      maximum.current_rss_bytes =
          std::max(maximum.current_rss_bytes, sample.process.current_rss_bytes);
    }
    if (sample.process.peak_rss_supported) {
      maximum.peak_rss_supported = true;
      maximum.peak_rss_bytes =
          std::max(maximum.peak_rss_bytes, sample.process.peak_rss_bytes);
    }
  }
  return maximum;
}

MemoryAttributionSnapshot
attributeProcessMemory(ProcessMemorySnapshot process,
                       std::vector<RetainedMemoryOwner> owners) {
  MemoryAttributionSnapshot result;
  result.process = process;
  result.owners = std::move(owners);
  result.all_owners_complete = !result.owners.empty();
  for (const auto &owner : result.owners) {
    result.reported_retained_bytes =
        saturatingAdd(result.reported_retained_bytes, owner.retained_bytes);
    result.all_owners_complete = result.all_owners_complete && owner.complete;
  }
  if (process.current_rss_supported) {
    if (result.reported_retained_bytes <= process.current_rss_bytes) {
      result.unattributed_rss_bytes =
          process.current_rss_bytes - result.reported_retained_bytes;
    } else {
      result.reported_over_rss_bytes =
          result.reported_retained_bytes - process.current_rss_bytes;
    }
  }
  return result;
}

} // namespace crimson::diagnostics
