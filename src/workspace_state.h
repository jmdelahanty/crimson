#pragma once

#include "crop_source_contract.h"
#include "read_only_overlay_controls.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crimson::workspace {

constexpr uint32_t kWorkspaceSnapshotVersion = 1;

enum class Window : uint8_t {
  FileBrowser,
  FrameInspect,
  Diagnostics,
  FramesInBuffer,
  CameraViews,
  AdvancedCropPreview,
  Stimulus,
  StimulusFramesInBuffer,
  StimulusEventTimeline,
  AnalysisTimeline,
  DetectionQualityTimeline,
  Help,
};

enum class Command : uint8_t {
  OpenRecording,
  LoadZarrArchive,
  LoadStimulusVideo,
  EditPathPresets,
  TogglePlayback,
  Seek,
  StepBackward,
  StepForward,
  DumpDecodeBuffers,
  RandomSeekAndDump,
  ToggleAdvancedCropPreview,
  ToggleStimulusDebug,
  ToggleHelp,
  RunDetection,
  WriteRecordingData,
  EnableLegacyManualLabeling,
};

enum class FrameInspectView : uint8_t {
  Detect,
  Keypoints,
  EyeMasks,
  TailKinematics,
  EyeAngles,
};

enum class PlaybackIntentKind : uint8_t {
  Play,
  Pause,
  Seek,
};

struct WorkspaceCapabilities {
  bool video_loaded = false;
  bool playback_ready = false;
  bool playing = false;
  bool zarr_loaded = false;
  bool crop_preview_available = false;
  bool stimulus_video_loaded = false;
  bool analysis_timeline_available = false;
  bool detection_quality_available = false;
  bool write_repository_open = false;
};

struct WorkspaceWindowState {
  bool advanced_crop_preview = false;
  bool stimulus_debug = false;
  bool detection_quality_timeline = false;
  bool help = false;
};

struct WorkspaceSelectionState {
  FrameInspectView frame_inspect_view = FrameInspectView::Detect;
  std::string motion_source_key;
  std::string swim_bout_candidate_key;
  std::string eye_angle_representation_key;
  std::string tail_kinematics_source_key;
  std::optional<size_t> stimulus_event_index;
};

struct WorkspaceSelectionCatalog {
  std::vector<std::string> motion_source_keys;
  std::string default_motion_source_key;
  std::vector<std::string> swim_bout_candidate_keys;
  std::string default_swim_bout_candidate_key;
  std::vector<std::string> eye_angle_representation_keys;
  std::string default_eye_angle_representation_key;
  std::vector<std::string> tail_kinematics_source_keys;
  std::string default_tail_kinematics_source_key;
  size_t stimulus_event_count = 0;
};

struct WorkspaceSnapshot {
  uint32_t version = kWorkspaceSnapshotVersion;
  WorkspaceWindowState windows;
  WorkspaceSelectionState selections;
  crop::CropSourcePreference crop_source_preference =
      crop::CropSourcePreference::PreferAcquisitionVideo;
  overlay::ReadOnlyOverlayControlState overlay_controls;
};

struct PlaybackIntent {
  PlaybackIntentKind kind = PlaybackIntentKind::Seek;
  int64_t target_frame = -1;
};

struct CameraView {
  double x = 0.0;
  double y = 0.0;
  double width = 1.0;
  double height = 1.0;
};

class WorkspaceState {
public:
  bool shouldSubmit(Window window,
                    const WorkspaceCapabilities &capabilities) const;
  bool commandEnabled(Command command,
                      const WorkspaceCapabilities &capabilities) const;
  bool readOnlyInvariant(const WorkspaceCapabilities &capabilities) const {
    return !capabilities.write_repository_open;
  }

  bool windowRequested(Window window) const;
  void setWindowRequested(Window window, bool requested);

  WorkspaceSelectionState &selections() { return selections_; }
  const WorkspaceSelectionState &selections() const { return selections_; }

  crop::CropSourcePreference cropSourcePreference() const {
    return crop_source_preference_;
  }
  void setCropSourcePreference(crop::CropSourcePreference preference) {
    crop_source_preference_ = preference;
  }

  overlay::ReadOnlyOverlayControlState &overlayControls() {
    return overlay_controls_;
  }
  const overlay::ReadOnlyOverlayControlState &overlayControls() const {
    return overlay_controls_;
  }

  WorkspaceSnapshot snapshot() const;
  bool restore(const WorkspaceSnapshot &snapshot,
               const WorkspaceSelectionCatalog &catalog);

private:
  WorkspaceWindowState windows_;
  WorkspaceSelectionState selections_;
  crop::CropSourcePreference crop_source_preference_ =
      crop::CropSourcePreference::PreferAcquisitionVideo;
  overlay::ReadOnlyOverlayControlState overlay_controls_;
};

std::optional<PlaybackIntent>
makePlaybackIntent(Command command, const WorkspaceCapabilities &capabilities,
                   int64_t current_frame, int64_t frame_count,
                   std::optional<int64_t> seek_target = std::nullopt,
                   int64_t step_magnitude = 1);

CameraView autofitCameraView();
CameraView panCameraView(const CameraView &view, double delta_x,
                         double delta_y);
CameraView zoomCameraView(const CameraView &view, double zoom_factor,
                          double anchor_x, double anchor_y);
bool isValidCameraView(const CameraView &view);

std::optional<int64_t>
adjacentBufferedFrame(const std::vector<int64_t> &buffered_frames,
                      int64_t current_frame, int direction);

} // namespace crimson::workspace
