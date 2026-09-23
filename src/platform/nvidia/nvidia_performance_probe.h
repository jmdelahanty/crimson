#pragma once

// Opt-in instrumentation only. No archive reads, cache flushing, or scheduling.
#include "data_access_scheduler.h"
#include <tensorstore/internal/metrics/registry.h>
#include <nlohmann/json.hpp>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace crimson::platform::nvidia {
inline nlohmann::json performancePeakRssKiB() {
#ifdef _WIN32
  return nullptr; // The NFS suite is Linux-only; do not invent a measurement.
#else
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return nullptr;
#ifdef __APPLE__
  return usage.ru_maxrss / 1024;
#else
  return usage.ru_maxrss;
#endif
#endif
}
struct PerformanceProbe {
  bool enabled = false;
  bool shading = true;
  bool overlays = true;
  std::vector<int> seeks;
  size_t seek_index = 0;
  bool seek_active = false;
  int settled_draws = 0;
  double seek_timeout_ms = 15000.0;
  double initial_timeout_ms = 30000.0;

  static PerformanceProbe load(const char* path) {
    PerformanceProbe result;
    if (!path) return result;
    if (std::filesystem::file_size(path) > 65536)
      throw std::runtime_error("Performance case exceeds 64 KiB");
    std::ifstream stream(path);
    const auto j = nlohmann::json::parse(stream);
    if (j.at("schema") != "crimson.august_performance_case.v1")
      throw std::runtime_error("Invalid performance case schema");
    result.enabled = true;
    result.shading = j.at("shading").get<bool>();
    result.overlays = j.at("overlays").get<bool>();
    const auto& seeks = j.at("seek_frames");
    if (!seeks.is_array() || seeks.size() > 64)
      throw std::runtime_error("Performance case permits at most 64 seeks");
    for (const auto& item : seeks) {
      if (!item.is_number_integer())
        throw std::runtime_error("Performance seek frame must be an integer");
      if (item.is_number_unsigned() && item.get<uint64_t>() >
          static_cast<uint64_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Performance seek frame outside supported range");
      const auto frame = item.get<int64_t>();
      if (frame < 0 || frame > std::numeric_limits<int>::max())
        throw std::runtime_error("Performance seek frame outside supported range");
      result.seeks.push_back(static_cast<int>(frame));
    }
    return result;
  }
};

inline nlohmann::json performanceFileCounters() {
  nlohmann::json result = nlohmann::json::object();
  for (const auto* name : {"read", "batch_read", "bytes_read"}) {
    const auto metric = tensorstore::internal_metrics::GetMetricRegistry().Collect(
        std::string("/tensorstore/kvstore/file/") + name);
    if (!metric || metric->values.empty()) {
      result[name] = nullptr; // Missing instrumentation is not a measured zero.
      continue;
    }
    int64_t total = 0;
    for (const auto& value : metric->values)
      total += std::get<int64_t>(value.value);
    result[name] = total;
  }
  return result;
}

inline nlohmann::json performanceSchedulerCounters(
    const data::DataAccessSchedulerMetrics& metrics) {
  nlohmann::json result = {
      {"failed", metrics.queue.failed_completions},
      {"exceptions", metrics.work_exceptions},
      {"pending", metrics.queue.pending_requests},
      {"active", metrics.queue.active_requests},
      {"completed", metrics.queue.completed_requests}};
  result["priorities"] = nlohmann::json::array();
  for (size_t i = 0; i < metrics.timing_by_priority.size(); ++i) {
    const auto& timing = metrics.timing_by_priority[i];
    result["priorities"].push_back({
        {"priority", data::requestPriorityName(static_cast<data::RequestPriority>(i))},
        {"started", timing.started}, {"completed", timing.completed},
        {"queue_total_ms", timing.total_queue_wait_ms},
        {"queue_max_ms", timing.maximum_queue_wait_ms},
        {"service_total_ms", timing.total_service_ms},
        {"service_max_ms", timing.maximum_service_ms}});
  }
  return result;
}
} // namespace crimson::platform::nvidia
