#pragma once

#include "zarr/eye_geometry_overlay_repository.h"
#include "data_access_scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct EyeGeometryFrameDemand {
  int64_t frame = -1;
  crimson::zarr::EyeGeometryFieldMask fields = 0;
  bool lookahead = false;
};

struct EyeGeometryOverlayBufferMetrics {
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

class EyeGeometryOverlayBuffer {
public:
  EyeGeometryOverlayBuffer();
  EyeGeometryOverlayBuffer(
      std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
      std::string archive_identity);
  ~EyeGeometryOverlayBuffer();

  EyeGeometryOverlayBuffer(const EyeGeometryOverlayBuffer &) = delete;
  EyeGeometryOverlayBuffer &
  operator=(const EyeGeometryOverlayBuffer &) = delete;

  bool
  open(std::unique_ptr<crimson::zarr::EyeGeometryOverlayRepository> repository,
       size_t lookahead_frames = 6, size_t cache_capacity = 16,
       std::string *error = nullptr);
  void close();
  // Cancel queued canonical work when eye overlays are no longer requested.
  void suspend();
  bool isOpen() const;

  bool requestFrame(int64_t camera_frame, int full_frame_width,
                    int full_frame_height, bool discontinuity = false,
                    std::string *error = nullptr);
  // Scheduler-backed canonical demand. Each call replaces the desired frame
  // set; callers combine camera and inspection demand before submitting it.
  bool requestFrames(const std::vector<EyeGeometryFrameDemand> &demands,
                     int full_frame_width, int full_frame_height,
                     bool discontinuity = false,
                     std::string *error = nullptr);
  bool waitForFrame(int64_t camera_frame,
                    std::chrono::milliseconds timeout) const;
  std::shared_ptr<const crimson::zarr::EyeGeometryOverlayResolution>
  frame(int64_t camera_frame) const;

  crimson::zarr::EyeGeometryOverlayDescriptor descriptor() const;
  EyeGeometryOverlayBufferMetrics metrics() const;
  crimson::zarr::EyeGeometryOverlayRepository::AccessMetrics
  repositoryMetrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
