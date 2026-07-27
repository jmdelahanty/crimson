#include "workspace_state.h"

#include <algorithm>
#include <cmath>

namespace crimson::workspace {
namespace {

bool contains(const std::vector<std::string> &values,
              const std::string &candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

std::string restoredKey(const std::string &requested,
                        const std::vector<std::string> &available,
                        const std::string &default_key) {
  if (contains(available, requested)) {
    return requested;
  }
  if (contains(available, default_key)) {
    return default_key;
  }
  return available.empty() ? std::string{} : available.front();
}

bool validFrameInspectView(FrameInspectView view) {
  switch (view) {
  case FrameInspectView::Detect:
  case FrameInspectView::Keypoints:
  case FrameInspectView::EyeMasks:
  case FrameInspectView::TailKinematics:
  case FrameInspectView::EyeAngles:
    return true;
  }
  return false;
}

bool validCropSourcePreference(crop::CropSourcePreference preference) {
  switch (preference) {
  case crop::CropSourcePreference::PreferLiveGeometry:
  case crop::CropSourcePreference::PreferAcquisitionVideo:
  case crop::CropSourcePreference::PreferPersistedZarr:
    return true;
  }
  return false;
}

bool validMaskOverlayMode(overlay::ReadOnlyMaskOverlayMode mode) {
  switch (mode) {
  case overlay::ReadOnlyMaskOverlayMode::Realtime:
  case overlay::ReadOnlyMaskOverlayMode::Review:
  case overlay::ReadOnlyMaskOverlayMode::Debug:
    return true;
  }
  return false;
}

} // namespace

bool WorkspaceState::shouldSubmit(
    Window window, const WorkspaceCapabilities &capabilities) const {
  switch (window) {
  case Window::FileBrowser:
    return true;
  case Window::FrameInspect:
  case Window::Diagnostics:
  case Window::CameraViews:
    return capabilities.video_loaded;
  case Window::FramesInBuffer:
    return capabilities.video_loaded && capabilities.playback_ready &&
           !capabilities.playing;
  case Window::AdvancedCropPreview:
    return windows_.advanced_crop_preview &&
           capabilities.crop_preview_available;
  case Window::Stimulus:
  case Window::StimulusFramesInBuffer:
    return windows_.stimulus_debug && capabilities.stimulus_video_loaded;
  case Window::StimulusEventTimeline:
    return capabilities.zarr_loaded;
  case Window::AnalysisTimeline:
    return capabilities.analysis_timeline_available;
  case Window::Help:
    return windows_.help;
  }
  return false;
}

bool WorkspaceState::commandEnabled(
    Command command, const WorkspaceCapabilities &capabilities) const {
  switch (command) {
  case Command::OpenRecording:
  case Command::LoadZarrArchive:
  case Command::EditPathPresets:
  case Command::ToggleHelp:
    return true;
  case Command::LoadStimulusVideo:
    return capabilities.video_loaded;
  case Command::TogglePlayback:
  case Command::Seek:
    return capabilities.video_loaded && capabilities.playback_ready;
  case Command::StepBackward:
  case Command::StepForward:
    return capabilities.video_loaded && capabilities.playback_ready;
  case Command::DumpDecodeBuffers:
    return capabilities.video_loaded;
  case Command::RandomSeekAndDump:
    return capabilities.video_loaded && capabilities.playback_ready;
  case Command::ToggleAdvancedCropPreview:
    return capabilities.crop_preview_available;
  case Command::ToggleStimulusDebug:
    return capabilities.stimulus_video_loaded;
  case Command::RunDetection:
  case Command::WriteRecordingData:
  case Command::EnableLegacyManualLabeling:
    return false;
  }
  return false;
}

bool WorkspaceState::windowRequested(Window window) const {
  switch (window) {
  case Window::AdvancedCropPreview:
    return windows_.advanced_crop_preview;
  case Window::Stimulus:
  case Window::StimulusFramesInBuffer:
    return windows_.stimulus_debug;
  case Window::Help:
    return windows_.help;
  case Window::FileBrowser:
  case Window::FrameInspect:
  case Window::Diagnostics:
  case Window::FramesInBuffer:
  case Window::CameraViews:
  case Window::StimulusEventTimeline:
  case Window::AnalysisTimeline:
    return true;
  }
  return false;
}

void WorkspaceState::setWindowRequested(Window window, bool requested) {
  switch (window) {
  case Window::AdvancedCropPreview:
    windows_.advanced_crop_preview = requested;
    break;
  case Window::Stimulus:
  case Window::StimulusFramesInBuffer:
    windows_.stimulus_debug = requested;
    break;
  case Window::Help:
    windows_.help = requested;
    break;
  case Window::FileBrowser:
  case Window::FrameInspect:
  case Window::Diagnostics:
  case Window::FramesInBuffer:
  case Window::CameraViews:
  case Window::StimulusEventTimeline:
  case Window::AnalysisTimeline:
    break;
  }
}

WorkspaceSnapshot WorkspaceState::snapshot() const {
  WorkspaceSnapshot result;
  result.windows = windows_;
  result.selections = selections_;
  result.crop_source_preference = crop_source_preference_;
  result.overlay_controls = overlay_controls_;
  return result;
}

bool WorkspaceState::restore(const WorkspaceSnapshot &snapshot,
                             const WorkspaceSelectionCatalog &catalog) {
  if (snapshot.version != kWorkspaceSnapshotVersion) {
    return false;
  }

  windows_ = snapshot.windows;
  selections_ = snapshot.selections;
  if (!validFrameInspectView(selections_.frame_inspect_view)) {
    selections_.frame_inspect_view = FrameInspectView::Detect;
  }
  selections_.motion_source_key =
      restoredKey(selections_.motion_source_key, catalog.motion_source_keys,
                  catalog.default_motion_source_key);
  selections_.swim_bout_candidate_key = restoredKey(
      selections_.swim_bout_candidate_key, catalog.swim_bout_candidate_keys,
      catalog.default_swim_bout_candidate_key);
  selections_.eye_angle_representation_key = restoredKey(
      selections_.eye_angle_representation_key,
      catalog.eye_angle_representation_keys,
      catalog.default_eye_angle_representation_key);
  selections_.tail_kinematics_source_key = restoredKey(
      selections_.tail_kinematics_source_key,
      catalog.tail_kinematics_source_keys,
      catalog.default_tail_kinematics_source_key);
  if (selections_.stimulus_event_index.has_value() &&
      *selections_.stimulus_event_index >= catalog.stimulus_event_count) {
    selections_.stimulus_event_index.reset();
  }
  crop_source_preference_ =
      validCropSourcePreference(snapshot.crop_source_preference)
          ? snapshot.crop_source_preference
          : crop::CropSourcePreference::PreferAcquisitionVideo;
  overlay_controls_ = snapshot.overlay_controls;
  if (!validMaskOverlayMode(overlay_controls_.mask_mode)) {
    overlay_controls_.mask_mode = overlay::ReadOnlyMaskOverlayMode::Review;
  }
  return true;
}

std::optional<PlaybackIntent>
makePlaybackIntent(Command command, const WorkspaceCapabilities &capabilities,
                   int64_t current_frame, int64_t frame_count,
                   std::optional<int64_t> seek_target,
                   int64_t step_magnitude) {
  WorkspaceState state;
  if (!state.commandEnabled(command, capabilities) || frame_count <= 0) {
    return std::nullopt;
  }
  const int64_t bounded_current =
      std::clamp(current_frame, int64_t{0}, frame_count - 1);

  switch (command) {
  case Command::TogglePlayback:
    return PlaybackIntent{capabilities.playing ? PlaybackIntentKind::Pause
                                               : PlaybackIntentKind::Play,
                          -1};
  case Command::Seek:
    if (!seek_target.has_value()) {
      return std::nullopt;
    }
    return PlaybackIntent{PlaybackIntentKind::Seek,
                          std::clamp(*seek_target, int64_t{0},
                                     frame_count - 1)};
  case Command::StepBackward:
    return PlaybackIntent{
        PlaybackIntentKind::Seek,
        bounded_current <= 0
            ? 0
            : bounded_current -
                  std::min(bounded_current,
                           std::max<int64_t>(1, step_magnitude))};
  case Command::StepForward:
    return PlaybackIntent{
        PlaybackIntentKind::Seek,
        bounded_current >= frame_count - 1
            ? frame_count - 1
            : bounded_current +
                  std::min(frame_count - 1 - bounded_current,
                           std::max<int64_t>(1, step_magnitude))};
  case Command::OpenRecording:
  case Command::LoadZarrArchive:
  case Command::LoadStimulusVideo:
  case Command::EditPathPresets:
  case Command::DumpDecodeBuffers:
  case Command::RandomSeekAndDump:
  case Command::ToggleAdvancedCropPreview:
  case Command::ToggleStimulusDebug:
  case Command::ToggleHelp:
  case Command::RunDetection:
  case Command::WriteRecordingData:
  case Command::EnableLegacyManualLabeling:
    return std::nullopt;
  }
  return std::nullopt;
}

CameraView autofitCameraView() { return {}; }

bool isValidCameraView(const CameraView &view) {
  constexpr double epsilon = 1e-9;
  return std::isfinite(view.x) && std::isfinite(view.y) &&
         std::isfinite(view.width) && std::isfinite(view.height) &&
         view.x >= -epsilon && view.y >= -epsilon && view.width > 0.0 &&
         view.height > 0.0 && view.x + view.width <= 1.0 + epsilon &&
         view.y + view.height <= 1.0 + epsilon;
}

CameraView panCameraView(const CameraView &view, double delta_x,
                         double delta_y) {
  if (!isValidCameraView(view) || !std::isfinite(delta_x) ||
      !std::isfinite(delta_y)) {
    return autofitCameraView();
  }
  CameraView result = view;
  result.x = std::clamp(view.x + delta_x, 0.0, 1.0 - view.width);
  result.y = std::clamp(view.y + delta_y, 0.0, 1.0 - view.height);
  return result;
}

CameraView zoomCameraView(const CameraView &view, double zoom_factor,
                          double anchor_x, double anchor_y) {
  constexpr double minimum_extent = 1.0 / 64.0;
  if (!isValidCameraView(view) || !std::isfinite(zoom_factor) ||
      zoom_factor <= 0.0 || !std::isfinite(anchor_x) ||
      !std::isfinite(anchor_y)) {
    return autofitCameraView();
  }
  anchor_x = std::clamp(anchor_x, 0.0, 1.0);
  anchor_y = std::clamp(anchor_y, 0.0, 1.0);
  const double new_width =
      std::clamp(view.width / zoom_factor, minimum_extent, 1.0);
  const double new_height =
      std::clamp(view.height / zoom_factor, minimum_extent, 1.0);
  const double source_anchor_x = view.x + anchor_x * view.width;
  const double source_anchor_y = view.y + anchor_y * view.height;
  CameraView result;
  result.width = new_width;
  result.height = new_height;
  result.x = std::clamp(source_anchor_x - anchor_x * new_width, 0.0,
                        1.0 - new_width);
  result.y = std::clamp(source_anchor_y - anchor_y * new_height, 0.0,
                        1.0 - new_height);
  return result;
}

std::optional<int64_t>
adjacentBufferedFrame(const std::vector<int64_t> &buffered_frames,
                      int64_t current_frame, int direction) {
  if (buffered_frames.empty() || direction == 0) {
    return std::nullopt;
  }
  std::vector<int64_t> ordered = buffered_frames;
  std::sort(ordered.begin(), ordered.end());
  ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
  if (direction < 0) {
    const auto found = std::lower_bound(ordered.begin(), ordered.end(),
                                        current_frame);
    if (found == ordered.begin()) {
      return std::nullopt;
    }
    return *std::prev(found);
  }
  const auto found =
      std::upper_bound(ordered.begin(), ordered.end(), current_frame);
  return found == ordered.end() ? std::nullopt
                                : std::optional<int64_t>(*found);
}

} // namespace crimson::workspace
