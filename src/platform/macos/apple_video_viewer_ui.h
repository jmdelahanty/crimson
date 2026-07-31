#pragma once

#include "analysis_series_timeline.h"
#include "apple_stimulus_playback_session.h"
#include "apple_video_metal_renderer.h"
#include "apple_video_playback_buffer.h"
#include "chaser_distance_polar_scene.h"
#include "crop_presentation_coordinator.h"
#include "detection_quality_timeline.h"
#include "eye_angle_timeline.h"
#include "gui/quality_timeline_window.h"
#include "keypoint_quality_timeline.h"
#include "platform/macos/apple_workspace_layout.h"
#include "playback_clock.h"
#include "read_only_overlay_controls.h"
#include "read_only_overlay_scene.h"
#include "roi_inset_presentation.h"
#include "session_lifecycle.h"
#include "stimulus_camera_overlay_scene.h"
#include "stimulus_context_timeline.h"
#include "stimulus_presentation_coordinator.h"
#include "swim_bout_timeline.h"
#include "ui_path_config.h"
#include "workspace_state.h"
#include "zarr/canonical_detection_repository.h"
#include "zarr/keypoint_overlay_repository.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

enum class AppleViewerThermalState {
  Nominal,
  Fair,
  Serious,
  Critical,
  Unknown,
};

struct AppleVideoViewerStats {
  int64_t requested_frame = 0;
  int64_t presented_frame = -1;
  int64_t last_presented_frame = -1;
  uint64_t repeated_presentations = 0;
  uint64_t skipped_source_frames = 0;
  uint64_t late_presentations = 0;
  uint64_t presentation_count = 0;
  double pts_error_frames = 0.0;
  double max_lag_frames = 0.0;
  double process_memory_mib = 0.0;
  double peak_process_memory_mib = 0.0;
  double max_next_drawable_ms = 0.0;
  double max_command_wait_ms = 0.0;
  AppleViewerThermalState thermal_state = AppleViewerThermalState::Nominal;
};

struct AppleVideoControlResult {
  bool camera_discontinuity = false;
};

struct AppleFileBrowserState {
  UiPathConfig path_config;
  UiPathConfig persisted_path_config;
  std::string start_folder;
  std::string selected_zarr_path;
  int video_buffer_capacity = 6;
  int stimulus_buffer_capacity = 6;
  int seek_step = 10;
  int accurate_seek_frame = 0;
  bool path_editor_open = false;
  bool path_editor_initialized = false;
  char default_start_path[4096] = {};
  char preferred_roots[8][4096] = {};
  size_t preferred_root_count = 0;
};

using AppleSessionRelaunchRequest = crimson::session::SessionReplacementRequest;

struct AppleFileBrowserResult {
  AppleSessionRelaunchRequest relaunch;
  std::optional<std::string> zarr_open_request;
  std::optional<int64_t> accurate_seek_frame;
  std::string error;
};

struct AppleDiagnosticsResult {
  bool request_dump_decode_buffers = false;
  bool request_random_seek_dump = false;
};

struct AppleCameraViewState {
  crimson::workspace::CameraView source_region;
};

struct AppleFrameInspectPresentationState {
  crimson::workspace::FrameInspectViewSyncState tab_sync;
  crimson::crop::RoiInsetPresentationState roi_inset;
  bool show_motion_trail = true;
  float motion_trail_seconds = 2.0f;
  bool motion_valid_only = true;
  bool show_stimulus_inset = true;
  int stimulus_inset_width = 220;
  float stimulus_inset_opacity = 0.82f;
  bool show_stimulus_frame_label = true;
  crimson::polar::ChaserDistancePolarSceneControls polar_inset;
};

struct AppleStimulusDebugState {
  std::optional<int64_t> selected_frame;
};

struct AppleEyeAngleTimelineControls {
  bool show_left = true;
  bool show_right = true;
  bool show_vergence = true;
};

struct AppleSeriesTimelineControls {
  std::string initialized_source_key;
  std::unordered_map<std::string, bool> trace_visibility;
};

struct AppleSwimBoutTimelineControls {
  std::string initialized_motion_source_key;
  bool show_bouts = true;
  bool show_detector_response = true;
};

enum class AppleAnalysisTimelineTab {
  Automatic,
  Motion,
  EyeAngles,
  TailKinematics,
  Stimulus,
};

struct AppleAnalysisTimelineControls {
  crimson::workspace::WorkspaceSelectionState *selections = nullptr;
  bool open = false;
  float half_span_seconds = 5.0f;
  AppleAnalysisTimelineTab initial_tab = AppleAnalysisTimelineTab::Automatic;
  AppleSeriesTimelineControls motion;
  AppleSwimBoutTimelineControls swim_bouts;
  AppleEyeAngleTimelineControls eye_angles;
  AppleSeriesTimelineControls tail_kinematics;
  bool show_stimulus_context = true;
  std::string stimulus_run_name;
  std::unordered_map<int32_t, bool> stimulus_event_type_filter;
};

using AppleDetectionQualityLoadState = crimson::gui::QualityTimelineLoadState;
using AppleDetectionQualityTimelineControls =
    crimson::gui::DetectionQualityTimelineControls;

struct AppleDetectionInspectState {
  uint64_t selected_instance_key = 0;
};

using AppleKeypointQualityLoadState = crimson::gui::QualityTimelineLoadState;
using AppleKeypointQualityTimelineControls =
    crimson::gui::KeypointQualityTimelineControls;

struct AppleKeypointInspectState {
  uint64_t selected_instance_key = 0;
};

struct AppleCropViewerControls {
  crimson::crop::CropSourcePreference preference =
      crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
  bool acquisition_available = false;
  bool live_geometry_available = false;
  crimson::crop::CropSourceSelectionStatus selection_status =
      crimson::crop::CropSourceSelectionStatus::NoCapableSource;
  const crimson::crop::CropPresentationMetrics *metrics = nullptr;
};

struct AppleCompositeVideoViewports {
  AppleMetalVideoViewport camera;
  AppleMetalVideoViewport crop_inset;
  AppleMetalVideoViewport crop_preview;
  AppleMetalVideoViewport stimulus_inset;
  AppleMetalVideoViewport stimulus_debug;
};

void sampleAppleVideoViewerSystemMetrics(AppleVideoViewerStats &stats);
void setAppleWorkspaceLayoutProfile(
    crimson::macos::workspace::LayoutProfile profile);
const char *appleViewerThermalStateName(AppleViewerThermalState state);

AppleFileBrowserResult drawAppleFileBrowserWindow(
    AppleFileBrowserState *state, const std::string &video_path,
    const std::string &zarr_path, const std::string &stimulus_video_path,
    double average_frame_ms, LogicalPlaybackClock *clock, bool interactive);

AppleDiagnosticsResult drawAppleDiagnosticsWindow(
    const AppleVideoViewerStats &stats,
    const AppleVideoPlaybackBufferMetrics &metrics, size_t buffer_capacity,
    bool playing, const std::string &dump_root,
    const std::string &decode_debug_status,
    const crimson::playback::StimulusPresentationMetrics *stimulus_metrics,
    bool interactive);

bool drawAppleFramesInBufferWindow(
    const AppleVideoViewerStats &stats,
    const AppleVideoPlaybackBufferMetrics &metrics, size_t buffer_capacity,
    const std::vector<int64_t> &buffered_frame_numbers,
    LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
    bool interactive);

AppleVideoControlResult drawAppleCameraViewWindow(
    const std::string &camera_name, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, const AppleVideoViewerStats &stats,
    AppleMetalVideoViewport *viewport, AppleCameraViewState *view_state,
    bool interactive);

void drawAppleHelpWindow(bool show);
void drawAppleErrorPopup(bool *show, const std::string &message);

bool drawAppleAnalysisTimeline(
    AppleAnalysisTimelineControls *controls,
    const crimson::timeline::AnalysisSeriesTimelineDescriptor
        *motion_descriptor,
    const std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
        &motion_window,
    const crimson::timeline::SwimBoutTimelineDescriptor *swim_bout_descriptor,
    const std::shared_ptr<const crimson::timeline::SwimBoutTimelineWindow>
        &swim_bout_window,
    const crimson::timeline::EyeAngleTimelineDescriptor *eye_descriptor,
    const std::shared_ptr<const crimson::timeline::EyeAngleTimelineWindow>
        &eye_window,
    const crimson::timeline::AnalysisSeriesTimelineDescriptor *tail_descriptor,
    const std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
        &tail_window,
    const crimson::timeline::StimulusContextTimelineDescriptor
        *stimulus_descriptor,
    const std::shared_ptr<
        const crimson::timeline::StimulusContextTimelineSnapshot>
        &stimulus_snapshot,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive);

bool drawAppleStimulusEventTimeline(
    AppleAnalysisTimelineControls *controls,
    const crimson::timeline::StimulusContextTimelineDescriptor &descriptor,
    const std::shared_ptr<
        const crimson::timeline::StimulusContextTimelineSnapshot> &snapshot,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive);

bool drawAppleDetectionQualityTimeline(
    AppleDetectionQualityTimelineControls *controls, bool *open,
    AppleDetectionQualityLoadState load_state,
    const crimson::timeline::DetectionQualityTimelineDescriptor *descriptor,
    const std::shared_ptr<
        const crimson::timeline::DetectionQualityTimelineWindow> &window,
    const std::shared_ptr<
        const crimson::timeline::DetectionQualityTimelineOverview> &overview,
    const std::string &error, int64_t current_frame,
    LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
    bool interactive);

bool drawAppleKeypointQualityTimeline(
    AppleKeypointQualityTimelineControls *controls, bool *open,
    AppleKeypointQualityLoadState load_state,
    const crimson::timeline::KeypointQualityTimelineDescriptor *descriptor,
    const std::shared_ptr<
        const crimson::timeline::KeypointQualityTimelineWindow> &window,
    const std::shared_ptr<
        const crimson::timeline::KeypointQualityTimelineOverview> &overview,
    const std::string &error, int64_t current_frame,
    LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
    bool interactive);

void drawAppleFrameInspectWindow(
    crimson::workspace::WorkspaceSelectionState *selections,
    crimson::overlay::ReadOnlyOverlayControlState *controls,
    const crimson::overlay::ReadOnlyOverlayAvailability &availability,
    AppleCropViewerControls *crop_controls, bool stimulus_available,
    bool polar_available,
    const crimson::zarr::CanonicalDetectionDescriptor *detection_descriptor,
    const std::shared_ptr<const crimson::zarr::CanonicalDetectionFrame>
        &detection_frame,
    AppleDetectionQualityLoadState detection_quality_state,
    const std::string &detection_quality_error,
    AppleDetectionInspectState *detection_inspect,
    bool *detection_quality_timeline,
    const crimson::zarr::KeypointOverlayDescriptor *keypoint_descriptor,
    const std::shared_ptr<const crimson::zarr::KeypointOverlayResolution>
        &keypoint_frame,
    AppleKeypointQualityLoadState keypoint_quality_state,
    const std::string &keypoint_quality_error,
    AppleKeypointInspectState *keypoint_inspect,
    bool *keypoint_quality_timeline, const AppleVideoViewerStats &stats,
    AppleFrameInspectPresentationState *presentation,
    bool *advanced_crop_preview, bool *stimulus_debug, bool interactive);

void drawAppleAdvancedCropPreviewWindow(bool *open,
                                        const AppleVideoAssetInfo &crop_info,
                                        AppleMetalVideoViewport *viewport);

void drawAppleStimulusDebugWindows(
    bool enabled, const AppleVideoAssetInfo &stimulus_info,
    const AppleStimulusPlaybackMetrics &metrics,
    const std::vector<int64_t> &buffered_frame_numbers,
    AppleStimulusDebugState *state, AppleMetalVideoViewport *viewport,
    bool interactive);

AppleCompositeVideoViewports
appleWorkspaceVideoViewports(const AppleMetalVideoViewport &camera,
                             const AppleMetalVideoViewport &crop_preview,
                             const AppleMetalVideoViewport &stimulus_debug,
                             const AppleVideoAssetInfo *crop_info,
                             const AppleVideoAssetInfo *stimulus_info,
                             double crop_inset_width = 0.0,
                             double stimulus_inset_width = 0.0);

void drawAppleCropPreviewOverlay(
    const AppleMetalVideoViewport &viewport, float framebuffer_scale,
    const crimson::crop::CropSourceSelection *selection,
    crimson::crop::CropSourceSelectionStatus status);

void drawAppleStimulusInsetOverlay(const AppleMetalVideoViewport &viewport,
                                   float framebuffer_scale,
                                   int64_t camera_frame,
                                   int64_t stimulus_frame);

size_t drawAppleReadOnlyOverlayText(
    const crimson::overlay::ReadOnlyOverlayScene &scene,
    const crimson::overlay::SourceViewportTransform &transform,
    float framebuffer_scale_x, float framebuffer_scale_y);

size_t drawAppleChaserDistancePolarText(
    const crimson::polar::ChaserDistancePolarScene &scene,
    const AppleMetalVideoViewport &camera_viewport, float framebuffer_scale_x,
    float framebuffer_scale_y);

size_t drawAppleStimulusCameraOverlayText(
    const crimson::stimulus::StimulusCameraOverlayScene &scene,
    const AppleMetalVideoViewport &camera_viewport, float framebuffer_scale_x,
    float framebuffer_scale_y);
