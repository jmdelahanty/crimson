#pragma once

#include "data_access_scheduler.h"
#include "keypoint_quality_timeline.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct KeypointQualityTimelineBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t resolved_windows = 0;
  uint64_t failed_windows = 0;
  uint64_t discarded_results = 0;
  uint64_t overview_requests = 0;
  uint64_t overview_cache_hits = 0;
  uint64_t resolved_overviews = 0;
  uint64_t failed_overviews = 0;
  uint64_t discarded_overviews = 0;
  size_t peak_cached_windows = 0;
  size_t peak_pending_windows = 0;
  double maximum_resolve_ms = 0.0;
  double maximum_overview_resolve_ms = 0.0;
  std::string last_error;
};

class KeypointQualityTimelineBuffer {
public:
  explicit KeypointQualityTimelineBuffer(
      std::shared_ptr<crimson::data::DataAccessScheduler> scheduler = nullptr,
      std::string archive_identity = {});
  ~KeypointQualityTimelineBuffer();

  KeypointQualityTimelineBuffer(const KeypointQualityTimelineBuffer &) = delete;
  KeypointQualityTimelineBuffer &
  operator=(const KeypointQualityTimelineBuffer &) = delete;

  bool
  open(std::unique_ptr<crimson::timeline::KeypointQualityTimelineRepository>
           repository,
       size_t page_span_frames = 4096, size_t page_step_frames = 2048,
       size_t cache_capacity = 3, std::string *error = nullptr);
  void close();
  bool isOpen() const;
  bool requestFrame(int64_t frame, bool discontinuity = false,
                    std::string *error = nullptr);
  std::shared_ptr<const crimson::timeline::KeypointQualityTimelineWindow>
  window(int64_t frame) const;
  bool requestOverview(size_t maximum_points_per_trace = 1200,
                       size_t maximum_decoded_bytes = 8 * 1024 * 1024,
                       std::string *error = nullptr);
  std::shared_ptr<const crimson::timeline::KeypointQualityTimelineOverview>
  overview() const;
  crimson::timeline::KeypointQualityTimelineDescriptor descriptor() const;
  KeypointQualityTimelineBufferMetrics metrics() const;
  crimson::timeline::KeypointQualityTimelineRepositoryMetrics
  repositoryMetrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
