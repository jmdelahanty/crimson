#pragma once

#include "analysis_series_timeline.h"
#include "apple_video_metal_renderer.h"
#include "apple_video_playback_buffer.h"
#include "crop_presentation_coordinator.h"
#include "eye_angle_timeline.h"
#include "playback_clock.h"
#include "read_only_overlay_controls.h"
#include "read_only_overlay_scene.h"
#include "stimulus_context_timeline.h"
#include "stimulus_presentation_coordinator.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

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
  bool toggle_overlay_controls = false;
  bool toggle_analysis_timeline = false;
};

struct AppleEyeAngleTimelineControls {
  std::string representation_key;
  bool show_left = true;
  bool show_right = true;
  bool show_vergence = true;
};

struct AppleSeriesTimelineControls {
  std::string source_key;
  std::string initialized_source_key;
  std::unordered_map<std::string, bool> trace_visibility;
};

enum class AppleAnalysisTimelineTab {
  Automatic,
  Motion,
  EyeAngles,
  TailKinematics,
  Stimulus,
};

struct AppleAnalysisTimelineControls {
  bool open = false;
  float half_span_seconds = 5.0f;
  AppleAnalysisTimelineTab initial_tab = AppleAnalysisTimelineTab::Automatic;
  AppleSeriesTimelineControls motion;
  AppleEyeAngleTimelineControls eye_angles;
  AppleSeriesTimelineControls tail_kinematics;
  bool show_stimulus_context = true;
  std::string stimulus_run_name;
  std::unordered_map<int32_t, bool> stimulus_event_type_filter;
  size_t selected_stimulus_event = std::numeric_limits<size_t>::max();
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
  AppleMetalVideoViewport crop;
  AppleMetalVideoViewport stimulus;
};

void sampleAppleVideoViewerSystemMetrics(AppleVideoViewerStats &stats);
const char *appleViewerThermalStateName(AppleViewerThermalState state);

AppleVideoControlResult drawAppleVideoControls(
    LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
    const AppleVideoViewerStats &stats,
    const crimson::playback::StimulusPresentationMetrics *stimulus_metrics,
    AppleCropViewerControls *crop_controls, bool analysis_timeline_available,
    bool interactive);

bool drawAppleAnalysisTimeline(
    AppleAnalysisTimelineControls *controls,
    const crimson::timeline::AnalysisSeriesTimelineDescriptor
        *motion_descriptor,
    const std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
        &motion_window,
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

void drawAppleReadOnlyOverlayControls(
    bool *open, crimson::overlay::ReadOnlyOverlayControlState *controls,
    const crimson::overlay::ReadOnlyOverlayAvailability &availability,
    bool interactive);

void drawAppleCropPreviewOverlay(
    const AppleMetalVideoViewport &viewport, float framebuffer_scale,
    const crimson::crop::CropSourceSelection *selection,
    crimson::crop::CropSourceSelectionStatus status);

size_t drawAppleReadOnlyOverlayText(
    const crimson::overlay::ReadOnlyOverlayScene &scene,
    const crimson::overlay::SourceViewportTransform &transform,
    float framebuffer_scale_x, float framebuffer_scale_y);

AppleMetalVideoViewport appleVideoViewport(int framebuffer_width,
                                           int framebuffer_height,
                                           float framebuffer_scale,
                                           const AppleVideoAssetInfo &info);

AppleCompositeVideoViewports appleCompositeVideoViewports(
    int framebuffer_width, int framebuffer_height, float framebuffer_scale,
    const AppleVideoAssetInfo &camera_info,
    const AppleVideoAssetInfo *crop_info,
    const AppleVideoAssetInfo *stimulus_info);
