#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ui_reference_contract.h"

namespace crimson::platform::nvidia {

using UiReferenceState = crimson::ui_reference::State;

const char *uiReferenceStateName(UiReferenceState state);

struct ArtifactSelection {
  std::string archive_path;
  std::string run_name;
  std::string manifest_digest;

  bool empty() const;
  bool complete() const;
};

struct FrameRangeLaunchOptions {
  bool enabled = false;
  int start_frame = -1;
  int end_frame = -1;
};

struct PlaybackSmokeLaunchOptions : FrameRangeLaunchOptions {
  double timeout_s = 20.0;
  double warmup_s = 0.0;
};

using UiReferenceLaunchOptions = crimson::ui_reference::LaunchOptions;

struct NvidiaLaunchOptions {
  std::string zarr_override_path;
  std::string recording_path;
  std::string recording_clip_index_path;
  std::string subject_shape_run;
  std::string refined_subject_mask_run;
  std::string refined_subject_mask_storage;
  std::string tail_kinematics_run;
  std::string eye_angle_run;
  std::string stimulus_run;
  std::string detection_run;
  std::string refined_detection_run;
  bool allow_selector_ineligible_refined_detection = false;

  ArtifactSelection keypoint_v2_raw;
  ArtifactSelection keypoint_v2_quality;
  ArtifactSelection keypoint_v2_refined;
  ArtifactSelection keypoint_v2_body_frame;
  bool allow_selector_ineligible_keypoints = false;

  std::filesystem::path perf_log_path;
  std::filesystem::path mask_perf_log_path;
  std::filesystem::path playback_trace_log_path;
  std::filesystem::path frame_sync_trace_log_path;
  int swap_interval = 1;
  int mask_perf_sample_every = 10;
  double frame_cap_fps = 0.0;
  bool mask_perf_log_enabled = true;
  bool show_eye_masks = false;

  PlaybackSmokeLaunchOptions playback_smoke;
  FrameRangeLaunchOptions clipped_boundary_smoke;
  UiReferenceLaunchOptions ui_reference;
};

struct NvidiaLaunchParseResult {
  bool ok = false;
  NvidiaLaunchOptions options;
  std::vector<std::string> diagnostics;
  std::string error;
};

NvidiaLaunchParseResult parseNvidiaLaunchOptions(int argc,
                                                 const char *const *argv);

bool parseIntegerArgument(const char *text, int &out);
std::optional<int> parseCudaDeviceIndexString(const std::string &value);
std::optional<std::filesystem::path> cudaDeviceConfigPath();
std::optional<int>
loadSavedCudaDeviceIndex(const std::filesystem::path &config_path,
                         std::string &source_description);
int resolveCudaDeviceIndex();

} // namespace crimson::platform::nvidia
