#include "apple_video_viewer_ui.h"

#include "imgui.h"

#import <Foundation/Foundation.h>
#import <mach/mach.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
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

ImU32 overlayColor(crimson::overlay::Color color) {
  return IM_COL32(static_cast<int>(std::lround(color.red * 255.0f)),
                  static_cast<int>(std::lround(color.green * 255.0f)),
                  static_cast<int>(std::lround(color.blue * 255.0f)),
                  static_cast<int>(std::lround(color.alpha * 255.0f)));
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

const char *cropSourceName(crimson::crop::CropSourceKind source) {
  switch (source) {
  case crimson::crop::CropSourceKind::LiveGeometry:
    return "Live geometry";
  case crimson::crop::CropSourceKind::AcquisitionVideo:
    return "Acquisition video";
  case crimson::crop::CropSourceKind::PersistedZarr:
    return "Persisted Zarr";
  }
  return "Unavailable";
}

const char *cropStatusName(crimson::crop::CropSourceSelectionStatus status) {
  switch (status) {
  case crimson::crop::CropSourceSelectionStatus::Selected:
    return "exact";
  case crimson::crop::CropSourceSelectionStatus::NoCapableSource:
    return "unavailable";
  case crimson::crop::CropSourceSelectionStatus::MissingFrame:
    return "missing frame";
  case crimson::crop::CropSourceSelectionStatus::MissingGeometry:
    return "missing geometry";
  case crimson::crop::CropSourceSelectionStatus::AwaitingExactFrame:
    return "waiting";
  case crimson::crop::CropSourceSelectionStatus::OutOfRange:
    return "out of range";
  case crimson::crop::CropSourceSelectionStatus::InvalidState:
    return "invalid";
  }
  return "unavailable";
}

double transportHeight(bool has_stimulus, bool has_crop) {
  return 108.0 + (has_stimulus ? 24.0 : 0.0) +
         (has_crop ? 48.0 : 0.0);
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
    AppleCropViewerControls *crop_controls,
    bool interactive) {
  AppleVideoControlResult result;
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const float panel_height = static_cast<float>(transportHeight(
      stimulus_metrics != nullptr, crop_controls != nullptr));
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
  if (crop_controls != nullptr) {
    ImGui::TextUnformatted("Crop source");
    ImGui::SameLine();
    const bool acquisition_selected =
        crop_controls->preference ==
        crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
    if (!crop_controls->acquisition_available) {
      ImGui::BeginDisabled();
    }
    if (ImGui::RadioButton("Acquisition video", acquisition_selected) &&
        interactive) {
      crop_controls->preference =
          crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
    }
    if (!crop_controls->acquisition_available) {
      ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const bool geometry_selected =
        crop_controls->preference ==
        crimson::crop::CropSourcePreference::PreferLiveGeometry;
    if (!crop_controls->live_geometry_available) {
      ImGui::BeginDisabled();
    }
    if (ImGui::RadioButton("Live geometry", geometry_selected) &&
        interactive) {
      crop_controls->preference =
          crimson::crop::CropSourcePreference::PreferLiveGeometry;
    }
    if (!crop_controls->live_geometry_available) {
      ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::Text("Status %s", cropStatusName(crop_controls->selection_status));
    if (crop_controls->metrics != nullptr) {
      const auto &metrics = *crop_controls->metrics;
      ImGui::Text(
          "Crop camera %lld   source frame %lld   holds %llu   deferred %llu "
          "(max run %llu)   mismatches %llu",
          static_cast<long long>(metrics.presented_crop_camera_frame),
          static_cast<long long>(metrics.presented_source_frame),
          static_cast<unsigned long long>(metrics.held_presentations),
          static_cast<unsigned long long>(metrics.deferred_presentations),
          static_cast<unsigned long long>(metrics.max_consecutive_unavailable),
          static_cast<unsigned long long>(
              metrics.mismatched_selection_frames +
              metrics.mismatched_surface_frames));
    }
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

void drawAppleCropPreviewOverlay(
    const AppleMetalVideoViewport &viewport, float framebuffer_scale,
    const crimson::crop::CropSourceSelection *selection,
    crimson::crop::CropSourceSelectionStatus status) {
  if (viewport.width <= 0.0 || viewport.height <= 0.0 ||
      framebuffer_scale <= 0.0f) {
    return;
  }
  const float x = static_cast<float>(viewport.x / framebuffer_scale);
  const float y = static_cast<float>(viewport.y / framebuffer_scale);
  const float width =
      static_cast<float>(viewport.width / framebuffer_scale);
  const float height =
      static_cast<float>(viewport.height / framebuffer_scale);
  ImDrawList *draw_list = ImGui::GetForegroundDrawList();
  draw_list->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
                     IM_COL32(210, 216, 222, 210), 0.0f, 0, 1.0f);

  const char *source_name = "Unavailable";
  if (selection != nullptr && selection->source) {
    source_name = cropSourceName(*selection->source);
  }
  const std::string label =
      std::string("Crop Preview  ") + source_name + "  " +
      cropStatusName(status);
  const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
  draw_list->AddRectFilled(ImVec2(x, y),
                           ImVec2(std::min(x + width,
                                           x + label_size.x + 14.0f),
                                  y + label_size.y + 8.0f),
                           IM_COL32(12, 15, 18, 220));
  draw_list->AddText(ImVec2(x + 7.0f, y + 4.0f),
                     IM_COL32(240, 243, 246, 255), label.c_str());

  if (selection == nullptr || !selection->geometry ||
      selection->blank_frame ||
      !selection->geometry->full_frame_detection) {
    return;
  }
  const auto crop_detection = selection->geometry->fullFrameToCrop(
      *selection->geometry->full_frame_detection);
  if (!crop_detection) {
    return;
  }
  const auto &geometry = *selection->geometry;
  const float x0 = x + static_cast<float>(
                           crop_detection->x / geometry.output_width * width);
  const float y0 = y + static_cast<float>(
                           crop_detection->y / geometry.output_height * height);
  const float x1 = x + static_cast<float>(
                           (crop_detection->x + crop_detection->width) /
                           geometry.output_width * width);
  const float y1 = y + static_cast<float>(
                           (crop_detection->y + crop_detection->height) /
                           geometry.output_height * height);
  draw_list->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                     IM_COL32(58, 214, 132, 255), 0.0f, 0, 2.0f);
}

size_t drawAppleReadOnlyOverlayText(
    const crimson::overlay::ReadOnlyOverlayScene &scene,
    const crimson::overlay::SourceViewportTransform &transform,
    float framebuffer_scale_x, float framebuffer_scale_y) {
  if (framebuffer_scale_x <= 0.0f || framebuffer_scale_y <= 0.0f) {
    return 0;
  }
  const auto labels = crimson::overlay::layoutReadOnlyOverlayText(scene,
                                                                  transform);
  ImDrawList *draw_list = ImGui::GetForegroundDrawList();
  ImFont *font = ImGui::GetFont();
  size_t drawn = 0;
  for (const auto &label : labels) {
    const ImVec2 clip_min(
        static_cast<float>(label.clip_rect.x / framebuffer_scale_x),
        static_cast<float>(label.clip_rect.y / framebuffer_scale_y));
    const ImVec2 clip_max(
        static_cast<float>((label.clip_rect.x + label.clip_rect.width) /
                           framebuffer_scale_x),
        static_cast<float>((label.clip_rect.y + label.clip_rect.height) /
                           framebuffer_scale_y));
    const float font_size =
        ImGui::GetFontSize() * static_cast<float>(label.font_scale);
    const ImVec2 text_size = font->CalcTextSizeA(
        font_size, FLT_MAX, 0.0f, label.content.c_str());
    const ImVec2 anchor(static_cast<float>(label.anchor.x / framebuffer_scale_x),
                        static_cast<float>(label.anchor.y / framebuffer_scale_y));
    const ImVec2 text_min(
        label.centered ? anchor.x - text_size.x * 0.5f : anchor.x,
        label.centered ? anchor.y - text_size.y * 0.5f : anchor.y);
    constexpr float kHorizontalPadding = 5.0f;
    constexpr float kVerticalPadding = 3.0f;
    const ImVec2 box_min(text_min.x - kHorizontalPadding,
                         text_min.y - kVerticalPadding);
    const ImVec2 box_max(text_min.x + text_size.x + kHorizontalPadding,
                         text_min.y + text_size.y + kVerticalPadding);
    draw_list->PushClipRect(clip_min, clip_max, true);
    draw_list->AddRectFilled(box_min, box_max, overlayColor(label.background));
    draw_list->AddRect(box_min, box_max, overlayColor(label.border), 0.0f, 0,
                       1.0f);
    draw_list->AddText(font, font_size, text_min, overlayColor(label.text),
                       label.content.c_str());
    draw_list->PopClipRect();
    ++drawn;
  }
  return drawn;
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
    const AppleVideoAssetInfo *crop_info,
    const AppleVideoAssetInfo *stimulus_info) {
  AppleCompositeVideoViewports result;
  if (stimulus_info == nullptr && crop_info == nullptr) {
    result.camera = appleVideoViewport(framebuffer_width, framebuffer_height,
                                       framebuffer_scale, camera_info);
    return result;
  }

  const double control_height =
      transportHeight(stimulus_info != nullptr, crop_info != nullptr) *
      framebuffer_scale;
  const double available_width = std::max(1, framebuffer_width);
  const double available_height =
      std::max(1.0, framebuffer_height - control_height);
  if (available_width < 4.0) {
    result.camera = fitVideoViewport(0.0, 0.0, available_width,
                                     available_height, camera_info);
    if (crop_info != nullptr) {
      result.crop = fitVideoViewport(
          std::max(0.0, available_width - 1.0), 0.0, 1.0,
          available_height, *crop_info);
    } else if (stimulus_info != nullptr) {
      result.stimulus = fitVideoViewport(
          std::max(0.0, available_width - 1.0), 0.0, 1.0,
          available_height, *stimulus_info);
    }
    return result;
  }
  const double gutter = std::min(
      std::max(8.0, 12.0 * framebuffer_scale), available_width * 0.05);
  const double maximum_rail_width = std::max(
      1.0, std::min(360.0 * framebuffer_scale, available_width * 0.4));
  const double minimum_rail_width =
      std::min(160.0 * framebuffer_scale, maximum_rail_width);
  const double rail_width =
      std::clamp(available_width * 0.24, minimum_rail_width,
                 maximum_rail_width);
  const double camera_width =
      std::max(1.0, available_width - rail_width - gutter);
  result.camera = fitVideoViewport(0.0, 0.0, camera_width, available_height,
                                   camera_info);
  const double rail_x = camera_width + gutter;
  if (crop_info != nullptr && stimulus_info != nullptr) {
    const double rail_gutter = std::min(8.0 * framebuffer_scale,
                                        available_height * 0.04);
    const double panel_height =
        std::max(1.0, (available_height - rail_gutter) * 0.5);
    result.crop = fitVideoViewport(rail_x, 0.0, rail_width, panel_height,
                                   *crop_info);
    result.stimulus = fitVideoViewport(
        rail_x, panel_height + rail_gutter, rail_width, panel_height,
        *stimulus_info);
  } else if (crop_info != nullptr) {
    result.crop = fitVideoViewport(rail_x, 0.0, rail_width, available_height,
                                   *crop_info);
  } else {
    result.stimulus = fitVideoViewport(
        rail_x, 0.0, rail_width, available_height, *stimulus_info);
  }
  return result;
}
