#include "platform/nvidia/nvidia_launch_options.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

using crimson::platform::nvidia::NvidiaLaunchParseResult;
using crimson::platform::nvidia::UiReferenceState;

NvidiaLaunchParseResult parse(const std::vector<std::string> &arguments) {
  std::vector<const char *> argv;
  argv.reserve(arguments.size());
  for (const auto &argument : arguments) {
    argv.push_back(argument.c_str());
  }
  return crimson::platform::nvidia::parseNvidiaLaunchOptions(
      static_cast<int>(argv.size()), argv.data());
}

std::filesystem::path temporaryRoot() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("crimson_nvidia_launch_options_" + std::to_string(nonce));
}

bool testDefaultsAndUnknownArguments() {
  auto result = parse({"redgui", "--future-option"});
  CHECK(result.ok);
  CHECK(result.error.empty());
  CHECK(result.options.swap_interval == 1);
  CHECK(result.options.frame_cap_fps == 0.0);
  CHECK(result.options.mask_perf_log_enabled);
  CHECK(result.options.mask_perf_sample_every == 10);
  CHECK(result.options.playback_smoke.timeout_s == 20.0);
  CHECK(result.options.ui_reference.timeout_s == 60.0);
  CHECK(result.diagnostics.size() == 1);
  CHECK(result.diagnostics.front() ==
        "Ignoring unknown argument: --future-option");
  return true;
}

bool testCompleteOptionSurface() {
  const auto result = parse({"redgui",
                             "--zarr",
                             "analysis.zarr",
                             "--recording-clip-index",
                             "/tmp/does-not-exist-yet.json",
                             "--subject-shape-run",
                             "shape",
                             "--refined-subject-mask-run",
                             "masks",
                             "--refined-subject-mask-storage",
                             "dense",
                             "--tail-kinematics-run",
                             "tail",
                             "--eye-angle-run",
                             "eyes",
                             "--stimulus-run",
                             "stimulus",
                             "--benchmark-refined-detection-run",
                             "refined",
                             "--benchmark-keypoint-v2-raw",
                             "raw.zarr",
                             "raw-run",
                             "raw-digest",
                             "--benchmark-keypoint-v2-quality",
                             "quality.zarr",
                             "quality-run",
                             "quality-digest",
                             "--benchmark-keypoint-v2-refined",
                             "refined.zarr",
                             "refined-run",
                             "refined-digest",
                             "--benchmark-keypoint-v2-body-frame",
                             "body.zarr",
                             "body-run",
                             "body-digest",
                             "--show-subject-masks",
                             "--perf-log",
                             "perf.jsonl",
                             "--mask-perf-log",
                             "mask.jsonl",
                             "--playback-trace-log",
                             "playback.jsonl",
                             "--frame-sync-trace-log",
                             "sync.jsonl",
                             "--playback-smoke",
                             "20:90",
                             "--playback-smoke-timeout",
                             "8.5",
                             "--mask-perf-sample-every",
                             "4",
                             "--no-mask-perf-log",
                             "--swap-interval",
                             "0",
                             "--frame-cap-fps",
                             "144"});

  CHECK(!result.ok);
  CHECK(result.error.find("recording-clip-index path is not a file") !=
        std::string::npos);

  auto without_clip = parse({"redgui",
                             "--zarr",
                             "analysis.zarr",
                             "--subject-shape-run",
                             "shape",
                             "--refined-subject-mask-run",
                             "masks",
                             "--refined-subject-mask-storage",
                             "dense",
                             "--tail-kinematics-run",
                             "tail",
                             "--eye-angle-run",
                             "eyes",
                             "--stimulus-run",
                             "stimulus",
                             "--benchmark-refined-detection-run",
                             "refined",
                             "--benchmark-keypoint-v2-raw",
                             "raw.zarr",
                             "raw-run",
                             "raw-digest",
                             "--benchmark-keypoint-v2-quality",
                             "quality.zarr",
                             "quality-run",
                             "quality-digest",
                             "--benchmark-keypoint-v2-refined",
                             "refined.zarr",
                             "refined-run",
                             "refined-digest",
                             "--benchmark-keypoint-v2-body-frame",
                             "body.zarr",
                             "body-run",
                             "body-digest",
                             "--show-subject-masks",
                             "--perf-log",
                             "perf.jsonl",
                             "--mask-perf-log",
                             "mask.jsonl",
                             "--playback-trace-log",
                             "playback.jsonl",
                             "--frame-sync-trace-log",
                             "sync.jsonl",
                             "--playback-smoke",
                             "20:90",
                             "--playback-smoke-timeout",
                             "8.5",
                             "--mask-perf-sample-every",
                             "4",
                             "--no-mask-perf-log",
                             "--swap-interval",
                             "0",
                             "--frame-cap-fps",
                             "144"});
  CHECK(without_clip.ok);
  const auto &options = without_clip.options;
  CHECK(options.zarr_override_path == "analysis.zarr");
  CHECK(options.subject_shape_run == "shape");
  CHECK(options.refined_subject_mask_run == "masks");
  CHECK(options.refined_subject_mask_storage == "dense");
  CHECK(options.tail_kinematics_run == "tail");
  CHECK(options.eye_angle_run == "eyes");
  CHECK(options.stimulus_run == "stimulus");
  CHECK(options.refined_detection_run == "refined");
  CHECK(options.allow_selector_ineligible_refined_detection);
  CHECK(options.keypoint_v2_raw.complete());
  CHECK(options.keypoint_v2_quality.complete());
  CHECK(options.keypoint_v2_refined.complete());
  CHECK(options.keypoint_v2_body_frame.complete());
  CHECK(options.allow_selector_ineligible_keypoints);
  CHECK(options.show_eye_masks);
  CHECK(options.perf_log_path == "perf.jsonl");
  CHECK(!options.mask_perf_log_enabled);
  CHECK(options.mask_perf_sample_every == 4);
  CHECK(options.playback_smoke.enabled);
  CHECK(options.playback_smoke.start_frame == 20);
  CHECK(options.playback_smoke.end_frame == 90);
  CHECK(options.playback_smoke.timeout_s == 8.5);
  CHECK(options.swap_interval == 0);
  CHECK(options.frame_cap_fps == 144.0);
  return true;
}

bool testUiReferenceAndFramePacing() {
  auto ui = parse({"redgui", "--ui-reference-state", "stimulus-overlay",
                   "--ui-reference-frame", "52", "--ui-reference-ready-file",
                   "ready.json", "--ui-reference-timeout", "15"});
  CHECK(ui.ok);
  CHECK(ui.options.ui_reference.enabled);
  CHECK(ui.options.ui_reference.state == UiReferenceState::StimulusOverlay);
  CHECK(ui.options.ui_reference.target_frame == 52);
  CHECK(ui.options.ui_reference.ready_file == "ready.json");
  CHECK(ui.options.ui_reference.timeout_s == 15.0);

  auto pacing = parse({"redgui", "--swap-interval", "0"});
  CHECK(pacing.ok);
  CHECK(pacing.options.frame_cap_fps == 60.0);
  CHECK(pacing.diagnostics.size() == 1);
  CHECK(pacing.diagnostics.front().find("capping at 60 FPS") !=
        std::string::npos);
  return true;
}

bool testValidationFailures() {
  struct Case {
    std::vector<std::string> arguments;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {{"redgui", "--zarr"}, "Missing value for --zarr"},
      {{"redgui", "--playback-smoke", "9:2"}, "Invalid --playback-smoke"},
      {{"redgui", "--playback-smoke", "0:2", "--clipped-boundary-smoke", "0:2"},
       "cannot be used in the same run"},
      {{"redgui", "--detection-run", "raw", "--refined-detection-run",
        "refined"},
       "mutually exclusive"},
      {{"redgui", "--benchmark-keypoint-v2-raw", "a", "r", "d"},
       "require complete raw"},
      {{"redgui", "--ui-reference-frame", "4"},
       "requires --ui-reference-state"},
      {{"redgui", "--ui-reference-state", "workspace", "--ui-reference-frame",
        "4", "--ui-reference-ready-file", "ready", "--playback-smoke", "0:2"},
       "cannot run with playback"},
      {{"redgui", "--swap-interval", "2"}, "expected 0 or 1"},
      {{"redgui", "--frame-cap-fps", "nan"}, "non-negative number"},
  };
  for (const auto &test_case : cases) {
    const auto result = parse(test_case.arguments);
    CHECK(!result.ok);
    CHECK(result.error.find(test_case.expected) != std::string::npos);
  }
  return true;
}

bool testRecordingPrecedenceAndClipIndex() {
  const auto root = temporaryRoot();
  const auto recording = root / "recording";
  const auto clip_index = root / "recording_clip_index.json";
  std::filesystem::create_directories(recording);
  {
    std::ofstream stream(clip_index);
    stream << "{}";
  }

  auto precedence =
      parse({"redgui", "--recording", recording.string(), "--zarr",
             "ignored.zarr", "--recording-clip-index", clip_index.string()});
  CHECK(precedence.ok);
  CHECK(precedence.options.recording_path == recording.string());
  CHECK(precedence.options.zarr_override_path.empty());
  CHECK(precedence.options.recording_clip_index_path == clip_index.string());
  CHECK(precedence.diagnostics.size() == 1);

  auto invalid_recording =
      parse({"redgui", "--recording", (root / "missing").string()});
  CHECK(invalid_recording.ok);
  CHECK(invalid_recording.options.recording_path.empty());
  CHECK(invalid_recording.diagnostics.size() == 1);

  auto orphan_clip =
      parse({"redgui", "--recording-clip-index", clip_index.string()});
  CHECK(!orphan_clip.ok);
  CHECK(orphan_clip.error.find("requires --zarr or --recording") !=
        std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(root, error);
  return true;
}

bool testCudaConfigurationParsing() {
  using crimson::platform::nvidia::loadSavedCudaDeviceIndex;
  using crimson::platform::nvidia::parseCudaDeviceIndexString;
  CHECK(parseCudaDeviceIndexString("0") == 0);
  CHECK(parseCudaDeviceIndexString("12") == 12);
  CHECK(!parseCudaDeviceIndexString("-1").has_value());
  CHECK(!parseCudaDeviceIndexString("2x").has_value());
  CHECK(!parseCudaDeviceIndexString("").has_value());

  const auto root = temporaryRoot();
  std::filesystem::create_directories(root);
  const auto config = root / "cuda_device.json";
  {
    std::ofstream stream(config);
    stream << R"({"selected_cuda_device_index": 7})";
  }
  std::string source;
  CHECK(loadSavedCudaDeviceIndex(config, source) == 7);
  CHECK(source == config.string());

  {
    std::ofstream stream(config);
    stream << R"({"selected_cuda_device_index": -2})";
  }
  CHECK(!loadSavedCudaDeviceIndex(config, source).has_value());
  std::error_code error;
  std::filesystem::remove_all(root, error);
  return true;
}

} // namespace

int main() {
  if (!testDefaultsAndUnknownArguments() || !testCompleteOptionSurface() ||
      !testUiReferenceAndFramePacing() || !testValidationFailures() ||
      !testRecordingPrecedenceAndClipIndex() ||
      !testCudaConfigurationParsing()) {
    return EXIT_FAILURE;
  }
  std::cout << "nvidia_launch_options_tests: PASS\n";
  return EXIT_SUCCESS;
}
