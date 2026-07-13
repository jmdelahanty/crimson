#include "apple_video_viewer_ui.h"

#include "imgui.h"

#import <Foundation/Foundation.h>
#import <mach/mach.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

void showItemTooltip(const char *text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("%s", text);
  }
}

double processMemoryMiB() {
  task_vm_info_data_t task_info_data{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&task_info_data),
                &count) != KERN_SUCCESS) {
    return 0.0;
  }
  return static_cast<double>(task_info_data.phys_footprint) /
         (1024.0 * 1024.0);
}

AppleViewerThermalState currentThermalState() {
  switch (NSProcessInfo.processInfo.thermalState) {
  case NSProcessInfoThermalStateNominal:
    return AppleViewerThermalState::Nominal;
  case NSProcessInfoThermalStateFair:
    return AppleViewerThermalState::Fair;
  case NSProcessInfoThermalStateSerious:
    return AppleViewerThermalState::Serious;
  case NSProcessInfoThermalStateCritical:
    return AppleViewerThermalState::Critical;
  }
  return AppleViewerThermalState::Unknown;
}

bool seekViewer(LogicalPlaybackClock &clock,
                AppleVideoPlaybackBuffer &playback, int64_t frame_number) {
  clock.pause();
  clock.seek(frame_number);
  std::string error;
  if (!playback.requestSeek(frame_number, &error)) {
    std::fprintf(stderr, "[AppleVideo] Seek request failed: %s\n",
                 error.c_str());
    return false;
  }
  return true;
}

AppleMetalVideoViewport fitVideoViewport(double x, double y, double width,
                                         double height,
                                         const AppleVideoAssetInfo &info) {
  if (width <= 0.0 || height <= 0.0 || info.width <= 0 || info.height <= 0) {
    return {};
  }
  const double scale =
      std::min(width / static_cast<double>(info.width),
               height / static_cast<double>(info.height));
  const double fitted_width = info.width * scale;
  const double fitted_height = info.height * scale;
  return {x + (width - fitted_width) * 0.5,
          y + (height - fitted_height) * 0.5, fitted_width, fitted_height};
}

}  // namespace

void sampleAppleVideoViewerSystemMetrics(AppleVideoViewerStats &stats) {
  stats.process_memory_mib = processMemoryMiB();
  stats.peak_process_memory_mib =
      std::max(stats.peak_process_memory_mib, stats.process_memory_mib);
  stats.thermal_state = currentThermalState();
}

const char *appleViewerThermalStateName(AppleViewerThermalState state) {
  switch (state) {
  case AppleViewerThermalState::Nominal:
    return "nominal";
  case AppleViewerThermalState::Fair:
    return "fair";
  case AppleViewerThermalState::Serious:
    return "serious";
  case AppleViewerThermalState::Critical:
    return "critical";
  case AppleViewerThermalState::Unknown:
    return "unknown";
  }
  return "unknown";
}

AppleVideoControlResult drawAppleVideoControls(
    LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
    const AppleVideoViewerStats &stats,
    const crimson::playback::StimulusPresentationMetrics *stimulus_metrics,
    bool interactive) {
  AppleVideoControlResult result;
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const float panel_height = stimulus_metrics == nullptr ? 108.0f : 132.0f;
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x,
             viewport->WorkPos.y + viewport->WorkSize.y - panel_height));
  ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, panel_height));
  ImGui::SetNextWindowBgAlpha(0.94f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin("Video transport", nullptr, flags);
  if (!interactive) {
    ImGui::BeginDisabled();
  }

  if (ImGui::Button(clock.isPlaying() ? "||" : ">", ImVec2(34.0f, 28.0f))) {
    if (clock.isPlaying()) {
      clock.pause();
      result.camera_discontinuity = true;
    } else {
      clock.play();
    }
  }
  showItemTooltip(clock.isPlaying() ? "Pause" : "Play");
  ImGui::SameLine();
  if (ImGui::ArrowButton("step-back", ImGuiDir_Left)) {
    result.camera_discontinuity =
        seekViewer(clock, playback,
                   std::max<int64_t>(0, clock.requestedFrame() - 1)) ||
        result.camera_discontinuity;
  }
  showItemTooltip("Previous frame");
  ImGui::SameLine();
  if (ImGui::ArrowButton("step-forward", ImGuiDir_Right)) {
    result.camera_discontinuity =
        seekViewer(clock, playback,
                   std::min<int64_t>(clock.frameCount() - 1,
                                     clock.requestedFrame() + 1)) ||
        result.camera_discontinuity;
  }
  showItemTooltip("Next frame");
  ImGui::SameLine();
  ImGui::Text("Frame %lld / %lld",
              static_cast<long long>(stats.requested_frame),
              static_cast<long long>(clock.frameCount() - 1));

  static int timeline_frame = 0;
  static bool timeline_active = false;
  if (!timeline_active) {
    timeline_frame = static_cast<int>(stats.requested_frame);
  }
  ImGui::SetNextItemWidth(-1.0f);
  const int maximum_frame =
      static_cast<int>(std::max<int64_t>(0, clock.frameCount() - 1));
  if (ImGui::SliderInt("##timeline", &timeline_frame, 0, maximum_frame, "")) {
    timeline_active = true;
    clock.pause();
    clock.seek(timeline_frame);
  }
  if (timeline_active && ImGui::IsItemDeactivatedAfterEdit()) {
    result.camera_discontinuity =
        seekViewer(clock, playback, timeline_frame) ||
        result.camera_discontinuity;
    timeline_active = false;
  }
  if (!interactive) {
    ImGui::EndDisabled();
  }

  const AppleVideoPlaybackBufferMetrics metrics = playback.metrics();
  const double buffer_memory_mib =
      stats.presented_frame >= 0
          ? static_cast<double>(metrics.buffered_frames) *
                static_cast<double>(playback.info().width) *
                static_cast<double>(playback.info().height) * 1.5 /
                (1024.0 * 1024.0)
          : 0.0;
  ImGui::Text(
      "Presented %lld   PTS error %+.3f frames   Buffer %zu/%zu (%.0f MiB)   "
      "Process %.0f MiB (peak %.0f)   Thermal %s",
      static_cast<long long>(stats.presented_frame), stats.pts_error_frames,
      metrics.buffered_frames, playback.capacity(), buffer_memory_mib,
      stats.process_memory_mib, stats.peak_process_memory_mib,
      appleViewerThermalStateName(stats.thermal_state));
  ImGui::Text(
      "Repeats %llu   Skipped source %llu   Late %llu (max %.1f frames)   "
      "Catch-up decode drops %llu   Startup %.1f ms   Seek %.1f ms",
      static_cast<unsigned long long>(stats.repeated_presentations),
      static_cast<unsigned long long>(stats.skipped_source_frames),
      static_cast<unsigned long long>(stats.late_presentations),
      stats.max_lag_frames,
      static_cast<unsigned long long>(metrics.catchup_discarded_frames),
      metrics.startup_ms, metrics.last_seek_ms);
  if (stimulus_metrics != nullptr) {
    ImGui::Text(
        "Stimulus target %d   presented %d   camera skew %+.0f frames   "
        "holds %llu   deferred %llu (max run %llu)",
        stimulus_metrics->last_target_stimulus_frame,
        stimulus_metrics->presented_stimulus_frame,
        static_cast<double>(stimulus_metrics->camera_skew_frames),
        static_cast<unsigned long long>(stimulus_metrics->held_presentations),
        static_cast<unsigned long long>(
            stimulus_metrics->unavailable_presentations),
        static_cast<unsigned long long>(
            stimulus_metrics->max_consecutive_unavailable));
  }
  ImGui::End();

  if (interactive && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    if (clock.isPlaying()) {
      clock.pause();
      result.camera_discontinuity = true;
    } else {
      clock.play();
    }
  }
  if (interactive && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
    result.camera_discontinuity =
        seekViewer(clock, playback,
                   std::max<int64_t>(0, clock.requestedFrame() - 1)) ||
        result.camera_discontinuity;
  }
  if (interactive && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
    result.camera_discontinuity =
        seekViewer(clock, playback,
                   std::min<int64_t>(clock.frameCount() - 1,
                                     clock.requestedFrame() + 1)) ||
        result.camera_discontinuity;
  }
  return result;
}

AppleMetalVideoViewport appleVideoViewport(int framebuffer_width,
                                           int framebuffer_height,
                                           float framebuffer_scale,
                                           const AppleVideoAssetInfo &info) {
  const double control_height = 108.0 * framebuffer_scale;
  const double available_width = framebuffer_width;
  const double available_height =
      std::max(1.0, framebuffer_height - control_height);
  return fitVideoViewport(0.0, 0.0, available_width, available_height, info);
}

AppleCompositeVideoViewports appleCompositeVideoViewports(
    int framebuffer_width, int framebuffer_height, float framebuffer_scale,
    const AppleVideoAssetInfo &camera_info,
    const AppleVideoAssetInfo *stimulus_info) {
  AppleCompositeVideoViewports result;
  if (stimulus_info == nullptr) {
    result.camera = appleVideoViewport(framebuffer_width, framebuffer_height,
                                       framebuffer_scale, camera_info);
    return result;
  }

  const double control_height = 132.0 * framebuffer_scale;
  const double available_width = std::max(1, framebuffer_width);
  const double available_height =
      std::max(1.0, framebuffer_height - control_height);
  if (available_width < 4.0) {
    result.camera = fitVideoViewport(0.0, 0.0, available_width,
                                     available_height, camera_info);
    result.stimulus = fitVideoViewport(
        std::max(0.0, available_width - 1.0), 0.0, 1.0,
        available_height, *stimulus_info);
    return result;
  }
  const double gutter = std::min(
      std::max(8.0, 12.0 * framebuffer_scale), available_width * 0.05);
  const double maximum_stimulus_width = std::max(
      1.0, std::min(360.0 * framebuffer_scale, available_width * 0.4));
  const double minimum_stimulus_width =
      std::min(160.0 * framebuffer_scale, maximum_stimulus_width);
  const double stimulus_width =
      std::clamp(available_width * 0.24, minimum_stimulus_width,
                 maximum_stimulus_width);
  const double camera_width =
      std::max(1.0, available_width - stimulus_width - gutter);
  result.camera = fitVideoViewport(0.0, 0.0, camera_width, available_height,
                                   camera_info);
  result.stimulus = fitVideoViewport(
      camera_width + gutter, 0.0, stimulus_width, available_height,
      *stimulus_info);
  return result;
}
