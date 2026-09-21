#pragma once

#include "data_access_scheduler.h"
#include "zarr/subject_mask_overlay_repository.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class SubjectMaskPlaybackDirection : uint8_t {
  Paused,
  Forward,
  Reverse,
};

// The explicit-demand overload lets session callers opt in or out without
// changing the configured policy used by the legacy requestFrame overload. A
// paused read-ahead demand seeds the minimum window in the most recently
// requested direction (forward before playback starts).
struct SubjectMaskPlaybackDemand {
  bool read_ahead = false;
  SubjectMaskPlaybackDirection direction = SubjectMaskPlaybackDirection::Paused;
  double source_frames_per_second = 0.0;
  double playback_rate = 1.0;
};

struct SubjectMaskOverlayBufferPolicy {
  size_t minimum_lookahead_frames = 4;
  size_t maximum_lookahead_frames = 24;
  size_t maximum_cached_frames = 25;
  uint64_t maximum_cached_payload_bytes = 96ULL * 1024ULL * 1024ULL;
  double minimum_lead_seconds = 0.25;
  double resolve_latency_multiplier = 2.5;
};

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
  uint64_t cache_evictions = 0;
  uint64_t byte_budget_evictions = 0;
  uint64_t oversized_result_rejections = 0;
  size_t peak_cached_frames = 0;
  size_t peak_pending_frames = 0;
  size_t cached_frames = 0;
  size_t pending_frames = 0;
  size_t effective_lookahead_frames = 0;
  // Contiguous terminal resolutions after current_frame. Missing and failed
  // frames count as resolved coverage; this is not a drawable-mask count.
  size_t contiguous_ready_ahead = 0;
  uint64_t maximum_cached_payload_bytes = 0;
  int64_t current_frame = -1;
  bool current_frame_ready = false;
  double average_resolve_ms = 0.0;
  double maximum_resolve_ms = 0.0;
  double current_average_queue_wait_ms = 0.0;
  double current_maximum_queue_wait_ms = 0.0;
  double current_average_service_ms = 0.0;
  double current_maximum_service_ms = 0.0;
  double speculative_average_queue_wait_ms = 0.0;
  double speculative_maximum_queue_wait_ms = 0.0;
  double speculative_average_service_ms = 0.0;
  double speculative_maximum_service_ms = 0.0;
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
  bool
  open(std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> repository,
       const SubjectMaskOverlayBufferPolicy &policy,
       std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t camera_frame, int full_frame_width,
                    int full_frame_height, bool discontinuity = false,
                    std::string *error = nullptr);
  bool requestFrame(int64_t camera_frame, int full_frame_width,
                    int full_frame_height,
                    const SubjectMaskPlaybackDemand &playback,
                    bool discontinuity = false,
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
