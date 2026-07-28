#pragma once

#include "data_access_scheduler.h"
#include "zarr/canonical_detection_repository.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class CanonicalDetectionResidencyState : uint8_t {
  Disabled,
  Ineligible,
  Loading,
  Ready,
  Cancelled,
  Failed,
};

struct CanonicalDetectionResidencyPolicy {
  uint64_t maximum_resident_bytes = 0;
  uint64_t maximum_chunk_decoded_bytes = 512 * 1024;

  bool enabled() const {
    return maximum_resident_bytes > 0 && maximum_chunk_decoded_bytes > 0;
  }
  bool admits(uint64_t decoded_bytes) const {
    return enabled() && decoded_bytes <= maximum_resident_bytes;
  }
};

CanonicalDetectionResidencyPolicy canonicalDetectionProductionResidencyPolicy();

struct CanonicalDetectionResidencyMetrics {
  CanonicalDetectionResidencyState state =
      CanonicalDetectionResidencyState::Disabled;
  uint64_t attempts = 0;
  uint64_t decoded_hot_bytes = 0;
  uint64_t maximum_resident_bytes = 0;
  uint64_t maximum_chunk_decoded_bytes = 0;
  uint64_t planned_chunks = 0;
  uint64_t completed_chunks = 0;
  uint64_t decoded_source_bytes = 0;
  uint64_t retained_bytes = 0;
  uint64_t stale_chunks = 0;
  uint64_t failed_chunks = 0;
  uint64_t publications = 0;
  double elapsed_ms = 0.0;
  double maximum_chunk_ms = 0.0;
  std::string last_error;
};

struct CanonicalDetectionBufferMetrics {
  uint64_t requests = 0;
  uint64_t cache_hits = 0;
  uint64_t demand_pages = 0;
  uint64_t lead_pages = 0;
  uint64_t resolved_pages = 0;
  uint64_t failed_pages = 0;
  uint64_t discarded_pages = 0;
  uint64_t evicted_pages = 0;
  size_t peak_cached_pages = 0;
  size_t peak_pending_pages = 0;
  uint64_t cached_bytes = 0;
  uint64_t peak_cached_bytes = 0;
  double maximum_resolve_ms = 0.0;
  std::string last_error;
};

class CanonicalDetectionBuffer {
public:
  explicit CanonicalDetectionBuffer(
      std::shared_ptr<crimson::data::DataAccessScheduler> scheduler = nullptr,
      std::string archive_identity = {});
  ~CanonicalDetectionBuffer();

  CanonicalDetectionBuffer(const CanonicalDetectionBuffer &) = delete;
  CanonicalDetectionBuffer &
  operator=(const CanonicalDetectionBuffer &) = delete;

  bool
  open(std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> repository,
       size_t page_frames = 70, size_t cache_pages = 32,
       std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool requestFrame(int64_t camera_frame, bool discontinuity = false,
                    std::string *error = nullptr);
  bool waitForFrame(int64_t camera_frame,
                    std::chrono::milliseconds timeout) const;
  std::shared_ptr<const crimson::zarr::CanonicalDetectionFrame>
  frame(int64_t camera_frame) const;

  bool startUiResidency(const CanonicalDetectionResidencyPolicy &policy,
                        std::string *error = nullptr);
  bool waitForUiResidency(std::chrono::milliseconds timeout) const;
  void cancelUiResidency();
  CanonicalDetectionResidencyMetrics residencyMetrics() const;

  crimson::zarr::CanonicalDetectionDescriptor descriptor() const;
  CanonicalDetectionBufferMetrics metrics() const;
  crimson::zarr::CanonicalDetectionRepositoryMetrics repositoryMetrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char *
canonicalDetectionResidencyStateName(CanonicalDetectionResidencyState state);
