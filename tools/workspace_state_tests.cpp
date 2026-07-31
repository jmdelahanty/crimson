#include "workspace_state.h"

#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "  \
                << #condition << '\n';                                         \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testDefaultWindowVisibility() {
  using namespace crimson::workspace;
  WorkspaceState state;
  WorkspaceCapabilities empty;
  CHECK(state.shouldSubmit(Window::FileBrowser, empty));
  CHECK(!state.shouldSubmit(Window::FrameInspect, empty));
  CHECK(!state.shouldSubmit(Window::AnalysisTimeline, empty));
  CHECK(!state.shouldSubmit(Window::DetectionQualityTimeline, empty));
  CHECK(!state.shouldSubmit(Window::KeypointQualityTimeline, empty));
  CHECK(!state.shouldSubmit(Window::Help, empty));

  WorkspaceCapabilities loaded;
  loaded.video_loaded = true;
  loaded.playback_ready = true;
  loaded.zarr_loaded = true;
  loaded.crop_preview_available = true;
  loaded.stimulus_video_loaded = true;
  loaded.analysis_timeline_available = true;
  loaded.detection_quality_available = true;
  loaded.keypoint_quality_available = true;
  CHECK(state.shouldSubmit(Window::FrameInspect, loaded));
  CHECK(state.shouldSubmit(Window::Diagnostics, loaded));
  CHECK(state.shouldSubmit(Window::FramesInBuffer, loaded));
  CHECK(state.shouldSubmit(Window::CameraViews, loaded));
  CHECK(state.shouldSubmit(Window::StimulusEventTimeline, loaded));
  CHECK(state.shouldSubmit(Window::AnalysisTimeline, loaded));
  CHECK(!state.shouldSubmit(Window::DetectionQualityTimeline, loaded));
  CHECK(!state.shouldSubmit(Window::KeypointQualityTimeline, loaded));
  CHECK(!state.shouldSubmit(Window::AdvancedCropPreview, loaded));
  CHECK(!state.shouldSubmit(Window::Stimulus, loaded));

  loaded.playing = true;
  CHECK(!state.shouldSubmit(Window::FramesInBuffer, loaded));
  state.setWindowRequested(Window::AdvancedCropPreview, true);
  state.setWindowRequested(Window::Stimulus, true);
  state.setWindowRequested(Window::Help, true);
  state.setWindowRequested(Window::DetectionQualityTimeline, true);
  state.setWindowRequested(Window::KeypointQualityTimeline, true);
  CHECK(state.shouldSubmit(Window::AdvancedCropPreview, loaded));
  CHECK(state.shouldSubmit(Window::Stimulus, loaded));
  CHECK(state.shouldSubmit(Window::StimulusFramesInBuffer, loaded));
  CHECK(state.shouldSubmit(Window::Help, loaded));
  CHECK(state.shouldSubmit(Window::DetectionQualityTimeline, loaded));
  CHECK(state.shouldSubmit(Window::KeypointQualityTimeline, loaded));
  return true;
}

bool testFrameInspectViewSynchronization() {
  using namespace crimson::workspace;
  FrameInspectViewSyncState sync;
  CHECK(sync.shouldApply(FrameInspectView::Detect));
  sync.observe(FrameInspectView::Detect);
  CHECK(!sync.shouldApply(FrameInspectView::Detect));

  sync.observe(FrameInspectView::Keypoints);
  CHECK(!sync.shouldApply(FrameInspectView::Keypoints));
  CHECK(sync.shouldApply(FrameInspectView::EyeMasks));
  sync.observe(FrameInspectView::EyeMasks);
  CHECK(!sync.shouldApply(FrameInspectView::EyeMasks));
  return true;
}

bool testCommandEnablementAndPlaybackIntent() {
  using namespace crimson::workspace;
  WorkspaceState state;
  WorkspaceCapabilities capabilities;
  CHECK(state.commandEnabled(Command::OpenRecording, capabilities));
  CHECK(state.commandEnabled(Command::LoadZarrArchive, capabilities));
  CHECK(!state.commandEnabled(Command::LoadStimulusVideo, capabilities));
  CHECK(state.commandEnabled(Command::EditPathPresets, capabilities));
  CHECK(!state.commandEnabled(Command::TogglePlayback, capabilities));
  CHECK(!state.commandEnabled(Command::WriteRecordingData, capabilities));
  CHECK(!state.commandEnabled(Command::RunDetection, capabilities));
  CHECK(state.readOnlyInvariant(capabilities));
  capabilities.write_repository_open = true;
  CHECK(!state.readOnlyInvariant(capabilities));
  capabilities.write_repository_open = false;

  capabilities.video_loaded = true;
  capabilities.playback_ready = true;
  capabilities.crop_preview_available = true;
  capabilities.stimulus_video_loaded = true;
  CHECK(state.commandEnabled(Command::TogglePlayback, capabilities));
  CHECK(state.commandEnabled(Command::LoadStimulusVideo, capabilities));
  CHECK(state.commandEnabled(Command::StepForward, capabilities));
  CHECK(state.commandEnabled(Command::DumpDecodeBuffers, capabilities));
  CHECK(state.commandEnabled(Command::ToggleAdvancedCropPreview,
                             capabilities));
  CHECK(state.commandEnabled(Command::ToggleStimulusDebug, capabilities));

  auto intent = makePlaybackIntent(Command::TogglePlayback, capabilities, 20,
                                   100);
  CHECK(intent.has_value());
  CHECK(intent->kind == PlaybackIntentKind::Play);
  intent = makePlaybackIntent(Command::StepBackward, capabilities, 0, 100);
  CHECK(intent.has_value());
  CHECK(intent->kind == PlaybackIntentKind::Seek);
  CHECK(intent->target_frame == 0);
  intent = makePlaybackIntent(Command::StepForward, capabilities, 95, 100,
                              std::nullopt, 10);
  CHECK(intent.has_value());
  CHECK(intent->target_frame == 99);
  intent = makePlaybackIntent(Command::Seek, capabilities, 20, 100, 140);
  CHECK(intent.has_value());
  CHECK(intent->target_frame == 99);

  capabilities.playing = true;
  CHECK(state.commandEnabled(Command::StepForward, capabilities));
  CHECK(makePlaybackIntent(Command::StepForward, capabilities, 20, 100)
            .has_value());
  intent = makePlaybackIntent(Command::TogglePlayback, capabilities, 20, 100);
  CHECK(intent.has_value());
  CHECK(intent->kind == PlaybackIntentKind::Pause);
  return true;
}

bool testPlaybackShortcutResolution() {
  using namespace crimson::workspace;
  PlaybackShortcutInput input;

  input.comma_pressed = true;
  CHECK(resolvePlaybackShortcut(input).step_delta == -1);

  input = {};
  input.period_pressed = true;
  CHECK(resolvePlaybackShortcut(input).step_delta == 1);

  input = {};
  input.left_arrow_pressed = true;
  input.shift_down = true;
  CHECK(resolvePlaybackShortcut(input).step_delta == -10);

  input = {};
  input.right_arrow_pressed = true;
  input.shift_down = true;
  CHECK(resolvePlaybackShortcut(input).step_delta == 10);

  input = {};
  input.comma_pressed = true;
  input.shift_down = true;
  CHECK(resolvePlaybackShortcut(input).step_delta == -1);

  input = {};
  input.space_pressed = true;
  CHECK(resolvePlaybackShortcut(input).toggle_playback);

  input.wants_text_input = true;
  const auto text_input_result = resolvePlaybackShortcut(input);
  CHECK(!text_input_result.toggle_playback);
  CHECK(text_input_result.step_delta == 0);

  input = {};
  input.comma_pressed = true;
  input.period_pressed = true;
  CHECK(resolvePlaybackShortcut(input).step_delta == 0);

  input = {};
  input.enabled = false;
  input.period_pressed = true;
  CHECK(resolvePlaybackShortcut(input).step_delta == 0);
  return true;
}

bool testCameraNavigation() {
  using namespace crimson::workspace;
  CameraView view = autofitCameraView();
  CHECK(isValidCameraView(view));
  view = zoomCameraView(view, 2.0, 0.25, 0.75);
  CHECK(std::abs(view.x - 0.125) < 1e-9);
  CHECK(std::abs(view.y - 0.375) < 1e-9);
  CHECK(std::abs(view.width - 0.5) < 1e-9);
  CHECK(std::abs(view.height - 0.5) < 1e-9);
  view = panCameraView(view, 1.0, -1.0);
  CHECK(std::abs(view.x - 0.5) < 1e-9);
  CHECK(std::abs(view.y) < 1e-9);
  view = zoomCameraView(view, 0.01, 0.5, 0.5);
  CHECK(view.x == 0.0 && view.y == 0.0);
  CHECK(view.width == 1.0 && view.height == 1.0);
  CHECK(isValidCameraView(zoomCameraView(autofitCameraView(), 1000.0, 0.5,
                                        0.5)));
  CHECK(autofitCameraView().width == 1.0);
  return true;
}

bool testBufferedFrameNavigation() {
  using namespace crimson::workspace;
  const std::vector<int64_t> frames = {20, 8, 12, 12, 17};
  CHECK(adjacentBufferedFrame(frames, 17, -1) == 12);
  CHECK(adjacentBufferedFrame(frames, 17, 1) == 20);
  CHECK(adjacentBufferedFrame(frames, 15, -1) == 12);
  CHECK(adjacentBufferedFrame(frames, 15, 1) == 17);
  CHECK(!adjacentBufferedFrame(frames, 8, -1).has_value());
  CHECK(!adjacentBufferedFrame(frames, 20, 1).has_value());
  CHECK(!adjacentBufferedFrame({}, 10, 1).has_value());
  return true;
}

bool testSelectionPropagation() {
  using namespace crimson::workspace;
  WorkspaceState state;
  auto &selection = state.selections();
  selection.frame_inspect_view = FrameInspectView::EyeAngles;
  selection.motion_source_key = "filtered";
  selection.swim_bout_candidate_key = "candidate-2";
  selection.eye_angle_representation_key = "camera";
  selection.tail_kinematics_source_key = "smoothed";
  selection.stimulus_event_index = 7;
  state.setCropSourcePreference(
      crimson::crop::CropSourcePreference::PreferLiveGeometry);
  state.overlayControls().show_keypoints = false;

  const auto snapshot = state.snapshot();
  CHECK(snapshot.selections.frame_inspect_view == FrameInspectView::EyeAngles);
  CHECK(snapshot.selections.motion_source_key == "filtered");
  CHECK(snapshot.selections.swim_bout_candidate_key == "candidate-2");
  CHECK(snapshot.selections.eye_angle_representation_key == "camera");
  CHECK(snapshot.selections.tail_kinematics_source_key == "smoothed");
  CHECK(snapshot.selections.stimulus_event_index == 7);
  CHECK(snapshot.crop_source_preference ==
        crimson::crop::CropSourcePreference::PreferLiveGeometry);
  CHECK(!snapshot.overlay_controls.show_keypoints);
  return true;
}

bool testStateRestoration() {
  using namespace crimson::workspace;
  WorkspaceSnapshot snapshot;
  snapshot.windows.advanced_crop_preview = true;
  snapshot.windows.stimulus_debug = true;
  snapshot.windows.help = true;
  snapshot.windows.detection_quality_timeline = true;
  snapshot.selections.frame_inspect_view =
      static_cast<FrameInspectView>(255);
  snapshot.selections.motion_source_key = "removed-motion";
  snapshot.selections.swim_bout_candidate_key = "candidate-2";
  snapshot.selections.eye_angle_representation_key = "removed-eye";
  snapshot.selections.tail_kinematics_source_key = "tail-raw";
  snapshot.selections.stimulus_event_index = 12;
  snapshot.crop_source_preference =
      static_cast<crimson::crop::CropSourcePreference>(255);
  snapshot.overlay_controls.show_subject_shape = false;
  snapshot.overlay_controls.mask_mode =
      static_cast<crimson::overlay::ReadOnlyMaskOverlayMode>(255);

  WorkspaceSelectionCatalog catalog;
  catalog.motion_source_keys = {"raw", "filtered"};
  catalog.default_motion_source_key = "filtered";
  catalog.swim_bout_candidate_keys = {"candidate-1", "candidate-2"};
  catalog.default_swim_bout_candidate_key = "candidate-1";
  catalog.eye_angle_representation_keys = {"world", "camera"};
  catalog.default_eye_angle_representation_key = "world";
  catalog.tail_kinematics_source_keys = {"tail-raw", "tail-smoothed"};
  catalog.default_tail_kinematics_source_key = "tail-smoothed";
  catalog.stimulus_event_count = 4;

  WorkspaceState restored;
  CHECK(restored.restore(snapshot, catalog));
  CHECK(restored.windowRequested(Window::AdvancedCropPreview));
  CHECK(restored.windowRequested(Window::Stimulus));
  CHECK(restored.windowRequested(Window::Help));
  CHECK(restored.windowRequested(Window::DetectionQualityTimeline));
  CHECK(restored.selections().frame_inspect_view == FrameInspectView::Detect);
  CHECK(restored.selections().motion_source_key == "filtered");
  CHECK(restored.selections().swim_bout_candidate_key == "candidate-2");
  CHECK(restored.selections().eye_angle_representation_key == "world");
  CHECK(restored.selections().tail_kinematics_source_key == "tail-raw");
  CHECK(!restored.selections().stimulus_event_index.has_value());
  CHECK(restored.cropSourcePreference() ==
        crimson::crop::CropSourcePreference::PreferAcquisitionVideo);
  CHECK(!restored.overlayControls().show_subject_shape);
  CHECK(restored.overlayControls().mask_mode ==
        crimson::overlay::ReadOnlyMaskOverlayMode::Review);

  snapshot.version = kWorkspaceSnapshotVersion + 1;
  CHECK(!restored.restore(snapshot, catalog));
  return true;
}

} // namespace

int main() {
  if (!testDefaultWindowVisibility() ||
      !testFrameInspectViewSynchronization() ||
      !testCommandEnablementAndPlaybackIntent() ||
      !testPlaybackShortcutResolution() || !testSelectionPropagation() ||
      !testStateRestoration() || !testCameraNavigation() ||
      !testBufferedFrameNavigation()) {
    return 1;
  }
  std::cout << "workspace_state_tests: PASS\n";
  return 0;
}
