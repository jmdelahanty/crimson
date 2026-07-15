#include "apple_video_viewer_ui.h"

#include "imgui.h"
#include "implot.h"

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

void drawAvailableCheckbox(const char *label, bool *value, bool available) {
  if (!available) {
    ImGui::BeginDisabled();
  }
  ImGui::Checkbox(label, value);
  if (!available) {
    ImGui::EndDisabled();
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

bool traceEnabled(const AppleEyeAngleTimelineControls &controls,
                  crimson::timeline::EyeAngleTraceRole role) {
  switch (role) {
  case crimson::timeline::EyeAngleTraceRole::Left:
    return controls.show_left;
  case crimson::timeline::EyeAngleTraceRole::Right:
    return controls.show_right;
  case crimson::timeline::EyeAngleTraceRole::Vergence:
    return controls.show_vergence;
  case crimson::timeline::EyeAngleTraceRole::Other:
    return true;
  }
  return true;
}

ImVec4 traceColor(crimson::timeline::EyeAngleTraceRole role) {
  switch (role) {
  case crimson::timeline::EyeAngleTraceRole::Left:
    return ImVec4(0.20f, 0.78f, 0.43f, 1.0f);
  case crimson::timeline::EyeAngleTraceRole::Right:
    return ImVec4(0.66f, 0.38f, 0.88f, 1.0f);
  case crimson::timeline::EyeAngleTraceRole::Vergence:
    return ImVec4(0.96f, 0.68f, 0.16f, 1.0f);
  case crimson::timeline::EyeAngleTraceRole::Other:
    return ImVec4(0.30f, 0.68f, 0.90f, 1.0f);
  }
  return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
}

ImVec4 traceColor(crimson::timeline::AnalysisSeriesTraceRole role) {
  using Role = crimson::timeline::AnalysisSeriesTraceRole;
  switch (role) {
  case Role::PrimarySpeed:
  case Role::TailTipAngle:
    return ImVec4(0.20f, 0.78f, 0.43f, 1.0f);
  case Role::SecondarySpeed:
  case Role::MaxAbsTailAngle:
    return ImVec4(0.96f, 0.68f, 0.16f, 1.0f);
  case Role::HeadingRaw:
  case Role::TailTipLateralDeflection:
    return ImVec4(0.30f, 0.68f, 0.90f, 1.0f);
  case Role::HeadingSmoothed:
  case Role::MaxAbsTailCurvature:
    return ImVec4(0.66f, 0.38f, 0.88f, 1.0f);
  case Role::PositionX:
    return ImVec4(0.93f, 0.36f, 0.34f, 1.0f);
  case Role::PositionY:
    return ImVec4(0.28f, 0.78f, 0.82f, 1.0f);
  case Role::Other:
    return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
  }
  return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
}

const char *
timelineStatusName(crimson::timeline::EyeAngleTimelineStatus status) {
  switch (status) {
  case crimson::timeline::EyeAngleTimelineStatus::Mapped:
    return "ready";
  case crimson::timeline::EyeAngleTimelineStatus::Missing:
    return "missing";
  case crimson::timeline::EyeAngleTimelineStatus::OutOfRange:
    return "out of range";
  case crimson::timeline::EyeAngleTimelineStatus::InvalidRequest:
    return "invalid request";
  case crimson::timeline::EyeAngleTimelineStatus::ReadFailed:
    return "read failed";
  }
  return "unavailable";
}

const char *
timelineStatusName(crimson::timeline::AnalysisSeriesTimelineStatus status) {
  using Status = crimson::timeline::AnalysisSeriesTimelineStatus;
  switch (status) {
  case Status::Mapped:
    return "ready";
  case Status::Missing:
    return "missing";
  case Status::OutOfRange:
    return "out of range";
  case Status::InvalidRequest:
    return "invalid request";
  case Status::ReadFailed:
    return "read failed";
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
    AppleCropViewerControls *crop_controls, bool analysis_timeline_available,
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
  ImGui::SameLine();
  if (ImGui::Button("Overlays")) {
    result.toggle_overlay_controls = true;
  }
  showItemTooltip("Open or close overlay controls");
  ImGui::SameLine();
  if (!analysis_timeline_available) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Timeline")) {
    result.toggle_analysis_timeline = true;
  }
  showItemTooltip("Open or close analysis timelines");
  if (!analysis_timeline_available) {
    ImGui::EndDisabled();
  }

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

namespace {

void initializeSeriesControls(
    AppleSeriesTimelineControls *controls,
    const crimson::timeline::AnalysisSeriesTimelineDescriptor &descriptor) {
  if (controls->source_key.empty()) {
    controls->source_key =
        crimson::timeline::defaultAnalysisSeriesSource(descriptor);
  }
  if (controls->initialized_source_key == controls->source_key) {
    return;
  }
  controls->initialized_source_key = controls->source_key;
  controls->trace_visibility.clear();
  const auto *source = crimson::timeline::findAnalysisSeriesSource(
      descriptor, controls->source_key);
  if (source == nullptr) {
    return;
  }
  for (const auto &trace : source->traces) {
    controls->trace_visibility.emplace(trace.key, trace.default_visible);
  }
}

const char *seriesRowName(const std::string &row_key) {
  if (row_key == "speed") {
    return "Speed";
  }
  if (row_key == "heading") {
    return "Heading";
  }
  if (row_key == "position") {
    return "Position";
  }
  if (row_key == "tail_angle") {
    return "Tail angle";
  }
  if (row_key == "tail_deflection") {
    return "Tail deflection";
  }
  if (row_key == "tail_curvature") {
    return "Tail curvature";
  }
  return row_key.c_str();
}

bool drawSeriesTimelineTab(
    const char *plot_id_prefix, AppleSeriesTimelineControls *controls,
    float half_span_seconds,
    const crimson::timeline::AnalysisSeriesTimelineDescriptor &descriptor,
    const std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
        &window,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  initializeSeriesControls(controls, descriptor);
  const auto *source = crimson::timeline::findAnalysisSeriesSource(
      descriptor, controls->source_key);
  const char *source_preview =
      source != nullptr ? source->display_name.c_str() : "Unavailable";
  ImGui::SetNextItemWidth(360.0f);
  if (ImGui::BeginCombo("Source", source_preview)) {
    for (const auto &candidate : descriptor.sources) {
      const bool selected = candidate.key == controls->source_key;
      if (ImGui::Selectable(candidate.display_name.c_str(), selected)) {
        controls->source_key = candidate.key;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  initializeSeriesControls(controls, descriptor);
  source = crimson::timeline::findAnalysisSeriesSource(descriptor,
                                                       controls->source_key);
  if (source == nullptr) {
    ImGui::TextUnformatted("Timeline source unavailable");
    return false;
  }

  if (ImGui::BeginTable("##trace-visibility", 2,
                        ImGuiTableFlags_SizingStretchSame)) {
    for (const auto &trace : source->traces) {
      ImGui::TableNextColumn();
      bool &visible = controls->trace_visibility[trace.key];
      ImGui::Checkbox(trace.display_name.c_str(), &visible);
    }
    ImGui::EndTable();
  }

  const bool window_matches =
      window != nullptr && window->request.source_key == controls->source_key;
  if (current_frame < 0 ||
      current_frame >= static_cast<int64_t>(descriptor.frame_count)) {
    ImGui::TextUnformatted("Timeline out of range for this frame");
    return false;
  }
  if (!window_matches) {
    ImGui::TextUnformatted("Loading timeline data...");
    return false;
  }
  if (!window->ready()) {
    ImGui::Text("Timeline %s", timelineStatusName(window->status));
    if (!window->error.empty()) {
      ImGui::TextWrapped("%s", window->error.c_str());
    }
    return false;
  }

  bool camera_discontinuity = false;
  const double frames_per_second = clock.framesPerSecond();
  const double cursor_time =
      crimson::timeline::analysisSeriesTimelineTimeForFrame(
          *window, current_frame, frames_per_second);
  std::vector<std::string> row_keys;
  for (const auto &trace : source->traces) {
    if (controls->trace_visibility[trace.key] &&
        std::find(row_keys.begin(), row_keys.end(), trace.row_key) ==
            row_keys.end()) {
      row_keys.push_back(trace.row_key);
    }
  }
  if (row_keys.empty()) {
    ImGui::TextUnformatted("Select at least one trace");
    return false;
  }

  ImGui::BeginChild("##series-plots", ImVec2(0.0f, 0.0f), false,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  for (const auto &row_key : row_keys) {
    const crimson::timeline::AnalysisSeriesTraceDescriptor *row_trace = nullptr;
    for (const auto &trace : source->traces) {
      if (trace.row_key == row_key && controls->trace_visibility[trace.key]) {
        row_trace = &trace;
        break;
      }
    }
    if (row_trace == nullptr) {
      continue;
    }
    ImGui::Text("%s", seriesRowName(row_key));
    const std::string plot_id =
        std::string("##") + plot_id_prefix + "-" + row_key;
    if (ImPlot::BeginPlot(plot_id.c_str(), ImVec2(-1.0f, 180.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", row_trace->units.c_str(),
                        ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(ImAxis_X1, cursor_time - half_span_seconds,
                              cursor_time + half_span_seconds,
                              ImPlotCond_Always);
      for (const auto &trace : window->traces) {
        if (trace.descriptor.row_key != row_key ||
            !controls->trace_visibility[trace.descriptor.key] ||
            trace.times_seconds.empty() || trace.values.empty()) {
          continue;
        }
        const int count = static_cast<int>(
            std::min(trace.times_seconds.size(), trace.values.size()));
        ImPlot::PushStyleColor(ImPlotCol_Line,
                               traceColor(trace.descriptor.role));
        ImPlot::PlotLine(trace.descriptor.display_name.c_str(),
                         trace.times_seconds.data(), trace.values.data(),
                         count);
        ImPlot::PopStyleColor();
      }
      ImPlot::PushStyleColor(ImPlotCol_Line,
                             ImVec4(0.94f, 0.94f, 0.94f, 0.90f));
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::PopStyleColor();
      ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f),
                   "Frame %lld", static_cast<long long>(current_frame));
      if (interactive && ImPlot::IsPlotHovered() &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int64_t clicked_frame =
            crimson::timeline::analysisSeriesTimelineNearestFrame(
                *window, ImPlot::GetPlotMousePos().x, frames_per_second);
        if (clicked_frame >= 0) {
          const int64_t bounded_frame = std::clamp<int64_t>(
              clicked_frame, 0, std::max<int64_t>(0, clock.frameCount() - 1));
          camera_discontinuity = seekViewer(clock, playback, bounded_frame) ||
                                 camera_discontinuity;
        }
      }
      ImPlot::EndPlot();
    }
  }
  ImGui::EndChild();
  return camera_discontinuity;
}

bool drawEyeAngleTimelineTab(
    AppleEyeAngleTimelineControls *controls, float half_span_seconds,
    const crimson::timeline::EyeAngleTimelineDescriptor &descriptor,
    const std::shared_ptr<const crimson::timeline::EyeAngleTimelineWindow>
        &window,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  if (controls->representation_key.empty()) {
    controls->representation_key =
        crimson::timeline::defaultEyeAngleTimelineRepresentation(descriptor);
  }

  const auto *representation =
      crimson::timeline::findEyeAngleTimelineRepresentation(
          descriptor, controls->representation_key);
  const char *preview = representation != nullptr
                            ? representation->display_name.c_str()
                            : controls->representation_key.c_str();
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::BeginCombo("Representation", preview)) {
    for (const auto &candidate : descriptor.representations) {
      const bool selected = candidate.key == controls->representation_key;
      if (ImGui::Selectable(candidate.display_name.c_str(), selected)) {
        controls->representation_key = candidate.key;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Left", &controls->show_left);
  ImGui::SameLine();
  ImGui::Checkbox("Right", &controls->show_right);
  ImGui::SameLine();
  ImGui::Checkbox("Vergence", &controls->show_vergence);

  const bool window_matches =
      window != nullptr &&
      window->request.representation_key == controls->representation_key;
  if (current_frame < 0 ||
      current_frame >= static_cast<int64_t>(descriptor.frame_count)) {
    ImGui::TextUnformatted("Timeline out of range for this frame");
    return false;
  }
  if (!window_matches) {
    ImGui::TextUnformatted("Loading timeline data...");
    return false;
  }
  if (!window->ready()) {
    ImGui::Text("Timeline %s", timelineStatusName(window->status));
    if (!window->error.empty()) {
      ImGui::TextWrapped("%s", window->error.c_str());
    }
    return false;
  }

  bool camera_discontinuity = false;
    const double frames_per_second = clock.framesPerSecond();
    const double cursor_time = crimson::timeline::eyeAngleTimelineTimeForFrame(
        *window, current_frame, frames_per_second);
    if (ImPlot::BeginPlot("##eye-angle-plot", ImVec2(-1.0f, -1.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
    ImPlot::SetupAxes("Time (s)", "Angle (deg)", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
    ImPlot::SetupAxisLimits(ImAxis_X1, cursor_time - half_span_seconds,
                            cursor_time + half_span_seconds, ImPlotCond_Always);
      for (const auto &trace : window->traces) {
        if (!traceEnabled(*controls, trace.field.role) ||
            trace.times_seconds.empty() || trace.values.empty()) {
          continue;
        }
        const int count = static_cast<int>(
            std::min(trace.times_seconds.size(), trace.values.size()));
        ImPlot::PushStyleColor(ImPlotCol_Line, traceColor(trace.field.role));
        ImPlot::PlotLine(trace.field.display_name.c_str(),
                         trace.times_seconds.data(), trace.values.data(), count);
        ImPlot::PopStyleColor();
      }
    ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(0.94f, 0.94f, 0.94f, 0.90f));
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::PopStyleColor();
    ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f), "Frame %lld",
                 static_cast<long long>(current_frame));
      if (interactive && ImPlot::IsPlotHovered() &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int64_t clicked_frame =
            crimson::timeline::eyeAngleTimelineNearestFrame(
              *window, ImPlot::GetPlotMousePos().x, frames_per_second);
        if (clicked_frame >= 0) {
          const int64_t bounded_frame = std::clamp<int64_t>(
              clicked_frame, 0, std::max<int64_t>(0, clock.frameCount() - 1));
        camera_discontinuity = seekViewer(clock, playback, bounded_frame);
      }
    }
    ImPlot::EndPlot();
  }
  return camera_discontinuity;
}

} // namespace

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
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  if (controls == nullptr || !controls->open) {
    return false;
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const float initial_width =
      std::max(620.0f, std::min(1040.0f, viewport->WorkSize.x - 32.0f));
  const float initial_height =
      std::max(420.0f, std::min(720.0f, viewport->WorkSize.y - 144.0f));
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x +
                 (viewport->WorkSize.x - initial_width) * 0.5f,
             viewport->WorkPos.y + 16.0f),
      ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(initial_width, initial_height),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(600.0f, 380.0f),
                                      ImVec2(FLT_MAX, FLT_MAX));
  if (!ImGui::Begin("Analysis timeline", &controls->open)) {
    ImGui::End();
    return false;
  }
  if (!interactive) {
    ImGui::BeginDisabled();
  }
  ImGui::SetNextItemWidth(150.0f);
  ImGui::SliderFloat("Span", &controls->half_span_seconds, 1.0f, 30.0f,
                     "+/- %.0f s");

  bool camera_discontinuity = false;
  if (ImGui::BeginTabBar("##analysis-timeline-tabs")) {
    const ImGuiTabItemFlags motion_flags =
        controls->initial_tab == AppleAnalysisTimelineTab::Motion
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    if (motion_descriptor != nullptr && !motion_descriptor->sources.empty() &&
        ImGui::BeginTabItem("Motion", nullptr, motion_flags)) {
      if (controls->initial_tab == AppleAnalysisTimelineTab::Motion) {
        controls->initial_tab = AppleAnalysisTimelineTab::Automatic;
      }
          camera_discontinuity =
          drawSeriesTimelineTab("motion", &controls->motion,
                                controls->half_span_seconds, *motion_descriptor,
                                motion_window, current_frame, clock, playback,
                                interactive) ||
              camera_discontinuity;
      ImGui::EndTabItem();
        }
    const ImGuiTabItemFlags eye_flags =
        controls->initial_tab == AppleAnalysisTimelineTab::EyeAngles
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    if (eye_descriptor != nullptr && !eye_descriptor->representations.empty() &&
        ImGui::BeginTabItem("Eye angles", nullptr, eye_flags)) {
      if (controls->initial_tab == AppleAnalysisTimelineTab::EyeAngles) {
        controls->initial_tab = AppleAnalysisTimelineTab::Automatic;
      }
      camera_discontinuity =
          drawEyeAngleTimelineTab(&controls->eye_angles,
                                  controls->half_span_seconds, *eye_descriptor,
                                  eye_window, current_frame, clock, playback,
                                  interactive) ||
          camera_discontinuity;
      ImGui::EndTabItem();
    }
    const ImGuiTabItemFlags tail_flags =
        controls->initial_tab == AppleAnalysisTimelineTab::TailKinematics
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    if (tail_descriptor != nullptr && !tail_descriptor->sources.empty() &&
        ImGui::BeginTabItem("Tail", nullptr, tail_flags)) {
      if (controls->initial_tab == AppleAnalysisTimelineTab::TailKinematics) {
        controls->initial_tab = AppleAnalysisTimelineTab::Automatic;
      }
      camera_discontinuity =
          drawSeriesTimelineTab("tail", &controls->tail_kinematics,
                                controls->half_span_seconds, *tail_descriptor,
                                tail_window, current_frame, clock, playback,
                                interactive) ||
          camera_discontinuity;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  if (!interactive) {
    ImGui::EndDisabled();
  }
  ImGui::End();
  return camera_discontinuity;
}

void drawAppleReadOnlyOverlayControls(
    bool *open, crimson::overlay::ReadOnlyOverlayControlState *controls,
    const crimson::overlay::ReadOnlyOverlayAvailability &availability,
    bool interactive) {
  if (open == nullptr || controls == nullptr || !*open) {
    return;
  }

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + 16.0f),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(340.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 300.0f),
                                      ImVec2(460.0f, 720.0f));
  if (!ImGui::Begin("Overlay controls", open)) {
    ImGui::End();
    return;
  }
  if (!interactive) {
    ImGui::BeginDisabled();
  }

  if (ImGui::Button("Reset defaults")) {
    *controls = {};
  }

  if (ImGui::CollapsingHeader("Keypoints and heading",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    drawAvailableCheckbox("Keypoint markers", &controls->show_keypoints,
                          availability.keypoints);
    ImGui::SameLine();
    drawAvailableCheckbox("Heading arrows", &controls->show_headings,
                          availability.headings);
  }

  if (ImGui::CollapsingHeader("Subject masks",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    drawAvailableCheckbox("Show masks", &controls->show_subject_masks,
                          availability.subject_masks);

    const bool mode_available =
        availability.subject_masks || availability.eye_geometry;
    if (!mode_available) {
      ImGui::BeginDisabled();
    }
    int mode = static_cast<int>(controls->mask_mode);
    const char *mode_labels[] = {"Realtime", "Review", "Debug"};
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::Combo("Mode", &mode, mode_labels, 3)) {
      controls->mask_mode =
          static_cast<crimson::overlay::ReadOnlyMaskOverlayMode>(mode);
    }
    showItemTooltip(
        "Realtime draws fills only; Review and Debug add contours and eye geometry");
    if (!mode_available) {
      ImGui::EndDisabled();
    }

    const bool mask_components_enabled =
        availability.subject_masks && controls->show_subject_masks;
    drawAvailableCheckbox("Subject body", &controls->show_subject_body_mask,
                          mask_components_enabled);
    ImGui::SameLine();
    drawAvailableCheckbox("Swim bladder", &controls->show_swim_bladder_mask,
                          mask_components_enabled);
    const bool eye_components_available =
        availability.subject_masks || availability.eye_geometry;
    drawAvailableCheckbox("Left eye", &controls->show_eye_left_mask,
                          eye_components_available);
    ImGui::SameLine();
    drawAvailableCheckbox("Right eye", &controls->show_eye_right_mask,
                          eye_components_available);
  }

  if (ImGui::CollapsingHeader("Eye geometry",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    const bool detailed =
        controls->mask_mode !=
        crimson::overlay::ReadOnlyMaskOverlayMode::Realtime;
    drawAvailableCheckbox("Show eye geometry", &controls->show_eye_geometry,
                          availability.eye_geometry && detailed);
    const bool eye_details_enabled = availability.eye_geometry && detailed &&
                                     controls->show_eye_geometry;
    drawAvailableCheckbox("Visual cones",
                          &controls->show_eye_direction_beams,
                          eye_details_enabled);
    ImGui::SameLine();
    drawAvailableCheckbox("Gaze rays", &controls->show_eye_gaze_rays,
                          eye_details_enabled);
    drawAvailableCheckbox("Angle arcs", &controls->show_eye_angle_arcs,
                          eye_details_enabled);
    ImGui::SameLine();
    drawAvailableCheckbox("Angle labels", &controls->show_eye_angle_labels,
                          eye_details_enabled);
  }

  if (ImGui::CollapsingHeader("Subject shape",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    drawAvailableCheckbox("Show subject shape", &controls->show_subject_shape,
                          availability.subject_shape);
    const bool shape_enabled =
        availability.subject_shape && controls->show_subject_shape;
    drawAvailableCheckbox("Snout tip",
                          &controls->show_subject_shape_snout_tip,
                          shape_enabled);
    ImGui::SameLine();
    drawAvailableCheckbox("Tail base",
                          &controls->show_subject_shape_tail_base,
                          shape_enabled);
    ImGui::SameLine();
    drawAvailableCheckbox("Tail tip",
                          &controls->show_subject_shape_tail_tip,
                          shape_enabled);
    drawAvailableCheckbox("Caudal anchor",
                          &controls->show_subject_shape_caudal_anchor,
                          shape_enabled);
    drawAvailableCheckbox("Centerline",
                          &controls->show_subject_shape_centerline,
                          shape_enabled);
    drawAvailableCheckbox("Dense B-spline",
                          &controls->show_subject_shape_bspline,
                          shape_enabled);
    drawAvailableCheckbox("Body frame axes",
                          &controls->show_subject_shape_body_axes,
                          shape_enabled);
    drawAvailableCheckbox(
        "Spline debug points",
        &controls->show_subject_shape_bspline_debug_points, shape_enabled);
    ImGui::SameLine();
    drawAvailableCheckbox(
        "Control points",
        &controls->show_subject_shape_bspline_control_points, shape_enabled);
    drawAvailableCheckbox("Tail samples",
                          &controls->show_subject_shape_tail_samples,
                          shape_enabled);
    ImGui::SameLine();
    drawAvailableCheckbox("Tail normals",
                          &controls->show_subject_shape_tail_normals,
                          shape_enabled);
  }

  if (!interactive) {
    ImGui::EndDisabled();
  }
  ImGui::End();
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
  ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
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
  ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
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
