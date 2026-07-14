#pragma once

#include "eye_angle_timeline.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct EyeAngleTimelineBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t resolved_windows = 0;
  uint64_t missing_windows = 0;
  uint64_t failed_windows = 0;
  uint64_t discarded_results = 0;
  uint64_t source_rows_read = 0;
  uint64_t published_points = 0;
  size_t peak_pending_windows = 0;
  size_t peak_cached_windows = 0;
  double maximum_resolve_ms = 0.0;
  std::string last_error;
};

class EyeAngleTimelineBuffer {
 public:
  EyeAngleTimelineBuffer();
  ~EyeAngleTimelineBuffer();

  EyeAngleTimelineBuffer(const EyeAngleTimelineBuffer&) = delete;
  EyeAngleTimelineBuffer& operator=(const EyeAngleTimelineBuffer&) = delete;

  bool open(
      std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository> repository,
      size_t page_span_frames = 4096, size_t page_step_frames = 2048,
      size_t max_points_per_trace = 1200, size_t cache_capacity = 3,
      std::string* error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t frame, const std::string& representation_key,
                    double fallback_frames_per_second,
                    bool discontinuity = false, std::string* error = nullptr);
  bool waitForFrame(int64_t frame, const std::string& representation_key,
                    double fallback_frames_per_second,
                    std::chrono::milliseconds timeout) const;
  std::shared_ptr<const crimson::timeline::EyeAngleTimelineWindow> window(
      int64_t frame, const std::string& representation_key) const;

  crimson::timeline::EyeAngleTimelineDescriptor descriptor() const;
  EyeAngleTimelineBufferMetrics metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
