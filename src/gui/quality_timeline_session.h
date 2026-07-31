#pragma once

#include "data_access_scheduler.h"
#include "detection_quality_timeline_buffer.h"
#include "gui/quality_timeline_window.h"
#include "keypoint_quality_timeline_buffer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace crimson::gui {

struct QualityTimelineArtifactSelection {
  std::string archive_path;
  std::string run_name;
  std::string manifest_digest;

  bool empty() const;
  bool complete() const;
  bool operator==(const QualityTimelineArtifactSelection &other) const;
};

struct QualityTimelineSessionRequest {
  std::string detection_archive_path;
  zarr::DetectionSurfaceKind detection_surface =
      zarr::DetectionSurfaceKind::CanonicalRawV1;
  std::string detection_run_name;
  bool allow_selector_ineligible_refined_run = false;

  QualityTimelineArtifactSelection raw_keypoints;
  QualityTimelineArtifactSelection keypoint_quality;
  QualityTimelineArtifactSelection refined_keypoints;
  QualityTimelineArtifactSelection body_frame;
  bool allow_selector_ineligible_keypoints = false;
  bool deep_validate_keypoint_identity = false;

  bool operator==(const QualityTimelineSessionRequest &other) const;
};

using DetectionQualityTimelineRepositoryFactory =
    std::function<std::unique_ptr<timeline::DetectionQualityTimelineRepository>(
        std::string *)>;
using KeypointQualityTimelineRepositoryFactory =
    std::function<std::unique_ptr<timeline::KeypointQualityTimelineRepository>(
        std::string *)>;

struct QualityTimelineRepositoryFactories {
  DetectionQualityTimelineRepositoryFactory detection;
  KeypointQualityTimelineRepositoryFactory keypoints;
};

struct QualityTimelineOpenMetrics {
  double total_ms = 0.0;
  size_t offset_read_calls = 0;
  size_t retained_offset_bytes = 0;
};

struct QualityTimelineSessionMetrics {
  timeline::DetectionQualityTimelineDescriptor detection_descriptor;
  QualityTimelineOpenMetrics detection_open;
  timeline::DetectionQualityTimelineRepositoryMetrics detection_repository;
  DetectionQualityTimelineBufferMetrics detection_buffer;

  timeline::KeypointQualityTimelineDescriptor keypoint_descriptor;
  QualityTimelineOpenMetrics keypoint_open;
  timeline::KeypointQualityTimelineRepositoryMetrics keypoint_repository;
  KeypointQualityTimelineBufferMetrics keypoint_buffer;
};

class QualityTimelineSession {
public:
  explicit QualityTimelineSession(
      std::shared_ptr<data::DataAccessScheduler> scheduler);
  ~QualityTimelineSession();

  QualityTimelineSession(const QualityTimelineSession &) = delete;
  QualityTimelineSession &operator=(const QualityTimelineSession &) = delete;

  void configure(QualityTimelineSessionRequest request,
                 QualityTimelineRepositoryFactories factories = {});
  void update(int64_t current_frame, bool discontinuity,
              bool detection_requested,
              const DetectionQualityTimelineControls &detection_controls,
              bool keypoint_requested,
              const KeypointQualityTimelineControls &keypoint_controls);
  void close();

  bool detectionConfigured() const;
  QualityTimelineLoadState detectionState() const;
  const std::string &detectionError() const;
  const timeline::DetectionQualityTimelineDescriptor *
  detectionDescriptor() const;
  std::shared_ptr<const timeline::DetectionQualityTimelineWindow>
  detectionWindow() const;
  std::shared_ptr<const timeline::DetectionQualityTimelineOverview>
  detectionOverview() const;

  bool keypointConfigured() const;
  QualityTimelineLoadState keypointState() const;
  const std::string &keypointError() const;
  const timeline::KeypointQualityTimelineDescriptor *keypointDescriptor() const;
  std::shared_ptr<const timeline::KeypointQualityTimelineWindow>
  keypointWindow() const;
  std::shared_ptr<const timeline::KeypointQualityTimelineOverview>
  keypointOverview() const;
  QualityTimelineSessionMetrics metrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace crimson::gui
