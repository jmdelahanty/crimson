#pragma once

#include "apple_video_metal_renderer.h"
#include "apple_video_playback_buffer.h"
#include "playback_clock.h"
#include "stimulus_presentation_coordinator.h"

#include <cstdint>

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

struct AppleCompositeVideoViewports {
  AppleMetalVideoViewport camera;
  AppleMetalVideoViewport stimulus;
};

void sampleAppleVideoViewerSystemMetrics(AppleVideoViewerStats &stats);
const char *appleViewerThermalStateName(AppleViewerThermalState state);

AppleVideoControlResult drawAppleVideoControls(
    LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
    const AppleVideoViewerStats &stats,
    const crimson::playback::StimulusPresentationMetrics *stimulus_metrics,
    bool interactive);

AppleMetalVideoViewport appleVideoViewport(int framebuffer_width,
                                           int framebuffer_height,
                                           float framebuffer_scale,
                                           const AppleVideoAssetInfo &info);

AppleCompositeVideoViewports appleCompositeVideoViewports(
    int framebuffer_width, int framebuffer_height, float framebuffer_scale,
    const AppleVideoAssetInfo &camera_info,
    const AppleVideoAssetInfo *stimulus_info);
