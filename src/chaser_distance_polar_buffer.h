#pragma once

#include "chaser_distance_polar.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crimson::polar {

struct ChaserDistancePolarBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t ready_frames = 0;
  uint64_t empty_frames = 0;
  uint64_t missing_frames = 0;
  uint64_t unavailable_frames = 0;
  uint64_t unsupported_frames = 0;
  uint64_t failed_frames = 0;
  uint64_t discarded_results = 0;
  uint64_t source_points = 0;
  uint64_t published_points = 0;
  size_t peak_cached_frames = 0;
  size_t peak_pending_frames = 0;
  double maximum_resolve_ms = 0.0;
  std::string last_error;
};

class ChaserDistancePolarBuffer {
 public:
  ChaserDistancePolarBuffer();
  ~ChaserDistancePolarBuffer();

  ChaserDistancePolarBuffer(const ChaserDistancePolarBuffer&) = delete;
  ChaserDistancePolarBuffer& operator=(
      const ChaserDistancePolarBuffer&) = delete;

  bool open(std::unique_ptr<ChaserDistancePolarRepository> repository,
            size_t lookahead_frames = 8,
            size_t cache_capacity = 16,
            std::string* error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t camera_frame,
                    bool discontinuity = false,
                    std::string* error = nullptr);
  bool waitForFrame(int64_t camera_frame,
                    std::chrono::milliseconds timeout) const;
  std::shared_ptr<const ChaserDistancePolarFrameSample> frame(
      int64_t camera_frame) const;

  ChaserDistancePolarDescriptor descriptor() const;
  ChaserDistancePolarBufferMetrics metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace crimson::polar
