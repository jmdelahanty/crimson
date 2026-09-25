#pragma once

#include "canonical_detection_buffer.h"
#include "data_access_scheduler.h"
#include "zarr/detection_repository.h"
#include "zarr/detection_repository_selection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace crimson::platform::nvidia {

enum class NvidiaDetectionState : uint8_t {
  Closed,
  Opening,
  Ready,
  Failed,
};

struct NvidiaDetectionOpenRequest {
  std::string archive_path;
  std::string canonical_raw_run;
  double frames_per_second = 0.0;
  // Non-zero values bind the selected canonical surface to the active indexed
  // video before the bridge can become Ready.
  size_t expected_camera_frame_count = 0;
  size_t expected_source_width = 0;
  size_t expected_source_height = 0;
  size_t page_frames = 70;
  size_t cache_pages = 32;
  // Empty preserves compatibility with canonical fixtures/manifests that do
  // not carry recording identity. Non-empty requires an exact manifest match.
  std::string expected_recording_identity;
};

// An injected opener is a test seam for slow, failed, or deterministic storage
// opens. Production callers should omit it. It runs only on the bridge's open
// worker, never on the caller/UI thread.
using NvidiaDetectionStorageOpener = std::function<
    std::unique_ptr<crimson::zarr::CanonicalDetectionRepository>(
        const NvidiaDetectionOpenRequest &request,
        crimson::zarr::DetectionRepositorySelectionMetrics *metrics,
        std::string *error)>;

struct NvidiaDetectionRepositoryMetrics {
  NvidiaDetectionState state = NvidiaDetectionState::Closed;
  uint64_t generation = 0;
  uint64_t open_attempts = 0;
  uint64_t successful_opens = 0;
  uint64_t failed_opens = 0;
  uint64_t cancelled_opens = 0;
  uint64_t frame_requests = 0;
  uint64_t rejected_frame_requests = 0;
  uint64_t cache_resolves = 0;
  uint64_t cache_misses = 0;
  uint64_t stale_frames_discarded = 0;
  uint64_t invalid_geometry_frames = 0;
  double last_open_ms = 0.0;
  // CanonicalDetectionBuffer is page-count bounded. This flag is deliberately
  // false: cached_bytes is observed usage, not a hard admission budget.
  bool page_cache_hard_byte_budgeted = false;
  std::string archive_path;
  std::string canonical_raw_run;
  std::string last_error;
  crimson::zarr::DetectionRepositorySelectionMetrics selection;
  CanonicalDetectionBufferMetrics buffer;
  crimson::zarr::CanonicalDetectionRepositoryMetrics repository;
  crimson::data::DataAccessSchedulerMetrics scheduler;
};

class NvidiaDetectionRepository final
    : public crimson::zarr::DetectionRepository {
public:
  explicit NvidiaDetectionRepository(
      std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
      NvidiaDetectionStorageOpener storage_opener = {});
  ~NvidiaDetectionRepository() override;

  NvidiaDetectionRepository(const NvidiaDetectionRepository &) = delete;
  NvidiaDetectionRepository &
  operator=(const NvidiaDetectionRepository &) = delete;

  // Starts archive discovery, canonical offset loading, and repository open on
  // a worker. A failed canonical open remains Failed; no legacy fallback is
  // installed by this bridge.
  bool beginOpen(NvidiaDetectionOpenRequest request,
                 std::string *error = nullptr);

  // Direct adoption seam for a fake/in-memory canonical repository. Payload
  // range reads still happen only through CanonicalDetectionBuffer's scheduler.
  bool openForTesting(
      std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> repository,
      NvidiaDetectionOpenRequest request = {},
      std::string *error = nullptr);

  void close();
  NvidiaDetectionState state() const;
  // Returns true once the current open is in any terminal state (Ready, Failed,
  // or Closed); false means the timeout expired while it was still Opening.
  bool waitUntilOpen(std::chrono::milliseconds timeout) const;

  // Requests the exact presented parent/camera frame. This only schedules
  // bounded page work and never waits for storage.
  bool requestPresentedFrame(int64_t camera_frame,
                             bool discontinuity = false,
                             std::string *error = nullptr);

  crimson::zarr::DetectionRepositoryDescriptor descriptor() const override;
  std::vector<crimson::zarr::DetectionDatasetOption>
  availableDatasets() const override;
  bool selectDataset(crimson::zarr::DetectionDataset dataset) override;
  bool isDatasetAvailable(
      crimson::zarr::DetectionDataset dataset) const override;
  size_t observationCount(size_t frame_id) const override;
  bool isFrameInterpolated(size_t frame_id) const override;
  crimson::zarr::DetectionFrame
  resolveFrame(size_t frame_id, bool use_interpolated = false) const override;

  NvidiaDetectionRepositoryMetrics metrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char *nvidiaDetectionStateName(NvidiaDetectionState state);

} // namespace crimson::platform::nvidia
