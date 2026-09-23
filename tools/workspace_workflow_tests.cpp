#include "workspace_state.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "  \
                << #condition << '\n';                                         \
      return 1;                                                                \
    }                                                                          \
  } while (false)

} // namespace

int main() {
  using namespace crimson::workspace;

  WorkspaceCapabilities capabilities;
  capabilities.video_loaded = true;
  capabilities.playback_ready = true;
  capabilities.zarr_loaded = true;
  capabilities.crop_preview_available = true;
  capabilities.stimulus_video_loaded = true;
  capabilities.analysis_timeline_available = true;

  WorkspaceState state;
  CHECK(state.readOnlyInvariant(capabilities));
  CHECK(!state.commandEnabled(Command::RunDetection, capabilities));
  CHECK(!state.commandEnabled(Command::WriteRecordingData, capabilities));
  CHECK(!state.commandEnabled(Command::EnableLegacyManualLabeling,
                              capabilities));

  auto intent = makePlaybackIntent(Command::TogglePlayback, capabilities, 0,
                                   1000);
  CHECK(intent && intent->kind == PlaybackIntentKind::Play);
  capabilities.playing = true;
  intent = makePlaybackIntent(Command::TogglePlayback, capabilities, 90, 1000);
  CHECK(intent && intent->kind == PlaybackIntentKind::Pause);
  capabilities.playing = false;
  intent = makePlaybackIntent(Command::Seek, capabilities, 90, 1000, 300);
  CHECK(intent && intent->target_frame == 300);
  intent = makePlaybackIntent(Command::StepBackward, capabilities, 300, 1000,
                              std::nullopt, 10);
  CHECK(intent && intent->target_frame == 290);
  intent = makePlaybackIntent(Command::StepForward, capabilities, 290, 1000);
  CHECK(intent && intent->target_frame == 291);

  const std::vector<int64_t> buffered = {286, 288, 290, 291, 294};
  CHECK(adjacentBufferedFrame(buffered, 290, -1) == 288);
  CHECK(adjacentBufferedFrame(buffered, 290, 1) == 291);

  CameraView camera = zoomCameraView(autofitCameraView(), 2.0, 0.5, 0.5);
  CHECK(isValidCameraView(camera));
  camera = panCameraView(camera, 0.25, -0.25);
  CHECK(isValidCameraView(camera));
  camera = autofitCameraView();
  CHECK(camera.x == 0.0 && camera.y == 0.0 && camera.width == 1.0 &&
        camera.height == 1.0);

  state.setCropSourcePreference(
      crimson::crop::CropSourcePreference::PreferLiveGeometry);
  state.selections().motion_source_key = "smoothed";
  state.selections().swim_bout_candidate_key = "detector-response";
  state.selections().eye_angle_representation_key = "camera";
  state.selections().tail_kinematics_source_key = "smoothed";
  state.selections().stimulus_event_index = 4;
  state.overlayControls().show_keypoints = false;
  state.overlayControls().show_subject_masks = false;
  state.overlayControls().show_eye_geometry = false;

  state.setWindowRequested(Window::AdvancedCropPreview, true);
  state.setWindowRequested(Window::Stimulus, true);
  state.setWindowRequested(Window::Help, true);
  CHECK(state.shouldSubmit(Window::AdvancedCropPreview, capabilities));
  CHECK(state.shouldSubmit(Window::Stimulus, capabilities));
  CHECK(state.shouldSubmit(Window::StimulusFramesInBuffer, capabilities));
  CHECK(state.shouldSubmit(Window::Help, capabilities));
  state.setWindowRequested(Window::AdvancedCropPreview, false);
  state.setWindowRequested(Window::Stimulus, false);
  state.setWindowRequested(Window::Help, false);
  CHECK(!state.shouldSubmit(Window::AdvancedCropPreview, capabilities));
  CHECK(!state.shouldSubmit(Window::Stimulus, capabilities));
  CHECK(!state.shouldSubmit(Window::Help, capabilities));

  const WorkspaceSnapshot snapshot = state.snapshot();
  WorkspaceSelectionCatalog catalog;
  catalog.motion_source_keys = {"raw", "smoothed"};
  catalog.default_motion_source_key = "smoothed";
  catalog.swim_bout_candidate_keys = {"detector-response"};
  catalog.default_swim_bout_candidate_key = "detector-response";
  catalog.eye_angle_representation_keys = {"world", "camera"};
  catalog.default_eye_angle_representation_key = "world";
  catalog.tail_kinematics_source_keys = {"raw", "smoothed"};
  catalog.default_tail_kinematics_source_key = "smoothed";
  catalog.stimulus_event_count = 10;
  WorkspaceState restored;
  CHECK(restored.restore(snapshot, catalog));
  CHECK(restored.cropSourcePreference() ==
        crimson::crop::CropSourcePreference::PreferLiveGeometry);
  CHECK(restored.selections().stimulus_event_index == 4);
  CHECK(!restored.overlayControls().show_keypoints);
  CHECK(!restored.overlayControls().show_subject_masks);
  CHECK(!restored.overlayControls().show_eye_geometry);

  capabilities.write_repository_open = true;
  CHECK(!restored.readOnlyInvariant(capabilities));

  std::cout << "workspace_workflow_tests: PASS\n";
  return 0;
}
