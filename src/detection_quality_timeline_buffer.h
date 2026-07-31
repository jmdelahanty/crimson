#pragma once

#include "data_access_scheduler.h"
#include "detection_quality_timeline.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct DetectionQualityTimelineBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t resolved_windows = 0;
  uint64_t failed_windows = 0;
  uint64_t discarded_results = 0;
  size_t peak_cached_windows = 0;
  size_t peak_pending_windows = 0;
  double maximum_resolve_ms = 0.0;
  std::string last_error;
};

class DetectionQualityTimelineBuffer {
public:
  explicit DetectionQualityTimelineBuffer(
      std::shared_ptr<crimson::data::DataAccessScheduler> scheduler = nullptr,
      std::string archive_identity = {});
  ~DetectionQualityTimelineBuffer();

  DetectionQualityTimelineBuffer(const DetectionQualityTimelineBuffer &) =
      delete;
  DetectionQualityTimelineBuffer &
  operator=(const DetectionQualityTimelineBuffer &) = delete;

  bool
  open(std::unique_ptr<crimson::timeline::DetectionQualityTimelineRepository>
           repository,
       size_t page_span_frames = 8192, size_t page_step_frames = 4096,
       size_t cache_capacity = 3, std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t frame, bool discontinuity = false,
                    std::string *error = nullptr);
  std::shared_ptr<const crimson::timeline::DetectionQualityTimelineWindow>
  window(int64_t frame) const;

  crimson::timeline::DetectionQualityTimelineDescriptor descriptor() const;
  DetectionQualityTimelineBufferMetrics metrics() const;
  crimson::timeline::DetectionQualityTimelineRepositoryMetrics
  repositoryMetrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
