#pragma once

#include "swim_bout_timeline.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct SwimBoutTimelineBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t resolved_windows = 0;
  uint64_t missing_windows = 0;
  uint64_t failed_windows = 0;
  uint64_t discarded_results = 0;
  uint64_t candidate_intervals_scanned = 0;
  uint64_t source_detector_rows_read = 0;
  uint64_t published_intervals = 0;
  uint64_t published_detector_points = 0;
  size_t peak_pending_windows = 0;
  size_t peak_cached_windows = 0;
  double maximum_resolve_ms = 0.0;
  std::string last_error;
};

class SwimBoutTimelineBuffer {
public:
  SwimBoutTimelineBuffer();
  ~SwimBoutTimelineBuffer();

  SwimBoutTimelineBuffer(const SwimBoutTimelineBuffer &) = delete;
  SwimBoutTimelineBuffer &operator=(const SwimBoutTimelineBuffer &) = delete;

  bool open(
      std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository> repository,
      size_t page_span_frames = 4096, size_t page_step_frames = 2048,
      size_t max_detector_points = 1200, size_t cache_capacity = 4,
      std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t frame, const std::string &candidate_key,
                    double fallback_frames_per_second,
                    bool include_detector_trace, bool discontinuity = false,
                    std::string *error = nullptr);
  bool waitForFrame(int64_t frame, const std::string &candidate_key,
                    double fallback_frames_per_second,
                    bool include_detector_trace,
                    std::chrono::milliseconds timeout) const;
  std::shared_ptr<const crimson::timeline::SwimBoutTimelineWindow>
  window(int64_t frame, const std::string &candidate_key,
         bool include_detector_trace) const;

  crimson::timeline::SwimBoutTimelineDescriptor descriptor() const;
  SwimBoutTimelineBufferMetrics metrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
