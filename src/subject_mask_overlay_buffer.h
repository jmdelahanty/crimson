#pragma once

#include "data_access_scheduler.h"
#include "zarr/subject_mask_overlay_repository.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct SubjectMaskOverlayBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t resolved_frames = 0;
  uint64_t missing_frames = 0;
  uint64_t failed_frames = 0;
  uint64_t discarded_results = 0;
  uint64_t cached_payload_bytes = 0;
  uint64_t peak_cached_payload_bytes = 0;
  uint64_t released_payload_bytes = 0;
  size_t peak_cached_frames = 0;
  size_t peak_pending_frames = 0;
  double maximum_resolve_ms = 0.0;
  uint64_t scheduler_submissions = 0;
  uint64_t scheduler_duplicates = 0;
  uint64_t scheduler_promotions = 0;
  uint64_t scheduler_capacity_rejections = 0;
  std::string last_error;
};

class SubjectMaskOverlayBuffer {
public:
  explicit SubjectMaskOverlayBuffer(
      std::shared_ptr<crimson::data::DataAccessScheduler> scheduler = nullptr,
      std::string archive_identity = {});
  ~SubjectMaskOverlayBuffer();

  SubjectMaskOverlayBuffer(const SubjectMaskOverlayBuffer &) = delete;
  SubjectMaskOverlayBuffer &
  operator=(const SubjectMaskOverlayBuffer &) = delete;

  bool
  open(std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> repository,
       size_t lookahead_frames = 12, size_t cache_capacity = 24,
       std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t camera_frame, int full_frame_width,
                    int full_frame_height, bool discontinuity = false,
                    std::string *error = nullptr);
  bool waitForFrame(int64_t camera_frame,
                    std::chrono::milliseconds timeout) const;
  std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
  frame(int64_t camera_frame) const;

  crimson::zarr::SubjectMaskOverlayDescriptor descriptor() const;
  SubjectMaskOverlayBufferMetrics metrics() const;
  crimson::zarr::SubjectMaskOverlayRepositoryMetrics repositoryMetrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
