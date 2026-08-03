#include "apple_video_viewer_ui.h"

#include "IconsForkAwesome.h"
#include "gui/canonical_detection_inspect_adapter.h"
#include "gui/frame_inspect_detection_module.h"
#include "gui/frame_inspect_keypoint_module.h"
#include "gui/frame_inspect_window.h"
#include "gui/keypoint_overlay_inspect_adapter.h"
#include "imgui.h"
#include "implot.h"
#include "platform/macos/apple_workspace_layout.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <mach/mach.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

crimson::macos::workspace::LayoutProfile g_workspace_layout_profile =
    crimson::macos::workspace::LayoutProfile::Standard;

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
  return static_cast<double>(task_info_data.phys_footprint) / (1024.0 * 1024.0);
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

ImU32 polarColor(const crimson::polar::ChaserDistancePolarRgba &color) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(static_cast<float>(std::clamp(color.red, 0.0, 1.0)),
             static_cast<float>(std::clamp(color.green, 0.0, 1.0)),
             static_cast<float>(std::clamp(color.blue, 0.0, 1.0)),
             static_cast<float>(std::clamp(color.alpha, 0.0, 1.0))));
}

ImU32 stimulusOverlayColor(
    const crimson::stimulus::StimulusCameraOverlayColor &color) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(static_cast<float>(std::clamp(color.red, 0.0, 1.0)),
             static_cast<float>(std::clamp(color.green, 0.0, 1.0)),
             static_cast<float>(std::clamp(color.blue, 0.0, 1.0)),
             static_cast<float>(std::clamp(color.alpha, 0.0, 1.0))));
}

crimson::playback::PlaybackSeekTelemetryEvent
executeViewerSeek(LogicalPlaybackClock &clock,
                  AppleVideoPlaybackBuffer &playback,
                  const crimson::playback::PlaybackSeekRequest &request) {
  auto &coordinator = clock.seekCoordinator();
  const auto transaction = coordinator.begin(request);
  const auto plan = crimson::playback::planPlaybackSeek(
      transaction, crimson::playback::PlaybackSeekAdapterCapabilities{
                       true,
                       false,
                       true,
                       true,
                   });
  crimson::playback::PlaybackSeekExecutionResult result;
  result.resolved_frame = request.target_frame;
  if (!plan.valid) {
    result.status = crimson::playback::PlaybackSeekExecutionStatus::Rejected;
    result.error = std::string(
        crimson::playback::playbackSeekRejectionName(plan.rejection));
    return coordinator.record(transaction, plan, std::move(result));
  }

  if (plan.pause_playback) {
    clock.apply(crimson::playback::PlaybackTransportCommand::pause());
  }
  const auto transition = clock.apply(
      crimson::playback::PlaybackTransportCommand::seek(request.target_frame));
  if (!transition.accepted) {
    result.status = crimson::playback::PlaybackSeekExecutionStatus::Rejected;
    result.error = "logical playback cursor rejected the seek";
    return coordinator.record(transaction, plan, std::move(result));
  }
  result.resolved_frame = transition.target_frame;

  if (plan.mode ==
      crimson::playback::PlaybackSeekExecutionMode::LogicalCursorOnly) {
    result.status = crimson::playback::PlaybackSeekExecutionStatus::Completed;
    result.path = crimson::playback::PlaybackSeekExecutionPath::LogicalCursor;
    return coordinator.record(transaction, plan, std::move(result));
  }

  const auto service_start = std::chrono::steady_clock::now();
  if (plan.prefer_resident_frame &&
      playback.selectBufferedFrame(transition.target_frame)) {
    result.status = crimson::playback::PlaybackSeekExecutionStatus::Completed;
    result.path = crimson::playback::PlaybackSeekExecutionPath::ResidentBuffer;
  } else {
    std::string error;
    if (playback.requestSeek(transition.target_frame, &error)) {
      result.status = crimson::playback::PlaybackSeekExecutionStatus::Submitted;
      result.path =
          crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
    } else {
      result.status = crimson::playback::PlaybackSeekExecutionStatus::Failed;
      result.error = error;
      std::fprintf(stderr, "[AppleVideo] Seek request failed: %s\n",
                   error.c_str());
    }
  }
  result.service_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - service_start)
                          .count();
  return coordinator.record(transaction, plan, std::move(result));
}

bool viewerSeekCausesDiscontinuity(
    const crimson::playback::PlaybackSeekTelemetryEvent &event) {
  return event.result.status !=
             crimson::playback::PlaybackSeekExecutionStatus::Rejected &&
         event.result.status !=
             crimson::playback::PlaybackSeekExecutionStatus::Failed &&
         event.result.path !=
             crimson::playback::PlaybackSeekExecutionPath::LogicalCursor;
}

bool seekViewer(LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
                int64_t frame_number) {
  const auto request = crimson::playback::makePlaybackSeekRequest(
      crimson::playback::PlaybackSeekPhase::Discrete,
      crimson::playback::PlaybackSeekOrigin::Timeline, frame_number,
      clock.frameCount());
  return request.has_value() && viewerSeekCausesDiscontinuity(executeViewerSeek(
                                    clock, playback, *request));
}

crimson::workspace::WorkspaceCapabilities
viewerPlaybackCapabilities(const LogicalPlaybackClock &clock) {
  crimson::workspace::WorkspaceCapabilities capabilities;
  capabilities.video_loaded = clock.frameCount() > 0;
  capabilities.playback_ready = clock.configured() && clock.controlsEnabled();
  capabilities.playing = clock.isPlaying();
  return capabilities;
}

bool applyViewerPlaybackIntent(const crimson::workspace::PlaybackIntent &intent,
                               LogicalPlaybackClock &clock,
                               AppleVideoPlaybackBuffer &playback) {
  switch (intent.kind) {
  case crimson::workspace::PlaybackIntentKind::Play:
    clock.apply(crimson::playback::PlaybackTransportCommand::play());
    return false;
  case crimson::workspace::PlaybackIntentKind::Pause:
    clock.apply(crimson::playback::PlaybackTransportCommand::pause());
    return false;
  case crimson::workspace::PlaybackIntentKind::Seek:
    return seekViewer(clock, playback, intent.target_frame);
  }
  return false;
}

AppleMetalVideoViewport fitVideoViewport(double x, double y, double width,
                                         double height,
                                         const AppleVideoAssetInfo &info) {
  if (width <= 0.0 || height <= 0.0 || info.width <= 0 || info.height <= 0) {
    return {};
  }
  const double scale = std::min(width / static_cast<double>(info.width),
                                height / static_cast<double>(info.height));
  const double fitted_width = info.width * scale;
  const double fitted_height = info.height * scale;
  return {x + (width - fitted_width) * 0.5, y + (height - fitted_height) * 0.5,
          fitted_width, fitted_height};
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

crimson::macos::workspace::MaintainedWorkspaceLayout currentWorkspaceLayout() {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  return crimson::macos::workspace::makeMaintainedWorkspaceLayout(
      viewport->WorkSize.x, viewport->WorkSize.y, g_workspace_layout_profile);
}

void setFirstUseGeometry(const crimson::macos::workspace::Rect &rect) {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + static_cast<float>(rect.x),
             viewport->WorkPos.y + static_cast<float>(rect.y)),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(
      ImVec2(static_cast<float>(rect.width), static_cast<float>(rect.height)),
      ImGuiCond_FirstUseEver);
}

void constrainCurrentWindowToWorkspace() {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImVec2 position = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const auto constrained = crimson::macos::workspace::constrainToBounds(
      {position.x, position.y, size.x, size.y},
      {viewport->WorkPos.x, viewport->WorkPos.y, viewport->WorkSize.x,
       viewport->WorkSize.y});
  if (!constrained.valid()) {
    return;
  }
  if (std::abs(constrained.x - position.x) > 0.5 ||
      std::abs(constrained.y - position.y) > 0.5) {
    ImGui::SetWindowPos(ImVec2(static_cast<float>(constrained.x),
                               static_cast<float>(constrained.y)),
                        ImGuiCond_Always);
  }
  if (std::abs(constrained.width - size.x) > 0.5 ||
      std::abs(constrained.height - size.y) > 0.5) {
    ImGui::SetWindowSize(ImVec2(static_cast<float>(constrained.width),
                                static_cast<float>(constrained.height)),
                         ImGuiCond_Always);
  }
}

AppleMetalVideoViewport contentViewport(const ImVec2 &position,
                                        const ImVec2 &size,
                                        const AppleVideoAssetInfo &info) {
  const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
  const crimson::macos::workspace::Rect fitted =
      crimson::macos::workspace::fitMedia(
          {static_cast<double>(position.x * scale.x),
           static_cast<double>(position.y * scale.y),
           static_cast<double>(size.x * scale.x),
           static_cast<double>(size.y * scale.y)},
          info.width, info.height);
  return {fitted.x, fitted.y, fitted.width, fitted.height};
}

std::optional<std::string> chooseNativePath(const std::string &title,
                                            const std::string &start_folder,
                                            bool directory,
                                            NSArray<NSString *> *extensions) {
  NSOpenPanel *panel = [NSOpenPanel openPanel];
  panel.title = [NSString stringWithUTF8String:title.c_str()];
  panel.canChooseDirectories = directory ? YES : NO;
  panel.canChooseFiles = directory ? NO : YES;
  panel.allowsMultipleSelection = NO;
  if (!start_folder.empty()) {
    panel.directoryURL = [NSURL
        fileURLWithPath:[NSString stringWithUTF8String:start_folder.c_str()]];
  }
  if (extensions != nil) {
    NSMutableArray<UTType *> *content_types = [NSMutableArray array];
    for (NSString *extension in extensions) {
      UTType *content_type = [UTType typeWithFilenameExtension:extension];
      if (content_type != nil) {
        [content_types addObject:content_type];
      }
    }
    panel.allowedContentTypes = content_types;
  }
  if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
    return std::nullopt;
  }
  return std::string(panel.URL.path.UTF8String);
}

void copyPathBuffer(char *destination, size_t capacity,
                    const std::string &value) {
  if (destination == nullptr || capacity == 0) {
    return;
  }
  std::snprintf(destination, capacity, "%s", value.c_str());
}

void initializePathEditor(AppleFileBrowserState *state) {
  copyPathBuffer(state->default_start_path, sizeof(state->default_start_path),
                 state->path_config.default_start_path);
  state->preferred_root_count =
      std::min(state->path_config.preferred_roots.size(), size_t{8});
  for (size_t index = 0; index < 8; ++index) {
    const std::string value = index < state->preferred_root_count
                                  ? state->path_config.preferred_roots[index]
                                  : std::string{};
    copyPathBuffer(state->preferred_roots[index],
                   sizeof(state->preferred_roots[index]), value);
  }
  state->path_editor_initialized = true;
}

void drawPathEditor(AppleFileBrowserState *state,
                    AppleFileBrowserResult *result) {
  if (state->path_editor_open) {
    ImGui::OpenPopup("Edit Path Presets");
    state->path_editor_open = false;
  }
  ImGui::SetNextWindowSize(ImVec2(900.0f, 500.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::BeginPopupModal("Edit Path Presets", nullptr,
                              ImGuiWindowFlags_NoResize)) {
    return;
  }
  if (!state->path_editor_initialized) {
    initializePathEditor(state);
  }
  ImGui::TextUnformatted("Default start path");
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##default-start-path", state->default_start_path,
                   sizeof(state->default_start_path));
  ImGui::SeparatorText("Preferred roots");
  size_t remove_index = 8;
  for (size_t index = 0; index < state->preferred_root_count; ++index) {
    ImGui::PushID(static_cast<int>(index));
    ImGui::SetNextItemWidth(-42.0f);
    ImGui::InputText("##root", state->preferred_roots[index],
                     sizeof(state->preferred_roots[index]));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FK_TRASH)) {
      remove_index = index;
    }
    showItemTooltip("Remove path preset");
    ImGui::PopID();
  }
  if (remove_index < state->preferred_root_count) {
    for (size_t index = remove_index; index + 1 < state->preferred_root_count;
         ++index) {
      std::memcpy(state->preferred_roots[index],
                  state->preferred_roots[index + 1],
                  sizeof(state->preferred_roots[index]));
    }
    --state->preferred_root_count;
  }
  if (state->preferred_root_count < 8 && ImGui::Button(ICON_FK_PLUS " Add")) {
    copyPathBuffer(state->preferred_roots[state->preferred_root_count],
                   sizeof(state->preferred_roots[state->preferred_root_count]),
                   state->start_folder);
    ++state->preferred_root_count;
  }
  ImGui::Separator();
  auto apply_editor = [&]() -> bool {
    UiPathConfig edited;
    edited.default_start_path = state->default_start_path;
    for (size_t index = 0; index < state->preferred_root_count; ++index) {
      edited.preferred_roots.emplace_back(state->preferred_roots[index]);
    }
    UiPathConfig normalized;
    if (!NormalizeUiPathConfig(edited, normalized, result->error)) {
      return false;
    }
    state->path_config = std::move(normalized);
    state->start_folder = state->path_config.default_start_path;
    initializePathEditor(state);
    return true;
  };
  if (ImGui::Button("Apply") && apply_editor()) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button(ICON_FK_FLOPPY_O " Save") && apply_editor()) {
    std::filesystem::path saved_path;
    if (SaveUserUiPathConfig(state->path_config, saved_path, result->error)) {
      state->path_config.loaded_from = saved_path.string();
      state->persisted_path_config = state->path_config;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    state->path_config = state->persisted_path_config;
    state->start_folder = state->path_config.default_start_path;
    initializePathEditor(state);
  }
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

} // namespace

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

AppleFileBrowserResult drawAppleFileBrowserWindow(
    AppleFileBrowserState *state, const std::string &video_path,
    const std::string &recording_clip_index_path, const std::string &zarr_path,
    const std::string &stimulus_video_path, double average_frame_ms,
    LogicalPlaybackClock *clock, bool interactive) {
  AppleFileBrowserResult result;
  if (state == nullptr) {
    result.error = "File Browser state is unavailable";
    return result;
  }
  const std::string &effective_zarr_path =
      state->selected_zarr_path.empty() ? zarr_path : state->selected_zarr_path;
  setFirstUseGeometry(currentWorkspaceLayout().file_browser);
  if (!ImGui::Begin("File Browser", nullptr, ImGuiWindowFlags_MenuBar)) {
    ImGui::End();
    return result;
  }
  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Open...", nullptr, false, interactive)) {
        const auto path =
            chooseNativePath("Choose Media", state->start_folder, false, @[
              @"mp4", @"mov", @"m4v", @"tiff", @"tif", @"jpeg", @"jpg", @"png"
            ]);
        if (path) {
          result.relaunch = {true,
                             *path,
                             effective_zarr_path,
                             {},
                             state->video_buffer_capacity,
                             state->stimulus_buffer_capacity,
                             {}};
        }
      }
      if (ImGui::MenuItem("Open Recording Clip Index...", nullptr, false,
                          interactive)) {
        const auto path = chooseNativePath("Choose Recording Clip Index",
                                           state->start_folder, false,
                                           @[ @"json" ]);
        if (path) {
          result.relaunch.requested = true;
          result.relaunch.zarr_path = effective_zarr_path;
          result.relaunch.video_buffer_capacity =
              state->video_buffer_capacity;
          result.relaunch.stimulus_buffer_capacity =
              state->stimulus_buffer_capacity;
          result.relaunch.recording_clip_index_path = *path;
        }
      }
      if (ImGui::MenuItem("Load Zarr Archive...", nullptr, false,
                          interactive)) {
        const auto path = chooseNativePath("Choose Zarr Archive Directory",
                                           state->start_folder, true, nil);
        if (path) {
          result.zarr_open_request = *path;
        }
      }
      if (ImGui::MenuItem(
              "Load Stimulus Video...", nullptr, false,
              interactive &&
                  (!video_path.empty() || !recording_clip_index_path.empty()) &&
                  !effective_zarr_path.empty())) {
        const auto path =
            chooseNativePath("Choose Stimulus Video", state->start_folder,
                             false, @[ @"mp4", @"mov", @"m4v" ]);
        if (path) {
          result.relaunch.requested = true;
          result.relaunch.video_path = video_path;
          result.relaunch.zarr_path = effective_zarr_path;
          result.relaunch.stimulus_video_path = *path;
          result.relaunch.video_buffer_capacity = state->video_buffer_capacity;
          result.relaunch.stimulus_buffer_capacity =
              state->stimulus_buffer_capacity;
          result.relaunch.recording_clip_index_path = recording_clip_index_path;
        }
      }
      ImGui::Separator();
      if (ImGui::BeginMenu("Path Preset", interactive)) {
        for (const auto &path : state->path_config.preferred_roots) {
          if (ImGui::MenuItem(path.c_str(), nullptr,
                              state->start_folder == path)) {
            state->start_folder = path;
          }
        }
        if (!state->path_config.preferred_roots.empty()) {
          ImGui::Separator();
        }
        if (ImGui::MenuItem("Edit Path Presets...")) {
          state->path_editor_open = true;
          state->path_editor_initialized = false;
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
  }
  drawPathEditor(state, &result);
  if (!interactive) {
    ImGui::BeginDisabled();
  }
  const double fps = average_frame_ms > 0.0 ? 1000.0 / average_frame_ms : 0.0;
  ImGui::Text("Application average %.2f ms/frame (%.1f FPS)", average_frame_ms,
              fps);
  ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(
      std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.62f));
  if (ImGui::BeginCombo("Buffer Type", "VideoToolbox CVPixelBuffer")) {
    ImGui::Selectable("VideoToolbox CVPixelBuffer", true);
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  ImGui::SetNextItemWidth(
      std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.62f));
  ImGui::InputInt("Buffer Size", &state->video_buffer_capacity, 2, 16);
  state->video_buffer_capacity =
      std::clamp(state->video_buffer_capacity, 2, 256);
  ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(
      std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.62f));
  if (ImGui::BeginCombo("Playback Preview Scale", "Full Resolution (1x)")) {
    ImGui::Selectable("Full Resolution (1x)", true);
    ImGui::EndCombo();
  }
  showItemTooltip(
      "The current AVFoundation provider publishes full-resolution frames");
  ImGui::SetNextItemWidth(
      std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.62f));
  if (ImGui::BeginCombo("Playback Renderer", "Metal")) {
    ImGui::Selectable("Metal", true);
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled("Decoder: AVFoundation / VideoToolbox");
  ImGui::SetNextItemWidth(
      std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.62f));
  ImGui::InputInt("Stimulus Buffer Size", &state->stimulus_buffer_capacity, 2,
                  8);
  state->stimulus_buffer_capacity =
      std::clamp(state->stimulus_buffer_capacity, 2, 64);
  ImGui::TextDisabled("Stimulus Buffer Type: VideoToolbox CVPixelBuffer");
  ImGui::TextDisabled("Stimulus Decode Backend: AVFoundation / VideoToolbox");
  if (video_path.empty() && recording_clip_index_path.empty()) {
    ImGui::TextDisabled("No recording loaded");
  } else if (!video_path.empty()) {
    ImGui::TextWrapped("Video: %s", video_path.c_str());
  } else {
    ImGui::TextWrapped("Recording clip index: %s",
                       recording_clip_index_path.c_str());
  }
  if (!effective_zarr_path.empty()) {
    ImGui::TextWrapped("Zarr: %s", effective_zarr_path.c_str());
  }
  if (!stimulus_video_path.empty()) {
    ImGui::TextWrapped("Stimulus override: %s", stimulus_video_path.c_str());
  }
  if (clock != nullptr && clock->frameCount() > 0) {
    ImGui::InputInt("Seek Step", &state->seek_step, 10, 100);
    state->seek_step = std::max(1, state->seek_step);
    ImGui::InputInt("Seek Accurate", &state->accurate_seek_frame, 1, 100);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      result.accurate_seek_frame = state->accurate_seek_frame;
    }
    if (clock->isPlaying()) {
      float playback_rate = static_cast<float>(clock->playbackRate());
      if (ImGui::SliderFloat("Set Playback Speed", &playback_rate, 0.1f, 1.0f,
                             "%.1fx")) {
        clock->apply(crimson::playback::PlaybackTransportCommand::setRate(
            playback_rate));
      }
      ImGui::Text("Video FPS: %.1f", clock->framesPerSecond());
      ImGui::Text("Target playback: %.2fx", clock->playbackRate());
    }
  }
  if (!interactive) {
    ImGui::EndDisabled();
  }
  ImGui::End();
  return result;
}

AppleDiagnosticsResult drawAppleDiagnosticsWindow(
    const AppleVideoViewerStats &stats,
    const AppleVideoPlaybackBufferMetrics &metrics, size_t buffer_capacity,
    bool playing, const std::string &dump_root,
    const std::string &decode_debug_status,
    const crimson::playback::StimulusPresentationMetrics *stimulus_metrics,
    bool interactive) {
  AppleDiagnosticsResult result;
  setFirstUseGeometry(currentWorkspaceLayout().diagnostics);
  if (!ImGui::Begin("Diagnostics")) {
    ImGui::End();
    return result;
  }
  ImGui::Text("Runtime State: %s", playing ? "playing" : "paused");
  ImGui::Text("Display target frame: %lld",
              static_cast<long long>(stats.requested_frame));
  ImGui::Text("Presented frame: %lld",
              static_cast<long long>(stats.presented_frame));
  ImGui::Text("Buffer frames: valid=%zu capacity=%zu", metrics.buffered_frames,
              buffer_capacity);
  ImGui::Separator();
  ImGui::Text("Latest decoded: %lld",
              static_cast<long long>(metrics.last_decoded_frame));
  ImGui::Text(
      "Decoded=%llu evicted=%llu catch-up=%llu",
      static_cast<unsigned long long>(metrics.decoded_frames),
      static_cast<unsigned long long>(metrics.evicted_frames),
      static_cast<unsigned long long>(metrics.catchup_discarded_frames));
  ImGui::Text("PTS error: %+.3f frames", stats.pts_error_frames);
  ImGui::Text("Startup %.1f ms  Seek %.1f ms", metrics.startup_ms,
              metrics.last_seek_ms);
  ImGui::Text("Process %.0f MiB (peak %.0f)  Thermal %s",
              stats.process_memory_mib, stats.peak_process_memory_mib,
              appleViewerThermalStateName(stats.thermal_state));
  if (stimulus_metrics != nullptr) {
    ImGui::SeparatorText("Stimulus Alignment");
    ImGui::Text("Target %d  presented %d  skew %lld",
                stimulus_metrics->last_target_stimulus_frame,
                stimulus_metrics->presented_stimulus_frame,
                static_cast<long long>(stimulus_metrics->camera_skew_frames));
  }
  ImGui::SeparatorText("Decode Debug");
  if (!interactive) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Dump Decode Buffers")) {
    result.request_dump_decode_buffers = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Random Seek + Dump")) {
    result.request_random_seek_dump = true;
  }
  if (!interactive) {
    ImGui::EndDisabled();
  }
  ImGui::TextWrapped("Output dir: CRIMSON_BUFFER_DUMP_DIR (default %s)",
                     dump_root.c_str());
  if (!decode_debug_status.empty()) {
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s",
                       decode_debug_status.c_str());
  }
  ImGui::End();
  return result;
}

bool drawAppleFramesInBufferWindow(
    const AppleVideoViewerStats &stats,
    const AppleVideoPlaybackBufferMetrics &metrics, size_t buffer_capacity,
    const std::vector<int64_t> &buffered_frame_numbers,
    LogicalPlaybackClock &clock, AppleVideoPlaybackBuffer &playback,
    bool interactive) {
  bool discontinuity = false;
  setFirstUseGeometry(currentWorkspaceLayout().frames_in_buffer);
  if (!ImGui::Begin("Frames in the buffer")) {
    ImGui::End();
    return false;
  }
  ImGui::Text("Valid frames: %zu / %zu", metrics.buffered_frames,
              buffer_capacity);
  ImGui::Text("Selected/displayed frame: %lld",
              static_cast<long long>(stats.presented_frame));
  ImGui::Text("Display target: %lld",
              static_cast<long long>(stats.requested_frame));
  ImGui::Text("Newest buffered frame: %lld",
              static_cast<long long>(metrics.last_decoded_frame));
  ImGui::Separator();
  if (buffered_frame_numbers.empty()) {
    ImGui::TextDisabled("No decoded frames are buffered.");
  } else {
    for (const int64_t frame : buffered_frame_numbers) {
      const bool selected = frame == stats.presented_frame;
      if (ImGui::Selectable(("Frame " + std::to_string(frame)).c_str(),
                            selected) &&
          interactive) {
        discontinuity = seekViewer(clock, playback, frame) || discontinuity;
      }
    }
  }
  ImGui::End();
  return discontinuity;
}

AppleVideoControlResult drawAppleCameraViewWindow(
    const std::string &camera_name, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, const AppleVideoViewerStats &stats,
    AppleMetalVideoViewport *viewport, AppleCameraViewState *view_state,
    bool interactive) {
  AppleVideoControlResult result;
  if (viewport == nullptr || view_state == nullptr) {
    return result;
  }
  *viewport = {};
  setFirstUseGeometry(currentWorkspaceLayout().camera);
  ImGui::SetNextWindowBgAlpha(0.0f);
  const std::string window_title = camera_name + "###crimson-camera-view-v2";
  const bool expanded = ImGui::Begin(window_title.c_str(), nullptr,
                                     ImGuiWindowFlags_NoBackground);
  constrainCurrentWindowToWorkspace();
  if (!expanded) {
    ImGui::End();
    return result;
  }
  if (!interactive) {
    ImGui::BeginDisabled();
  }

  const float transport_height = ImGui::GetFrameHeightWithSpacing();
  const ImVec2 available = ImGui::GetContentRegionAvail();
  const ImVec2 media_position = ImGui::GetCursorScreenPos();
  const ImVec2 media_size(std::max(1.0f, available.x),
                          std::max(1.0f, available.y - transport_height));
  ImGui::InvisibleButton("##camera-presentation", media_size);
  *viewport = contentViewport(media_position, media_size, playback.info());
  if (interactive && ImGui::IsItemHovered()) {
    ImGuiIO &io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f) {
      const double anchor_x = std::clamp(
          (io.MousePos.x - media_position.x) / media_size.x, 0.0f, 1.0f);
      const double anchor_y = std::clamp(
          (io.MousePos.y - media_position.y) / media_size.y, 0.0f, 1.0f);
      view_state->source_region = crimson::workspace::zoomCameraView(
          view_state->source_region, std::pow(1.2, io.MouseWheel), anchor_x,
          anchor_y);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
      view_state->source_region = crimson::workspace::panCameraView(
          view_state->source_region,
          -static_cast<double>(io.MouseDelta.x) / media_size.x *
              view_state->source_region.width,
          -static_cast<double>(io.MouseDelta.y) / media_size.y *
              view_state->source_region.height);
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      view_state->source_region = crimson::workspace::autofitCameraView();
    }
  }

  const auto apply_intent =
      [&](const crimson::workspace::PlaybackIntent &intent) {
        result.camera_discontinuity =
            applyViewerPlaybackIntent(intent, clock, playback) ||
            result.camera_discontinuity;
      };
  const auto apply_command = [&](crimson::workspace::Command command,
                                 int64_t magnitude = 1,
                                 std::optional<int64_t> target = std::nullopt) {
    const auto intent = crimson::workspace::makePlaybackIntent(
        command, viewerPlaybackCapabilities(clock), clock.requestedFrame(),
        clock.frameCount(), target, magnitude);
    if (intent.has_value()) {
      if (intent->kind == crimson::workspace::PlaybackIntentKind::Seek) {
        const auto request = crimson::playback::makePlaybackSeekRequest(
            crimson::playback::PlaybackSeekPhase::Discrete,
            crimson::playback::PlaybackSeekOrigin::KeyboardShortcut,
            intent->target_frame, clock.frameCount());
        if (request.has_value()) {
          result.camera_discontinuity =
              viewerSeekCausesDiscontinuity(
                  executeViewerSeek(clock, playback, *request)) ||
              result.camera_discontinuity;
        }
      } else {
        apply_intent(*intent);
      }
    }
  };

  if (!view_state->transport_slider_active) {
    view_state->transport_slider_frame = stats.requested_frame;
  }
  const int64_t maximum_frame = std::max<int64_t>(0, clock.frameCount() - 1);
  const CameraViewTransportControlsResult transport =
      drawCameraViewTransportControls(CameraViewTransportControlsContext{
          stats.requested_frame,
          clock.frameCount(),
          maximum_frame,
          clock.framesPerSecond(),
          clock.isPlaying(),
          view_state->transport_slider_frame,
          interactive,
      });
  view_state->transport_slider_frame = transport.slider_frame_number;
  view_state->transport_slider_active = transport.slider_active;
  if (transport.seek_request.has_value()) {
    result.camera_discontinuity =
        viewerSeekCausesDiscontinuity(
            executeViewerSeek(clock, playback, *transport.seek_request)) ||
        result.camera_discontinuity;
  } else if (transport.intent.has_value()) {
    apply_intent(*transport.intent);
  }
  if (!interactive) {
    ImGui::EndDisabled();
  }
  ImGui::End();

  const auto shortcuts = handleCameraViewPlaybackShortcuts(interactive);
  if (shortcuts.toggle_playback) {
    apply_command(crimson::workspace::Command::TogglePlayback);
  }
  if (shortcuts.step_delta != 0) {
    apply_command(shortcuts.step_delta < 0
                      ? crimson::workspace::Command::StepBackward
                      : crimson::workspace::Command::StepForward,
                  std::abs(shortcuts.step_delta));
  }
  return result;
}

void drawAppleHelpWindow(bool show) {
  if (!show) {
    return;
  }
  if (ImGui::Begin("Help Menu")) {
    ImGui::Text("<Space>: toggle play and pause");
    ImGui::Text("<Left/Right>: seek one frame");
    ImGui::Text("<Shift+Left/Right>: seek ten frames");
    ImGui::Text("<,>: seek one frame backward");
    ImGui::Text("<.>: seek one frame forward");
    ImGui::SeparatorText("While hovering camera");
    ImGui::Text("<Drag>: pan");
    ImGui::Text("<Scroll>: zoom");
    ImGui::Text("<Double-click>: autofit");
    ImGui::SeparatorText("Editing");
    ImGui::TextDisabled("Annotation and review mutation shortcuts are "
                        "unavailable in this read-only phase.");
  }
  ImGui::End();
}

void drawAppleErrorPopup(bool *show, const std::string &message) {
  if (show == nullptr) {
    return;
  }
  if (*show) {
    ImGui::OpenPopup("Error");
    *show = false;
  }
  if (!ImGui::BeginPopupModal("Error", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextWrapped("%s", message.c_str());
  ImGui::Separator();
  if (ImGui::Button("OK")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

namespace {

ImU32 stimulusEventColor(int32_t event_type_id) {
  const uint32_t hash =
      static_cast<uint32_t>(event_type_id) * 2654435761u + 0x9e3779b9u;
  const int red = 96 + static_cast<int>((hash >> 16) & 0x7f);
  const int green = 96 + static_cast<int>((hash >> 8) & 0x7f);
  const int blue = 96 + static_cast<int>(hash & 0x7f);
  return IM_COL32(red, green, blue, 255);
}

ImU32 stimulusStepColor(crimson::timeline::StimulusStepKind kind) {
  using Kind = crimson::timeline::StimulusStepKind;
  switch (kind) {
  case Kind::MovingGrating:
    return IM_COL32(54, 151, 96, 165);
  case Kind::ConcentricGrating:
    return IM_COL32(54, 139, 190, 165);
  case Kind::LoomingDot:
    return IM_COL32(218, 145, 48, 165);
  case Kind::Chaser:
    return IM_COL32(184, 70, 74, 165);
  case Kind::Other:
    return IM_COL32(118, 122, 132, 165);
  }
  return IM_COL32(118, 122, 132, 165);
}

bool eventTypeVisible(const std::unordered_map<int32_t, bool> *filter,
                      int32_t event_type_id) {
  if (filter == nullptr) {
    return true;
  }
  const auto found = filter->find(event_type_id);
  return found == filter->end() || found->second;
}

void initializeStimulusControls(
    AppleAnalysisTimelineControls *controls,
    const crimson::timeline::StimulusContextTimelineDescriptor &descriptor) {
  if (controls->stimulus_run_name == descriptor.run_name) {
    return;
  }
  controls->stimulus_run_name = descriptor.run_name;
  controls->stimulus_event_type_filter.clear();
  for (const auto &type : descriptor.event_types) {
    controls->stimulus_event_type_filter.emplace(type.id, true);
  }
  controls->selections->stimulus_event_index.reset();
}

bool drawStimulusContextLane(
    const char *id, float height,
    const crimson::timeline::StimulusContextTimelineSnapshot &snapshot,
    const std::unordered_map<int32_t, bool> *event_filter,
    std::optional<size_t> *selected_event, float half_span_seconds,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  const int64_t final_frame = std::max<int64_t>(0, clock.frameCount() - 1);
  const double fps = clock.framesPerSecond();
  const int64_t half_span_frames = static_cast<int64_t>(
      std::ceil(std::max(1.0, static_cast<double>(half_span_seconds) * fps)));
  const int64_t first_frame =
      std::max<int64_t>(0, current_frame - half_span_frames);
  const int64_t last_frame =
      std::min<int64_t>(final_frame, current_frame + half_span_frames);
  const auto window = crimson::timeline::stimulusContextTimelineWindow(
      snapshot, first_frame, last_frame);
  if (!window.valid()) {
    return false;
  }

  const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  const ImVec2 top_left = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(width, height));
  const ImVec2 bottom_right(top_left.x + width, top_left.y + height);
  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(top_left, bottom_right,
                           ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
  draw_list->AddRect(top_left, bottom_right,
                     ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
  const double frame_span =
      static_cast<double>(std::max<int64_t>(1, last_frame - first_frame));
  auto frame_x = [&](int64_t frame) {
    return top_left.x +
           static_cast<float>(
               (static_cast<double>(frame - first_frame) / frame_span) *
               static_cast<double>(width));
  };

  for (const size_t index : window.step_indices) {
    const auto &step = snapshot.steps[index];
    const float x0 = frame_x(std::max(first_frame, step.start_camera_frame));
    const float x1 = frame_x(std::min(last_frame, step.end_camera_frame));
    const ImVec2 step_min(x0, top_left.y + 8.0f);
    const ImVec2 step_max(std::max(x0 + 1.0f, x1), top_left.y + 39.0f);
    draw_list->AddRectFilled(step_min, step_max, stimulusStepColor(step.kind),
                             2.0f);
    const std::string &label =
        !step.step_name.empty() ? step.step_name : step.stimulus_mode;
    if (!label.empty() &&
        step_max.x - step_min.x > ImGui::CalcTextSize(label.c_str()).x + 8.0f) {
      draw_list->PushClipRect(step_min, step_max, true);
      draw_list->AddText(ImVec2(step_min.x + 4.0f, step_min.y + 7.0f),
                         IM_COL32(242, 242, 244, 255), label.c_str());
      draw_list->PopClipRect();
    }
  }
  for (const size_t index : window.event_indices) {
    const auto &event = snapshot.events[index];
    if (!eventTypeVisible(event_filter, event.event_type_id)) {
      continue;
    }
    const float x = frame_x(event.camera_frame);
    draw_list->AddLine(ImVec2(x, top_left.y + 44.0f),
                       ImVec2(x, bottom_right.y - 7.0f),
                       stimulusEventColor(event.event_type_id), 2.0f);
  }
  const float cursor_x =
      frame_x(std::clamp(current_frame, first_frame, last_frame));
  draw_list->AddLine(ImVec2(cursor_x, top_left.y + 2.0f),
                     ImVec2(cursor_x, bottom_right.y - 2.0f),
                     IM_COL32(245, 245, 246, 230), 1.5f);

  size_t nearest_event = std::numeric_limits<size_t>::max();
  float nearest_distance = 8.0f;
  if (ImGui::IsItemHovered()) {
    const float mouse_x = ImGui::GetIO().MousePos.x;
    for (const size_t index : window.event_indices) {
      const auto &event = snapshot.events[index];
      if (!eventTypeVisible(event_filter, event.event_type_id)) {
        continue;
      }
      const float distance = std::fabs(frame_x(event.camera_frame) - mouse_x);
      if (distance < nearest_distance) {
        nearest_distance = distance;
        nearest_event = index;
      }
    }
    if (nearest_event != std::numeric_limits<size_t>::max()) {
      const auto &event = snapshot.events[nearest_event];
      ImGui::SetTooltip("Frame %lld\n%s",
                        static_cast<long long>(event.camera_frame),
                        event.label.c_str());
    } else {
      const double fraction =
          std::clamp(static_cast<double>(mouse_x - top_left.x) /
                         static_cast<double>(width),
                     0.0, 1.0);
      const int64_t hovered_frame =
          first_frame +
          static_cast<int64_t>(std::llround(fraction * frame_span));
      const auto *step = crimson::timeline::findStimulusContextStepForFrame(
          snapshot, hovered_frame);
      if (step != nullptr) {
        const std::string &label =
            !step->step_name.empty() ? step->step_name : step->stimulus_mode;
        ImGui::SetTooltip("Frame %lld\n%s\nFrames %lld-%lld",
                          static_cast<long long>(hovered_frame), label.c_str(),
                          static_cast<long long>(step->start_camera_frame),
                          static_cast<long long>(step->end_camera_frame));
      }
    }
  }
  if (interactive && nearest_event != std::numeric_limits<size_t>::max() &&
      ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    if (selected_event != nullptr) {
      *selected_event = nearest_event;
    }
    const int64_t frame = snapshot.events[nearest_event].camera_frame;
    return frame >= 0 && seekViewer(clock, playback, frame);
  }
  return false;
}

bool drawStimulusTimelineTab(
    AppleAnalysisTimelineControls *controls,
    const crimson::timeline::StimulusContextTimelineDescriptor &descriptor,
    const std::shared_ptr<
        const crimson::timeline::StimulusContextTimelineSnapshot> &snapshot,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  if (!snapshot) {
    ImGui::TextUnformatted("Stimulus context unavailable");
    return false;
  }
  initializeStimulusControls(controls, descriptor);
  bool camera_discontinuity = false;
  const auto *step = crimson::timeline::findStimulusContextStepForFrame(
      *snapshot, current_frame);
  if (step != nullptr) {
    ImGui::Text(
        "%s", (!step->step_name.empty() ? step->step_name : step->stimulus_mode)
                  .c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s  frames %lld-%lld", step->stimulus_mode.c_str(),
                        static_cast<long long>(step->start_camera_frame),
                        static_cast<long long>(step->end_camera_frame));
    if (step->moving_grating.present) {
      ImGui::Text("Direction %.1f deg   Speed %.2f mm/s   Frequency %.2f Hz",
                  step->moving_grating.grating_direction_camera_deg,
                  step->moving_grating.speed_mm_s,
                  step->moving_grating.temporal_frequency_hz);
    } else if (step->concentric_grating.present) {
      ImGui::Text("%s   %s   Radius %.2f-%.2f mm",
                  step->concentric_grating.stimulus_role.c_str(),
                  step->concentric_grating.radial_polarity_authored.c_str(),
                  step->concentric_grating.target_radius_min_mm,
                  step->concentric_grating.target_radius_max_mm);
    }
  } else {
    ImGui::TextDisabled("No canonical step at frame %lld",
                        static_cast<long long>(current_frame));
  }

  if (ImGui::BeginTable("##stimulus-event-filters", 3,
                        ImGuiTableFlags_SizingStretchSame)) {
    for (const auto &type : descriptor.event_types) {
      ImGui::TableNextColumn();
      bool &enabled = controls->stimulus_event_type_filter[type.id];
      std::string label = type.display_name + " (" +
                          std::to_string(type.event_count) + ")##type-" +
                          std::to_string(type.id);
      ImGui::Checkbox(label.c_str(), &enabled);
    }
    ImGui::EndTable();
  }

  camera_discontinuity =
      drawStimulusContextLane("##stimulus-context-full", 150.0f, *snapshot,
                              &controls->stimulus_event_type_filter,
                              &controls->selections->stimulus_event_index,
                              controls->half_span_seconds, current_frame, clock,
                              playback, interactive) ||
      camera_discontinuity;

  ImGui::BeginChild("##stimulus-event-list", ImVec2(0.0f, 150.0f), true);
  for (size_t index = 0; index < snapshot->events.size(); ++index) {
    const auto &event = snapshot->events[index];
    if (!eventTypeVisible(&controls->stimulus_event_type_filter,
                          event.event_type_id)) {
      continue;
    }
    const std::string label = std::to_string(event.camera_frame) + "  " +
                              event.label + "##stimulus-event-" +
                              std::to_string(index);
    const bool selected = controls->selections->stimulus_event_index == index;
    if (ImGui::Selectable(label.c_str(), selected) && interactive) {
      controls->selections->stimulus_event_index = index;
      if (event.camera_frame >= 0) {
        camera_discontinuity =
            seekViewer(clock, playback, event.camera_frame) ||
            camera_discontinuity;
      }
    }
  }
  ImGui::EndChild();
  if (controls->selections->stimulus_event_index.has_value() &&
      *controls->selections->stimulus_event_index < snapshot->events.size()) {
    const auto &event =
        snapshot->events[*controls->selections->stimulus_event_index];
    ImGui::Text("Frame %lld   stimulus %lld   type %d",
                static_cast<long long>(event.camera_frame),
                static_cast<long long>(event.stimulus_frame),
                event.event_type_id);
    if (!event.details_json.empty() && event.details_json != "{}") {
      ImGui::TextWrapped("%s", event.details_json.c_str());
    }
  }
  return camera_discontinuity;
}

void initializeSeriesControls(
    AppleSeriesTimelineControls *controls,
    const crimson::timeline::AnalysisSeriesTimelineDescriptor &descriptor,
    std::string &source_key) {
  if (source_key.empty()) {
    source_key = crimson::timeline::defaultAnalysisSeriesSource(descriptor);
  }
  if (controls->initialized_source_key == source_key) {
    return;
  }
  controls->initialized_source_key = source_key;
  controls->trace_visibility.clear();
  const auto *source =
      crimson::timeline::findAnalysisSeriesSource(descriptor, source_key);
  if (source == nullptr) {
    return;
  }
  for (const auto &trace : source->traces) {
    controls->trace_visibility.emplace(trace.key, trace.default_visible);
  }
}

void initializeSwimBoutControls(
    AppleSwimBoutTimelineControls *controls,
    const crimson::timeline::SwimBoutTimelineDescriptor &descriptor,
    const crimson::timeline::AnalysisSeriesSourceDescriptor &motion_source,
    std::string &candidate_key) {
  const auto compatible = crimson::timeline::compatibleSwimBoutCandidates(
      descriptor, motion_source);
  const bool selection_is_compatible =
      std::any_of(compatible.begin(), compatible.end(),
                  [&](const auto *item) { return item->key == candidate_key; });
  if (controls->initialized_motion_source_key == motion_source.key &&
      selection_is_compatible) {
    return;
  }
  controls->initialized_motion_source_key = motion_source.key;
  candidate_key = crimson::timeline::defaultSwimBoutCandidate(
      descriptor, motion_source, candidate_key);
}

void drawSwimBoutControls(
    AppleSwimBoutTimelineControls *controls,
    const crimson::timeline::SwimBoutTimelineDescriptor &descriptor,
    const crimson::timeline::AnalysisSeriesSourceDescriptor &motion_source,
    std::string &candidate_key) {
  initializeSwimBoutControls(controls, descriptor, motion_source,
                             candidate_key);
  const auto compatible = crimson::timeline::compatibleSwimBoutCandidates(
      descriptor, motion_source);
  if (compatible.empty()) {
    ImGui::TextDisabled("No compatible swim-bout candidates");
    return;
  }
  const auto *selected =
      crimson::timeline::findSwimBoutCandidate(descriptor, candidate_key);
  const std::string preview =
      selected != nullptr ? crimson::timeline::swimBoutCandidateLabel(*selected)
                          : "Unavailable";
  ImGui::SetNextItemWidth(
      std::min(360.0f, std::max(180.0f, ImGui::GetContentRegionAvail().x)));
  if (ImGui::BeginCombo("Swim bouts", preview.c_str())) {
    for (const auto *candidate : compatible) {
      const bool is_selected = candidate->key == candidate_key;
      const std::string label =
          crimson::timeline::swimBoutCandidateLabel(*candidate);
      if (ImGui::Selectable(label.c_str(), is_selected)) {
        candidate_key = candidate->key;
      }
      if (is_selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Checkbox("Bout spans", &controls->show_bouts);
  ImGui::SameLine();
  ImGui::Checkbox("Detector response", &controls->show_detector_response);
}

void drawSwimBoutIntervals(
    const crimson::timeline::SwimBoutTimelineWindow &swim_window,
    const crimson::timeline::AnalysisSeriesTimelineWindow &motion_window,
    double frames_per_second) {
  const ImPlotRect limits = ImPlot::GetPlotLimits();
  ImDrawList *draw_list = ImPlot::GetPlotDrawList();
  const ImVec2 plot_pos = ImPlot::GetPlotPos();
  const ImVec2 plot_size = ImPlot::GetPlotSize();
  const ImVec2 clip_max(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y);
  const ImU32 bout_fill =
      ImGui::GetColorU32(ImVec4(0.15f, 0.95f, 0.45f, 0.16f));
  const ImU32 core_fill =
      ImGui::GetColorU32(ImVec4(0.15f, 0.95f, 0.45f, 0.24f));
  draw_list->PushClipRect(plot_pos, clip_max, true);
  for (const auto &interval : swim_window.intervals) {
    const double start_time =
        crimson::timeline::analysisSeriesTimelineTimeForFrame(
            motion_window, interval.start_frame, frames_per_second);
    const double end_time =
        crimson::timeline::analysisSeriesTimelineTimeForFrame(
            motion_window, interval.end_frame, frames_per_second);
    if (end_time < limits.X.Min || start_time > limits.X.Max) {
      continue;
    }
    const double visible_start =
        std::clamp(start_time, limits.X.Min, limits.X.Max);
    const double visible_end = std::clamp(end_time, limits.X.Min, limits.X.Max);
    if (visible_end <= visible_start) {
      continue;
    }
    draw_list->AddRectFilled(
        ImPlot::PlotToPixels(ImPlotPoint(visible_start, limits.Y.Max)),
        ImPlot::PlotToPixels(ImPlotPoint(visible_end, limits.Y.Min)),
        bout_fill);
    if (!interval.hasCore()) {
      continue;
    }
    const double core_start =
        crimson::timeline::analysisSeriesTimelineTimeForFrame(
            motion_window, interval.core_start_frame, frames_per_second);
    const double core_end =
        crimson::timeline::analysisSeriesTimelineTimeForFrame(
            motion_window, interval.core_end_frame, frames_per_second);
    const double visible_core_start =
        std::clamp(core_start, limits.X.Min, limits.X.Max);
    const double visible_core_end =
        std::clamp(core_end, limits.X.Min, limits.X.Max);
    if (core_end >= limits.X.Min && core_start <= limits.X.Max &&
        visible_core_end > visible_core_start) {
      draw_list->AddRectFilled(
          ImPlot::PlotToPixels(ImPlotPoint(visible_core_start, limits.Y.Max)),
          ImPlot::PlotToPixels(ImPlotPoint(visible_core_end, limits.Y.Min)),
          core_fill);
    }
  }
  draw_list->PopClipRect();
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
    std::string &source_key, float half_span_seconds,
    const crimson::timeline::AnalysisSeriesTimelineDescriptor &descriptor,
    const std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
        &window,
    AppleSwimBoutTimelineControls *swim_controls,
    std::string *swim_candidate_key,
    const crimson::timeline::SwimBoutTimelineDescriptor *swim_descriptor,
    const std::shared_ptr<const crimson::timeline::SwimBoutTimelineWindow>
        &swim_window,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  initializeSeriesControls(controls, descriptor, source_key);
  const auto *source =
      crimson::timeline::findAnalysisSeriesSource(descriptor, source_key);
  const char *source_preview =
      source != nullptr ? source->display_name.c_str() : "Unavailable";
  ImGui::SetNextItemWidth(360.0f);
  if (ImGui::BeginCombo("Source", source_preview)) {
    for (const auto &candidate : descriptor.sources) {
      const bool selected = candidate.key == source_key;
      if (ImGui::Selectable(candidate.display_name.c_str(), selected)) {
        source_key = candidate.key;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  initializeSeriesControls(controls, descriptor, source_key);
  source = crimson::timeline::findAnalysisSeriesSource(descriptor, source_key);
  if (source == nullptr) {
    ImGui::TextUnformatted("Timeline source unavailable");
    return false;
  }

  if (swim_controls != nullptr && swim_candidate_key != nullptr &&
      swim_descriptor != nullptr) {
    drawSwimBoutControls(swim_controls, *swim_descriptor, *source,
                         *swim_candidate_key);
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
      window != nullptr && window->request.source_key == source_key;
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
      const bool swim_window_matches =
          swim_controls != nullptr && swim_candidate_key != nullptr &&
          swim_window != nullptr &&
          swim_window->request.candidate_key == *swim_candidate_key;
      if (row_key == "speed" && swim_window_matches &&
          swim_controls->show_bouts) {
        drawSwimBoutIntervals(*swim_window, *window, frames_per_second);
      }
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
      if (row_key == "speed" && swim_window_matches &&
          swim_controls->show_detector_response &&
          !swim_window->detector_frames.empty() &&
          !swim_window->detector_values.empty()) {
        std::vector<double> detector_times;
        detector_times.reserve(swim_window->detector_frames.size());
        for (const int64_t frame : swim_window->detector_frames) {
          detector_times.push_back(
              crimson::timeline::analysisSeriesTimelineTimeForFrame(
                  *window, frame, frames_per_second));
        }
        const auto *candidate = crimson::timeline::findSwimBoutCandidate(
            *swim_descriptor, *swim_candidate_key);
        std::string label = "Detector response";
        if (candidate != nullptr && !candidate->detector_trace_units.empty()) {
          label +=
              " (" + candidate->detector_trace_units + "; not physical speed)";
        } else {
          label += " (not physical speed)";
        }
        const int count = static_cast<int>(std::min(
            detector_times.size(), swim_window->detector_values.size()));
        ImPlot::PushStyleColor(ImPlotCol_Line,
                               ImVec4(0.20f, 0.78f, 0.42f, 0.95f));
        ImPlot::PlotLine(label.c_str(), detector_times.data(),
                         swim_window->detector_values.data(), count);
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
    AppleEyeAngleTimelineControls *controls, std::string &representation_key,
    float half_span_seconds,
    const crimson::timeline::EyeAngleTimelineDescriptor &descriptor,
    const std::shared_ptr<const crimson::timeline::EyeAngleTimelineWindow>
        &window,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  if (representation_key.empty()) {
    representation_key =
        crimson::timeline::defaultEyeAngleTimelineRepresentation(descriptor);
  }

  const auto *representation =
      crimson::timeline::findEyeAngleTimelineRepresentation(descriptor,
                                                            representation_key);
  const char *preview = representation != nullptr
                            ? representation->display_name.c_str()
                            : representation_key.c_str();
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::BeginCombo("Representation", preview)) {
    for (const auto &candidate : descriptor.representations) {
      const bool selected = candidate.key == representation_key;
      if (ImGui::Selectable(candidate.display_name.c_str(), selected)) {
        representation_key = candidate.key;
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
      window->request.representation_key == representation_key;
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

void setAppleWorkspaceLayoutProfile(
    crimson::macos::workspace::LayoutProfile profile) {
  g_workspace_layout_profile = profile;
}

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
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  if (controls == nullptr || controls->selections == nullptr) {
    return false;
  }
  auto &selections = *controls->selections;
  setFirstUseGeometry(currentWorkspaceLayout().analysis_timeline);
  if (!ImGui::Begin("Analysis Timeline")) {
    ImGui::End();
    return false;
  }
  if (!interactive) {
    ImGui::BeginDisabled();
  }
  ImGui::SetNextItemWidth(150.0f);
  ImGui::SliderFloat("Span", &controls->half_span_seconds, 1.0f, 30.0f,
                     "+/- %.0f s");
  if (stimulus_descriptor != nullptr && stimulus_snapshot) {
    ImGui::SameLine();
    ImGui::Checkbox("Stimulus context", &controls->show_stimulus_context);
  }

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
      if (controls->show_stimulus_context && stimulus_snapshot) {
        camera_discontinuity =
            drawStimulusContextLane("##motion-stimulus-context", 78.0f,
                                    *stimulus_snapshot, nullptr, nullptr,
                                    controls->half_span_seconds, current_frame,
                                    clock, playback, interactive) ||
            camera_discontinuity;
      }
      camera_discontinuity =
          drawSeriesTimelineTab(
              "motion", &controls->motion, selections.motion_source_key,
              controls->half_span_seconds, *motion_descriptor, motion_window,
              &controls->swim_bouts, &selections.swim_bout_candidate_key,
              swim_bout_descriptor, swim_bout_window, current_frame, clock,
              playback, interactive) ||
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
      if (controls->show_stimulus_context && stimulus_snapshot) {
        camera_discontinuity =
            drawStimulusContextLane("##eye-stimulus-context", 78.0f,
                                    *stimulus_snapshot, nullptr, nullptr,
                                    controls->half_span_seconds, current_frame,
                                    clock, playback, interactive) ||
            camera_discontinuity;
      }
      camera_discontinuity =
          drawEyeAngleTimelineTab(
              &controls->eye_angles, selections.eye_angle_representation_key,
              controls->half_span_seconds, *eye_descriptor, eye_window,
              current_frame, clock, playback, interactive) ||
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
      if (controls->show_stimulus_context && stimulus_snapshot) {
        camera_discontinuity =
            drawStimulusContextLane("##tail-stimulus-context", 78.0f,
                                    *stimulus_snapshot, nullptr, nullptr,
                                    controls->half_span_seconds, current_frame,
                                    clock, playback, interactive) ||
            camera_discontinuity;
      }
      camera_discontinuity =
          drawSeriesTimelineTab("tail", &controls->tail_kinematics,
                                selections.tail_kinematics_source_key,
                                controls->half_span_seconds, *tail_descriptor,
                                tail_window, nullptr, nullptr, nullptr, {},
                                current_frame, clock, playback, interactive) ||
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

bool drawAppleStimulusEventTimeline(
    AppleAnalysisTimelineControls *controls,
    const crimson::timeline::StimulusContextTimelineDescriptor &descriptor,
    const std::shared_ptr<
        const crimson::timeline::StimulusContextTimelineSnapshot> &snapshot,
    int64_t current_frame, LogicalPlaybackClock &clock,
    AppleVideoPlaybackBuffer &playback, bool interactive) {
  if (controls == nullptr || controls->selections == nullptr) {
    return false;
  }
  setFirstUseGeometry(currentWorkspaceLayout().stimulus_event_timeline);
  if (!ImGui::Begin("Stimulus Event Timeline")) {
    ImGui::End();
    return false;
  }
  if (!interactive) {
    ImGui::BeginDisabled();
  }
  ImGui::SetNextItemWidth(160.0f);
  ImGui::SliderFloat("Scrolling Window (+/- s)", &controls->half_span_seconds,
                     1.0f, 30.0f, "%.0f");
  const bool camera_discontinuity =
      drawStimulusTimelineTab(controls, descriptor, snapshot, current_frame,
                              clock, playback, interactive);
  if (!interactive) {
    ImGui::EndDisabled();
  }
  ImGui::End();
  return camera_discontinuity;
}

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
    bool interactive) {
  return crimson::gui::drawDetectionQualityTimelineWindow(
      controls, open, load_state, descriptor, window, overview, error,
      current_frame, clock.framesPerSecond(),
      [&](int64_t frame) { return seekViewer(clock, playback, frame); },
      interactive);
}

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
    bool interactive) {
  return crimson::gui::drawKeypointQualityTimelineWindow(
      controls, open, load_state, descriptor, window, overview, error,
      current_frame, clock.framesPerSecond(),
      [&](int64_t frame) { return seekViewer(clock, playback, frame); },
      interactive);
}

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
    bool *advanced_crop_preview, bool *stimulus_debug, bool interactive) {
  if (selections == nullptr || controls == nullptr || presentation == nullptr ||
      detection_inspect == nullptr || detection_quality_timeline == nullptr ||
      keypoint_inspect == nullptr || keypoint_quality_timeline == nullptr ||
      advanced_crop_preview == nullptr || stimulus_debug == nullptr) {
    return;
  }

  constexpr crimson::crop::RoiInsetPresentationCapabilities
      kAppleRoiInsetCapabilities{};
  presentation->roi_inset = crimson::crop::resolveRoiInsetPresentation(
      presentation->roi_inset, kAppleRoiInsetCapabilities);
  auto &roi_inset = presentation->roi_inset;

  crimson::gui::FrameInspectWindowComposition composition;
  composition.draw_header = [&]() {
    ImGui::Text("Requested frame: %lld",
                static_cast<long long>(stats.requested_frame));
    ImGui::SameLine();
    ImGui::Text("Presented: %lld",
                static_cast<long long>(stats.presented_frame));
    ImGui::SeparatorText("ROI Inset");
    if (crop_controls == nullptr) {
      ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Show ROI inset", &roi_inset.visible);
    ImGui::SameLine();
    ImGui::Checkbox("Label", &roi_inset.show_label);
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderFloat("Inset width", &roi_inset.width_px,
                       crimson::crop::kMinimumRoiInsetWidthPx,
                       crimson::crop::kMaximumRoiInsetWidthPx, "%.0f px");
    bool normalize_heading =
        roi_inset.orientation ==
        crimson::crop::RoiInsetOrientation::HeadingNormalized;
    const bool heading_normalization_supported =
        crimson::crop::supportsRoiInsetOrientation(
            kAppleRoiInsetCapabilities,
            crimson::crop::RoiInsetOrientation::HeadingNormalized);
    ImGui::BeginDisabled(!heading_normalization_supported);
    if (ImGui::Checkbox("Normalize heading", &normalize_heading)) {
      roi_inset.orientation =
          normalize_heading
              ? crimson::crop::RoiInsetOrientation::HeadingNormalized
              : crimson::crop::RoiInsetOrientation::Acquisition;
    }
    ImGui::EndDisabled();
    if (!heading_normalization_supported) {
      showItemTooltip(
          "Heading normalization awaits the shared coordinate contract");
    }
    ImGui::Checkbox("Advanced Crop Preview", advanced_crop_preview);
    if (crop_controls == nullptr) {
      ImGui::EndDisabled();
    }
    ImGui::SeparatorText("Stimulus");
    if (!stimulus_available) {
      ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Show stimulus inset", &presentation->show_stimulus_inset);
    ImGui::SameLine();
    ImGui::Checkbox("Stimulus debug windows", stimulus_debug);
    ImGui::BeginDisabled(!presentation->show_stimulus_inset);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt("Stimulus inset width",
                     &presentation->stimulus_inset_width, 120, 360, "%d px");
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Stimulus inset opacity",
                       &presentation->stimulus_inset_opacity, 0.20f, 1.0f,
                       "%.2f");
    ImGui::Checkbox("Stimulus frame label",
                    &presentation->show_stimulus_frame_label);
    ImGui::EndDisabled();
    if (!stimulus_available) {
      ImGui::EndDisabled();
    }
    if (crop_controls != nullptr) {
      ImGui::TextUnformatted("Crop source:");
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
      ImGui::TextDisabled("%s",
                          cropStatusName(crop_controls->selection_status));
    }
  };

  composition.modules.push_back(
      {crimson::workspace::FrameInspectView::Detect, "Detect", true, [&]() {
         auto detection_presentation =
             crimson::gui::makeCanonicalDetectionInspectPresentation(
                 detection_descriptor, detection_frame.get(),
                 stats.presented_frame);
         detection_presentation.timeline_visible =
             detection_presentation.available;
         detection_presentation.timeline_error = detection_quality_error;
         switch (detection_quality_state) {
         case AppleDetectionQualityLoadState::Closed:
           detection_presentation.timeline_state =
               crimson::gui::DetectionInspectTimelineState::Closed;
           break;
         case AppleDetectionQualityLoadState::Opening:
           detection_presentation.timeline_state =
               crimson::gui::DetectionInspectTimelineState::Opening;
           break;
         case AppleDetectionQualityLoadState::Ready:
           detection_presentation.timeline_state =
               crimson::gui::DetectionInspectTimelineState::Ready;
           break;
         case AppleDetectionQualityLoadState::Failed:
           detection_presentation.timeline_state =
               crimson::gui::DetectionInspectTimelineState::Failed;
           break;
         }
         const auto detection_result =
             crimson::gui::drawFrameInspectDetectionModule(
                 detection_presentation, *detection_inspect);
         if (detection_result.request_open_timeline) {
           *detection_quality_timeline = true;
         }
         ImGui::Separator();
         bool bbox_editing_enabled = false;
         ImGui::BeginDisabled();
         ImGui::Checkbox("Enable bbox draw editing", &bbox_editing_enabled);
         ImGui::Button("Run Detection");
         ImGui::EndDisabled();
       }});
  composition.modules.push_back(
      {crimson::workspace::FrameInspectView::Keypoints, "Keypoints", true,
       [&]() {
         drawAvailableCheckbox("Keypoint markers", &controls->show_keypoints,
                               availability.keypoints);
         ImGui::SameLine();
         drawAvailableCheckbox("Heading arrows", &controls->show_headings,
                               availability.headings);
         auto keypoint_presentation =
             crimson::gui::makeKeypointOverlayInspectPresentation(
                 keypoint_descriptor, keypoint_frame.get(),
                 stats.presented_frame);
         keypoint_presentation.timeline_visible =
             keypoint_presentation.available;
         keypoint_presentation.timeline_error = keypoint_quality_error;
         switch (keypoint_quality_state) {
         case AppleKeypointQualityLoadState::Closed:
           keypoint_presentation.timeline_state =
               crimson::gui::KeypointInspectTimelineState::Closed;
           break;
         case AppleKeypointQualityLoadState::Opening:
           keypoint_presentation.timeline_state =
               crimson::gui::KeypointInspectTimelineState::Opening;
           break;
         case AppleKeypointQualityLoadState::Ready:
           keypoint_presentation.timeline_state =
               crimson::gui::KeypointInspectTimelineState::Ready;
           break;
         case AppleKeypointQualityLoadState::Failed:
           keypoint_presentation.timeline_state =
               crimson::gui::KeypointInspectTimelineState::Failed;
           break;
         }
         const auto keypoint_result =
             crimson::gui::drawFrameInspectKeypointModule(keypoint_presentation,
                                                          *keypoint_inspect);
         if (keypoint_result.request_open_timeline) {
           *keypoint_quality_timeline = true;
         }
         ImGui::Separator();
         if (ImGui::Button("Reset overlay defaults")) {
           *controls = {};
         }
       }});

  composition.modules.push_back(
      {crimson::workspace::FrameInspectView::EyeMasks, "Subject Masks", true,
       [&]() {
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
             "Realtime draws fills only; Review and Debug add contours "
             "and eye geometry");
         if (!mode_available) {
           ImGui::EndDisabled();
         }

         const bool mask_components_enabled =
             availability.subject_masks && controls->show_subject_masks;
         drawAvailableCheckbox("Subject body",
                               &controls->show_subject_body_mask,
                               mask_components_enabled);
         ImGui::SameLine();
         drawAvailableCheckbox("Swim bladder",
                               &controls->show_swim_bladder_mask,
                               mask_components_enabled);
         const bool eye_components_available =
             availability.subject_masks || availability.eye_geometry;
         drawAvailableCheckbox("Left eye", &controls->show_eye_left_mask,
                               eye_components_available);
         ImGui::SameLine();
         drawAvailableCheckbox("Right eye", &controls->show_eye_right_mask,
                               eye_components_available);
         ImGui::SeparatorText("Subject shape");
         drawAvailableCheckbox("Show subject shape",
                               &controls->show_subject_shape,
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
         drawAvailableCheckbox(
             "Tail tip", &controls->show_subject_shape_tail_tip, shape_enabled);
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
             &controls->show_subject_shape_bspline_control_points,
             shape_enabled);
         drawAvailableCheckbox("Tail samples",
                               &controls->show_subject_shape_tail_samples,
                               shape_enabled);
         ImGui::SameLine();
         drawAvailableCheckbox("Tail normals",
                               &controls->show_subject_shape_tail_normals,
                               shape_enabled);
       }});

  composition.modules.push_back(
      {crimson::workspace::FrameInspectView::EyeAngles, "Eye Angles", true,
       [&]() {
         const bool detailed =
             controls->mask_mode !=
             crimson::overlay::ReadOnlyMaskOverlayMode::Realtime;
         drawAvailableCheckbox("Show eye geometry",
                               &controls->show_eye_geometry,
                               availability.eye_geometry && detailed);
         const bool eye_details_enabled = availability.eye_geometry &&
                                          detailed &&
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
       }});

  composition.draw_footer = [&]() {
    ImGui::SeparatorText("Motion and Insets");
    ImGui::BeginDisabled();
    ImGui::Checkbox("Motion trail", &presentation->show_motion_trail);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Trail duration", &presentation->motion_trail_seconds,
                       0.25f, 10.0f, "%.2f s");
    ImGui::Checkbox("Valid samples only", &presentation->motion_valid_only);
    ImGui::EndDisabled();
    ImGui::TextDisabled(
        "Motion-trail input is unavailable in the current read-only adapter.");

    if (!polar_available) {
      ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Polar inset", &presentation->polar_inset.show_inset);
    ImGui::BeginDisabled(!presentation->polar_inset.show_inset);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Polar inset width", &presentation->polar_inset.width_px,
                       140.0f, 360.0f, "%.0f px");
    presentation->polar_inset.width_px =
        std::clamp(presentation->polar_inset.width_px, 140.0f, 360.0f);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Polar inset opacity",
                       &presentation->polar_inset.opacity, 0.20f, 1.0f, "%.2f");
    presentation->polar_inset.opacity =
        std::clamp(presentation->polar_inset.opacity, 0.20f, 1.0f);
    ImGui::Checkbox("Polar labels", &presentation->polar_inset.show_labels);
    ImGui::SameLine();
    ImGui::Checkbox("Polar readout", &presentation->polar_inset.show_readout);
    ImGui::EndDisabled();
    if (!polar_available) {
      ImGui::EndDisabled();
    }
  };

  setFirstUseGeometry(currentWorkspaceLayout().frame_inspect);
  crimson::gui::FrameInspectWindowOptions options;
  options.interactive = interactive;
  crimson::gui::drawFrameInspectWindow(options, selections->frame_inspect_view,
                                       presentation->tab_sync, composition);
}

void drawAppleAdvancedCropPreviewWindow(bool *open,
                                        const AppleVideoAssetInfo &crop_info,
                                        AppleMetalVideoViewport *viewport) {
  if (viewport == nullptr) {
    return;
  }
  *viewport = {};
  if (open == nullptr || !*open) {
    return;
  }
  setFirstUseGeometry(currentWorkspaceLayout().advanced_crop_preview);
  ImGui::SetNextWindowSizeConstraints(ImVec2(120.0f, 120.0f),
                                      ImVec2(420.0f, 700.0f));
  ImGui::SetNextWindowBgAlpha(0.0f);
  if (!ImGui::Begin("Advanced Crop Preview", open,
                    ImGuiWindowFlags_NoBackground)) {
    ImGui::End();
    return;
  }
  constrainCurrentWindowToWorkspace();
  const ImVec2 position = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton(
      "##advanced-crop-presentation",
      ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
  *viewport = contentViewport(position, size, crop_info);
  ImGui::End();
}

void drawAppleStimulusDebugWindows(
    bool enabled, const AppleVideoAssetInfo &stimulus_info,
    const AppleStimulusPlaybackMetrics &metrics,
    const std::vector<int64_t> &buffered_frame_numbers,
    AppleStimulusDebugState *state, AppleMetalVideoViewport *viewport,
    bool interactive) {
  if (viewport == nullptr || state == nullptr) {
    return;
  }
  *viewport = {};
  if (!enabled) {
    return;
  }
  setFirstUseGeometry(currentWorkspaceLayout().stimulus);
  ImGui::SetNextWindowBgAlpha(0.0f);
  if (ImGui::Begin("Stimulus", nullptr, ImGuiWindowFlags_NoBackground)) {
    constrainCurrentWindowToWorkspace();
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton(
        "##stimulus-presentation",
        ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
    *viewport = contentViewport(position, size, stimulus_info);
  }
  ImGui::End();

  setFirstUseGeometry(currentWorkspaceLayout().stimulus_frames_in_buffer);
  if (ImGui::Begin("Stimulus Frames in Buffer")) {
    ImGui::Text("Valid frames: %zu", metrics.decoder.buffered_frames);
    ImGui::Text("Target stimulus frame: %d",
                metrics.last_target_stimulus_frame);
    ImGui::Text("Last decoded frame: %lld",
                static_cast<long long>(metrics.decoder.last_decoded_frame));
    ImGui::Separator();
    if (buffered_frame_numbers.empty()) {
      ImGui::TextDisabled("No decoded stimulus frames are buffered.");
    } else {
      for (const int64_t frame : buffered_frame_numbers) {
        const bool selected = state->selected_frame == frame;
        if (ImGui::Selectable(
                ("Stimulus frame " + std::to_string(frame)).c_str(),
                selected) &&
            interactive) {
          state->selected_frame = frame;
        }
      }
    }
    ImGui::Separator();
    ImGui::Text("Mapped requests: %llu",
                static_cast<unsigned long long>(metrics.mapped_requests));
    ImGui::Text("Hold %llu  Follow %llu  Seek %llu",
                static_cast<unsigned long long>(metrics.hold_requests),
                static_cast<unsigned long long>(metrics.follow_requests),
                static_cast<unsigned long long>(metrics.seek_requests));
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
  const float width = static_cast<float>(viewport.width / framebuffer_scale);
  const float height = static_cast<float>(viewport.height / framebuffer_scale);
  ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
  draw_list->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
                     IM_COL32(210, 216, 222, 210), 0.0f, 0, 1.0f);

  const char *source_name = "Unavailable";
  if (selection != nullptr && selection->source) {
    source_name = cropSourceName(*selection->source);
  }
  const std::string label = std::string("Crop Preview  ") + source_name + "  " +
                            cropStatusName(status);
  const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
  draw_list->AddRectFilled(ImVec2(x, y),
                           ImVec2(std::min(x + width, x + label_size.x + 14.0f),
                                  y + label_size.y + 8.0f),
                           IM_COL32(12, 15, 18, 220));
  draw_list->AddText(ImVec2(x + 7.0f, y + 4.0f), IM_COL32(240, 243, 246, 255),
                     label.c_str());

  if (selection == nullptr || !selection->geometry || selection->blank_frame ||
      !selection->geometry->full_frame_detection) {
    return;
  }
  const auto crop_detection = selection->geometry->fullFrameToCrop(
      *selection->geometry->full_frame_detection);
  if (!crop_detection) {
    return;
  }
  const auto &geometry = *selection->geometry;
  const float x0 =
      x + static_cast<float>(crop_detection->x / geometry.output_width * width);
  const float y0 = y + static_cast<float>(crop_detection->y /
                                          geometry.output_height * height);
  const float x1 =
      x + static_cast<float>((crop_detection->x + crop_detection->width) /
                             geometry.output_width * width);
  const float y1 =
      y + static_cast<float>((crop_detection->y + crop_detection->height) /
                             geometry.output_height * height);
  draw_list->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                     IM_COL32(58, 214, 132, 255), 0.0f, 0, 2.0f);
}

void drawAppleStimulusInsetOverlay(const AppleMetalVideoViewport &viewport,
                                   float framebuffer_scale,
                                   int64_t camera_frame,
                                   int64_t stimulus_frame) {
  if (viewport.width <= 0.0 || viewport.height <= 0.0 ||
      framebuffer_scale <= 0.0f || camera_frame < 0 || stimulus_frame < 0) {
    return;
  }
  const float x = static_cast<float>(viewport.x / framebuffer_scale);
  const float y = static_cast<float>(viewport.y / framebuffer_scale);
  const float width = static_cast<float>(viewport.width / framebuffer_scale);
  const float height = static_cast<float>(viewport.height / framebuffer_scale);
  const std::string label = "Stimulus " + std::to_string(stimulus_frame) +
                            "  Camera " + std::to_string(camera_frame);
  const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
  ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
  draw_list->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
                     IM_COL32(210, 216, 222, 210), 0.0f, 0, 1.0f);
  draw_list->AddRectFilled(ImVec2(x, y),
                           ImVec2(std::min(x + width, x + label_size.x + 14.0f),
                                  y + label_size.y + 8.0f),
                           IM_COL32(12, 15, 18, 220));
  draw_list->AddText(ImVec2(x + 7.0f, y + 4.0f), IM_COL32(240, 243, 246, 255),
                     label.c_str());
}

size_t drawAppleReadOnlyOverlayText(
    const crimson::overlay::ReadOnlyOverlayScene &scene,
    const crimson::overlay::SourceViewportTransform &transform,
    float framebuffer_scale_x, float framebuffer_scale_y) {
  if (framebuffer_scale_x <= 0.0f || framebuffer_scale_y <= 0.0f) {
    return 0;
  }
  const auto labels =
      crimson::overlay::layoutReadOnlyOverlayText(scene, transform);
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
    const ImVec2 text_size =
        font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.content.c_str());
    const ImVec2 anchor(
        static_cast<float>(label.anchor.x / framebuffer_scale_x),
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

size_t drawAppleChaserDistancePolarText(
    const crimson::polar::ChaserDistancePolarScene &scene,
    const AppleMetalVideoViewport &camera_viewport, float framebuffer_scale_x,
    float framebuffer_scale_y) {
  if (!scene.ready() || framebuffer_scale_x <= 0.0f ||
      framebuffer_scale_y <= 0.0f || camera_viewport.width <= 0.0 ||
      camera_viewport.height <= 0.0) {
    return 0;
  }
  const ImVec2 origin(
      static_cast<float>(camera_viewport.x / framebuffer_scale_x),
      static_cast<float>(camera_viewport.y / framebuffer_scale_y));
  const ImVec2 clip_min = origin;
  const ImVec2 clip_max(origin.x + static_cast<float>(camera_viewport.width /
                                                      framebuffer_scale_x),
                        origin.y + static_cast<float>(camera_viewport.height /
                                                      framebuffer_scale_y));
  ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
  draw_list->PushClipRect(clip_min, clip_max, true);
  for (const auto &annotation : scene.text) {
    ImVec2 anchor(origin.x + static_cast<float>(annotation.anchor.x),
                  origin.y + static_cast<float>(annotation.anchor.y));
    if (annotation.centered) {
      const ImVec2 text_size = ImGui::CalcTextSize(annotation.content.c_str());
      anchor.x -= text_size.x * 0.5f;
      anchor.y -= text_size.y * 0.5f;
    }
    draw_list->AddText(anchor, polarColor(annotation.color),
                       annotation.content.c_str());
  }
  draw_list->PopClipRect();
  return scene.text.size();
}

size_t drawAppleStimulusCameraOverlayText(
    const crimson::stimulus::StimulusCameraOverlayScene &scene,
    const AppleMetalVideoViewport &camera_viewport, float framebuffer_scale_x,
    float framebuffer_scale_y) {
  if (!scene.ready() || framebuffer_scale_x <= 0.0f ||
      framebuffer_scale_y <= 0.0f || camera_viewport.width <= 0.0 ||
      camera_viewport.height <= 0.0) {
    return 0;
  }
  const ImVec2 origin(
      static_cast<float>(camera_viewport.x / framebuffer_scale_x),
      static_cast<float>(camera_viewport.y / framebuffer_scale_y));
  const ImVec2 clip_min = origin;
  const ImVec2 clip_max(origin.x + static_cast<float>(camera_viewport.width /
                                                      framebuffer_scale_x),
                        origin.y + static_cast<float>(camera_viewport.height /
                                                      framebuffer_scale_y));
  ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
  draw_list->PushClipRect(clip_min, clip_max, true);
  for (const auto &annotation : scene.text) {
    const ImVec2 anchor(origin.x + static_cast<float>(annotation.anchor.x),
                        origin.y + static_cast<float>(annotation.anchor.y));
    draw_list->AddText(anchor, stimulusOverlayColor(annotation.color),
                       annotation.content.c_str());
  }
  draw_list->PopClipRect();
  return scene.text.size();
}

AppleCompositeVideoViewports
appleWorkspaceVideoViewports(const AppleMetalVideoViewport &camera,
                             const AppleMetalVideoViewport &crop_preview,
                             const AppleMetalVideoViewport &stimulus_debug,
                             const AppleVideoAssetInfo *crop_info,
                             const AppleVideoAssetInfo *stimulus_info,
                             double crop_inset_width,
                             double stimulus_inset_width) {
  AppleCompositeVideoViewports result;
  result.camera = camera;
  result.crop_preview = crop_preview;
  result.stimulus_debug = stimulus_debug;
  if (camera.width <= 0.0 || camera.height <= 0.0) {
    return result;
  }

  auto insets = crimson::macos::workspace::makeCameraInsetLayout(
      {camera.x, camera.y, camera.width, camera.height}, crop_info != nullptr,
      stimulus_info != nullptr);
  if (crop_info != nullptr && crop_inset_width > 0.0 && insets.crop.valid()) {
    const double edge = std::clamp(crop_inset_width, 1.0,
                                   std::min(camera.width, camera.height) * 0.8);
    const double right_margin = camera.x + camera.width - insets.crop.right();
    const double bottom_margin =
        camera.y + camera.height - insets.crop.bottom();
    insets.crop = {camera.x + camera.width - right_margin - edge,
                   camera.y + camera.height - bottom_margin - edge, edge, edge};
  }
  if (stimulus_info != nullptr && stimulus_inset_width > 0.0 &&
      insets.stimulus.valid()) {
    const double edge = std::clamp(stimulus_inset_width, 1.0,
                                   std::min(camera.width, camera.height) * 0.8);
    const double left_margin = insets.stimulus.x - camera.x;
    const double bottom_margin =
        camera.y + camera.height - insets.stimulus.bottom();
    insets.stimulus = {camera.x + left_margin,
                       camera.y + camera.height - bottom_margin - edge, edge,
                       edge};
  }
  if (crop_info != nullptr && insets.crop.valid()) {
    result.crop_inset =
        fitVideoViewport(insets.crop.x, insets.crop.y, insets.crop.width,
                         insets.crop.height, *crop_info);
  }
  if (stimulus_info != nullptr && insets.stimulus.valid()) {
    result.stimulus_inset = fitVideoViewport(
        insets.stimulus.x, insets.stimulus.y, insets.stimulus.width,
        insets.stimulus.height, *stimulus_info);
  }
  return result;
}
