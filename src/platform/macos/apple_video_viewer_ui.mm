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

void seekViewer(LogicalPlaybackClock &clock,
                AppleVideoPlaybackBuffer &playback, int64_t frame_number) {
  clock.pause();
  clock.seek(frame_number);
  std::string error;
  if (!playback.requestSeek(frame_number, &error)) {
    std::fprintf(stderr, "[AppleVideo] Seek request failed: %s\n",
                 error.c_str());
  }
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

void drawAppleVideoControls(LogicalPlaybackClock &clock,
                            AppleVideoPlaybackBuffer &playback,
                            const AppleVideoViewerStats &stats,
                            bool interactive) {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  constexpr float panel_height = 108.0f;
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
    } else {
      clock.play();
    }
  }
  showItemTooltip(clock.isPlaying() ? "Pause" : "Play");
  ImGui::SameLine();
  if (ImGui::ArrowButton("step-back", ImGuiDir_Left)) {
    seekViewer(clock, playback,
               std::max<int64_t>(0, clock.requestedFrame() - 1));
  }
  showItemTooltip("Previous frame");
  ImGui::SameLine();
  if (ImGui::ArrowButton("step-forward", ImGuiDir_Right)) {
    seekViewer(clock, playback,
               std::min<int64_t>(clock.frameCount() - 1,
                                 clock.requestedFrame() + 1));
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
    seekViewer(clock, playback, timeline_frame);
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
  ImGui::End();

  if (interactive && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    if (clock.isPlaying()) {
      clock.pause();
    } else {
      clock.play();
    }
  }
  if (interactive && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
    seekViewer(clock, playback,
               std::max<int64_t>(0, clock.requestedFrame() - 1));
  }
  if (interactive && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
    seekViewer(clock, playback,
               std::min<int64_t>(clock.frameCount() - 1,
                                 clock.requestedFrame() + 1));
  }
}

AppleMetalVideoViewport appleVideoViewport(int framebuffer_width,
                                           int framebuffer_height,
                                           float framebuffer_scale,
                                           const AppleVideoAssetInfo &info) {
  const double control_height = 108.0 * framebuffer_scale;
  const double available_width = framebuffer_width;
  const double available_height =
      std::max(1.0, framebuffer_height - control_height);
  const double scale =
      std::min(available_width / static_cast<double>(info.width),
               available_height / static_cast<double>(info.height));
  const double width = info.width * scale;
  const double height = info.height * scale;
  return {(available_width - width) * 0.5,
          (available_height - height) * 0.5, width, height};
}
