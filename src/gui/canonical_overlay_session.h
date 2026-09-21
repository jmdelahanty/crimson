#pragma once

#include "data_access_scheduler.h"
#include "zarr/canonical_overlay_selection.h"
#include "zarr/keypoint_overlay_repository.h"
#include "zarr/subject_mask_overlay_repository.h"
#include "zarr/subject_shape_overlay_repository.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace crimson::gui {

enum class CanonicalOverlayState : uint8_t {
  Closed, Opening, Pending, Ready, Empty, Unavailable, Failed
};
const char* canonicalOverlayStateName(CanonicalOverlayState state);

struct CanonicalOverlayOpenRequest {
  std::string archive_path;
  std::string recording_id;
  std::string eye_run;
  size_t frame_count = 0;
  int source_width = 0;
  int source_height = 0;
};

struct CanonicalOverlayRepositories {
  zarr::CanonicalOverlaySelection selection;
  std::unique_ptr<zarr::KeypointOverlayRepository> keypoints;
  std::unique_ptr<zarr::SubjectMaskOverlayRepository> masks;
  std::unique_ptr<zarr::SubjectShapeOverlayRepository> shapes;
  std::string keypoint_error, mask_error, shape_error, error;
};
using CanonicalOverlayOpenFunction =
    std::function<CanonicalOverlayRepositories(const CanonicalOverlayOpenRequest&)>;

template <typename Descriptor, typename Resolution>
struct CanonicalOverlayProductSnapshot {
  CanonicalOverlayState state = CanonicalOverlayState::Unavailable;
  Descriptor descriptor;
  std::shared_ptr<const Resolution> frame;
  std::string error;
};
struct CanonicalOverlaySnapshot {
  uint64_t generation = 0;
  int64_t requested_frame = -1;
  CanonicalOverlayState state = CanonicalOverlayState::Closed;
  std::shared_ptr<const zarr::CanonicalOverlaySelection> selection;
  CanonicalOverlayProductSnapshot<zarr::KeypointOverlayDescriptor,
                                  zarr::KeypointOverlayResolution> keypoints;
  CanonicalOverlayProductSnapshot<zarr::SubjectMaskOverlayDescriptor,
                                  zarr::SubjectMaskOverlayResolution> masks;
  CanonicalOverlayProductSnapshot<zarr::SubjectShapeOverlayDescriptor,
                                  zarr::SubjectShapeOverlayResolution> shapes;
  zarr::SubjectMaskOverlayRepositoryMetrics mask_metrics;
  double open_ms = 0.0;
  std::string error;
};

// Open/retirement occurs on one lifecycle worker; the UI only submits demand
// and inspects immutable per-frame snapshots. Source epochs are scheduler-unique.
class CanonicalOverlaySession {
 public:
  explicit CanonicalOverlaySession(
      std::shared_ptr<data::DataAccessScheduler> scheduler,
      CanonicalOverlayOpenFunction opener = {});
  ~CanonicalOverlaySession();
  CanonicalOverlaySession(const CanonicalOverlaySession&) = delete;
  CanonicalOverlaySession& operator=(const CanonicalOverlaySession&) = delete;

  bool beginOpen(CanonicalOverlayOpenRequest request, std::string* error = nullptr);
  void close(); // Invalidates immediately; draining remains on lifecycle worker.
  void shutdown(); // Blocking teardown; call before shutting down the scheduler.
  bool requestFrame(int64_t frame, bool keypoints, bool masks, bool shapes,
                    bool discontinuity = false);
  CanonicalOverlaySnapshot snapshot(int64_t frame) const;
  bool waitUntilOpen(std::chrono::milliseconds timeout) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

CanonicalOverlayRepositories openCanonicalOverlayRepositories(
    const CanonicalOverlayOpenRequest& request);

} // namespace crimson::gui
