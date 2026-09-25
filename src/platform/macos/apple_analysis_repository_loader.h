#pragma once

#include "analysis_series_timeline.h"
#include "chaser_distance_polar.h"
#include "data_access.h"
#include "data_access_scheduler.h"
#include "eye_angle_timeline.h"
#include "loading_progress.h"
#include "stimulus_context_timeline.h"
#include "swim_bout_timeline.h"
#include "zarr/acquisition_crop_repository.h"
#include "zarr/analysis_crop_geometry_repository.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_detection_repository.h"
#include "zarr/detection_repository_selection.h"
#include "zarr/eye_geometry_overlay_repository.h"
#include "zarr/keypoint_overlay_repository.h"
#include "zarr/stimulus_repository.h"
#include "zarr/subject_mask_overlay_repository.h"
#include "zarr/subject_shape_overlay_repository.h"
#include "zarr/tensorstore_keypoint_overlay_repository.h"
#include "zarr/tensorstore_keypoint_v2_repository.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct AppleKeypointV2ArtifactSelection {
  std::string archive_path;
  std::string run;
  std::string manifest_digest;

  bool empty() const {
    return archive_path.empty() && run.empty() && manifest_digest.empty();
  }

  bool complete() const {
    return !archive_path.empty() && !run.empty() && !manifest_digest.empty();
  }
};

struct AppleKeypointV2LoadRequest {
  AppleKeypointV2ArtifactSelection raw;
  AppleKeypointV2ArtifactSelection quality;
  AppleKeypointV2ArtifactSelection refined;
  AppleKeypointV2ArtifactSelection body_frame;
  bool allow_selector_ineligible = false;
  bool deep_validate_identity = false;

  bool enabled() const { return !raw.empty(); }
};

struct AppleAnalysisRepositoryLoadRequest {
  std::string archive_path;
  std::string detection_run;
  std::string refined_detection_run;
  bool allow_selector_ineligible_refined_run = false;
  std::string stimulus_run;
  std::string stimulus_video_override;
  std::string crop_run;
  std::string swim_bout_run;
  AppleKeypointV2LoadRequest keypoint_v2;
  std::string subject_mask_run;
  std::string subject_mask_manifest_payload_digest;
  bool allow_selector_ineligible_subject_mask_run = false;
  bool require_subject_mask_v1 = false;
  std::string subject_mask_presentation_cache_path;
  std::string subject_mask_presentation_cache_run;
  std::string subject_mask_presentation_cache_manifest_payload_digest;
  bool subject_mask_contour_only = false;
  size_t camera_frame_count = 0;
  bool subject_masks_enabled = true;
  bool subject_shapes_enabled = true;
  bool eye_geometry_enabled = true;
  bool motion_timeline_enabled = true;
  bool swim_bout_timeline_enabled = true;
  bool eye_angle_timeline_enabled = true;
  bool tail_kinematics_timeline_enabled = true;
  bool stimulus_context_timeline_enabled = true;
  uint64_t small_trace_preload_budget_bytes = 128ULL * 1024ULL * 1024ULL;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
};

using AppleAnalysisRepositoryLoadProgress =
    crimson::loading::LoadingProgressSnapshot;

struct AppleAnalysisRepositoryLoadTiming {
  std::string product;
  double elapsed_ms = 0.0;
  bool available = false;
  std::string error;
};

struct AppleAnalysisRepositoryBundle {
  std::shared_ptr<crimson::zarr::ArchiveContext> archive;
  std::unique_ptr<crimson::polar::ChaserDistancePolarRepository>
      chaser_distance_polar;
  std::unique_ptr<crimson::zarr::StimulusRepository> stimulus;
  std::unique_ptr<crimson::zarr::KeypointOverlayRepository> keypoints;
  std::unique_ptr<crimson::zarr::CanonicalDetectionRepository>
      canonical_detection;
  std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> subject_masks;
  std::unique_ptr<crimson::zarr::SubjectShapeOverlayRepository> subject_shape;
  std::unique_ptr<crimson::zarr::EyeGeometryOverlayRepository> eye_geometry;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository> motion;
  std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository> swim_bouts;
  std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository> eye_angles;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
      tail_kinematics;
  std::unique_ptr<crimson::timeline::StimulusContextTimelineRepository>
      stimulus_context;
  std::unique_ptr<crimson::zarr::AnalysisCropGeometryRepository> crop_geometry;
  std::unique_ptr<crimson::zarr::AcquisitionCropRepository> acquisition_crop;
  crimson::zarr::KeypointRepositoryOpenMetrics keypoint_open_metrics;
  crimson::zarr::KeypointV2RepositoryOpenMetrics keypoint_v2_open_metrics;
  bool keypoint_v2_selected = false;
  crimson::zarr::CanonicalDetectionRepositoryOpenMetrics
      canonical_detection_open_metrics;
  crimson::zarr::DetectionRepositorySelectionMetrics
      detection_selection_metrics;

  std::vector<AppleAnalysisRepositoryLoadTiming> timings;
  double total_elapsed_ms = 0.0;
  uint64_t preloaded_trace_bytes = 0;
  bool cancelled = false;
  std::string archive_error;

  std::string errorFor(const std::string &product) const;
  bool hasResultFor(const std::string &product) const;
};

AppleAnalysisRepositoryBundle OpenAppleAnalysisRepositoryBundle(
    const AppleAnalysisRepositoryLoadRequest &request);

class AppleAnalysisRepositoryLoader {
public:
  AppleAnalysisRepositoryLoader();
  ~AppleAnalysisRepositoryLoader();

  AppleAnalysisRepositoryLoader(const AppleAnalysisRepositoryLoader &) = delete;
  AppleAnalysisRepositoryLoader &
  operator=(const AppleAnalysisRepositoryLoader &) = delete;

  bool start(AppleAnalysisRepositoryLoadRequest request,
             std::string *error = nullptr);
  bool loading() const;
  AppleAnalysisRepositoryLoadProgress progress() const;
  // Returns the next independently completed archive or product result. This
  // remains callable while loading() is true.
  std::optional<AppleAnalysisRepositoryBundle> takeReady();
  void cancel();
  void close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
