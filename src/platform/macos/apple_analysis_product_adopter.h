#pragma once

#include "analysis_product_lifecycle.h"
#include "analysis_series_timeline_buffer.h"
#include "apple_acquisition_crop_playback_session.h"
#include "apple_analysis_repository_loader.h"
#include "apple_stimulus_playback_session.h"
#include "apple_video_playback_buffer.h"
#include "apple_video_viewer_ui.h"
#include "canonical_detection_buffer.h"
#include "chaser_distance_polar_buffer.h"
#include "eye_angle_timeline_buffer.h"
#include "eye_geometry_overlay_buffer.h"
#include "keypoint_overlay_buffer.h"
#include "subject_mask_overlay_buffer.h"
#include "subject_mask_presentation_coordinator.h"
#include "subject_shape_overlay_buffer.h"
#include "swim_bout_timeline_buffer.h"
#include "workspace_state.h"
#include "zarr/analysis_crop_geometry_repository.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct AppleAnalysisProductAdoptionOptions {
  uint64_t generation = 0;
  int64_t initial_frame = 0;
  bool video_smoke = false;
  bool ui_reference_enabled = false;
  bool prefer_alternate_eye_angle_representation = false;
  bool show_analysis_timeline = false;
  bool subject_masks_enabled = true;
  bool subject_shapes_enabled = true;
  bool eye_geometry_enabled = true;
  bool motion_timeline_enabled = true;
  bool swim_bout_timeline_enabled = true;
  bool eye_angle_timeline_enabled = true;
  bool tail_kinematics_timeline_enabled = true;
  bool stimulus_context_timeline_enabled = true;
  bool require_motion_timeline = false;
  bool require_swim_bout_timeline = false;
  bool require_eye_angle_timeline = false;
  bool require_tail_kinematics_timeline = false;
  bool require_stimulus_context_timeline = false;
  bool explicit_detection_requested = false;
  bool keypoint_v2_requested = false;
  size_t stimulus_buffer_capacity = 0;
  size_t acquisition_crop_buffer_capacity = 0;
  uint64_t detection_residency_budget_bytes = 0;
  uint64_t detection_residency_chunk_bytes = 0;
};

struct AppleAnalysisProductAdoptionContext {
  struct Archive {
    std::shared_ptr<crimson::zarr::ArchiveContext> &repository;
    std::string &loading_start_error;
  } archive;

  struct Presentation {
    AppleVideoPlaybackBuffer &video;
    crimson::workspace::WorkspaceState &workspace;
    AppleAnalysisTimelineControls &timelines;
  } presentation;

  struct ChaserPolar {
    crimson::polar::ChaserDistancePolarBuffer &buffer;
    crimson::polar::ChaserDistancePolarDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
    int64_t &last_camera_request;
  } chaser_polar;

  struct Stimulus {
    AppleStimulusPlaybackSession &playback;
    bool &enabled;
    std::string &error;
  } stimulus;

  struct Detection {
    CanonicalDetectionBuffer &buffer;
    crimson::zarr::CanonicalDetectionDescriptor &descriptor;
    crimson::zarr::CanonicalDetectionRepositoryOpenMetrics &open_metrics;
    bool &available;
    bool &failed;
    std::string &error;
    int64_t &last_camera_request;
  } detection;

  struct Keypoints {
    KeypointOverlayBuffer &buffer;
    crimson::zarr::KeypointOverlayDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
    int64_t &last_camera_request;
  } keypoints;

  struct SubjectMasks {
    SubjectMaskOverlayBuffer &buffer;
    crimson::overlay::SubjectMaskPresentationCoordinator &presentation;
    crimson::zarr::SubjectMaskOverlayDescriptor &descriptor;
    bool &available;
    bool &failed;
    bool &smoke_start_pending;
    std::optional<std::chrono::steady_clock::time_point>
        &initial_request_started;
    std::optional<int64_t> &initial_request_frame;
    std::string &error;
    std::function<void()> log_first_ready;
  } subject_masks;

  struct SubjectShape {
    SubjectShapeOverlayBuffer &buffer;
    crimson::zarr::SubjectShapeOverlayDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
  } subject_shape;

  struct EyeGeometry {
    EyeGeometryOverlayBuffer &buffer;
    crimson::zarr::EyeGeometryOverlayDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
  } eye_geometry;

  struct MotionTimeline {
    AnalysisSeriesTimelineBuffer &buffer;
    crimson::timeline::AnalysisSeriesTimelineDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
  } motion;

  struct SwimBoutTimeline {
    SwimBoutTimelineBuffer &buffer;
    crimson::timeline::SwimBoutTimelineDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
  } swim_bouts;

  struct EyeAngleTimeline {
    EyeAngleTimelineBuffer &buffer;
    crimson::timeline::EyeAngleTimelineDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
  } eye_angles;

  struct TailKinematicsTimeline {
    AnalysisSeriesTimelineBuffer &buffer;
    crimson::timeline::AnalysisSeriesTimelineDescriptor &descriptor;
    bool &available;
    bool &failed;
    std::string &error;
  } tail_kinematics;

  struct StimulusContextTimeline {
    crimson::timeline::StimulusContextTimelineDescriptor &descriptor;
    std::shared_ptr<const crimson::timeline::StimulusContextTimelineSnapshot>
        &snapshot;
    bool &available;
    bool &failed;
    std::string &error;
  } stimulus_context;

  struct Crop {
    std::unique_ptr<crimson::zarr::AnalysisCropGeometryRepository> &geometry;
    AppleAcquisitionCropPlaybackSession &playback;
    std::optional<AppleVideoAssetInfo> &analysis_view_info;
    std::optional<AppleVideoAssetInfo> &view_info;
    AppleCropViewerControls &controls;
    crimson::crop::CropSourcePreference &active_preference;
    bool &enabled;
    bool &composite_enabled;
    std::string &error;
  } crop;
};

class AppleAnalysisProductAdopter {
public:
  AppleAnalysisProductAdopter(AppleAnalysisProductAdoptionOptions options,
                              AppleAnalysisProductAdoptionContext context);

  AppleAnalysisProductAdopter(const AppleAnalysisProductAdopter &) = delete;
  AppleAnalysisProductAdopter &
  operator=(const AppleAnalysisProductAdopter &) = delete;

  void adopt(AppleAnalysisRepositoryBundle result,
             bool analysis_loader_loading);
  void cancel();
  bool hasDeferredSwimBoutResult() const;

private:
  AppleAnalysisProductAdoptionOptions options_;
  AppleAnalysisProductAdoptionContext context_;
  crimson::analysis::AnalysisProductLifecycleController lifecycle_;
  bool lifecycle_ready_ = false;
  std::optional<AppleAnalysisRepositoryBundle> deferred_swim_bout_result_;
};
