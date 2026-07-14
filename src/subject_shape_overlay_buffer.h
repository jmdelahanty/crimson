#pragma once

#include "zarr/subject_shape_overlay_repository.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct SubjectShapeOverlayBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t resolved_frames = 0;
  uint64_t missing_frames = 0;
  uint64_t failed_frames = 0;
  uint64_t discarded_results = 0;
  size_t peak_pending_frames = 0;
  size_t peak_cached_frames = 0;
  double maximum_resolve_ms = 0.0;
  std::string last_error;
};

class SubjectShapeOverlayBuffer {
public:
  SubjectShapeOverlayBuffer();
  ~SubjectShapeOverlayBuffer();

  SubjectShapeOverlayBuffer(const SubjectShapeOverlayBuffer &) = delete;
  SubjectShapeOverlayBuffer &
  operator=(const SubjectShapeOverlayBuffer &) = delete;

  bool open(
      std::unique_ptr<crimson::zarr::SubjectShapeOverlayRepository> repository,
      size_t lookahead_frames = 6, size_t cache_capacity = 16,
      std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t camera_frame, int full_frame_width,
                    int full_frame_height, bool discontinuity = false,
                    std::string *error = nullptr);
  bool waitForFrame(int64_t camera_frame,
                    std::chrono::milliseconds timeout) const;
  std::shared_ptr<const crimson::zarr::SubjectShapeOverlayResolution>
  frame(int64_t camera_frame) const;

  crimson::zarr::SubjectShapeOverlayDescriptor descriptor() const;
  SubjectShapeOverlayBufferMetrics metrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
