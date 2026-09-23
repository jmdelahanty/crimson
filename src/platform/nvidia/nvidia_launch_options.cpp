#include "platform/nvidia/nvidia_launch_options.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace crimson::platform::nvidia {
namespace {

bool parseDoubleArgument(const char *text, double &out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || !std::isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

bool parseFrameRangeArgument(const char *text, int &start, int &end) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  const std::string value(text);
  const size_t colon = value.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
    return false;
  }
  int parsed_start = -1;
  int parsed_end = -1;
  if (!parseIntegerArgument(value.substr(0, colon).c_str(), parsed_start) ||
      !parseIntegerArgument(value.substr(colon + 1).c_str(), parsed_end) ||
      parsed_start < 0 || parsed_end < parsed_start) {
    return false;
  }
  start = parsed_start;
  end = parsed_end;
  return true;
}

constexpr crimson::ui_reference::StateMask kNvidiaUiReferenceStates =
    crimson::ui_reference::stateBit(UiReferenceState::Workspace) |
    crimson::ui_reference::stateBit(UiReferenceState::Overlays) |
    crimson::ui_reference::stateBit(UiReferenceState::Polar) |
    crimson::ui_reference::stateBit(UiReferenceState::StimulusOverlay) |
    crimson::ui_reference::stateBit(UiReferenceState::StimulusDebug) |
    crimson::ui_reference::stateBit(UiReferenceState::CropPreview) |
    crimson::ui_reference::stateBit(UiReferenceState::AnalysisEye) |
    crimson::ui_reference::stateBit(UiReferenceState::AnalysisTailStimulus);

constexpr crimson::ui_reference::LaunchParsePolicy kUiReferenceParsePolicy{
    kNvidiaUiReferenceStates, false, 4096};

bool isDirectory(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

bool isRegularFile(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

} // namespace

const char *uiReferenceStateName(UiReferenceState state) {
  return crimson::ui_reference::stateName(state);
}

bool ArtifactSelection::empty() const {
  return archive_path.empty() && run_name.empty() && manifest_digest.empty();
}

bool ArtifactSelection::complete() const {
  return !archive_path.empty() && !run_name.empty() && !manifest_digest.empty();
}

bool parseIntegerArgument(const char *text, int &out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

NvidiaLaunchParseResult parseNvidiaLaunchOptions(int argc,
                                                 const char *const *argv) {
  NvidiaLaunchParseResult result;
  auto &options = result.options;
  auto fail = [&](std::string error) {
    result.error = std::move(error);
    return result;
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i] != nullptr ? argv[i] : "";
    auto requireValue = [&](const std::string &message) -> const char * {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        result.error = message;
        return nullptr;
      }
      return argv[++i];
    };

    const auto ui_reference_argument =
        crimson::ui_reference::consumeLaunchArgument(
            argc, argv, i, options.ui_reference, kUiReferenceParsePolicy);
    if (ui_reference_argument.status ==
        crimson::ui_reference::ArgumentParseStatus::Error) {
      return fail(ui_reference_argument.error);
    }
    if (ui_reference_argument.status ==
        crimson::ui_reference::ArgumentParseStatus::Consumed) {
      continue;
    }

    if (arg == "--zarr") {
      const char *value = requireValue("Missing value for --zarr");
      if (value == nullptr) {
        return result;
      }
      options.zarr_override_path = value;
      continue;
    }
    if (arg == "--recording") {
      const char *value = requireValue("Missing value for --recording");
      if (value == nullptr) {
        return result;
      }
      options.recording_path = value;
      continue;
    }
    if (arg == "--recording-clip-index") {
      const char *value =
          requireValue("Missing value for --recording-clip-index");
      if (value == nullptr) {
        return result;
      }
      options.recording_clip_index_path = value;
      continue;
    }
    if (arg == "--subject-shape-run") {
      const char *value = requireValue("Missing value for --subject-shape-run");
      if (value == nullptr) {
        return result;
      }
      options.subject_shape_run = value;
      continue;
    }
    if (arg == "--refined-subject-mask-run") {
      const char *value =
          requireValue("Missing value for --refined-subject-mask-run");
      if (value == nullptr) {
        return result;
      }
      options.refined_subject_mask_run = value;
      continue;
    }
    if (arg == "--refined-subject-mask-storage") {
      const char *value =
          requireValue("Missing value for --refined-subject-mask-storage");
      if (value == nullptr) {
        return result;
      }
      options.refined_subject_mask_storage = value;
      continue;
    }
    if (arg == "--show-subject-masks" || arg == "--show-eye-masks") {
      options.show_eye_masks = true;
      continue;
    }
    if (arg == "--show-eye-geometry" || arg == "--no-eye-geometry") {
      options.show_eye_geometry = arg == "--show-eye-geometry";
      continue;
    }
    if (arg == "--tail-kinematics-run") {
      const char *value =
          requireValue("Missing value for --tail-kinematics-run");
      if (value == nullptr) {
        return result;
      }
      options.tail_kinematics_run = value;
      continue;
    }
    if (arg == "--eye-angle-run") {
      const char *value = requireValue("Missing value for --eye-angle-run");
      if (value == nullptr) {
        return result;
      }
      options.eye_angle_run = value;
      continue;
    }
    if (arg == "--stimulus-run") {
      const char *value = requireValue("Missing value for --stimulus-run");
      if (value == nullptr) {
        return result;
      }
      options.stimulus_run = value;
      continue;
    }
    if (arg == "--detection-run") {
      const char *value = requireValue("Missing value for --detection-run");
      if (value == nullptr || *value == '\0') {
        result.error = "Missing value for --detection-run";
        return result;
      }
      options.detection_run = value;
      continue;
    }
    if (arg == "--refined-detection-run" ||
        arg == "--benchmark-refined-detection-run") {
      const char *value = requireValue("Missing value for " + arg);
      if (value == nullptr || *value == '\0') {
        result.error = "Missing value for " + arg;
        return result;
      }
      options.refined_detection_run = value;
      options.allow_selector_ineligible_refined_detection =
          arg == "--benchmark-refined-detection-run";
      continue;
    }
    if (arg == "--benchmark-keypoint-v2-raw" ||
        arg == "--benchmark-keypoint-v2-quality" ||
        arg == "--benchmark-keypoint-v2-refined" ||
        arg == "--benchmark-keypoint-v2-body-frame") {
      if (i + 3 >= argc) {
        return fail(arg + " requires ARCHIVE RUN MANIFEST_DIGEST");
      }
      ArtifactSelection selection{argv[i + 1], argv[i + 2], argv[i + 3]};
      if (arg == "--benchmark-keypoint-v2-raw") {
        options.keypoint_v2_raw = std::move(selection);
      } else if (arg == "--benchmark-keypoint-v2-quality") {
        options.keypoint_v2_quality = std::move(selection);
      } else if (arg == "--benchmark-keypoint-v2-refined") {
        options.keypoint_v2_refined = std::move(selection);
      } else {
        options.keypoint_v2_body_frame = std::move(selection);
      }
      options.allow_selector_ineligible_keypoints = true;
      i += 3;
      continue;
    }
    if (arg == "--perf-log") {
      const char *value = requireValue("Missing value for --perf-log");
      if (value == nullptr) {
        return result;
      }
      options.perf_log_path = value;
      continue;
    }
    if (arg == "--mask-perf-log") {
      const char *value = requireValue("Missing value for --mask-perf-log");
      if (value == nullptr) {
        return result;
      }
      options.mask_perf_log_path = value;
      continue;
    }
    if (arg == "--playback-trace-log") {
      const char *value =
          requireValue("Missing value for --playback-trace-log");
      if (value == nullptr) {
        return result;
      }
      options.playback_trace_log_path = value;
      continue;
    }
    if (arg == "--frame-sync-trace-log") {
      const char *value =
          requireValue("Missing value for --frame-sync-trace-log");
      if (value == nullptr) {
        return result;
      }
      options.frame_sync_trace_log_path = value;
      continue;
    }
    if (arg == "--clipped-boundary-smoke" || arg == "--playback-smoke") {
      const char *value = requireValue("Missing value for " + arg);
      if (value == nullptr) {
        return result;
      }
      int start_frame = -1;
      int end_frame = -1;
      if (!parseFrameRangeArgument(value, start_frame, end_frame)) {
        if (arg == "--clipped-boundary-smoke") {
          return fail("Invalid --clipped-boundary-smoke value; expected "
                      "START:END with END >= START");
        }
        return fail("Invalid --playback-smoke value; expected START:END with "
                    "END >= START");
      }
      auto &smoke =
          arg == "--playback-smoke"
              ? static_cast<FrameRangeLaunchOptions &>(options.playback_smoke)
              : options.clipped_boundary_smoke;
      smoke.enabled = true;
      smoke.start_frame = start_frame;
      smoke.end_frame = end_frame;
      continue;
    }
    if (arg == "--playback-smoke-timeout") {
      const char *value =
          requireValue("Missing value for --playback-smoke-timeout");
      if (value == nullptr) {
        return result;
      }
      double parsed = 0.0;
      if (!parseDoubleArgument(value, parsed) || parsed <= 0.0) {
        return fail("Invalid --playback-smoke-timeout value; expected a "
                    "positive number of seconds");
      }
      options.playback_smoke.timeout_s = parsed;
      continue;
    }
    if (arg == "--playback-smoke-warmup-seconds") {
      const char *value = requireValue("Missing value for --playback-smoke-warmup-seconds");
      if (value == nullptr) return result;
      double parsed = 0.0;
      if (!parseDoubleArgument(value, parsed) || parsed < 0.0 || parsed > 120.0) {
        return fail("Invalid --playback-smoke-warmup-seconds; expected 0 through 120 seconds");
      }
      options.playback_smoke.warmup_s = parsed;
      continue;
    }
    if (arg == "--no-mask-perf-log") {
      options.mask_perf_log_enabled = false;
      continue;
    }
    if (arg == "--mask-perf-sample-every") {
      const char *value =
          requireValue("Missing value for --mask-perf-sample-every");
      if (value == nullptr) {
        return result;
      }
      int parsed = 0;
      if (!parseIntegerArgument(value, parsed) || parsed < 1) {
        return fail("Invalid --mask-perf-sample-every value; expected an "
                    "integer >= 1");
      }
      options.mask_perf_sample_every = parsed;
      continue;
    }
    if (arg == "--swap-interval") {
      const char *value = requireValue("Missing value for --swap-interval");
      if (value == nullptr) {
        return result;
      }
      int parsed = 0;
      if (!parseIntegerArgument(value, parsed) ||
          (parsed != 0 && parsed != 1)) {
        return fail("Invalid --swap-interval value; expected 0 or 1");
      }
      options.swap_interval = parsed;
      continue;
    }
    if (arg == "--frame-cap-fps") {
      const char *value = requireValue("Missing value for --frame-cap-fps");
      if (value == nullptr) {
        return result;
      }
      double parsed = 0.0;
      if (!parseDoubleArgument(value, parsed) || parsed < 0.0) {
        return fail("Invalid --frame-cap-fps value; expected a non-negative "
                    "number");
      }
      options.frame_cap_fps = parsed;
      continue;
    }
    result.diagnostics.push_back("Ignoring unknown argument: " + arg);
  }

  if (options.swap_interval == 0 && options.frame_cap_fps <= 0.0) {
    options.frame_cap_fps = 60.0;
    result.diagnostics.push_back(
        "[FramePacing] --swap-interval 0 requested without --frame-cap-fps; "
        "capping at 60 FPS to avoid an uncapped render loop.");
  }
  if (options.playback_smoke.enabled &&
      options.clipped_boundary_smoke.enabled) {
    return fail("--playback-smoke and --clipped-boundary-smoke cannot be used "
                "in the same run");
  }
  if (!options.detection_run.empty() &&
      !options.refined_detection_run.empty()) {
    return fail("--detection-run and --refined-detection-run are mutually "
                "exclusive");
  }
  const bool keypoint_v2_requested = !options.keypoint_v2_raw.empty() ||
                                     !options.keypoint_v2_quality.empty() ||
                                     !options.keypoint_v2_refined.empty() ||
                                     !options.keypoint_v2_body_frame.empty();
  if (keypoint_v2_requested && (!options.keypoint_v2_raw.complete() ||
                                !options.keypoint_v2_quality.complete() ||
                                !options.keypoint_v2_body_frame.complete() ||
                                (!options.keypoint_v2_refined.empty() &&
                                 !options.keypoint_v2_refined.complete()))) {
    return fail("Keypoint-v2 quality timelines require complete raw, quality, "
                "and body-frame selections; refined must be complete when "
                "provided");
  }
  if (const auto ui_reference_error =
          crimson::ui_reference::validateLaunchOptions(
              options.ui_reference, kUiReferenceParsePolicy)) {
    return fail(*ui_reference_error);
  }
  if (options.ui_reference.enabled &&
      (options.playback_smoke.enabled ||
       options.clipped_boundary_smoke.enabled)) {
    return fail("UI reference capture cannot run with playback or "
                "clipped-boundary smoke modes");
  }
  if (!options.recording_path.empty() && !options.zarr_override_path.empty()) {
    result.diagnostics.push_back(
        "Warning: both --recording and --zarr specified; using --recording, "
        "ignoring --zarr");
    options.zarr_override_path.clear();
  }
  if (!options.recording_path.empty() && !isDirectory(options.recording_path)) {
    result.diagnostics.push_back(
        "Error: --recording path is not a directory: " +
        options.recording_path);
    options.recording_path.clear();
  }
  if (!options.recording_clip_index_path.empty()) {
    if (!isRegularFile(options.recording_clip_index_path)) {
      return fail("Error: --recording-clip-index path is not a file: " +
                  options.recording_clip_index_path);
    }
    if (options.zarr_override_path.empty() && options.recording_path.empty()) {
      return fail("--recording-clip-index requires --zarr or --recording");
    }
  }

  result.ok = true;
  return result;
}

std::optional<int> parseCudaDeviceIndexString(const std::string &value) {
  if (value.empty()) {
    return std::nullopt;
  }
  try {
    size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed);
    if (consumed != value.size() || parsed < 0) {
      return std::nullopt;
    }
    return parsed;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::filesystem::path> cudaDeviceConfigPath() {
  if (const char *explicit_path = std::getenv("CRIMSON_CUDA_DEVICE_CONFIG");
      explicit_path && *explicit_path != '\0') {
    return std::filesystem::path(explicit_path);
  }
#ifdef _WIN32
  if (const char *localappdata = std::getenv("LOCALAPPDATA");
      localappdata && *localappdata != '\0') {
    return std::filesystem::path(localappdata) / "Crimson" / "config" /
           "cuda_device.json";
  }
  if (const char *appdata = std::getenv("APPDATA");
      appdata && *appdata != '\0') {
    return std::filesystem::path(appdata) / "crimson" / "cuda_device.json";
  }
#else
  if (const char *xdg_config_home = std::getenv("XDG_CONFIG_HOME");
      xdg_config_home && *xdg_config_home != '\0') {
    return std::filesystem::path(xdg_config_home) / "crimson" /
           "cuda_device.json";
  }
#endif
  if (const char *home = std::getenv("HOME"); home && *home != '\0') {
    return std::filesystem::path(home) / ".config" / "crimson" /
           "cuda_device.json";
  }
  return std::nullopt;
}

std::optional<int>
loadSavedCudaDeviceIndex(const std::filesystem::path &config_path,
                         std::string &source_description) {
  if (config_path.empty()) {
    return std::nullopt;
  }
  std::ifstream config_stream(config_path);
  if (!config_stream.is_open()) {
    return std::nullopt;
  }
  try {
    const nlohmann::json payload = nlohmann::json::parse(config_stream);
    const auto selected_index = payload.find("selected_cuda_device_index");
    if (selected_index == payload.end() ||
        !selected_index->is_number_integer()) {
      return std::nullopt;
    }
    const int parsed = selected_index->get<int>();
    if (parsed < 0) {
      return std::nullopt;
    }
    source_description = config_path.string();
    return parsed;
  } catch (const std::exception &error) {
    std::cerr << "[CudaDevice] Ignoring unreadable saved CUDA device config "
              << config_path << ": " << error.what() << std::endl;
    return std::nullopt;
  }
}

int resolveCudaDeviceIndex() {
  if (const char *env_device = std::getenv("CRIMSON_CUDA_DEVICE_INDEX");
      env_device && *env_device != '\0') {
    if (auto parsed = parseCudaDeviceIndexString(env_device)) {
      std::cout << "[CudaDevice] Using GPU " << *parsed
                << " from CRIMSON_CUDA_DEVICE_INDEX" << std::endl;
      return *parsed;
    }
    std::cerr << "[CudaDevice] Ignoring invalid CRIMSON_CUDA_DEVICE_INDEX="
              << env_device << std::endl;
  }
  if (auto config_path = cudaDeviceConfigPath()) {
    std::string source_description;
    if (auto saved_index =
            loadSavedCudaDeviceIndex(*config_path, source_description)) {
      std::cout << "[CudaDevice] Using GPU " << *saved_index << " from "
                << source_description << std::endl;
      return *saved_index;
    }
  }
  constexpr int default_cuda_device_index = 0;
  std::cout << "[CudaDevice] Using default GPU " << default_cuda_device_index
            << std::endl;
  return default_cuda_device_index;
}

} // namespace crimson::platform::nvidia
