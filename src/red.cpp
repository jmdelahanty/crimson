#include "IconsForkAwesome.h"
#include "Logger.h"
#include "app/frame_inspect_controller.h"
#include "archive_open_coordinator.h"
#include "camera.h"
#include "chained_crop_image_provider.h"
#include "data_access_diagnostics.h"
#include "debug_flags.h"
#include "decode_debug_workflow.h"
#include "diagnostic_report.h"
#include "filesystem"
#include "frame_presentation.h"
#include "frame_selection.h"
#include "global.h"
#include "gui.h"
#include "gui/analysis_timeline_window.h"
#include "gui/auxiliary_windows.h"
#include "gui/camera_view_frame_context_builder.h"
#include "gui/camera_view_manual_keypoint_input.h"
#include "gui/camera_view_overlay_renderer.h"
#include "gui/camera_view_presenter.h"
#include "gui/camera_view_transport_controls.h"
#include "gui/camera_view_window.h"
#include "gui/camera_view_window_identity.h"
#include "gui/canonical_timeline_session.h"
#include "gui/canonical_overlay_session.h"
#include "gui/canonical_overlay_presentation.h"
#include "gui/canonical_timeline_window.h"
#include "gui/crop_preview_window.h"
#include "gui/diagnostics_window.h"
#include "gui/file_browser_window.h"
#include "gui/frame_buffer_window.h"
#include "gui/frame_debug_window.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "gui/keypoints_window.h"
#include "gui/labeling_tool_window.h"
#include "gui/labeling_tool_workflow.h"
#include "gui/quality_timeline_session.h"
#include "gui/quality_timeline_window.h"
#include "gui/refined_keypoint_review_window.h"
#include "gui/refined_keypoint_write_workflow.h"
#include "gui/stimulus_event_timeline_window.h"
#include "gui/stimulus_playback_windows.h"
#include "gui_interpolation.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_semantic_snapshot.h"
#include "implot.h"
#include "legacy_labeling_state.h"
#include "live_crop_image_provider.h"
#include "loading_progress.h"
#include "manual_detect_payload_preview.h"
#include "media_session_loader.h"
#include "perf_logging.h"
#include "platform/nvidia/nvidia_camera_frame_data_adapter.h"
#include "platform/nvidia/nvidia_clipped_media_coordinator.h"
#include "platform/nvidia/nvidia_diagnostics_session.h"
#include "platform/nvidia/nvidia_detection_repository.h"
#include "platform/nvidia/nvidia_detection_presentation_adapter.h"
#include "platform/nvidia/nvidia_frame_buffer_adapter.h"
#include "platform/nvidia/nvidia_frame_inspect_adapter.h"
#include "platform/nvidia/nvidia_gl_diagnostics.h"
#include "platform/nvidia/nvidia_launch_options.h"
#include "platform/nvidia/nvidia_playback_diagnostics_adapter.h"
#include "platform/nvidia/nvidia_playback_trace_model.h"
#include "platform/nvidia/nvidia_refined_keypoint_write_session.h"
#include "playback_diagnostics.h"
#include "playback_session_controller.h"
#include "recording_open_workflow.h"
#include "refined_keypoint_repository.h"
#include "render.h"
#include "review_frame_index.h"
#include "session_lifecycle.h"
#include "skeleton.h"
#include "stimulus_open_coordinator.h"
#include "ui_reference_capture.h"
#include "ui_reference_contract.h"
#include "ui_reference_scene_evidence.h"
#include "utils.h"
#include "workspace_state.h"
#include "yolo_detection.h"
#include "zarr/chaser_distance_polar_legacy_repository.h"
#include "zarr/legacy_detection_repository.h"
#include "zarr/legacy_keypoint_overlay_repository.h"
#include "zarr/legacy_stimulus_repository.h"
#include "zarr/legacy_subject_mask_overlay_repository.h"
#include "zarr/stimulus_context_timeline_legacy_repository.h"
#include "zarr_loader.h"
#include "zarr_persisted_crop_provider.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

simplelogger::Logger *logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

#include "ui_path_config.h"
#include "windows_crash_dump.h"
#include "zarr_bbox_edit.h"

std::vector<std::mutex> g_mutexes(MAX_VIEWS);
std::vector<std::condition_variable> g_cvs(MAX_VIEWS);
std::vector<bool> g_ready(MAX_VIEWS);
std::vector<std::vector<cv::Rect>> yolo_boxes(MAX_VIEWS);
std::vector<std::vector<std::string>> yolo_labels(MAX_VIEWS);
std::vector<std::vector<int>> yolo_classid(MAX_VIEWS);
std::vector<unsigned char *> yolo_input_frames_rgba(MAX_VIEWS);
std::unordered_map<std::string, std::atomic<bool>> window_need_decoding;
std::unordered_map<std::string, std::atomic<int>> latest_decoded_frame;
std::unordered_map<std::string, std::shared_ptr<DecoderPerfSample>>
    decoder_perf_samples;
std::mutex g_seek_info_mutex;
std::mutex g_decoder_perf_mutex;

// Global variables
bool show_interpolation_debug = false;
std::vector<crimson::zarr::DetectionDataset> detection_dataset_ids;
std::vector<std::string> detection_dataset_labels;
int detection_dataset_choice = 0;
ZarrBBoxEditState g_zarr_bbox_edit_state;

#include "review_frame_state.h"

void refreshDetectionDatasetOptions(
    crimson::zarr::DetectionRepository &repository) {
  detection_dataset_ids.clear();
  detection_dataset_labels.clear();
  detection_dataset_choice = 0;
  const auto options = repository.availableDatasets();
  const auto active = repository.descriptor().active_dataset;
  for (size_t i = 0; i < options.size(); ++i) {
    detection_dataset_ids.push_back(options[i].dataset);
    detection_dataset_labels.push_back(options[i].label);
    if (options[i].dataset == active) {
      detection_dataset_choice = static_cast<int>(i);
    }
  }
}

#include "stimulus_playback.h"

static StimulusPlayback stimulus_player;

namespace {

using json = nlohmann::json;

namespace nvidia_trace = crimson::platform::nvidia::trace;
namespace nvidia_diagnostics = crimson::platform::nvidia::diagnostics;
using crimson::platform::nvidia::UiReferenceState;
using crimson::platform::nvidia::uiReferenceStateName;

struct ClippedBoundarySmokeConfig {
  bool enabled = false;
  int start_frame = -1;
  int end_frame = -1;
  std::chrono::steady_clock::time_point start_time{};
  bool started = false;
  bool completed = false;
  bool endpoint_seek_requested = false;
};

struct PlaybackSmokeConfig {
  bool enabled = false;
  int start_frame = -1;
  int end_frame = -1;
  double timeout_s = 20.0;
  double warmup_s = 0.0;
  std::chrono::steady_clock::time_point start_time{};
  bool started = false;
  bool completed = false;
  bool playback_started = false;
  int presented_count = 0;
  int last_presented_frame = -1;
  int max_presented_frame = -1;
  int last_presented_slot = -1;
  int last_view_idx = -1;
};

struct UiReferenceConfig {
  bool enabled = false;
  bool state_set = false;
  bool frame_set = false;
  bool ready_file_set = false;
  UiReferenceState state = UiReferenceState::Workspace;
  int target_frame = -1;
  std::filesystem::path ready_file;
  double timeout_s = 60.0;
  bool analysis_state_applied = false;
  bool exact_presented_this_frame = false;
  int presented_frame = -1;
  int presented_slot = -1;
  int view_idx = -1;
  int bbox_query_frame = -1;
  int target_stimulus_frame = -1;
  int presented_stimulus_frame = -1;
  bool crop_ready = false;
  int crop_source_frame = -1;
  std::string crop_source_label;
  std::filesystem::path rendered_image_file;
  int rendered_image_width = 0;
  int rendered_image_height = 0;
  int camera_buffer_valid = 0;
  int camera_buffer_capacity = 0;
  int stimulus_buffer_valid = 0;
  int stimulus_buffer_capacity = 0;
};

double durationMs(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

std::optional<json> readBoundedJsonObject(const std::filesystem::path &path,
                                          size_t maximum_bytes,
                                          std::string *error) {
  std::error_code size_error;
  const auto bytes = std::filesystem::file_size(path, size_error);
  if (size_error || bytes > maximum_bytes) {
    if (error != nullptr) {
      *error = size_error ? "Cannot inspect " + path.string() + ": " +
                                size_error.message()
                          : "Metadata exceeds bounded inspection limit: " +
                                path.string();
    }
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error != nullptr) {
      *error = "Cannot read metadata: " + path.string();
    }
    return std::nullopt;
  }
  try {
    json value;
    input >> value;
    if (!value.is_object()) {
      if (error != nullptr) {
        *error = "Metadata is not a JSON object: " + path.string();
      }
      return std::nullopt;
    }
    return value;
  } catch (const json::exception &exception) {
    if (error != nullptr) {
      *error = "Invalid JSON metadata " + path.string() + ": " +
               exception.what();
    }
    return std::nullopt;
  }
}

bool isRollingRecordingClipIndex(const std::filesystem::path &index_path,
                                 std::string *error) {
  constexpr size_t kMaximumClipIndexBytes = 8 * 1024 * 1024;
  const auto root =
      readBoundedJsonObject(index_path, kMaximumClipIndexBytes, error);
  if (!root) {
    return false;
  }
  try {
    return root->value("schema_id", "") ==
               "palette.orange_external_ipc_recording_clip_index.v1" &&
           root->value("schema_version", 0) == 1 &&
           root->value("mode", "") == "rolling_clips" &&
           root->value("source_layout", "") == "rolling_clips";
  } catch (const json::exception &exception) {
    if (error != nullptr) {
      *error = "Recording clip index schema fields are invalid: " +
               std::string(exception.what());
    }
    return false;
  }
}

std::optional<std::string> resolveAuthoritativeCanonicalRawRun(
    const std::filesystem::path &archive_path,
    const std::string &loader_selected_run, std::string *error) {
  constexpr size_t kMaximumDetectRunsMetadataBytes = 1024 * 1024;
  const auto metadata = readBoundedJsonObject(
      archive_path / "detect_runs" / "zarr.json",
      kMaximumDetectRunsMetadataBytes, error);
  if (!metadata) {
    return std::nullopt;
  }
  try {
    const auto attributes = metadata->find("attributes");
    if (metadata->value("zarr_format", 0) != 3 ||
        metadata->value("node_type", "") != "group" ||
        attributes == metadata->end() || !attributes->is_object()) {
      if (error != nullptr) {
        *error = "detect_runs metadata is not a Zarr v3 group";
      }
      return std::nullopt;
    }
    const auto latest = attributes->find("latest");
    const auto latest_complete = attributes->find("latest_complete");
    if (latest == attributes->end() || !latest->is_string() ||
        latest->get_ref<const std::string &>().empty() ||
        latest_complete == attributes->end() ||
        !latest_complete->is_string() ||
        latest_complete->get_ref<const std::string &>().empty()) {
      if (error != nullptr) {
        *error = "detect_runs latest/latest_complete selection is missing";
      }
      return std::nullopt;
    }
    const std::string selected = latest->get<std::string>();
    if (latest_complete->get_ref<const std::string &>() != selected) {
      if (error != nullptr) {
        *error = "detect_runs latest and latest_complete disagree";
      }
      return std::nullopt;
    }
    if (!loader_selected_run.empty() && loader_selected_run != selected) {
      if (error != nullptr) {
        *error = "Legacy and canonical raw-run selectors disagree";
      }
      return std::nullopt;
    }
    return selected;
  } catch (const json::exception &exception) {
    if (error != nullptr) {
      *error = "detect_runs selection metadata is invalid: " +
               std::string(exception.what());
    }
    return std::nullopt;
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::filesystem::path argv0_path = (argc > 0) ? argv[0] : "";
  InstallWindowsCrashHandler(argv0_path);
  std::error_code cwd_error;
  const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);

  auto launch_result =
      crimson::platform::nvidia::parseNvidiaLaunchOptions(argc, argv);
  for (const auto &diagnostic : launch_result.diagnostics) {
    std::cerr << diagnostic << std::endl;
  }
  if (!launch_result.ok) {
    std::cerr << launch_result.error << std::endl;
    return 1;
  }
  const auto launch_options = std::move(launch_result.options);
  const auto &cli_zarr_override_path = launch_options.zarr_override_path;
  const auto &cli_recording_path = launch_options.recording_path;
  const auto &cli_recording_clip_index_path =
      launch_options.recording_clip_index_path;
  const auto &cli_subject_shape_run = launch_options.subject_shape_run;
  const auto &cli_refined_subject_mask_run =
      launch_options.refined_subject_mask_run;
  const auto &cli_refined_subject_mask_storage =
      launch_options.refined_subject_mask_storage;
  const auto &cli_tail_kinematics_run = launch_options.tail_kinematics_run;
  const auto &cli_eye_angle_run = launch_options.eye_angle_run;
  const auto &cli_stimulus_run = launch_options.stimulus_run;
  const auto &cli_detection_run = launch_options.detection_run;
  const auto &cli_refined_detection_run = launch_options.refined_detection_run;
  const bool cli_allow_selector_ineligible_refined_detection =
      launch_options.allow_selector_ineligible_refined_detection;
  const auto &cli_keypoint_v2_raw = launch_options.keypoint_v2_raw;
  const auto &cli_keypoint_v2_quality = launch_options.keypoint_v2_quality;
  const auto &cli_keypoint_v2_refined = launch_options.keypoint_v2_refined;
  const auto &cli_keypoint_v2_body_frame =
      launch_options.keypoint_v2_body_frame;
  const bool cli_allow_selector_ineligible_keypoints =
      launch_options.allow_selector_ineligible_keypoints;
  const auto &cli_perf_log_path = launch_options.perf_log_path;
  const auto &cli_mask_perf_log_path = launch_options.mask_perf_log_path;
  const auto &cli_playback_trace_log_path =
      launch_options.playback_trace_log_path;
  const auto &cli_frame_sync_trace_log_path =
      launch_options.frame_sync_trace_log_path;
  const int cli_swap_interval = launch_options.swap_interval;
  const int cli_mask_perf_sample_every = launch_options.mask_perf_sample_every;
  const double cli_frame_cap_fps = launch_options.frame_cap_fps;
  const bool mask_perf_log_enabled = launch_options.mask_perf_log_enabled;
  const bool cli_show_eye_masks = launch_options.show_eye_masks;

  PlaybackSmokeConfig playback_smoke;
  playback_smoke.enabled = launch_options.playback_smoke.enabled;
  playback_smoke.start_frame = launch_options.playback_smoke.start_frame;
  playback_smoke.end_frame = launch_options.playback_smoke.end_frame;
  playback_smoke.timeout_s = launch_options.playback_smoke.timeout_s;
  playback_smoke.warmup_s = launch_options.playback_smoke.warmup_s;
  ClippedBoundarySmokeConfig clipped_boundary_smoke;
  clipped_boundary_smoke.enabled =
      launch_options.clipped_boundary_smoke.enabled;
  clipped_boundary_smoke.start_frame =
      launch_options.clipped_boundary_smoke.start_frame;
  clipped_boundary_smoke.end_frame =
      launch_options.clipped_boundary_smoke.end_frame;
  UiReferenceConfig ui_reference;
  ui_reference.enabled = launch_options.ui_reference.enabled;
  ui_reference.state_set = launch_options.ui_reference.state_set;
  ui_reference.frame_set = launch_options.ui_reference.frame_set;
  ui_reference.ready_file_set = launch_options.ui_reference.ready_file_set;
  ui_reference.state = launch_options.ui_reference.state;
  ui_reference.target_frame = launch_options.ui_reference.target_frame;
  ui_reference.ready_file = launch_options.ui_reference.ready_file;
  ui_reference.timeout_s = launch_options.ui_reference.timeout_seconds;
  crimson::ui_reference::CaptureCoordinator ui_reference_capture(
      {60, ui_reference.timeout_s});
  crimson::ui::SemanticSnapshot ui_semantic_snapshot;
  int app_exit_code = 0;
  gx_context *window = new gx_context();
  *window = gx_context{};
  window->swap_interval = cli_swap_interval;
  window->width = 1920;
  window->height = 1080;
  window->render_target_title = (char *)malloc(100); // window title
  window->glsl_version = (char *)malloc(100);

  const int kCudaDeviceIndex =
      crimson::platform::nvidia::resolveCudaDeviceIndex();
  render_initialize_target(window, kCudaDeviceIndex, argv0_path);
  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(),
                                         ui_reference.enabled);

  render_scene *scene = new render_scene();

  std::string root_dir;
  std::string skeleton_dir;
  std::vector<std::string> camera_names;
  std::vector<CameraParams> camera_params;
  std::vector<std::thread> decoder_threads;
  std::vector<std::unique_ptr<FFmpegDemuxer>> demuxers;

  // Zarr loading
  ZarrDetectionLoader zarr_loader;
  crimson::zarr::LegacyDetectionRepository detection_repository(zarr_loader);
  crimson::zarr::LegacyKeypointOverlayRepository keypoint_repository(
      zarr_loader);
  auto analysis_data_scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(64, 4, 1, 1);
  zarr_loader.setDataAccessScheduler(analysis_data_scheduler);
  crimson::platform::nvidia::NvidiaDetectionRepository
      canonical_detection_repository(analysis_data_scheduler);
  crimson::platform::nvidia::CameraFrameDataAdapter
      legacy_camera_frame_data_adapter(
      detection_repository, keypoint_repository,
      crimson::platform::nvidia::CameraFrameDataCallbacks{
          [&zarr_loader](size_t frame, size_t lookahead_frames) {
            zarr_loader.requestEyeMaskCacheForFrame(frame, lookahead_frames);
          },
          [&zarr_loader](size_t frame, bool include_eye_masks,
                         bool include_subject_shapes,
                         bool allow_blocking_eye_mask_load) {
            return zarr_loader.getRawDetections(
                frame, false, include_eye_masks, include_subject_shapes, false,
                allow_blocking_eye_mask_load);
          },
      });
  crimson::platform::nvidia::CameraFrameDataAdapter
      canonical_camera_frame_data_adapter(canonical_detection_repository,
                                          keypoint_repository);
  crimson::zarr::LegacyStimulusRepository stimulus_repository(zarr_loader);
  crimson::gui::QualityTimelineSession quality_timeline_session(
      analysis_data_scheduler);
  crimson::gui::CanonicalTimelineSession canonical_timeline_session(
      analysis_data_scheduler);
  crimson::gui::CanonicalOverlaySession canonical_overlay_session(analysis_data_scheduler);
  crimson::gui::CanonicalOverlaySnapshot last_canonical_overlay_snapshot;
  int64_t canonical_overlay_draw_frame = -1;
  int canonical_keypoint_draw_count = 0;
  int canonical_heading_draw_count = 0;
  int canonical_shape_draw_count = 0;
  int canonical_expected_shape_draw_count = 0;
  int canonical_expected_mask_draw_count = 0;
  crimson::gui::DetectionQualityTimelineControls
      detection_quality_timeline_controls;
  crimson::gui::KeypointQualityTimelineControls
      keypoint_quality_timeline_controls;
  std::unique_ptr<crimson::polar::ChaserDistancePolarRepository>
      chaser_distance_polar_repository;
  std::unique_ptr<crimson::timeline::StimulusContextTimelineRepository>
      stimulus_context_timeline_repository;
  std::shared_ptr<const crimson::timeline::StimulusContextTimelineSnapshot>
      stimulus_context_timeline;
  if (!cli_subject_shape_run.empty()) {
    zarr_loader.setRequestedSubjectShapeRunName(cli_subject_shape_run);
  }
  if (!cli_refined_subject_mask_run.empty()) {
    zarr_loader.setRequestedRefinedSubjectMaskRunName(
        cli_refined_subject_mask_run);
  }
  if (!cli_refined_subject_mask_storage.empty()) {
    zarr_loader.setRequestedRefinedSubjectMaskStorage(
        cli_refined_subject_mask_storage);
  }
  if (!cli_tail_kinematics_run.empty()) {
    zarr_loader.setRequestedTailKinematicsRunName(cli_tail_kinematics_run);
  }
  if (!cli_eye_angle_run.empty()) {
    zarr_loader.setRequestedEyeAngleRunName(cli_eye_angle_run);
  }
  if (!cli_stimulus_run.empty()) {
    zarr_loader.setRequestedStimulusRunName(cli_stimulus_run);
  }
  bool zarr_loaded = false;

  DecoderContext *dc_context = new DecoderContext();
  *dc_context = DecoderContext{};
  dc_context->decoding_flag = false;
  dc_context->stop_flag = false;
  dc_context->total_num_frame = int(INT_MAX);
  dc_context->estimated_num_frames = 0;
  dc_context->gpu_index = kCudaDeviceIndex;
  dc_context->seek_interval = 250;

  // gui states, todo: bundle this later
  crimson::workspace::WorkspaceState workspace_state;
  bool video_loaded = false;
  bool cpu_buffer_toggle = true;
  bool show_keypoint_markers = true;
  bool show_heading_arrows = true;
  bool show_eye_masks = cli_show_eye_masks;
  bool show_subject_body_mask = true;
  bool show_eye_left_mask = true;
  bool show_eye_right_mask = true;
  bool show_swim_bladder_mask = true;
  bool show_eye_direction_beams = true;
  bool show_eye_gaze_rays = true;
  bool show_eye_angle_arcs = true;
  bool show_eye_angle_labels = true;
  bool show_movement_trail = true;
  float movement_trail_seconds = 2.0f;
  bool movement_trail_valid_samples_only = true;
  CameraViewStimulusInsetOptions stimulus_inset_options;
  CameraViewChaserDistancePolarInsetOptions chaser_distance_polar_inset_options;
  bool show_stimulus_debug_windows = false;
  CameraViewMaskOverlayMode mask_overlay_mode =
      CameraViewMaskOverlayMode::Review;
  CameraViewSubjectShapeOverlayOptions subject_shape_overlay_options;
  CameraViewTailKinematicsOverlayOptions tail_kinematics_overlay_options;
  int current_frame_num = 0;
  std::vector<std::string> imgs_names;

  constexpr bool kHeadingDebugLoggingEnabled = false;
  constexpr int kHeadingDebugMaxMessages = 400;
  constexpr bool kPlaybackDebugLoggingEnabled = false;
  int heading_debug_message_count = 0;
  int heading_debug_draw_log_count = 0;
  int heading_debug_entry_log_count = 0;
  int heading_debug_last_frame_logged = -1;
  bool heading_debug_logged_toggle_disabled = false;
  bool heading_debug_logged_no_data = false;
  bool heading_debug_logged_interpolated = false;
  auto headingDebugLog = [&](const std::string &message) {
    if (!kHeadingDebugLoggingEnabled) {
      return;
    }
    if (heading_debug_message_count >= kHeadingDebugMaxMessages) {
      if (heading_debug_message_count == kHeadingDebugMaxMessages) {
        std::cout
            << "[HEADING_DEBUG] Log limit reached, suppressing further messages"
            << std::endl;
      }
      heading_debug_message_count++;
      return;
    }
    std::cout << "[HEADING_DEBUG] " << message << std::endl;
    heading_debug_message_count++;
  };

  constexpr bool kEyeMaskDebugLoggingEnabled = false;
  constexpr int kEyeMaskDebugMaxMessages = 100;
  int eye_mask_debug_message_count = 0;
  int eye_mask_debug_entry_log_count = 0;
  int eye_mask_debug_draw_log_count = 0;
  int eye_mask_debug_last_frame_logged = -1;
  bool eye_mask_debug_logged_toggle_disabled = false;
  bool eye_mask_debug_logged_no_data = false;
  auto eyeMaskDebugLog = [&](const std::string &message) {
    if (!kEyeMaskDebugLoggingEnabled) {
      return;
    }
    if (eye_mask_debug_message_count >= kEyeMaskDebugMaxMessages) {
      if (eye_mask_debug_message_count == kEyeMaskDebugMaxMessages) {
        std::cout << "[EYE_MASK_DEBUG] Log limit reached, suppressing further "
                     "messages"
                  << std::endl;
      }
      eye_mask_debug_message_count++;
      return;
    }
    std::cout << "[EYE_MASK_DEBUG] " << message << std::endl;
    eye_mask_debug_message_count++;
  };

  // for labeling
  LegacyLabelingState legacy_labeling_state;

  // others
  UiPathConfig ui_path_config = LoadUiPathConfig(cwd, argv0_path);
  std::string start_folder_name = ui_path_config.default_start_path;
  if (start_folder_name.empty() || !IsDirectoryNoThrow(start_folder_name)) {
    start_folder_name = cwd.string();
  }
  const std::filesystem::path default_buffer_dump_root =
      GetDefaultCrimsonBufferDumpRoot();
  if (!ui_path_config.loaded_from.empty()) {
    std::cout << "[UIPathConfig] Loaded: " << ui_path_config.loaded_from
              << std::endl;
  } else {
    std::cout << "[UIPathConfig] Using default start path: "
              << start_folder_name << std::endl;
  }
  ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
  ImGuiIO &io = ImGui::GetIO();

  ImPlotStyle &style = ImPlot::GetStyle();
  ImVec4 *colors = style.Colors;
  colors[ImPlotCol_Crosshairs] = ImVec4(0.3f, 0.10f, 0.64f, 1.00f);

  bool yolo_detection = false;
  std::vector<std::thread> yolo_threads;
  yolo_param yolo_setting = yolo_param();
  int label_buffer_size = 100;
  int playback_preview_scale_mode = 0;
  int playback_renderer_mode = 1;
  int stimulus_buffer_size = 12;
  bool stimulus_use_cpu_buffer = false;
#ifdef _WIN32
  bool stimulus_use_software_decode = true;
#else
  bool stimulus_use_software_decode = false;
#endif
  uint64_t stimulus_catchup_seek_generation = 1;
  StimulusPlaybackPresentationState stimulus_playback_presentation_state;
  bool show_help_window = false;
  std::vector<bool> is_view_focused;
  bool input_is_imgs = false;
  bool show_error = false;
  std::string error_message;
  std::unordered_map<std::string, bool> window_was_decoding;
  double inst_speed = 1.0;
  double video_fps = 60.0f;
  float set_playback_speed = 1.0f;
  PlaybackState ps;
  crimson::zarr::LegacySubjectMaskOverlayRepository subject_mask_repository(
      zarr_loader, [&ps] { return !ps.play_video; });
  crimson::playback::PlaybackTransportController playback_transport;
  SeekProgress seek_progress;
  std::string frame_sync_debug_line;
  int frame_sync_valid_slots = -1;
  int frame_sync_empty_slots = -1;
  int frame_sync_latest_decoded = -1;
  int frame_sync_recording_remaining = -1;
  int frame_sync_recording_total = -1;
  nvidia_diagnostics::NvidiaDiagnosticsSession diagnostics_session;
  diagnostics_session.open(
      nvidia_diagnostics::NvidiaDiagnosticsOptions{
          default_buffer_dump_root,
          cli_perf_log_path,
          mask_perf_log_enabled,
          cli_mask_perf_log_path,
          cli_mask_perf_sample_every,
          cli_playback_trace_log_path,
          cli_frame_sync_trace_log_path,
          nvidia_diagnostics::captureNvidiaDiagnosticsEnvironment(),
      },
      std::cout, std::cerr);
  auto &perf_log_writer = diagnostics_session.perfLogWriter();
  auto &mask_perf_log_writer = diagnostics_session.maskPerfLogWriter();
  auto &clipped_frame_trace_stats =
      diagnostics_session.clippedFrameTraceStats();
  auto &clipped_texture_dump = diagnostics_session.clippedTextureDump();
  const bool clipped_rebase_before_play =
      crimson_env_flag_enabled("CRIMSON_CLIPPED_REBASE_BEFORE_PLAY");
  constexpr auto kPerfLogSamplePeriod = std::chrono::milliseconds(250);
  constexpr uint64_t kPlaybackWarmupPerfFrames = 120;
  constexpr uint64_t kPlaybackWarmupPerfSampleStride = 2;
  std::unordered_map<std::string, nvidia_trace::FrameSyncTraceState>
      frame_sync_trace_last_by_camera;
  uint64_t mask_perf_sample_index = 0;
  int perf_playback_start_frame = -1;
  uint64_t perf_frames_since_playback_start = 0;
  std::string perf_playback_resume_path = resumePathName(ResumePath::None);
  int perf_playback_resume_target_frame = -1;

  window_need_decoding[stimulus_player.window_name].store(false);
  latest_decoded_frame[stimulus_player.window_name].store(-1);
  window_was_decoding[stimulus_player.window_name] = false;
  PaletteClippedMediaState clipped_media_state;
  crimson::session::SessionLifecycle session_lifecycle;
  crimson::loading::LoadingProgressTracker session_loading_progress;
  crimson::session::RecordingOpenWorkflowController recording_open_workflow(
      session_lifecycle, session_loading_progress);
  crimson::playback::FramePresentationTracker camera_presentation_tracker;
  MediaSessionLoader media_session_loader(MediaSessionLoaderContext{
      scene,
      dc_context,
      &zarr_loader,
      &stimulus_repository,
      &stimulus_player,
      &ps,
      &playback_transport,
      &root_dir,
      &skeleton_dir,
      &camera_names,
      &imgs_names,
      &camera_params,
      &decoder_threads,
      &demuxers,
      &is_view_focused,
      &window_need_decoding,
      &window_was_decoding,
      &clipped_media_state,
      &video_loaded,
      &zarr_loaded,
      &input_is_imgs,
      &show_error,
      &error_message,
      &label_buffer_size,
      &stimulus_buffer_size,
      &stimulus_use_cpu_buffer,
      &stimulus_use_software_decode,
      &video_fps,
      &recording_open_workflow,
      kCudaDeviceIndex,
  });
  bool canonical_detection_route = false;
  uint64_t canonical_detection_session_generation =
      std::numeric_limits<uint64_t>::max();
  std::string canonical_detection_archive;
  std::string canonical_detection_index;
  std::string canonical_detection_run;
  std::string canonical_detection_expected_recording_identity;
  size_t canonical_detection_expected_frames = 0;
  size_t canonical_detection_expected_width = 0;
  size_t canonical_detection_expected_height = 0;
  uint64_t canonical_detection_seen_seek_requests = 0;
  int canonical_detection_last_requested_frame = -1;
  std::string canonical_detection_last_request_error;
  uint64_t canonical_overlay_seen_seek_requests = 0;
  auto requestCanonicalOverlayFrame = [&](int frame, bool keypoints, bool masks, bool shapes) {
    const uint64_t seek_requests = playback_transport.seekCoordinator().metrics().requests;
    const bool accepted = canonical_overlay_session.requestFrame(
        frame, keypoints, masks, shapes,
        seek_requests != canonical_overlay_seen_seek_requests,
        {true, ps.play_video ? crimson::gui::CanonicalOverlayPlaybackDirection::Forward
                            : crimson::gui::CanonicalOverlayPlaybackDirection::Paused,
         video_fps, playback_transport.playbackRate()});
    // A partial product rejection must not repeatedly invalidate products
    // which already accepted this seek. New opens have their own source epoch.
    canonical_overlay_seen_seek_requests = seek_requests;
    return accepted;
  };
  auto requestCanonicalDetectionFrame = [&](int frame) {
    if (!canonical_detection_route || frame < 0 ||
        canonical_detection_repository.state() !=
            crimson::platform::nvidia::NvidiaDetectionState::Ready) {
      return false;
    }
    const uint64_t seek_requests =
        playback_transport.seekCoordinator().metrics().requests;
    const bool seek_generation_changed =
        seek_requests != canonical_detection_seen_seek_requests;
    const bool request_discontinuity =
        seek_generation_changed ||
        canonical_detection_last_requested_frame < 0 ||
        std::abs(frame - canonical_detection_last_requested_frame) > 70;
    std::string request_error;
    const bool accepted = canonical_detection_repository.requestPresentedFrame(
        frame, request_discontinuity, &request_error);
    if (!accepted && !request_error.empty() &&
        request_error != canonical_detection_last_request_error) {
      canonical_detection_last_request_error = request_error;
      std::cerr << "[NvidiaDetection] frame_request=failed frame=" << frame
                << " seek_requests=" << seek_requests
                << " error=" << request_error << std::endl;
    }
    canonical_detection_seen_seek_requests = seek_requests;
    canonical_detection_last_requested_frame = frame;
    return accepted;
  };
  auto activeDetectionRepository = [&]()
      -> crimson::zarr::DetectionRepository & {
    if (canonical_detection_route) {
      return canonical_detection_repository;
    }
    return detection_repository;
  };
  auto activeCameraFrameDataAdapter = [&]()
      -> crimson::platform::nvidia::CameraFrameDataAdapter & {
    if (canonical_detection_route) {
      return canonical_camera_frame_data_adapter;
    }
    return legacy_camera_frame_data_adapter;
  };
  auto refreshActiveDetectionDatasetOptions = [&]() {
    refreshDetectionDatasetOptions(activeDetectionRepository());
  };
  auto syncCanonicalDetectionRoute = [&]() {
    const std::string index_path =
        media_session_loader.activeRecordingClipIndexPath();
    const auto recording_provider =
        clipped_media_state.source == ClippedMediaSource::RecordingClipIndex
            ? clipped_media_state.recording_clip_provider
            : nullptr;
    const std::string archive_path =
        zarr_loaded ? zarr_loader.getArchivePath() : std::string{};
    const std::string loader_selected_raw_run =
        zarr_loaded ? zarr_loader.getDetectRunName() : std::string{};
    size_t expected_frames = 0;
    double expected_fps = video_fps;
    std::string expected_recording_identity;
    if (recording_provider != nullptr) {
      const auto &index = recording_provider->index();
      expected_frames = static_cast<size_t>(std::max<int64_t>(
          0, index.totalFrameCount()));
      expected_fps = index.framesPerSecond();
      expected_recording_identity = index.recordingId();
    }
    size_t expected_width = 0;
    size_t expected_height = 0;
    if (scene != nullptr && !scene->cameras.empty()) {
      expected_width = scene->cameras.front().image_width;
      expected_height = scene->cameras.front().image_height;
    }

    const uint64_t session_generation = recording_open_workflow.generation();
    const bool signature_changed =
        session_generation != canonical_detection_session_generation ||
        archive_path != canonical_detection_archive ||
        index_path != canonical_detection_index ||
        loader_selected_raw_run != canonical_detection_run ||
        expected_recording_identity !=
            canonical_detection_expected_recording_identity ||
        expected_frames != canonical_detection_expected_frames ||
        expected_width != canonical_detection_expected_width ||
        expected_height != canonical_detection_expected_height;
    if (!signature_changed) {
      return;
    }

    std::string eligibility_error;
    const bool rolling_index =
        recording_provider != nullptr && !index_path.empty() &&
        isRollingRecordingClipIndex(index_path, &eligibility_error);
    const bool rolling_index_inspection_failed =
        recording_provider != nullptr && !index_path.empty() &&
        !rolling_index && !eligibility_error.empty();
    const bool route_selected = rolling_index || rolling_index_inspection_failed;
    std::string selected_raw_run = loader_selected_raw_run;
    if (rolling_index && eligibility_error.empty() && !archive_path.empty()) {
      const auto authoritative_run = resolveAuthoritativeCanonicalRawRun(
          archive_path, loader_selected_raw_run, &eligibility_error);
      if (authoritative_run.has_value()) {
        selected_raw_run = *authoritative_run;
      }
    }

    canonical_detection_session_generation = session_generation;
    canonical_detection_archive = archive_path;
    canonical_detection_index = index_path;
    // Keep the loader-published seed in the signature. The authoritative run
    // is resolved from bounded group metadata only when this signature changes.
    canonical_detection_run = loader_selected_raw_run;
    canonical_detection_expected_recording_identity =
        expected_recording_identity;
    canonical_detection_expected_frames = expected_frames;
    canonical_detection_expected_width = expected_width;
    canonical_detection_expected_height = expected_height;
    canonical_detection_route = route_selected;
    canonical_detection_seen_seek_requests = 0;
    canonical_detection_last_requested_frame = -1;

    clearCameraViewReadOnlyMaskTextureCache();
    if (!route_selected) {
      canonical_detection_repository.close();
      canonical_timeline_session.close();
      canonical_overlay_session.close();
      refreshDetectionDatasetOptions(detection_repository);
      return;
    }

    const bool canonical_sources_eligible =
        eligibility_error.empty() && !archive_path.empty() &&
        !selected_raw_run.empty() && !expected_recording_identity.empty() &&
        expected_frames > 0 && expected_width > 0 && expected_height > 0 &&
        std::isfinite(expected_fps) && expected_fps > 0.0;
    if (canonical_sources_eligible) {
      std::string timeline_open_error;
      crimson::gui::CanonicalTimelineOpenRequest timeline_request;
      timeline_request.archive_path = archive_path;
      timeline_request.expected_frame_count = expected_frames;
      timeline_request.frames_per_second = expected_fps;
      timeline_request.eye_angle_run = cli_eye_angle_run;
      crimson::gui::CanonicalOverlayOpenRequest overlay_request;
      overlay_request.archive_path = archive_path;
      overlay_request.recording_id = expected_recording_identity;
      overlay_request.eye_run = cli_eye_angle_run;
      overlay_request.frame_count = expected_frames;
      overlay_request.source_width = static_cast<int>(expected_width);
      overlay_request.source_height = static_cast<int>(expected_height);
      std::string overlay_error;
      if (!canonical_overlay_session.beginOpen(std::move(overlay_request), &overlay_error)) {
        std::cerr << "[CanonicalOverlay] state=failed error=" << overlay_error << std::endl;
      }
      if (!canonical_timeline_session.beginOpen(std::move(timeline_request),
                                                &timeline_open_error)) {
        std::cerr << "[CanonicalTimeline] state=failed archive="
                  << archive_path << " error=" << timeline_open_error
                  << std::endl;
      }
    } else {
      canonical_timeline_session.close();
      canonical_overlay_session.close();
    }

    detection_dataset_ids.clear();
    detection_dataset_labels.clear();
    detection_dataset_choice = 0;
    g_zarr_bbox_edit_state.clearAll();
    if (!canonical_sources_eligible) {
      canonical_detection_repository.close();
      std::ostringstream message;
      message << "Canonical detection route is not ready:";
      if (!eligibility_error.empty()) {
        message << " " << eligibility_error;
      } else if (archive_path.empty()) {
        message << " archive path is unavailable";
      } else if (selected_raw_run.empty()) {
        message << " selected raw run is unavailable";
      } else if (expected_recording_identity.empty()) {
        message << " validated recording identity is unavailable";
      } else if (expected_frames == 0) {
        message << " validated recording frame count is unavailable";
      } else if (!std::isfinite(expected_fps) || expected_fps <= 0.0) {
        message << " validated recording frame rate is unavailable";
      } else {
        message << " decoded source dimensions are unavailable";
      }
      show_error = true;
      error_message = message.str();
      std::cerr << "[NvidiaDetection] state=not_ready source=canonical "
                << "archive=" << archive_path << " run=" << selected_raw_run
                << " index=" << index_path << " error=" << error_message
                << std::endl;
      return;
    }

    std::string open_error;
    const bool accepted = canonical_detection_repository.beginOpen(
        crimson::platform::nvidia::NvidiaDetectionOpenRequest{
            archive_path,
            selected_raw_run,
            expected_fps,
            expected_frames,
            expected_width,
            expected_height,
            70,
            32,
            expected_recording_identity,
        },
        &open_error);
    std::cout << "[NvidiaDetection] state=opening source=canonical archive="
              << archive_path << " run=" << selected_raw_run
              << " index=" << index_path << " frames=" << expected_frames
              << " recording_identity=" << expected_recording_identity
              << " source_size=" << expected_width << "x" << expected_height
              << " page_frames=70 cache_pages=32" << std::endl;
    if (!accepted) {
      show_error = true;
      error_message = "Canonical detection open was rejected: " + open_error;
      std::cerr << "[NvidiaDetection] state=failed source=canonical error="
                << error_message << std::endl;
    }
  };
  auto last_reported_canonical_state =
      crimson::platform::nvidia::NvidiaDetectionState::Closed;
  uint64_t last_reported_canonical_generation = 0;
  auto reportCanonicalDetectionState = [&]() {
    if (!canonical_detection_route) {
      return;
    }
    const auto metrics = canonical_detection_repository.metrics();
    if (metrics.state == last_reported_canonical_state &&
        metrics.generation == last_reported_canonical_generation) {
      return;
    }
    last_reported_canonical_state = metrics.state;
    last_reported_canonical_generation = metrics.generation;
    const auto descriptor = canonical_detection_repository.descriptor();
    std::cout << "[NvidiaDetection] state="
              << crimson::platform::nvidia::nvidiaDetectionStateName(
                     metrics.state)
              << " source=canonical generation=" << metrics.generation
              << " archive=" << metrics.archive_path
              << " run=" << metrics.canonical_raw_run
              << " frames=" << descriptor.total_frames
              << " rows=" << metrics.repository.resolved_rows
              << " range_reads=" << metrics.repository.range_reads
              << " cached_pages=" << metrics.buffer.peak_cached_pages
              << " cached_bytes=" << metrics.buffer.cached_bytes
              << " page_cache_hard_byte_budgeted="
              << (metrics.page_cache_hard_byte_budgeted ? "true" : "false")
              << " error=" << metrics.last_error << std::endl;
    if (metrics.state ==
        crimson::platform::nvidia::NvidiaDetectionState::Ready) {
      refreshActiveDetectionDatasetOptions();
    } else if (metrics.state ==
               crimson::platform::nvidia::NvidiaDetectionState::Failed) {
      show_error = true;
      error_message = "Canonical detection open failed: " + metrics.last_error;
    }
  };
  auto last_reported_timeline_state =
      crimson::gui::CanonicalTimelineSessionState::Closed;
  uint64_t last_reported_timeline_generation = 0;
  auto reportCanonicalTimelineState = [&]() {
    if (!canonical_detection_route) {
      return;
    }
    const auto metrics = canonical_timeline_session.metrics();
    if (metrics.state == last_reported_timeline_state &&
        metrics.generation == last_reported_timeline_generation) {
      return;
    }
    last_reported_timeline_state = metrics.state;
    last_reported_timeline_generation = metrics.generation;
    std::cout << "[CanonicalTimeline] state="
              << crimson::gui::canonicalTimelineSessionStateName(metrics.state)
              << " generation=" << metrics.generation
              << " archive=" << metrics.archive_path
              << " eye_run=" << metrics.eye_angle_run
              << " motion_run=" << metrics.motion_run
              << " bout_run=" << metrics.swim_bout_run
              << " eye_error=" << metrics.eye_angle_error
              << " motion_error=" << metrics.motion_error
              << " bout_error=" << metrics.swim_bout_error
              << " error=" << metrics.last_error << std::endl;
  };
  auto refreshChaserDistancePolarRepository = [&]() {
    chaser_distance_polar_repository.reset();
    stimulus_context_timeline.reset();
    stimulus_context_timeline_repository.reset();
    if (zarr_loaded) {
      chaser_distance_polar_repository =
          crimson::zarr::MakeLegacyChaserDistancePolarRepository(zarr_loader);
      stimulus_context_timeline_repository =
          crimson::zarr::MakeLegacyStimulusContextTimelineRepository(
              zarr_loader);
      stimulus_context_timeline =
          stimulus_context_timeline_repository->snapshot();
    }
  };

  auto warmEyeMaskCacheForFrame = [&](const char *reason, int frame) {
    if (!zarr_loaded || !zarr_loader.hasEyeMasks() || frame < 0) {
      return;
    }
    const auto warm_start = std::chrono::steady_clock::now();
    const bool warmed =
        zarr_loader.warmEyeMaskCacheForFrame(static_cast<size_t>(frame));
    if (!warmed) {
      return;
    }
    std::cout << "[SUBJECT_MASK_PREWARM] reason="
              << (reason != nullptr ? reason : "unknown") << " frame=" << frame
              << " total_ms="
              << durationMs(std::chrono::steady_clock::now() - warm_start)
              << std::endl;
  };

  media_session_loader.bootstrapFromCli(
      cli_zarr_override_path, cli_recording_path, cli_recording_clip_index_path,
      [&]() { refreshDetectionDatasetOptions(detection_repository); },
      [&]() { g_zarr_bbox_edit_state.clearAll(); });
  syncCanonicalDetectionRoute();
  if (session_lifecycle.snapshot().phase !=
      crimson::session::SessionPhase::Empty) {
    crimson::diagnostics::writeRuntimeDiagnostics(
        std::cout, "Nvidia",
        {session_lifecycle.snapshot(), session_loading_progress.snapshot(),
         std::nullopt});
  }
  refreshChaserDistancePolarRepository();
  warmEyeMaskCacheForFrame("cli_bootstrap", current_frame_num);

  ReviewFrameFilters review_frame_filters;
  ReviewFrameCache review_frame_cache;
  std::string review_frame_status;
  std::string decode_debug_status;
  std::string bbox_payload_status;
  std::mt19937 debug_rng(static_cast<uint32_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count()));

  std::optional<ManualDetectPayloadPreview> manual_payload_preview;
  CropPreviewWindowState crop_preview_window_state;
  LabelingToolWindowState labeling_tool_window_state;
  FrameDebugWindowState frame_debug_window_state;
  crimson::app::FrameInspectControllerState frame_inspect_controller_state;
  crimson::platform::nvidia::RefinedKeypointWriteSession
      refined_keypoint_write_session(
          [](const crimson::platform::nvidia::RefinedKeypointWriteRequest
                 &request) {
            return crimson::platform::nvidia::executeRefinedKeypointWrite(
                request, crimson::zarr::OpenLegacyReviewWriteRepository);
          });
  auto startRefinedKeypointWrite =
      [&](const CropKeypointEditorAction &action,
          const std::optional<RefinedKeypointSelection> &selection) {
        auto outcome = refined_keypoint_write_session.start(
            {recording_open_workflow.generation(), zarr_loader.getArchivePath(),
             action, selection, true, true});
        if (!outcome.status_message.empty()) {
          frame_debug_window_state.keypoint_review_panel.manual_write_status =
              std::move(outcome.status_message);
        }
      };
  PlaybackSessionController playback_session_controller(
      PlaybackSessionControllerContext{
          scene,
          dc_context,
          &stimulus_repository,
          &stimulus_player,
          &ps,
          &playback_transport,
          &seek_progress,
          &current_frame_num,
          &video_fps,
          &camera_names,
          &window_was_decoding,
          &window_need_decoding,
          [&](int parent_frame) {
            return media_session_loader.resolveDecoderFrameForParentFrame(
                parent_frame);
          },
      });
  auto currentPlaybackFrameCount = [&]() -> int64_t {
    if (zarr_loaded && zarr_loader.getTotalFrames() > 0) {
      return static_cast<int64_t>(std::min<size_t>(
          zarr_loader.getTotalFrames(),
          static_cast<size_t>(std::numeric_limits<int64_t>::max())));
    }
    if (!demuxers.empty() && demuxers.front() != nullptr &&
        demuxers.front()->GetNumFrames() > 0) {
      return static_cast<int64_t>(demuxers.front()->GetNumFrames());
    }
    if (input_is_imgs && !imgs_names.empty()) {
      return static_cast<int64_t>(imgs_names.size());
    }
    if (dc_context->total_num_frame > 0 &&
        dc_context->total_num_frame != std::numeric_limits<int>::max()) {
      return dc_context->total_num_frame;
    }
    return 0;
  };
  auto refreshPlaybackTimeline =
      [&](crimson::playback::PlaybackTransportController::TimePoint now,
          bool reset) {
        if (!video_loaded) {
          playback_transport.setControlsEnabled(false, now);
          return;
        }
        const int64_t frame_count = currentPlaybackFrameCount();
        if (frame_count <= 0 || video_fps <= 0.0) {
          return;
        }
        if (reset || !playback_transport.configured()) {
          playback_transport.configure(video_fps, frame_count, now,
                                       std::max(0, ps.to_display_frame_number));
        } else if (playback_transport.frameCount() != frame_count ||
                   playback_transport.framesPerSecond() != video_fps) {
          playback_transport.updateTimeline(video_fps, frame_count, now);
        }
        playback_transport.setControlsEnabled(true, now);
      };
  auto resetPlaybackStartPerf = [&]() {
    perf_playback_start_frame = -1;
    perf_frames_since_playback_start = 0;
    perf_playback_resume_path = resumePathName(ResumePath::None);
    perf_playback_resume_target_frame = -1;
  };
  auto markPlaybackStartForPerf = [&]() {
    perf_playback_start_frame = std::max(0, ps.to_display_frame_number);
    perf_frames_since_playback_start = 0;
    perf_playback_resume_path = resumePathName(ps.last_resume_path);
    perf_playback_resume_target_frame = ps.last_resume_target_frame;
  };
  auto buildCurrentMaskOverlayOptions = [&]() {
    CameraViewMaskOverlayOptions options;
    options.show_subject_body = show_subject_body_mask;
    options.show_eye_left = show_eye_left_mask;
    options.show_eye_right = show_eye_right_mask;
    options.show_swim_bladder = show_swim_bladder_mask;
    options.show_eye_direction_beams = show_eye_direction_beams;
    options.show_eye_gaze_rays = show_eye_gaze_rays;
    options.show_eye_angle_arcs = show_eye_angle_arcs;
    options.show_eye_angle_labels = show_eye_angle_labels;
    options.mode = mask_overlay_mode;
    if (frame_debug_window_state.subject_mask_edit_session.active()) {
      const auto &target =
          frame_debug_window_state.subject_mask_edit_session.target();
      options.highlighted_roi_index = target.roi_index;
      options.highlighted_component_name = target.component_name;
    }
    return options;
  };
  auto prewarmEyeMaskOverlayTexturesForPlayback = [&](const char *reason,
                                                      int start_frame) {
    if (!zarr_loaded || !zarr_loader.hasEyeMasks() || start_frame < 0) {
      return;
    }
    const bool prewarm_full_overlay = show_eye_masks;
    const bool prewarm_inset =
        prewarm_full_overlay &&
        frame_debug_window_state.active_roi_inset_options.visible;
    if (!prewarm_full_overlay && !prewarm_inset) {
      return;
    }
    const auto prewarm_start = std::chrono::steady_clock::now();
    const CameraViewMaskOverlayOptions mask_options =
        buildCurrentMaskOverlayOptions();
    const std::string smoothing_key = zarr_loader.getEyeMaskSourcePath() + "|" +
                                      zarr_loader.getEyeAngleRunName();
    CameraViewMaskPerfMetrics aggregate;
    constexpr int kTexturePrewarmLookaheadFrames = 4;
    const int max_frame =
        zarr_loader.getTotalFrames() > 0
            ? static_cast<int>(std::min<size_t>(
                  zarr_loader.getTotalFrames() - 1,
                  static_cast<size_t>(std::numeric_limits<int>::max())))
            : start_frame;
    int frames_checked = 0;
    int frames_with_masks = 0;
    for (int frame = start_frame;
         frame <=
         std::min(max_frame, start_frame + kTexturePrewarmLookaheadFrames);
         ++frame) {
      ++frames_checked;
      (void)zarr_loader.warmEyeMaskCacheForFrame(static_cast<size_t>(frame));
      auto mask_details = zarr_loader.getRawDetections(
          static_cast<size_t>(frame),
          /*use_interpolated=*/false,
          /*include_eye_masks=*/true,
          /*include_subject_shapes=*/false,
          /*suppress_subject_mask_smoke_log=*/true);
      if (!mask_details.includes_eye_masks || mask_details.eye_masks.empty()) {
        continue;
      }
      ++frames_with_masks;
      auto metrics = prewarmCameraViewEyeMaskOverlayTextures(
          mask_details, smoothing_key, mask_options, prewarm_full_overlay,
          prewarm_inset ? &frame_debug_window_state.active_roi_inset_options
                        : nullptr,
          nullptr);
      accumulateCameraViewMaskPerfMetrics(aggregate, metrics);
    }
    if (frames_with_masks == 0) {
      return;
    }
    std::cout << "[SUBJECT_MASK_TEXTURE_PREWARM] reason="
              << (reason != nullptr ? reason : "unknown")
              << " start_frame=" << start_frame
              << " frames_checked=" << frames_checked
              << " frames_with_masks=" << frames_with_masks
              << " uploads=" << aggregate.texture_uploads
              << " cache_hits=" << aggregate.texture_cache_hits
              << " cache_misses=" << aggregate.texture_cache_misses
              << " texture_upload_ms=" << aggregate.texture_upload_ms
              << " texture_lookup_ms=" << aggregate.texture_lookup_ms
              << " total_ms="
              << durationMs(std::chrono::steady_clock::now() - prewarm_start)
              << std::endl;
  };
  prewarmEyeMaskOverlayTexturesForPlayback("cli_bootstrap", current_frame_num);
  constexpr size_t kPlaybackMaskPrefetchLookaheadFrames = 1024;
  auto playbackSnapshot = [&]() {
    crimson::playback::diagnostics::PlaybackSnapshot snapshot;
    snapshot.play_video = ps.play_video;
    snapshot.to_display_frame_number = ps.to_display_frame_number;
    snapshot.slider_frame_number = ps.slider_frame_number;
    snapshot.read_head = ps.read_head;
    snapshot.pause_selected = ps.pause_selected;
    snapshot.pause_seeked = ps.pause_seeked;
    snapshot.just_seeked = ps.just_seeked;
    snapshot.slider_just_changed = ps.slider_just_changed;
    snapshot.buffer_browsed_since_pause = ps.buffer_browsed_since_pause;
    snapshot.paused_frame_on_toggle = ps.paused_frame_on_toggle;
    snapshot.last_resume_path = resumePathName(ps.last_resume_path);
    snapshot.last_resume_target_frame = ps.last_resume_target_frame;
    snapshot.accumulated_play_time = ps.accumulated_play_time;
    snapshot.current_stimulus_frame = ps.current_stimulus_frame;
    return snapshot;
  };
  auto seekProgressSnapshot = [&]() {
    return crimson::playback::diagnostics::SeekProgressSnapshot{
        seekStateName(seek_progress.state),
        seek_progress.seek_id,
        seek_progress.requested_camera_frame,
        seek_progress.target_camera_frame,
        seek_progress.target_stimulus_frame,
        seek_progress.accurate,
        seek_progress.skip_stimulus_hard_seek,
        seek_progress.cameras_settled,
        seek_progress.cameras_total,
    };
  };
  auto cameraBufferSnapshot = [&](int visible_idx, int target_frame,
                                  int selected_frame,
                                  bool require_video_loaded = true) {
    return nvidia_diagnostics::makeCameraBufferSnapshot(
        {video_loaded, require_video_loaded, scene, visible_idx, ps.read_head,
         target_frame, selected_frame, &camera_names, &latest_decoded_frame});
  };
  auto clippedPlaybackSnapshot = [&]() {
    const int visible_idx = playback_session_controller.getVisibleCameraIndex();
    return nvidia_diagnostics::makeClippedPlaybackSnapshot(
        current_frame_num, video_fps, playbackSnapshot(),
        seekProgressSnapshot(),
        cameraBufferSnapshot(visible_idx, ps.to_display_frame_number,
                             ps.to_display_frame_number,
                             /*require_video_loaded=*/false));
  };
  auto clippedPlaybackEventStateJson = [&]() -> json {
    return crimson::playback::diagnostics::clippedPlaybackStateJson(
        clippedPlaybackSnapshot());
  };
  auto writeClippedPlaybackStateEvent = [&](const std::string &event_name,
                                            const json &details,
                                            bool force_flush) {
    if (!diagnostics_session.clippedFrameTraceEnabled() ||
        !media_session_loader.hasMappedMedia()) {
      return;
    }
    diagnostics_session.writeClippedFrameTrace(
        crimson::playback::diagnostics::clippedPlaybackStateEventJson(
            event_name, details, clippedPlaybackSnapshot()),
        force_flush);
  };
  auto clippedPlaybackRebaseTargetBeforePlay = [&]() {
    json target = {
        {"target_frame", std::max(0, ps.to_display_frame_number)},
        {"source", "selected_parent_frame"},
        {"visible_idx", nullptr},
        {"front_parent_frame", nullptr},
        {"front_local_frame", nullptr},
        {"staging_parent_frame", nullptr},
        {"read_head_frame", nullptr},
    };
    int target_frame = std::max(0, ps.to_display_frame_number);
    std::string source = "selected_parent_frame";
    const int visible_idx = playback_session_controller.getVisibleCameraIndex();
    target["visible_idx"] =
        visible_idx >= 0 ? json(visible_idx) : json(nullptr);
    if (scene != nullptr && visible_idx >= 0 && visible_idx < scene->num_cams &&
        scene->size_of_buffer > 0) {
      const auto &camera = scene->cameras[visible_idx];
      target["front_parent_frame"] =
          camera.texture_has_valid_frame && camera.last_uploaded_frame >= 0
              ? json(camera.last_uploaded_frame)
              : json(nullptr);
      target["front_local_frame"] =
          camera.texture_has_valid_frame &&
                  camera.last_uploaded_local_frame >= 0
              ? json(camera.last_uploaded_local_frame)
              : json(nullptr);
      target["staging_parent_frame"] =
          camera.playback_staging_valid && camera.playback_staging_frame >= 0
              ? json(camera.playback_staging_frame)
              : json(nullptr);
      const int read_head_slot =
          ps.read_head >= 0
              ? ps.read_head % static_cast<int>(scene->size_of_buffer)
              : -1;
      if (read_head_slot >= 0) {
        const auto &slot = camera.display_buffer[read_head_slot];
        target["read_head_frame"] =
            !slot.available_to_write && slot.frame_number >= 0
                ? json(slot.frame_number)
                : json(nullptr);
      }

      const bool explicit_paused_selection = ps.pause_seeked ||
                                             ps.buffer_browsed_since_pause ||
                                             ps.slider_just_changed;
      if (!explicit_paused_selection && camera.texture_has_valid_frame &&
          camera.last_uploaded_frame >= 0) {
        target_frame = camera.last_uploaded_frame;
        source = "front_texture_parent_frame";
      }
    }
    target["target_frame"] = target_frame;
    target["source"] = source;
    return target;
  };
  auto applyPlaybackToggleForPerf = [&]() {
    const bool was_playing = ps.play_video;
    if (!was_playing && clipped_rebase_before_play &&
        media_session_loader.hasMappedMedia()) {
      const json rebase_target = clippedPlaybackRebaseTargetBeforePlay();
      const int target_frame = rebase_target.value(
          "target_frame", std::max(0, ps.to_display_frame_number));
      if (diagnostics_session.clippedFrameTraceEnabled()) {
        diagnostics_session.writeClippedFrameTrace(
            json{{"event", "clipped_rebase_before_play"},
                 {"phase", "before_seek"},
                 {"target", rebase_target},
                 {"state", clippedPlaybackEventStateJson()}},
            /*force_flush=*/true);
      }
      playback_session_controller.seekToFrame(
          target_frame,
          /*prefer_buffer_when_paused=*/false,
          /*force_inaccurate=*/true,
          /*skip_stimulus_hard_seek=*/true);
      if (diagnostics_session.clippedFrameTraceEnabled()) {
        diagnostics_session.writeClippedFrameTrace(
            json{{"event", "clipped_rebase_before_play"},
                 {"phase", "after_seek"},
                 {"target", rebase_target},
                 {"state", clippedPlaybackEventStateJson()},
                 {"playback",
                  {{"to_display_frame_number", ps.to_display_frame_number},
                   {"slider_frame_number", ps.slider_frame_number},
                   {"read_head", ps.read_head},
                   {"just_seeked", ps.just_seeked},
                   {"pause_seeked", ps.pause_seeked}}}},
            /*force_flush=*/true);
      }
    }
    writeClippedPlaybackStateEvent(
        "toggle_playback",
        json{{"phase", "before"}, {"was_playing", was_playing}}, true);
    if (!was_playing) {
      prewarmEyeMaskOverlayTexturesForPlayback(
          "playback_start", std::max(0, ps.to_display_frame_number));
    }
    playback_session_controller.applyPlaybackToggle();
    writeClippedPlaybackStateEvent("toggle_playback",
                                   json{{"phase", "after"},
                                        {"was_playing", was_playing},
                                        {"is_playing", ps.play_video}},
                                   true);
    if (!was_playing && ps.play_video) {
      markPlaybackStartForPerf();
    } else if (was_playing && !ps.play_video) {
      resetPlaybackStartPerf();
    }
  };
  auto playbackTraceSnapshot = [&](int presenter_view_idx,
                                   int presenter_target_frame,
                                   int presenter_preferred_paused_slot,
                                   int presenter_presented_slot,
                                   int presenter_presented_frame,
                                   int presenter_resolved_frame,
                                   bool presenter_prewarm_active) {
    crimson::playback::diagnostics::PlaybackTraceSnapshot snapshot;
    snapshot.video_loaded = video_loaded;
    snapshot.video_fps = video_fps;
    snapshot.current_frame_num = current_frame_num;
    for (const auto &entry : window_need_decoding) {
      if (entry.second.load()) {
        ++snapshot.window_need_decoding_count;
      }
    }
    snapshot.playback = playbackSnapshot();
    snapshot.seek = seekProgressSnapshot();
    snapshot.presenter = {presenter_view_idx,
                          presenter_target_frame,
                          presenter_preferred_paused_slot,
                          presenter_presented_slot,
                          presenter_presented_frame,
                          presenter_resolved_frame,
                          presenter_prewarm_active};
    const int visible_idx =
        presenter_view_idx >= 0
            ? presenter_view_idx
            : playback_session_controller.getVisibleCameraIndex();
    snapshot.camera_buffer = cameraBufferSnapshot(
        visible_idx,
        std::max(0, presenter_target_frame >= 0 ? presenter_target_frame
                                                : ps.to_display_frame_number),
        presenter_presented_frame);
    snapshot.stimulus.loaded = stimulus_player.loaded;
    snapshot.stimulus.window_name = stimulus_player.window_name;
    snapshot.stimulus.current_stimulus_frame = ps.current_stimulus_frame;
    const auto stimulus_latest =
        latest_decoded_frame.find(stimulus_player.window_name);
    snapshot.stimulus.latest_decoded_frame =
        stimulus_latest != latest_decoded_frame.end()
            ? stimulus_latest->second.load()
            : -1;
    snapshot.stimulus.last_displayed_frame =
        stimulus_player.last_displayed_frame;
    snapshot.stimulus.buffer_size = stimulus_player.buffer_size;
    if (stimulus_player.display_buffer != nullptr &&
        stimulus_player.buffer_size > 0) {
      snapshot.stimulus.slots.reserve(stimulus_player.buffer_size);
      for (int slot_idx = 0; slot_idx < stimulus_player.buffer_size;
           ++slot_idx) {
        const auto metadata =
            frameSlotSnapshotReadable(stimulus_player.display_buffer[slot_idx]);
        snapshot.stimulus.slots.push_back(
            {metadata.has_value(),
             metadata.has_value() ? metadata->frame_number : -1});
      }
    }
    {
      std::lock_guard<std::mutex> lock(g_seek_info_mutex);
      snapshot.stimulus.seek_use = stimulus_player.seek.use_seek;
      snapshot.stimulus.seek_done = stimulus_player.seek.seek_done;
      snapshot.stimulus.seek_id = stimulus_player.seek.seek_id;
      snapshot.stimulus.seek_frame = stimulus_player.seek.seek_frame;
      snapshot.stimulus.settled_seek_id = stimulus_player.seek.settled_seek_id;
    }
    return snapshot;
  };
  auto writePlaybackTraceEvent =
      [&](const std::string &event_name, const json &details,
          int presenter_view_idx = -1, int presenter_target_frame = -1,
          int presenter_preferred_paused_slot = -1,
          int presenter_presented_slot = -1, int presenter_presented_frame = -1,
          int presenter_resolved_frame = -1,
          bool presenter_prewarm_active = false) {
        if (!diagnostics_session.playbackTraceEnabled()) {
          return;
        }
        diagnostics_session.writePlaybackTrace(
            crimson::playback::diagnostics::playbackTraceEventJson(
                event_name, details,
                playbackTraceSnapshot(
                    presenter_view_idx, presenter_target_frame,
                    presenter_preferred_paused_slot, presenter_presented_slot,
                    presenter_presented_frame, presenter_resolved_frame,
                    presenter_prewarm_active)),
            event_name != "frame" && event_name != "canonical_overlay_present");
      };

  auto playbackSeekEventDetails =
      [](const crimson::playback::PlaybackSeekTelemetryEvent &event,
         const char *stage) {
        const auto &transaction = event.transaction;
        const auto &plan = event.plan;
        const auto &result = event.result;
        return json{
            {"stage", stage},
            {"generation", transaction.generation},
            {"phase", std::string(crimson::playback::playbackSeekPhaseName(
                          transaction.request.phase))},
            {"origin", std::string(crimson::playback::playbackSeekOriginName(
                           transaction.request.origin))},
            {"target_frame", transaction.request.target_frame},
            {"frame_count", transaction.request.frame_count},
            {"plan_valid", plan.valid},
            {"accuracy",
             std::string(
                 crimson::playback::playbackSeekAccuracyName(plan.accuracy))},
            {"execution_mode",
             std::string(
                 crimson::playback::playbackSeekExecutionModeName(plan.mode))},
            {"prefer_resident_frame", plan.prefer_resident_frame},
            {"status",
             std::string(stage) == "requested"
                 ? std::string("requested")
                 : std::string(
                       crimson::playback::playbackSeekExecutionStatusName(
                           result.status))},
            {"execution_path",
             std::string(crimson::playback::playbackSeekExecutionPathName(
                 result.path))},
            {"resolved_frame", result.resolved_frame},
            {"queue_ms", result.queue_ms},
            {"service_ms", result.service_ms},
            {"error", result.error},
        };
      };

  auto executePlaybackSeek =
      [&](const crimson::playback::PlaybackSeekRequest &request) {
        auto &coordinator = playback_transport.seekCoordinator();
        const auto transaction = coordinator.begin(request);
        const auto plan = crimson::playback::planPlaybackSeek(
            transaction, crimson::playback::PlaybackSeekAdapterCapabilities{
                             false,
                             true,
                             true,
                             true,
                         });
        crimson::playback::PlaybackSeekTelemetryEvent requested_event{
            transaction, plan, {}};
        requested_event.result.resolved_frame = request.target_frame;
        const json requested_details =
            playbackSeekEventDetails(requested_event, "requested");
        writeClippedPlaybackStateEvent("transport_seek", requested_details,
                                       true);
        writePlaybackTraceEvent("transport_seek", requested_details);

        crimson::playback::PlaybackSeekExecutionResult execution;
        execution.resolved_frame = request.target_frame;
        if (!plan.valid) {
          execution.status =
              crimson::playback::PlaybackSeekExecutionStatus::Rejected;
          execution.error = std::string(
              crimson::playback::playbackSeekRejectionName(plan.rejection));
        } else {
          if (plan.pause_playback && ps.play_video) {
            applyPlaybackToggleForPerf();
          }
          const int target_frame = static_cast<int>(std::clamp<int64_t>(
              request.target_frame, 0, std::numeric_limits<int>::max()));
          execution = playback_session_controller.seekToFrame(
              target_frame, plan.prefer_resident_frame,
              plan.accuracy ==
                  crimson::playback::PlaybackSeekAccuracy::ApproximateAllowed);
        }

        const auto event =
            coordinator.record(transaction, plan, std::move(execution));
        const json outcome_details = playbackSeekEventDetails(event, "outcome");
        writeClippedPlaybackStateEvent("transport_seek", outcome_details, true);
        writePlaybackTraceEvent("transport_seek", outcome_details);
        return event;
      };

  auto writeFrameSyncTraceEvent =
      [&](const json &details, int presenter_view_idx,
          int presenter_target_frame, int presenter_preferred_paused_slot,
          int presenter_presented_slot, int presenter_presented_frame,
          int presenter_resolved_frame, bool presenter_prewarm_active) {
        if (!diagnostics_session.frameSyncTraceEnabled()) {
          return;
        }
        diagnostics_session.writeFrameSyncTrace(
            crimson::playback::diagnostics::frameSyncTraceEventJson(
                details,
                playbackTraceSnapshot(
                    presenter_view_idx, presenter_target_frame,
                    presenter_preferred_paused_slot, presenter_presented_slot,
                    presenter_presented_frame, presenter_resolved_frame,
                    presenter_prewarm_active)),
            /*force_flush=*/true);
      };

  auto clippedMediaStateSnapshot =
      [&]() -> std::optional<
                crimson::playback::diagnostics::ClippedMediaStateSnapshot> {
    if (!media_session_loader.hasMappedMedia()) {
      return std::nullopt;
    }
    const auto &handoff = clipped_media_state.handoff;
    return crimson::playback::diagnostics::ClippedMediaStateSnapshot{
        clipped_media_state.current_video_path,
        handoff.clip_id,
        clipped_media_state.camera_serial,
        static_cast<uint64_t>(handoff.selected_run_index),
        handoff.first_parent_frame,
        handoff.last_parent_frame,
        handoff.pending_switch_parent_frame,
        handoff.switch_in_progress,
        handoff.last_presented_parent_frame};
  };
  auto clippedStateJson = [&]() -> json {
    const auto state = clippedMediaStateSnapshot();
    return state ? crimson::playback::diagnostics::clippedMediaStateJson(*state)
                 : json(nullptr);
  };

  auto writeClippedHandoffTraceEvent = [&](const std::string &event_name,
                                           const json &details) {
    if (!diagnostics_session.frameSyncTraceEnabled()) {
      return;
    }
    diagnostics_session.writeFrameSyncTrace(
        crimson::playback::diagnostics::clippedHandoffTraceEventJson(
            event_name, details, video_loaded, current_frame_num,
            playbackSnapshot(), clippedMediaStateSnapshot()),
        /*force_flush=*/true);
  };

  auto clippedSelectedRunForFrame = [&](int parent_frame) -> size_t {
    const auto binding =
        media_session_loader.resolveMappedMediaFrame(parent_frame);
    return binding.mapped ? binding.selected_run_index
                          : crimson::playback::kNoClippedSelectedRun;
  };

  auto clippedResolverTraceSource =
      [&](const PaletteClippedResolver::FrameRunRow *row)
      -> std::optional<nvidia_diagnostics::ResolverSourceSnapshot> {
    if (row == nullptr) {
      return std::nullopt;
    }
    std::optional<nvidia_diagnostics::SelectedRunSourceSnapshot>
        selected_snapshot;
    const auto *selected =
        zarr_loader.getClippedResolver().selectedRun(row->selected_run_index);
    if (selected != nullptr) {
      selected_snapshot = nvidia_diagnostics::SelectedRunSourceSnapshot{
          selected->work_unit_id,       selected->detect_run,
          selected->refined_detect_run, selected->detect_group_path,
          selected->refined_group_path, selected->video_path};
    }
    return nvidia_diagnostics::ResolverSourceSnapshot{
        row->parent_frame_index,
        row->recording_frame_id,
        row->clip_id,
        static_cast<uint64_t>(row->selected_run_index),
        row->camera_serial,
        row->clip_local_frame_index,
        static_cast<uint64_t>(row->selected_run_index),
        std::move(selected_snapshot)};
  };

  if (ui_reference.enabled) {
    auto rejectUiReferenceStart = [&]() {
      ui_reference.enabled = false;
      ui_reference_capture.fail("UI-reference startup rejected");
      app_exit_code = 2;
      glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
    };
    do {
      if (!video_loaded || scene == nullptr || scene->num_cams <= 0 ||
          scene->size_of_buffer <= 0) {
        std::cerr << "[UiReference] requested but no camera video is "
                     "loaded"
                  << std::endl;
        rejectUiReferenceStart();
        break;
      }
      int max_frame = std::max(0, dc_context->total_num_frame - 1);
      if (zarr_loaded && zarr_loader.getTotalFrames() > 0) {
        max_frame = static_cast<int>(std::min<size_t>(
            zarr_loader.getTotalFrames() - 1,
            static_cast<size_t>(std::numeric_limits<int>::max())));
      }
      if (ui_reference.target_frame > max_frame) {
        std::cerr << "[UiReference] target frame " << ui_reference.target_frame
                  << " is outside loaded recording max frame " << max_frame
                  << std::endl;
        rejectUiReferenceStart();
        break;
      }

      const bool needs_zarr = ui_reference.state != UiReferenceState::Workspace;
      if (needs_zarr && !zarr_loaded) {
        std::cerr << "[UiReference] state "
                  << uiReferenceStateName(ui_reference.state)
                  << " requires a loaded Zarr archive" << std::endl;
        rejectUiReferenceStart();
        break;
      }
      if (ui_reference.state == UiReferenceState::StimulusDebug) {
        if (!stimulus_player.loaded || !stimulus_repository.hasMapping()) {
          std::cerr << "[UiReference] stimulus-debug requires an "
                       "auto-loaded stimulus video and alignment"
                    << std::endl;
          rejectUiReferenceStart();
          break;
        }
        const auto mapped = crimson::zarr::StimulusFrameForCamera(
            &stimulus_repository, ui_reference.target_frame);
        if (!mapped.has_value() || *mapped < 0) {
          std::cerr << "[UiReference] stimulus-debug target camera "
                       "frame is not mapped to a stimulus frame"
                    << std::endl;
          rejectUiReferenceStart();
          break;
        }
        ui_reference.target_stimulus_frame = *mapped;
        show_stimulus_debug_windows = true;
        stimulus_inset_options.show_inset = true;
      }
      if (ui_reference.state == UiReferenceState::CropPreview) {
        if (!(zarr_loader.hasCropImages() ||
              !keypoint_repository.descriptor().run_name.empty() ||
              zarr_loader.hasEyeMasks()) ||
            !activeDetectionRepository().descriptor().available) {
          std::cerr << "[UiReference] crop-preview requires detection "
                       "and crop/keypoint/mask data"
                    << std::endl;
          rejectUiReferenceStart();
          break;
        }
        frame_debug_window_state.active_view =
            crimson::workspace::FrameInspectView::Keypoints;
        frame_debug_window_state.keypoint_review_panel
            .show_advanced_crop_preview = true;
        g_zarr_bbox_edit_state.selected_frame = ui_reference.target_frame;
        g_zarr_bbox_edit_state.selected_box = 0;
      } else if (ui_reference.state == UiReferenceState::Overlays) {
        frame_debug_window_state.active_view =
            crimson::workspace::FrameInspectView::EyeMasks;
        show_eye_masks = true;
        if (canonical_detection_route) {
          show_keypoint_markers = true;
          show_heading_arrows = true;
          subject_shape_overlay_options.show_overlay = true;
        }
      } else if (ui_reference.state == UiReferenceState::StimulusOverlay) {
        if (stimulus_context_timeline == nullptr) {
          std::cerr << "[UiReference] stimulus-overlay requires a "
                       "ready stimulus context timeline"
                    << std::endl;
          rejectUiReferenceStart();
          break;
        }
        show_keypoint_markers = false;
        show_heading_arrows = false;
        show_eye_masks = false;
        show_movement_trail = false;
        subject_shape_overlay_options.show_overlay = false;
        tail_kinematics_overlay_options.show_overlay = false;
        stimulus_inset_options.show_inset = false;
        frame_debug_window_state.active_roi_inset_options.visible = false;
        chaser_distance_polar_inset_options.show_inset = false;
      } else if (ui_reference.state == UiReferenceState::Polar) {
        if (chaser_distance_polar_repository == nullptr ||
            !chaser_distance_polar_repository->descriptor().ready()) {
          std::cerr << "[UiReference] polar requires a ready "
                       "chaser-distance repository"
                    << std::endl;
          rejectUiReferenceStart();
          break;
        }
        show_keypoint_markers = false;
        show_heading_arrows = false;
        show_eye_masks = false;
        show_movement_trail = false;
        subject_shape_overlay_options.show_overlay = false;
        tail_kinematics_overlay_options.show_overlay = false;
        stimulus_inset_options.show_inset = false;
        frame_debug_window_state.active_roi_inset_options.visible = false;
        chaser_distance_polar_inset_options = {};
      } else if (ui_reference.state == UiReferenceState::AnalysisEye) {
        const bool canonical_analysis_available =
            canonical_detection_route &&
            canonical_timeline_session.state() !=
                crimson::gui::CanonicalTimelineSessionState::Closed &&
            canonical_timeline_session.state() !=
                crimson::gui::CanonicalTimelineSessionState::Failed;
        if (!canonical_analysis_available &&
            !zarr_loader.hasEyeAngleAnalysisData()) {
          std::cerr << "[UiReference] analysis-eye requires eye-angle "
                       "analysis data"
                    << std::endl;
          rejectUiReferenceStart();
          break;
        }
        if (!canonical_analysis_available) {
          frame_debug_window_state.active_view =
              crimson::workspace::FrameInspectView::EyeMasks;
          show_eye_masks = true;
        }
      } else if (ui_reference.state == UiReferenceState::AnalysisTailStimulus) {
        if (!zarr_loader.hasTailKinematicsData() ||
            !(zarr_loader.hasStimulusSteps() ||
              zarr_loader.hasStimulusEvents())) {
          std::cerr << "[UiReference] analysis-tail-stimulus requires "
                       "tail and stimulus analysis data"
                    << std::endl;
          rejectUiReferenceStart();
          break;
        }
        frame_debug_window_state.active_view =
            crimson::workspace::FrameInspectView::TailKinematics;
      }

      std::string output_error;
      if (!crimson::ui_reference::prepareUiReferenceOutput(
              ui_reference.ready_file, &output_error)) {
        std::cerr << "[UiReference] failed to prepare output: " << output_error
                  << std::endl;
        rejectUiReferenceStart();
        break;
      }

      ui_reference.rendered_image_file =
          crimson::ui_reference::uiReferenceImagePath(ui_reference.ready_file);
      if (!ui_reference_capture.start()) {
        std::cerr << "[UiReference] failed to start capture coordinator: "
                  << ui_reference_capture.snapshot().failure_reason << std::endl;
        rejectUiReferenceStart();
        break;
      }
      playback_session_controller.seekToFrame(
          ui_reference.target_frame,
          /*prefer_buffer_when_paused=*/false,
          /*force_inaccurate=*/false,
          /*skip_stimulus_hard_seek=*/false);
      if (!canonical_detection_route &&
          (ui_reference.state == UiReferenceState::Overlays ||
           ui_reference.state == UiReferenceState::AnalysisEye)) {
        zarr_loader.requestRefinedSubjectMaskOptionalOverlayPrefetch();
        prewarmEyeMaskOverlayTexturesForPlayback("ui_reference",
                                                 ui_reference.target_frame);
      }
      std::cout << "[UiReference] started state="
                << uiReferenceStateName(ui_reference.state)
                << " target_frame=" << ui_reference.target_frame
                << " ready_file=" << ui_reference.ready_file
                << " timeout_s=" << ui_reference.timeout_s << std::endl;
    } while (false);
  }

  if (playback_smoke.enabled) {
    if (!video_loaded) {
      std::cerr << "[PlaybackSmoke] requested but no video is loaded"
                << std::endl;
      return 2;
    }
    if (input_is_imgs) {
      std::cerr << "[PlaybackSmoke] image-sequence input is not "
                   "supported by this smoke"
                << std::endl;
      return 2;
    }
    if (scene == nullptr || scene->num_cams <= 0 ||
        scene->size_of_buffer <= 0) {
      std::cerr << "[PlaybackSmoke] requested but camera buffers are not "
                   "initialized"
                << std::endl;
      return 2;
    }
    if (zarr_loaded && zarr_loader.getTotalFrames() > 0) {
      const int max_frame = static_cast<int>(std::min<size_t>(
          zarr_loader.getTotalFrames() - 1,
          static_cast<size_t>(std::numeric_limits<int>::max())));
      if (playback_smoke.end_frame > max_frame) {
        std::cerr << "[PlaybackSmoke] end frame " << playback_smoke.end_frame
                  << " is outside loaded recording max frame " << max_frame
                  << std::endl;
        return 2;
      }
    }
    playback_smoke.started = true;
    playback_smoke.start_time = std::chrono::steady_clock::now();
    playback_session_controller.seekToFrame(playback_smoke.start_frame,
                                            /*prefer_buffer_when_paused=*/false,
                                            /*force_inaccurate=*/true,
                                            /*skip_stimulus_hard_seek=*/true);
    prewarmEyeMaskOverlayTexturesForPlayback("playback_smoke",
                                             playback_smoke.start_frame);
    playback_smoke.playback_started = playback_smoke.warmup_s <= 0.0;
    if (ps.play_video != playback_smoke.playback_started) {
      applyPlaybackToggleForPerf();
    }
    writePlaybackTraceEvent(
        "playback_smoke_started",
        {{"start_frame", playback_smoke.start_frame},
         {"end_frame", playback_smoke.end_frame},
         {"timeout_s", playback_smoke.timeout_s},
         {"warmup_s", playback_smoke.warmup_s}},
        playback_session_controller.getVisibleCameraIndex());
    std::cout << "[PlaybackSmoke] started range=" << playback_smoke.start_frame
              << "-" << playback_smoke.end_frame
              << " timeout_s=" << playback_smoke.timeout_s << std::endl;
  }

  if (clipped_boundary_smoke.enabled) {
    if (!media_session_loader.hasMappedMedia()) {
      std::cerr << "[ClippedBoundarySmoke] requested but the loaded "
                << "archive is not a clipped collection" << std::endl;
      return 2;
    }
    const int64_t mapped_frame_count =
        media_session_loader.mappedMediaFrameCount();
    const int max_frame = static_cast<int>(
        std::clamp<int64_t>(mapped_frame_count > 0 ? mapped_frame_count - 1 : 0,
                            0, std::numeric_limits<int>::max()));
    if (clipped_boundary_smoke.end_frame > max_frame) {
      std::cerr << "[ClippedBoundarySmoke] end frame "
                << clipped_boundary_smoke.end_frame
                << " is outside loaded clipped recording max frame "
                << max_frame << std::endl;
      return 2;
    }
    const size_t start_run =
        clippedSelectedRunForFrame(clipped_boundary_smoke.start_frame);
    const size_t end_run =
        clippedSelectedRunForFrame(clipped_boundary_smoke.end_frame);
    if (start_run == std::numeric_limits<size_t>::max() ||
        end_run == std::numeric_limits<size_t>::max() || start_run == end_run) {
      std::cerr << "[ClippedBoundarySmoke] range must cross a clipped "
                << "selected-run boundary: "
                << clipped_boundary_smoke.start_frame << ":"
                << clipped_boundary_smoke.end_frame << std::endl;
      return 2;
    }
    clipped_boundary_smoke.started = true;
    clipped_boundary_smoke.start_time = std::chrono::steady_clock::now();
    playback_session_controller.seekToFrame(clipped_boundary_smoke.start_frame,
                                            /*prefer_buffer_when_paused=*/false,
                                            /*force_inaccurate=*/true,
                                            /*skip_stimulus_hard_seek=*/true);
    if (!ps.play_video) {
      applyPlaybackToggleForPerf();
    }
    writeClippedHandoffTraceEvent(
        "smoke_started", {{"start_frame", clipped_boundary_smoke.start_frame},
                          {"end_frame", clipped_boundary_smoke.end_frame},
                          {"start_selected_run_index", start_run},
                          {"end_selected_run_index", end_run}});
    std::cout << "[ClippedBoundarySmoke] started range="
              << clipped_boundary_smoke.start_frame << "-"
              << clipped_boundary_smoke.end_frame << std::endl;
  }

  crimson::platform::nvidia::ClippedMediaCoordinator clipped_media_coordinator(
      crimson::platform::nvidia::ClippedMediaCoordinatorContext{
          &clipped_media_state.handoff,
          [&]() { return media_session_loader.mappedMediaFrameCount(); },
          [&](int64_t parent_frame) {
            return media_session_loader.resolveMappedMediaFrame(parent_frame);
          },
          [&](int64_t parent_frame) {
            if (parent_frame < 0 ||
                parent_frame > std::numeric_limits<int>::max()) {
              return false;
            }
            const auto seek_result = playback_session_controller.seekToFrame(
                static_cast<int>(parent_frame),
                /*prefer_buffer_when_paused=*/false,
                /*force_inaccurate=*/true,
                /*skip_stimulus_hard_seek=*/true);
            return seek_result.status ==
                       crimson::playback::PlaybackSeekExecutionStatus::
                           Submitted ||
                   seek_result.status ==
                       crimson::playback::PlaybackSeekExecutionStatus::
                           Completed ||
                   seek_result.status ==
                       crimson::playback::PlaybackSeekExecutionStatus::
                           Deduplicated;
          },
          [&](const crimson::platform::nvidia::ClippedMediaCoordinatorEvent
                  &event) {
            using EventKind =
                crimson::platform::nvidia::ClippedMediaCoordinatorEventKind;
            switch (event.kind) {
            case EventKind::SwitchRequested:
              std::cout << "[ClippedHandoff] switch_request parent_frame="
                        << event.parent_frame
                        << " old_clip=" << event.old_clip_id
                        << " new_clip=" << event.clip_id
                        << " old_selected_run_index="
                        << event.old_selected_run_index
                        << " new_selected_run_index="
                        << event.selected_run_index
                        << " local_frame=" << event.clip_local_frame_index
                        << std::endl;
              writeClippedHandoffTraceEvent(
                  "switch_request",
                  {{"parent_frame", event.parent_frame},
                   {"old_clip_id", event.old_clip_id},
                   {"new_clip_id", event.clip_id},
                   {"old_selected_run_index", event.old_selected_run_index},
                   {"new_selected_run_index", event.selected_run_index},
                   {"clip_local_frame_index", event.clip_local_frame_index}});
              break;
            case EventKind::SwitchLoaded:
              std::cout << "[ClippedHandoff] switch_loaded parent_frame="
                        << event.parent_frame << " clip=" << event.clip_id
                        << " selected_run_index=" << event.selected_run_index
                        << " local_frame=" << event.clip_local_frame_index
                        << " range=" << event.first_parent_frame << "-"
                        << event.last_parent_frame << std::endl;
              writeClippedHandoffTraceEvent(
                  "switch_loaded",
                  {{"parent_frame", event.parent_frame},
                   {"clip_id", event.clip_id},
                   {"selected_run_index", event.selected_run_index},
                   {"first_parent_frame", event.first_parent_frame},
                   {"last_parent_frame", event.last_parent_frame},
                   {"clip_local_frame_index", event.clip_local_frame_index}});
              break;
            case EventKind::SwitchPresented:
              std::cout << "[ClippedHandoff] switch_presented parent_frame="
                        << event.parent_frame << " clip=" << event.clip_id
                        << " selected_run_index=" << event.selected_run_index
                        << std::endl;
              writeClippedHandoffTraceEvent(
                  "switch_presented",
                  {{"presented_parent_frame", event.parent_frame},
                   {"pending_switch_parent_frame",
                    event.pending_switch_parent_frame},
                   {"clip_id", event.clip_id},
                   {"selected_run_index", event.selected_run_index}});
              break;
            case EventKind::SwitchFailed:
              std::cout << "[ClippedHandoff] switch_failed parent_frame="
                        << event.parent_frame
                        << " old_clip=" << event.old_clip_id << std::endl;
              writeClippedHandoffTraceEvent(
                  "switch_failed",
                  {{"parent_frame", event.parent_frame},
                   {"old_clip_id", event.old_clip_id},
                   {"new_selected_run_index", event.selected_run_index}});
              break;
            }
          }});

  auto makeDecodeDebugDumpContext = [&]() {
    return DecodeDebugDumpContext{
        video_loaded,
        scene,
        &camera_names,
        &window_need_decoding,
        &ps,
        &stimulus_player,
        default_buffer_dump_root,
        video_fps,
    };
  };
  auto reloadActiveZarrPreserveDataset =
      [&](std::string &reload_error,
          std::optional<crimson::zarr::DetectionDataset>
              preferred_dataset = std::nullopt) -> bool {
    const auto previous_dataset =
        detection_repository.descriptor().active_dataset;
    const std::string archive_path = zarr_loader.getArchivePath();
    if (archive_path.empty()) {
      reload_error = "No loaded Zarr archive.";
      return false;
    }

    if (!zarr_loader.loadZarrFile(archive_path, reload_error)) {
      zarr_loaded = false;
      refreshChaserDistancePolarRepository();
      return false;
    }

    zarr_loaded = true;
    const auto dataset_to_restore =
        preferred_dataset.value_or(previous_dataset);
    if (detection_repository.isDatasetAvailable(dataset_to_restore)) {
      (void)detection_repository.selectDataset(dataset_to_restore);
    }
    refreshDetectionDatasetOptions(detection_repository);
    invalidateReviewFrameCache(review_frame_cache);
    review_frame_status.clear();
    if (zarr_loader.getTotalFrames() > 0 &&
        current_frame_num >= static_cast<int>(zarr_loader.getTotalFrames())) {
      current_frame_num = static_cast<int>(zarr_loader.getTotalFrames()) - 1;
    }
    warmEyeMaskCacheForFrame("zarr_reload", current_frame_num);
    prewarmEyeMaskOverlayTexturesForPlayback("zarr_reload", current_frame_num);
    refreshChaserDistancePolarRepository();
    return true;
  };

  auto workspaceCapabilities = [&]() {
    crimson::workspace::WorkspaceCapabilities capabilities;
    capabilities.video_loaded = video_loaded;
    capabilities.playback_ready = video_loaded &&
                                  playback_transport.configured() &&
                                  playback_transport.controlsEnabled();
    capabilities.playing = playback_transport.isPlaying();
    capabilities.zarr_loaded = zarr_loaded;
    capabilities.crop_preview_available =
        zarr_loaded &&
        (zarr_loader.hasCropImages() ||
         !keypoint_repository.descriptor().run_name.empty() ||
         zarr_loader.hasEyeMasks());
    capabilities.stimulus_video_loaded = stimulus_player.loaded;
    capabilities.analysis_timeline_available =
        (canonical_detection_route &&
         canonical_timeline_session.state() !=
             crimson::gui::CanonicalTimelineSessionState::Closed) ||
        (zarr_loaded &&
         (zarr_loader.hasMovementData() ||
          zarr_loader.hasDeferredMovementData() ||
          zarr_loader.hasEyeAngleAnalysisData() ||
          zarr_loader.hasTailKinematicsData() ||
          zarr_loader.hasStimulusSteps() || zarr_loader.hasStimulusEvents()));
    capabilities.detection_quality_available =
        quality_timeline_session.detectionConfigured();
    capabilities.keypoint_quality_available =
        quality_timeline_session.keypointConfigured();
    return capabilities;
  };

  auto configureQualityTimelineSession = [&]() {
    crimson::gui::QualityTimelineSessionRequest request;
    request.detection_archive_path = zarr_loader.getArchivePath();
    if (request.detection_archive_path.empty()) {
      request.detection_archive_path = cli_zarr_override_path;
    }
    if (!cli_refined_detection_run.empty()) {
      request.detection_surface =
          crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1;
      request.detection_run_name = cli_refined_detection_run;
      request.allow_selector_ineligible_refined_run =
          cli_allow_selector_ineligible_refined_detection;
    } else {
      request.detection_surface =
          crimson::zarr::DetectionSurfaceKind::CanonicalRawV1;
      request.detection_run_name = !cli_detection_run.empty()
                                       ? cli_detection_run
                                       : zarr_loader.getDetectRunName();
    }
    request.raw_keypoints = {cli_keypoint_v2_raw.archive_path,
                             cli_keypoint_v2_raw.run_name,
                             cli_keypoint_v2_raw.manifest_digest};
    request.keypoint_quality = {cli_keypoint_v2_quality.archive_path,
                                cli_keypoint_v2_quality.run_name,
                                cli_keypoint_v2_quality.manifest_digest};
    request.refined_keypoints = {cli_keypoint_v2_refined.archive_path,
                                 cli_keypoint_v2_refined.run_name,
                                 cli_keypoint_v2_refined.manifest_digest};
    request.body_frame = {cli_keypoint_v2_body_frame.archive_path,
                          cli_keypoint_v2_body_frame.run_name,
                          cli_keypoint_v2_body_frame.manifest_digest};
    request.allow_selector_ineligible_keypoints =
        cli_allow_selector_ineligible_keypoints;
    quality_timeline_session.configure(std::move(request));
  };

  while (!glfwWindowShouldClose(window->render_target)) {
    static FileBrowserWindowState file_browser_window_state;
    syncCanonicalDetectionRoute();
    reportCanonicalDetectionState();
    reportCanonicalTimelineState();
    if (canonical_detection_route) {
      static std::string last_overlay_open_report;
      const auto snapshot = canonical_overlay_session.snapshot(-1);
      const std::string signature = std::to_string(snapshot.generation) + ":" +
          crimson::gui::canonicalOverlayStateName(snapshot.state) + snapshot.error +
          snapshot.keypoints.error + snapshot.masks.error + snapshot.shapes.error;
      if (signature != last_overlay_open_report) {
        last_overlay_open_report = signature;
        std::cout << "[CanonicalOverlay] state=" << crimson::gui::canonicalOverlayStateName(snapshot.state)
                  << " generation=" << snapshot.generation
                  << " keypoint_run=" << snapshot.keypoints.descriptor.run_name
                  << " mask_run=" << snapshot.masks.descriptor.run_name
                  << " shape_run=" << snapshot.shapes.descriptor.run_name
                  << " open_ms=" << snapshot.open_ms
                  << " keypoint_error=" << snapshot.keypoints.error
                  << " mask_error=" << snapshot.masks.error
                  << " shape_error=" << snapshot.shapes.error
                  << " error=" << snapshot.error << std::endl;
      }
    }
    if (ui_reference_capture.published()) {
      // Preserve the proven front buffer until the external harness has
      // captured it. Rendering another frame can race an X11 capture.
      glfwWaitEventsTimeout(0.05);
      continue;
    }
    const auto frame_loop_start = std::chrono::steady_clock::now();
    configureQualityTimelineSession();
    crimson::platform::nvidia::pollAndApplyRefinedKeypointWrite(
        refined_keypoint_write_session, recording_open_workflow.generation(),
        zarr_loader.getArchivePath(),
        frame_debug_window_state.keypoint_review_panel.manual_write_status,
        {
            [&](const RefinedKeypointCacheUpdate &update,
                std::string *error) {
              return zarr_loader.applyRefinedKeypointCacheUpdate(update,
                                                                 error);
            },
            reloadActiveZarrPreserveDataset,
            [&]() {
              resetCropKeypointEditorState(
                  crop_preview_window_state.editor_state);
            },
            [&]() {
              resetFullFrameKeypointEditState(
                  frame_debug_window_state.keypoint_review_panel
                      .full_frame_edit);
            },
            [&]() {
              invalidateReviewFrameCache(review_frame_cache);
              review_frame_status.clear();
              crop_preview_window_state.last_roi_index =
                  std::numeric_limits<int>::min();
              crop_preview_window_state.last_crop_preview_source_frame = -1;
              crop_preview_window_state.rotated_valid = false;
            },
        });
    double frame_camera_upload_ms = 0.0;
    int frame_camera_upload_count = 0;
    double frame_camera_texture_resize_ms = 0.0;
    double frame_camera_preview_resize_ms = 0.0;
    double frame_camera_display_convert_ms = 0.0;
    double frame_camera_pbo_copy_ms = 0.0;
    double frame_camera_texture_upload_ms = 0.0;
    double frame_camera_playback_front_path_ms = 0.0;
    double frame_camera_playback_stage_total_ms = 0.0;
    double frame_camera_playback_stage_upload_ms = 0.0;
    double frame_camera_playback_prewarm_total_ms = 0.0;
    double frame_camera_playback_prewarm_upload_ms = 0.0;
    int frame_camera_playback_prewarm_count = 0;
    double frame_camera_playback_swap_ms = 0.0;
    double frame_camera_plot_image_ui_ms = 0.0;
    double frame_camera_overlay_ui_ms = 0.0;
    double frame_bbox_get_boxes_ms = 0.0;
    double frame_bbox_edit_resolve_ms = 0.0;
    double frame_bbox_get_raw_detections_ms = 0.0;
    double frame_bbox_load_total_ms = 0.0;
    double frame_bbox_overlay_build_ms = 0.0;
    double frame_bbox_overlay_draw_ms = 0.0;
    int frame_bbox_overlay_item_count = 0;
    int frame_bbox_query_frame = -1;
    int frame_bbox_loaded_count = 0;
    int frame_bbox_display_count = 0;
    bool canonical_presented_request_this_frame = false;
    double frame_subject_shape_overlay_ms = 0.0;
    double frame_tail_kinematics_overlay_ms = 0.0;
    double frame_camera_scene_ui_ms = 0.0;
    double frame_file_browser_ui_ms = 0.0;
    double frame_frame_debug_ui_ms = 0.0;
    double frame_buffer_window_ui_ms = 0.0;
    double frame_crop_preview_ui_ms = 0.0;
    CropPreviewPerfMetrics frame_crop_preview_perf;
    double frame_stimulus_buffer_window_ui_ms = 0.0;
    double frame_keypoints_window_ui_ms = 0.0;
    double frame_labeling_tool_ui_ms = 0.0;
    double frame_stimulus_window_ui_ms = 0.0;
    double frame_stimulus_timeline_ui_ms = 0.0;
    double frame_movement_timeline_ui_ms = 0.0;
    AnalysisTimelinePerfStats frame_analysis_timeline_perf;
    double frame_help_menu_ui_ms = 0.0;
    double frame_gl_draw_ms = 0.0;
    double frame_swap_ms = 0.0;
    double frame_cap_sleep_ms = 0.0;
    double frame_ui_build_ms = 0.0;
    double frame_imgui_render_ms = 0.0;
    int frame_imgui_draw_cmd_count = 0;
    int frame_imgui_draw_list_count = 0;
    int frame_imgui_total_vtx_count = 0;
    int frame_imgui_total_idx_count = 0;
    double frame_mask_data_load_ms = 0.0;
    CameraViewMaskPerfMetrics frame_mask_overlay_perf;
    int playback_trace_presenter_view_idx = -1;
    int playback_trace_presenter_target_frame = -1;
    int playback_trace_presenter_preferred_paused_slot = -1;
    int playback_trace_presented_slot = -1;
    int playback_trace_presented_frame = -1;
    int playback_trace_presenter_resolved_frame = -1;
    bool playback_trace_prewarm_active = false;
    double perf_camera_viewport_x_px = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_viewport_y_px = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_viewport_width_px =
        std::numeric_limits<double>::quiet_NaN();
    double perf_camera_viewport_height_px =
        std::numeric_limits<double>::quiet_NaN();
    double perf_camera_media_x_px = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_media_y_px = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_media_width_px =
        std::numeric_limits<double>::quiet_NaN();
    double perf_camera_media_height_px =
        std::numeric_limits<double>::quiet_NaN();
    double perf_camera_view_x_min = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_view_x_max = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_view_y_min = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_view_y_max = std::numeric_limits<double>::quiet_NaN();
    double perf_camera_view_visible_fraction =
        std::numeric_limits<double>::quiet_NaN();
    int perf_camera_view_zoomed_in = -1;
    crimson::polar::ChaserDistancePolarScene perf_chaser_distance_polar_scene;
    double perf_chaser_distance_polar_origin_x_px =
        std::numeric_limits<double>::quiet_NaN();
    double perf_chaser_distance_polar_origin_y_px =
        std::numeric_limits<double>::quiet_NaN();
    crimson::stimulus::StimulusCameraOverlayScene
        perf_stimulus_camera_overlay_scene;
    double perf_stimulus_camera_overlay_origin_x_px =
        std::numeric_limits<double>::quiet_NaN();
    double perf_stimulus_camera_overlay_origin_y_px =
        std::numeric_limits<double>::quiet_NaN();
    int perf_requested_camera_frame = -1;
    int perf_min_decoded_camera_frame = -1;
    int playback_requested_camera_frame = -1;
    int playback_presenter_target_frame = ps.to_display_frame_number;
    int playback_presenter_target_slot = -1;
    bool playback_target_clamped_to_buffer = false;
    int playback_commit_previous_frame = -1;
    int playback_commit_frame = -1;
    int playback_commit_slot = -1;
    int playback_release_attempts = 0;
    int playback_release_count = 0;
    int playback_release_skip_count = 0;
    bool playback_release_deferred = false;

    // Poll and handle events (inputs, window resize, etc.)
    glfwPollEvents();
    if (ui_reference_capture.waitingForStableFrame()) {
      ui_reference.exact_presented_this_frame = false;
      ui_reference.crop_ready = false;
      ui_reference.crop_source_frame = -1;
      ui_reference.crop_source_label.clear();
    }
    if (ui_reference_capture.pollTimeout()) {
      std::cerr << "[UiReference] TIMEOUT state="
                << uiReferenceStateName(ui_reference.state)
                << " target_frame=" << ui_reference.target_frame
                << " presented_frame=" << ui_reference.presented_frame
                << " bbox_query_frame=" << ui_reference.bbox_query_frame
                << " target_stimulus_frame="
                << ui_reference.target_stimulus_frame
                << " presented_stimulus_frame="
                << ui_reference.presented_stimulus_frame
                << " crop_source_frame=" << ui_reference.crop_source_frame
                << " camera_buffer=" << ui_reference.camera_buffer_valid << "/"
                << ui_reference.camera_buffer_capacity
                << " stimulus_buffer=" << ui_reference.stimulus_buffer_valid
                << "/" << ui_reference.stimulus_buffer_capacity
                << " stable_frames="
                << ui_reference_capture.stableFrameCount() << std::endl;
      app_exit_code = 3;
      glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
    }
    if (playback_smoke.enabled && playback_smoke.started &&
        !playback_smoke.playback_started &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      playback_smoke.start_time).count() >=
            playback_smoke.warmup_s) {
      playback_smoke.playback_started = true;
      if (!ps.play_video) applyPlaybackToggleForPerf();
      writePlaybackTraceEvent("playback_smoke_warmup_complete",
                             {{"warmup_s", playback_smoke.warmup_s}});
      std::cout << "[PlaybackSmoke] warmup complete seconds="
                << playback_smoke.warmup_s << std::endl;
    }
    if (playback_smoke.enabled && playback_smoke.started &&
        !playback_smoke.completed &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      playback_smoke.start_time)
                .count() > playback_smoke.timeout_s) {
      int latest_decoded = -1;
      const int visible_idx =
          playback_session_controller.getVisibleCameraIndex();
      if (visible_idx >= 0 &&
          visible_idx < static_cast<int>(camera_names.size())) {
        auto latest_it = latest_decoded_frame.find(camera_names[visible_idx]);
        if (latest_it != latest_decoded_frame.end()) {
          latest_decoded = latest_it->second.load();
        }
      }
      std::cerr << "[PlaybackSmoke] TIMEOUT target_frame="
                << playback_smoke.end_frame
                << " current_frame=" << current_frame_num
                << " to_display_frame=" << ps.to_display_frame_number
                << " last_presented=" << playback_smoke.last_presented_frame
                << " max_presented=" << playback_smoke.max_presented_frame
                << " presented_count=" << playback_smoke.presented_count
                << " latest_decoded=" << latest_decoded << std::endl;
      writePlaybackTraceEvent(
          "playback_smoke_timeout",
          {{"start_frame", playback_smoke.start_frame},
           {"end_frame", playback_smoke.end_frame},
           {"current_frame", current_frame_num},
           {"to_display_frame", ps.to_display_frame_number},
           {"last_presented", playback_smoke.last_presented_frame},
           {"max_presented", playback_smoke.max_presented_frame},
           {"presented_count", playback_smoke.presented_count},
           {"latest_decoded", latest_decoded}},
          visible_idx, ps.to_display_frame_number, -1,
          playback_smoke.last_presented_slot,
          playback_smoke.last_presented_frame, current_frame_num);
      app_exit_code = 3;
      glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
    }
    if (clipped_boundary_smoke.enabled && clipped_boundary_smoke.started &&
        !clipped_boundary_smoke.completed &&
        std::chrono::steady_clock::now() - clipped_boundary_smoke.start_time >
            std::chrono::seconds(20)) {
      const auto canonical_metrics = canonical_detection_repository.metrics();
      std::cerr << "[ClippedBoundarySmoke] timeout waiting for frame "
                << clipped_boundary_smoke.end_frame
                << " current_frame=" << current_frame_num
                << " detection_source="
                << (canonical_detection_route ? "canonical" : "legacy")
                << " detection_state="
                << crimson::platform::nvidia::nvidiaDetectionStateName(
                       canonical_metrics.state)
                << " detection_run=" << canonical_metrics.canonical_raw_run
                << " frame_requests=" << canonical_metrics.frame_requests
                << " cache_hits=" << canonical_metrics.buffer.cache_hits
                << " cache_misses=" << canonical_metrics.cache_misses
                << " cached_pages="
                << canonical_metrics.buffer.peak_cached_pages
                << " cached_bytes=" << canonical_metrics.buffer.cached_bytes
                << " range_reads="
                << canonical_metrics.repository.range_reads
                << " resolved_rows="
                << canonical_metrics.repository.resolved_rows
                << " resident_range_reads="
                << canonical_metrics.repository.resident_range_reads
                << " residency_rows_read="
                << canonical_metrics.repository.residency_rows_read
                << " resident_bytes="
                << canonical_metrics.repository.resident_retained_bytes
                << " detection_error=" << canonical_metrics.last_error
                << " state=" << clippedStateJson().dump() << std::endl;
      writeClippedHandoffTraceEvent(
          "smoke_timeout", {{"start_frame", clipped_boundary_smoke.start_frame},
                            {"end_frame", clipped_boundary_smoke.end_frame},
                            {"current_frame", current_frame_num},
                            {"detection_source",
                             canonical_detection_route ? "canonical"
                                                       : "legacy"},
                            {"detection_state",
                             crimson::platform::nvidia::
                                 nvidiaDetectionStateName(
                                     canonical_metrics.state)},
                            {"detection_run",
                             canonical_metrics.canonical_raw_run},
                            {"frame_requests",
                             canonical_metrics.frame_requests},
                            {"cache_hits",
                             canonical_metrics.buffer.cache_hits},
                            {"cache_misses",
                             canonical_metrics.cache_misses},
                            {"cached_bytes",
                             canonical_metrics.buffer.cached_bytes},
                            {"range_reads",
                             canonical_metrics.repository.range_reads},
                            {"resolved_rows",
                             canonical_metrics.repository.resolved_rows},
                            {"resident_range_reads",
                             canonical_metrics.repository.resident_range_reads},
                            {"residency_rows_read",
                             canonical_metrics.repository.residency_rows_read},
                            {"resident_bytes",
                             canonical_metrics.repository
                                 .resident_retained_bytes},
                            {"detection_error",
                             canonical_metrics.last_error}});
      app_exit_code = 3;
      glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
    }

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
    ImGui::NewFrame();
    const auto ui_build_start = std::chrono::steady_clock::now();

    const auto seek_completion = playback_session_controller.pollSeekState();
    if (seek_completion.has_value() &&
        playback_transport.seekCoordinator().activeGeneration() != 0) {
      const auto completed_event =
          playback_transport.seekCoordinator().recordActive(*seek_completion);
      if (completed_event.has_value()) {
        const json details =
            playbackSeekEventDetails(*completed_event, "completion");
        writeClippedPlaybackStateEvent("transport_seek", details, true);
        writePlaybackTraceEvent("transport_seek", details);
      }
    }

    // --- Update playback time ---
    auto now = std::chrono::steady_clock::now();
    refreshPlaybackTimeline(now, false);
    playback_transport.setPlaybackRate(set_playback_speed, now);
    const auto transport_tick = playback_transport.update(now);
    const bool transport_stopped_playback =
        ps.play_video && !transport_tick.playing;
    ps.play_video = transport_tick.playing;
    ps.last_play_time_start = now;
    ps.accumulated_play_time =
        video_fps > 0.0
            ? playback_transport.requestedFramePosition(now) / video_fps
            : 0.0;
    if (transport_stopped_playback) {
      resetPlaybackStartPerf();
    }

    const bool clipped_collection_playback =
        media_session_loader.hasMappedMedia();
    const int requested_playback_frame = static_cast<int>(std::min<int64_t>(
        transport_tick.requested_frame, std::numeric_limits<int>::max()));
    int min_decoded_frame = INT_MAX;
    bool have_decode_bound = false;
    const bool should_plan_playback_target =
        !ps.just_seeked && dc_context->decoding_flag && ps.play_video &&
        scene->size_of_buffer > 0;
    if (should_plan_playback_target) {
      for (const auto &cam_name : camera_names) {
        const auto need_it = window_need_decoding.find(cam_name);
        const auto latest_it = latest_decoded_frame.find(cam_name);
        if (need_it == window_need_decoding.end() || !need_it->second.load() ||
            latest_it == latest_decoded_frame.end()) {
          continue;
        }
        const int decoded = latest_it->second.load();
        if (decoded >= 0) {
          min_decoded_frame = std::min(min_decoded_frame, decoded);
          have_decode_bound = true;
        }
      }
    }
    const auto playback_target =
        playback_session_controller.planPresentationTarget(
            requested_playback_frame,
            have_decode_bound ? std::optional<int>(min_decoded_frame)
                              : std::nullopt);
    if (playback_target.active) {
      playback_requested_camera_frame = requested_playback_frame;
      perf_requested_camera_frame = requested_playback_frame;
      perf_min_decoded_camera_frame = playback_target.minimum_decoded_frame;
      playback_presenter_target_frame = playback_target.frame;
      playback_presenter_target_slot = playback_target.slot;
      playback_target_clamped_to_buffer = playback_target.clamped_to_buffer;
      if (clipped_collection_playback && playback_target_clamped_to_buffer) {
        writeClippedPlaybackStateEvent(
            "playback_target_clamped_to_buffer",
            json{{"requested_frame", playback_target.bounded_target_frame},
                 {"selected_frame", playback_presenter_target_frame},
                 {"previous_committed_frame", ps.to_display_frame_number},
                 {"target_slot", playback_presenter_target_slot}},
            false);
      }
    }

    const auto file_browser_ui_start = std::chrono::steady_clock::now();
    if (video_loaded && !legacy_labeling_state.skeleton_chosen) {
      legacy_labeling_state.ensureSkeletonResources();
    }
    const std::string active_skeleton_name =
        legacy_labeling_state.activeSkeletonName();
    const bool has_active_zarr_keypoint_review =
        zarr_loaded && !keypoint_repository.descriptor().run_name.empty();
    FileBrowserWindowContext file_browser_context{
        ui_path_config,
        start_folder_name,
        root_dir,
        skeleton_dir,
        video_loaded,
        !has_active_zarr_keypoint_review,
        legacy_labeling_state.manual_label_mode,
        legacy_labeling_state.skeleton_chosen,
        active_skeleton_name,
        legacy_labeling_state.skeleton_map,
        cpu_buffer_toggle,
        scene->use_cpu_buffer,
        label_buffer_size,
        playback_preview_scale_mode,
        playback_renderer_mode,
        yolo_detection,
        ps.play_video,
        set_playback_speed,
        inst_speed,
        video_fps,
        current_frame_num,
        ps.last_frame_num_playspeed,
        ps.last_wall_time_playspeed,
        stimulus_player.loaded,
        stimulus_buffer_size,
        stimulus_use_cpu_buffer,
        stimulus_use_software_decode,
        stimulus_player.buffer_size,
        stimulus_player.use_cpu_buffer,
        stimulus_player.use_software_decode,
        dc_context->seek_interval,
        quality_timeline_session.detectionConfigured(),
        workspace_state.windowRequested(
            crimson::workspace::Window::DetectionQualityTimeline),
        quality_timeline_session.keypointConfigured(),
        workspace_state.windowRequested(
            crimson::workspace::Window::KeypointQualityTimeline),
    };
    FileBrowserWindowResult file_browser_result =
        drawFileBrowserWindow(file_browser_context, file_browser_window_state);
    if (file_browser_result.updated_path_config.has_value()) {
      ui_path_config = std::move(*file_browser_result.updated_path_config);
      start_folder_name = ui_path_config.default_start_path;
      std::cout << "[UIPathConfig] Applied default start path: "
                << start_folder_name << std::endl;
    }
    if (file_browser_result.detection_quality_requested.has_value()) {
      workspace_state.setWindowRequested(
          crimson::workspace::Window::DetectionQualityTimeline,
          *file_browser_result.detection_quality_requested);
    }
    if (file_browser_result.keypoint_quality_requested.has_value()) {
      workspace_state.setWindowRequested(
          crimson::workspace::Window::KeypointQualityTimeline,
          *file_browser_result.keypoint_quality_requested);
    }
    if (file_browser_result.skeleton_selection.has_value()) {
      const auto &selection = *file_browser_result.skeleton_selection;
      bool load_calibration = true;
      if (scene->num_cams > 1) {
        for (u32 i = 0; i < scene->num_cams; i++) {
          std::string cam_file =
              root_dir + "/calibration/" + camera_names[i] + ".yaml";

          if (!std::filesystem::exists(cam_file)) {
            load_calibration = false;
            error_message = "Calibration file not found: " + cam_file;
            show_error = true;
            break;
          }
          if (!camera_load_params_from_yaml(cam_file, camera_params[i],
                                            error_message)) {
            load_calibration = false;
            camera_params.clear();
            camera_params.resize(scene->num_cams);
            show_error = true;
            break;
          }
        }
      }

      if (load_calibration) {
        skeleton_initialize(selection.name, root_dir,
                            legacy_labeling_state.skeleton.get(),
                            selection.primitive);
        legacy_labeling_state.activateManualMode(root_dir);
      }
    }
    switch (file_browser_result.detection_action) {
    case FileBrowserDetectionAction::YOLOv5: {
      std::string yolov5_onnx = root_dir + "/yolo/v5/best.onnx";
      std::string yolov5_labelname = root_dir + "/yolo/v5/label.names";
      read_yolo_labels(yolov5_labelname, &yolo_setting);

      for (int i = 0; i < scene->num_cams; i++) {
        yolo_threads.push_back(
            std::thread(&yolo_process, yolov5_onnx, &yolo_setting, i));
      }
      yolo_detection = true;
      break;
    }
    case FileBrowserDetectionAction::YOLOv8: {
      std::string engine_file_path =
          root_dir + "/yolo/yolorat_bbox/rat_bbox.engine";
      for (int i = 0; i < scene->num_cams; i++) {
        yolo_threads.push_back(std::thread(&yolo_process_trt, engine_file_path,
                                           i, scene->cameras[i].image_width,
                                           scene->cameras[i].image_height));
      }
      yolo_detection = true;
      break;
    }
    case FileBrowserDetectionAction::YOLOv8Pose: {
      std::string engine_file_path =
          root_dir + "/yolo/yolopose/rat_pose.engine";
      for (int i = 0; i < scene->num_cams; i++) {
        yolo_threads.push_back(std::thread(
            &yolo_process_v8pose, engine_file_path, i,
            scene->cameras[i].image_width, scene->cameras[i].image_height));
      }
      yolo_detection = true;
      break;
    }
    case FileBrowserDetectionAction::None:
      break;
    }
    if (file_browser_result.accurate_seek_target_frame.has_value()) {
      playback_session_controller.seekToFrame(
          *file_browser_result.accurate_seek_target_frame, false);
    }
    frame_file_browser_ui_ms +=
        durationMs(std::chrono::steady_clock::now() - file_browser_ui_start);

    const bool use_legacy_manual_keypoint_tools =
        !canonical_detection_route &&
        legacy_labeling_state.toolsEnabled(has_active_zarr_keypoint_review);
    std::optional<RefinedKeypointSelection>
        active_full_frame_keypoint_selection;
    bool keypoint_tab_full_frame_edit_enabled = false;

    if (workspace_state.shouldSubmit(crimson::workspace::Window::FrameInspect,
                                     workspaceCapabilities())) {
      const auto frame_debug_ui_start = std::chrono::steady_clock::now();
      if (!use_legacy_manual_keypoint_tools) {
        legacy_labeling_state.keypoints_find = false;
      }
      std::vector<LoggedBoundingBox> zarr_boxes;
      const bool frame_has_bbox_edits =
          g_zarr_bbox_edit_state.isFrameDirty(current_frame_num);
      const bool subject_shape_needs_contours =
          subject_shape_overlay_options.show_overlay &&
          (subject_shape_overlay_options.show_body_contour ||
           subject_shape_overlay_options.show_swim_bladder_contour ||
           subject_shape_overlay_options.show_eye_contours);
      const bool include_subject_shapes_in_details =
          zarr_loaded && zarr_loader.hasSubjectShapeData() &&
          (subject_shape_overlay_options.show_overlay ||
           (zarr_loader.hasTailKinematicsData() &&
            tail_kinematics_overlay_options.show_overlay) ||
           (show_eye_masks && show_eye_angle_arcs &&
            zarr_loader.hasEyeAngleData()) ||
           frame_debug_window_state.active_view ==
               crimson::workspace::FrameInspectView::EyeMasks);
      const bool include_eye_masks_in_details =
          zarr_loaded && zarr_loader.hasEyeMasks() &&
          (show_eye_masks || subject_shape_needs_contours ||
           frame_debug_window_state.active_view ==
               crimson::workspace::FrameInspectView::EyeMasks);
      const bool keypoint_view_needs_legacy_details =
          frame_debug_window_state.active_view ==
          crimson::workspace::FrameInspectView::Keypoints;
      const bool allow_blocking_eye_mask_load = !ps.play_video;
      crimson::platform::nvidia::CameraFrameData frame_inspect_data =
          activeCameraFrameDataAdapter().resolve(
              crimson::platform::nvidia::CameraFrameDataRequest{
                  zarr_loaded,
                  current_frame_num >= 0,
                  current_frame_num,
                  zarr_loader.getImageWidth(),
                  zarr_loader.getImageHeight(),
                  !canonical_detection_route,
                  !canonical_detection_route &&
                      (include_eye_masks_in_details ||
                       include_subject_shapes_in_details),
                  !canonical_detection_route &&
                      keypoint_view_needs_legacy_details,
                  !canonical_detection_route,
                  include_eye_masks_in_details,
                  include_subject_shapes_in_details,
                  allow_blocking_eye_mask_load,
                  kPlaybackMaskPrefetchLookaheadFrames,
              });
      crimson::gui::CanonicalOverlaySnapshot inspect_overlays;
      if (canonical_detection_route) {
        const bool inspect_masks = frame_debug_window_state.active_view ==
            crimson::workspace::FrameInspectView::EyeMasks;
        requestCanonicalOverlayFrame(current_frame_num, true,
            show_eye_masks || inspect_masks,
            subject_shape_overlay_options.show_overlay || show_heading_arrows || inspect_masks);
        auto presentation = crimson::gui::makeCanonicalOverlayPresentation(
            canonical_overlay_session.snapshot(current_frame_num), 0, current_frame_num,
            static_cast<int>(canonical_detection_expected_width),
            static_cast<int>(canonical_detection_expected_height), {});
        inspect_overlays = std::move(presentation.snapshot);
        frame_inspect_data.keypoint_descriptor = inspect_overlays.keypoints.descriptor;
        frame_inspect_data.keypoint_frame = std::move(presentation.keypoints);
        frame_inspect_data.keypoint_frame_requested = presentation.keypoints_ready;
      }
      const auto &detection_descriptor =
          frame_inspect_data.detection_descriptor;
      const auto *detection_frame_ptr =
          frame_inspect_data.detection_frame_ready
              ? &frame_inspect_data.detection_frame
              : nullptr;
      const auto &keypoint_descriptor = frame_inspect_data.keypoint_descriptor;
      const auto *keypoint_descriptor_ptr =
          !keypoint_descriptor.run_name.empty() ? &keypoint_descriptor
                                                : nullptr;
      const auto *keypoint_frame_ptr =
          frame_inspect_data.keypoint_frame_requested
              ? &frame_inspect_data.keypoint_frame
              : nullptr;
      const auto *detection_details_ptr =
          frame_inspect_data.legacy_details_ready
              ? &frame_inspect_data.legacy_details
              : nullptr;
      const bool dataset_has_synthetic_boxes =
          detection_descriptor.active_dataset_has_synthetic_observations;
      const bool frame_is_interpolated =
          frame_inspect_data.frame_is_interpolated;
      const bool dataset_allows_bbox_edit =
          frame_inspect_data.dataset_allows_bbox_edit;
      if (zarr_loaded) {
        zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(
            current_frame_num, frame_inspect_data.source_boxes);
        if (!dataset_allows_bbox_edit) {
          g_zarr_bbox_edit_state.draw_mode = false;
          g_zarr_bbox_edit_state.cancelDraw();
          g_zarr_bbox_edit_state.clearSelection();
        }
        if (include_eye_masks_in_details &&
            frame_inspect_data.legacy_details_requested) {
          frame_mask_data_load_ms +=
              frame_inspect_data.metrics.legacy_details_ms;
        }
      }

      FrameDebugWindowContext frame_debug_context{
          current_frame_num,
          ps.to_display_frame_number,
          ps.slider_frame_number,
          frame_sync_valid_slots,
          frame_sync_empty_slots,
          scene->size_of_buffer,
          frame_sync_recording_remaining,
          frame_sync_recording_total,
          frame_sync_latest_decoded,
          frame_sync_debug_line,
          zarr_loaded,
          zarr_loader,
          &stimulus_repository,
          detection_descriptor,
          detection_frame_ptr,
          keypoint_descriptor_ptr,
          keypoint_frame_ptr,
          chaser_distance_polar_repository != nullptr
              ? &chaser_distance_polar_repository->descriptor()
              : nullptr,
          detection_dataset_labels,
          detection_dataset_choice,
          dataset_has_synthetic_boxes,
          frame_is_interpolated,
          frame_has_bbox_edits,
          dataset_allows_bbox_edit,
          zarr_boxes,
          detection_details_ptr,
          review_frame_filters,
          review_frame_cache.valid,
          review_frame_cache.frames.size(),
          review_frame_status,
          decode_debug_status,
          default_buffer_dump_root.string(),
          g_zarr_bbox_edit_state,
          ps.play_video,
          bbox_payload_status,
          show_keypoint_markers,
          show_heading_arrows,
          show_eye_masks,
          show_subject_body_mask,
          show_eye_left_mask,
          show_eye_right_mask,
          show_swim_bladder_mask,
          show_eye_direction_beams,
          show_eye_gaze_rays,
          show_eye_angle_arcs,
          show_eye_angle_labels,
          mask_overlay_mode,
          subject_shape_overlay_options,
          tail_kinematics_overlay_options,
          show_movement_trail,
          movement_trail_seconds,
          movement_trail_valid_samples_only,
          stimulus_inset_options,
          chaser_distance_polar_inset_options,
          show_stimulus_debug_windows,
      };
      frame_debug_context.canonical_overlays = canonical_detection_route ? &inspect_overlays : nullptr;
      const auto requested_frame_inspect_view =
          workspace_state.selections().frame_inspect_view;
      const crimson::app::FrameInspectTabState frame_inspect_tab_state{
          frame_debug_window_state.active_view,
          frame_debug_window_state.view_sync};
      (void)crimson::app::prepareFrameInspectPresentation(
          frame_inspect_tab_state, {requested_frame_inspect_view});
      const FrameDebugWindowResult frame_debug_result =
          drawFrameDebugWindow(frame_debug_context, frame_debug_window_state);
      const DiagnosticsWindowResult diagnostics_result =
          drawDiagnosticsWindow(frame_debug_context);

      crimson::app::observeFrameInspectActiveView(
          frame_inspect_tab_state, frame_debug_window_state.active_view);
      const auto frame_inspect_presentation =
          crimson::nvidia::makeFrameInspectPresentationOutput(
              frame_debug_result, workspace_state.overlayControls(),
              frame_debug_window_state.keypoint_review_panel.full_frame_edit
                  .enabled);
      crimson::app::applyFrameInspectPresentation(
          &frame_inspect_controller_state, frame_inspect_tab_state,
          frame_inspect_presentation, &workspace_state);

      show_keypoint_markers = frame_debug_result.show_keypoint_markers;
      show_heading_arrows = frame_debug_result.show_heading_arrows;
      show_eye_masks = frame_debug_result.show_eye_masks;
      show_subject_body_mask = frame_debug_result.show_subject_body_mask;
      show_eye_left_mask = frame_debug_result.show_eye_left_mask;
      show_eye_right_mask = frame_debug_result.show_eye_right_mask;
      show_swim_bladder_mask = frame_debug_result.show_swim_bladder_mask;
      show_eye_direction_beams = frame_debug_result.show_eye_direction_beams;
      show_eye_gaze_rays = frame_debug_result.show_eye_gaze_rays;
      show_eye_angle_arcs = frame_debug_result.show_eye_angle_arcs;
      show_eye_angle_labels = frame_debug_result.show_eye_angle_labels;
      mask_overlay_mode = frame_debug_result.mask_overlay_mode;
      subject_shape_overlay_options =
          frame_debug_result.subject_shape_overlay_options;
      tail_kinematics_overlay_options =
          frame_debug_result.tail_kinematics_overlay_options;
      show_movement_trail = frame_debug_result.show_movement_trail;
      movement_trail_seconds = frame_debug_result.movement_trail_seconds;
      movement_trail_valid_samples_only =
          frame_debug_result.movement_trail_valid_samples_only;
      stimulus_inset_options = frame_debug_result.stimulus_inset_options;
      chaser_distance_polar_inset_options =
          frame_debug_result.chaser_distance_polar_inset_options;
      show_stimulus_debug_windows =
          frame_debug_result.show_stimulus_debug_windows;
      active_full_frame_keypoint_selection =
          frame_debug_result.selected_keypoint_selection;
      keypoint_tab_full_frame_edit_enabled =
          frame_inspect_controller_state.keypoint_full_frame_edit_enabled;

      const auto frame_inspect_navigation_request =
          crimson::nvidia::makeFrameInspectNavigationRequest(
              frame_debug_result);
      auto frame_inspect_navigation_context =
          crimson::nvidia::FrameInspectNavigationContext{
              zarr_loaded,
              zarr_loader,
              activeDetectionRepository(),
              current_frame_num,
              detection_dataset_ids,
              detection_dataset_choice,
              review_frame_filters,
              review_frame_cache,
              review_frame_status,
              g_zarr_bbox_edit_state,
              frame_debug_window_state,
              [&](int frame, bool prefer_buffer_when_paused) {
                playback_session_controller.seekToFrame(
                    frame, prefer_buffer_when_paused);
              },
              [&]() { refreshActiveDetectionDatasetOptions(); },
          };
      const auto frame_inspect_navigation_result =
          crimson::nvidia::applyFrameInspectNavigation(
              frame_inspect_navigation_request,
              frame_inspect_navigation_context);
      if (diagnostics_result.request_dump_decode_buffers) {
        dumpDecodeBuffersToVideos(makeDecodeDebugDumpContext(), "manual_dump",
                                  decode_debug_status);
      }
      if (diagnostics_result.request_random_seek_dump) {
        randomSeekAndDumpBuffers(
            RandomSeekDumpContext{
                makeDecodeDebugDumpContext(),
                dc_context,
                &debug_rng,
                [&](int target_frame, bool prefer_buffer_when_paused) {
                  playback_session_controller.seekToFrame(
                      target_frame, prefer_buffer_when_paused);
                },
                [&](bool enabled) {
                  playback_session_controller.setCameraDecodeRequests(enabled);
                },
            },
            decode_debug_status);
      }
      if (crimson::nvidia::containsFrameInspectCommand(
              frame_inspect_navigation_result.unhandled_commands,
              crimson::app::FrameInspectCommandKind::
                  BuildManualPayloadPreview)) {
        manual_payload_preview = buildManualDetectPayloadPreview(
            zarr_loaded, zarr_loader, g_zarr_bbox_edit_state,
            scene->num_cams > 0
                ? static_cast<int>(scene->cameras[0].image_width)
                : 0,
            scene->num_cams > 0
                ? static_cast<int>(scene->cameras[0].image_height)
                : 0);
        if (!manual_payload_preview->valid) {
          bbox_payload_status =
              "Manual payload preview failed: " + manual_payload_preview->error;
        } else {
          bbox_payload_status = summarizeManualDetectPayloadPreview(
              *manual_payload_preview,
              g_zarr_bbox_edit_state.dirtyFrameCount());
        }
      }
      if (crimson::nvidia::containsFrameInspectCommand(
              frame_inspect_navigation_result.unhandled_commands,
              crimson::app::FrameInspectCommandKind::WriteManualPayload)) {
        if (zarr_loaded && zarr_loader.hasClippedCollection()) {
          manual_payload_preview.reset();
          bbox_payload_status =
              "Manual write disabled for clipped finalized collections.";
        } else {
          manual_payload_preview = buildManualDetectPayloadPreview(
              zarr_loaded, zarr_loader, g_zarr_bbox_edit_state,
              scene->num_cams > 0
                  ? static_cast<int>(scene->cameras[0].image_width)
                  : 0,
              scene->num_cams > 0
                  ? static_cast<int>(scene->cameras[0].image_height)
                  : 0);
          if (!manual_payload_preview->valid) {
            bbox_payload_status =
                "Manual write failed: payload preview invalid: " +
                manual_payload_preview->error;
          } else {
            std::string source_variant = "interpolated";
            if (detection_repository.descriptor().active_dataset ==
                crimson::zarr::DetectionDataset::RefinedFiltered) {
              source_variant = "filtered";
            }

            std::string write_error;
            std::string resolved_refined_run;
            ManualWriteReviewOptions review_opts;
            const auto review_metadata = resolveReviewMetadataValues(
                frame_debug_window_state.manual_write_review);
            review_opts.intended_use = review_metadata.intended_use;
            review_opts.state = review_metadata.review_state;
            review_opts.method = review_metadata.method;
            review_opts.reviewer = review_metadata.reviewer;
            review_opts.notes = review_metadata.notes;
            const bool write_ok = zarr_loader.writeManualRefinedDetections(
                manual_payload_preview->frame_indices,
                manual_payload_preview->bbox_norm_coords,
                manual_payload_preview->scores,
                manual_payload_preview->class_ids,
                manual_payload_preview->frame_counts,
                manual_payload_preview->detection_source,
                manual_payload_preview->reason, "manual", source_variant,
                write_error, &resolved_refined_run, review_opts);
            if (!write_ok) {
              bbox_payload_status = "Manual write failed: " + write_error;
            } else {
              const size_t written_detections =
                  manual_payload_preview->total_detections;
              std::string reload_error;
              const std::string archive_path = zarr_loader.getArchivePath();
              if (!archive_path.empty() &&
                  zarr_loader.loadZarrFile(archive_path, reload_error)) {
                zarr_loaded = true;
                if (detection_repository.isDatasetAvailable(
                        crimson::zarr::DetectionDataset::RefinedRoot)) {
                  (void)detection_repository.selectDataset(
                      crimson::zarr::DetectionDataset::RefinedRoot);
                }
                refreshDetectionDatasetOptions(detection_repository);
                g_zarr_bbox_edit_state.clearAll();
                manual_payload_preview.reset();
                invalidateReviewFrameCache(review_frame_cache);
                review_frame_status.clear();
                if (zarr_loader.getTotalFrames() > 0 &&
                    current_frame_num >=
                        static_cast<int>(zarr_loader.getTotalFrames())) {
                  current_frame_num =
                      static_cast<int>(zarr_loader.getTotalFrames()) - 1;
                }
                warmEyeMaskCacheForFrame("manual_write_reload",
                                         current_frame_num);
                prewarmEyeMaskOverlayTexturesForPlayback("manual_write_reload",
                                                         current_frame_num);
                refreshChaserDistancePolarRepository();
                std::ostringstream payload_msg;
                payload_msg
                    << "Manual write complete: run="
                    << (resolved_refined_run.empty() ? "<latest>"
                                                     : resolved_refined_run)
                    << " surface=instances"
                    << " resolved_group=refined"
                    << " detections=" << written_detections;
                bbox_payload_status = payload_msg.str();
              } else {
                zarr_loaded = false;
                refreshChaserDistancePolarRepository();
                g_zarr_bbox_edit_state.clearAll();
                bbox_payload_status =
                    "Manual write succeeded but reload failed: " + reload_error;
              }
            }
          }
        }
      }
      if (crimson::nvidia::containsFrameInspectCommand(
              frame_inspect_navigation_result.unhandled_commands,
              crimson::app::FrameInspectCommandKind::WriteKeypointReview)) {
        RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
        const RefinedKeypointReviewWriteWorkflowResult review_write_result =
            applyRefinedKeypointReviewWrite(
                refined_keypoint_repo,
                RefinedKeypointReviewPanelResult{
                    frame_debug_result.selected_keypoint_selection,
                    CropKeypointEditorAction{},
                    frame_debug_result.request_keypoint_review_write,
                    frame_debug_result.keypoint_review_options,
                },
                frame_debug_window_state.keypoint_review_panel
                    .review_write_status,
                reloadActiveZarrPreserveDataset);
        if (review_write_result.should_clear_zarr_loaded) {
          zarr_loaded = false;
          refreshChaserDistancePolarRepository();
        }
      }
      if (frame_debug_result.keypoint_edit_action.type !=
          CropKeypointEditorActionType::None) {
        startRefinedKeypointWrite(frame_debug_result.keypoint_edit_action,
                                  frame_debug_result
                                      .selected_keypoint_selection);
      }
      frame_frame_debug_ui_ms +=
          durationMs(std::chrono::steady_clock::now() - frame_debug_ui_start);
    }

    // file explorer display
    if (ImGuiFileDialog::Instance()->Display("ChooseMedia")) {
      if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
        auto selected_files = ImGuiFileDialog::Instance()->GetSelection();
        root_dir = ImGuiFileDialog::Instance()->GetCurrentPath();
        skeleton_dir = root_dir;
        crimson::session::SessionDescriptor requested_session =
            session_lifecycle.snapshot().active;
        if (!selected_files.empty()) {
          requested_session.video_path = selected_files.begin()->second;
        }
        recording_open_workflow.begin(
            {requested_session,
             "Opening media session",
             {{"archive",
               crimson::session::ProductAvailabilityRequirement::Optional},
              {"media",
               crimson::session::ProductAvailabilityRequirement::Required}}});

        // Reset any previously loaded stimulus video
        destroyStimulusPlayback(stimulus_player);
        window_need_decoding[stimulus_player.window_name].store(false);
        window_was_decoding[stimulus_player.window_name] = false;

        // Try to load Zarr detection file
        recording_open_workflow.startProduct("archive",
                                             "Resolving recording archive");
        std::string zarr_error;
        if (loadZarrDetectionFromDirectory(root_dir, zarr_loader, zarr_error)) {
          zarr_loaded = true;
          refreshDetectionDatasetOptions(detection_repository);
          refreshChaserDistancePolarRepository();
          g_zarr_bbox_edit_state.clearAll();
          warmEyeMaskCacheForFrame("media_dialog_zarr", current_frame_num);
          prewarmEyeMaskOverlayTexturesForPlayback("media_dialog_zarr",
                                                   current_frame_num);
          requested_session.zarr_path = zarr_loader.getArchivePath();
          recording_open_workflow.completeProduct(
              "archive", "Recording archive ready", true);
        } else {
          zarr_loaded = false;
          refreshChaserDistancePolarRepository();
          g_zarr_bbox_edit_state.clearAll();
          std::cout << "No Zarr detection file found (optional): " << zarr_error
                    << std::endl;
          detection_dataset_ids.clear();
          detection_dataset_labels.clear();
          detection_dataset_choice = 0;
          requested_session.zarr_path.clear();
          recording_open_workflow.completeProduct(
              "archive", "No recording archive", false);
        }

        recording_open_workflow.startProduct("media", "Opening camera media");
        std::vector<crimson::media::CameraMediaSelection> media_selections;
        media_selections.reserve(selected_files.size());
        for (const auto &selection : selected_files) {
          media_selections.push_back({selection.first, selection.second});
        }
        std::string media_error;
        const bool media_ready = media_session_loader.loadSelectedCameraMedia(
            media_selections, media_error);
        if (media_ready) {
          refreshPlaybackTimeline(std::chrono::steady_clock::now(), true);
          ps.play_video = playback_transport.isPlaying();
          recording_open_workflow.completeProduct("media", "Camera media ready",
                                                  true);
          std::string transaction_error;
          recording_open_workflow.commit(
              requested_session, "Media session ready",
              "Media session unavailable", &transaction_error);
        } else {
          if (media_error.empty()) {
            media_error = "Selected camera media could not be opened";
          }
          recording_open_workflow.failProduct(
              "media", "Camera media unavailable", media_error,
              "Media session unavailable");
        }
        crimson::diagnostics::writeRuntimeDiagnostics(
            std::cout, "Nvidia",
            {session_lifecycle.snapshot(), session_loading_progress.snapshot(),
             std::nullopt});
      }
      // close
      ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("ChooseZarrArchive")) {
      if (ImGuiFileDialog::Instance()->IsOk()) {
        auto selection = ImGuiFileDialog::Instance()->GetSelection();
        std::string selected_zarr_path;
        if (!selection.empty()) {
          selected_zarr_path = selection.begin()->second;
        } else {
          selected_zarr_path = ImGuiFileDialog::Instance()->GetCurrentPath();
        }

        const auto archive_result = crimson::session::executeArchiveOpen(
            recording_open_workflow,
            {session_lifecycle.snapshot().active, selected_zarr_path,
             current_frame_num},
            {
                [&](const std::string &path, std::string &resolved_path,
                    std::string &error) {
                  if (!loadZarrDetectionFromPath(path, zarr_loader, error)) {
                    return false;
                  }
                  resolved_path = zarr_loader.getArchivePath();
                  return true;
                },
                [&](int64_t frame) {
                  zarr_loaded = true;
                  refreshDetectionDatasetOptions(detection_repository);
                  refreshChaserDistancePolarRepository();
                  g_zarr_bbox_edit_state.clearAll();
                  invalidateReviewFrameCache(review_frame_cache);
                  review_frame_status.clear();
                  warmEyeMaskCacheForFrame("zarr_dialog",
                                           static_cast<int>(frame));
                  prewarmEyeMaskOverlayTexturesForPlayback(
                      "zarr_dialog", static_cast<int>(frame));
                  std::cout << "Loaded Zarr archive override: "
                            << zarr_loader.getArchivePath() << std::endl;
                },
                [&]() {
                  zarr_loaded = false;
                  refreshChaserDistancePolarRepository();
                  g_zarr_bbox_edit_state.clearAll();
                  invalidateReviewFrameCache(review_frame_cache);
                  review_frame_status.clear();
                  detection_dataset_ids.clear();
                  detection_dataset_labels.clear();
                  detection_dataset_choice = 0;
                },
                [&]() {
                  media_session_loader.tryAutoLoadAffiliatedVideoFromZarr(
                      "Load Zarr Archive");
                },
                [&]() {
                  media_session_loader.tryAutoLoadStimulusVideo("file-dialog");
                },
                [&]() {
                  return media_session_loader.activeRecordingClipIndexPath();
                },
            });
        if (!archive_result.ready) {
          zarr_loaded = false;
          std::cout << "Failed to load Zarr archive override: "
                    << archive_result.error << std::endl;
        }
        crimson::diagnostics::writeRuntimeDiagnostics(
            std::cout, "Nvidia",
            {session_lifecycle.snapshot(), session_loading_progress.snapshot(),
             std::nullopt});
      }
      ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("ChooseStimulus")) {
      if (ImGuiFileDialog::Instance()->IsOk()) {
        auto selection = ImGuiFileDialog::Instance()->GetSelection();
        if (!selection.empty()) {
          std::string stimulus_path = selection.begin()->second;
          const auto stimulus_result = crimson::session::executeStimulusOpen(
              recording_open_workflow,
              {session_lifecycle.snapshot().active,
               {stimulus_path, stimulus_buffer_size, stimulus_use_cpu_buffer,
                stimulus_use_software_decode, zarr_loaded,
                ps.to_display_frame_number, !ps.play_video}},
              {[&](const crimson::media::StimulusMediaOpenRequest &request) {
                 return media_session_loader.openStimulusMedia(request);
               },
               {}});
          if (!stimulus_result.ready) {
            show_error = true;
            error_message = stimulus_result.media.error;
          }
          crimson::diagnostics::writeRuntimeDiagnostics(
              std::cout, "Nvidia",
              {session_lifecycle.snapshot(),
               session_loading_progress.snapshot(), std::nullopt});
        }
      }
      ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("ChooseSkeleton")) {
      if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
        auto skeleton_file = ImGuiFileDialog::Instance()->GetSelection();

        if (!skeleton_file.empty()) {

          bool load_calibration = true;
          if (scene->num_cams > 1) {
            for (u32 i = 0; i < scene->num_cams; i++) {
              std::string cam_file =
                  root_dir + "/calibration/" + camera_names[i] + ".yaml";
              if (!camera_load_params_from_yaml(cam_file, camera_params[i],
                                                error_message)) {
                load_calibration = false;
                camera_params.clear();
                camera_params.resize(scene->num_cams);
                show_error = true;
                break;
              }
            }
          }

          if (load_calibration) {
            skeleton_dir = ImGuiFileDialog::Instance()->GetCurrentPath();
            skeleton_initialize("", skeleton_file.begin()->second,
                                legacy_labeling_state.skeleton.get(), SP_LOAD);
            legacy_labeling_state.activateManualMode(root_dir);
          }
        }
      }
      // close
      ImGuiFileDialog::Instance()->Close();
    }

    static int select_corr_head = 0;
    if (workspace_state.shouldSubmit(crimson::workspace::Window::FramesInBuffer,
                                     workspaceCapabilities())) {
      int visible_idx = 0;
      if (!ps.pause_seeked) {
        for (int i = 0; i < scene->num_cams; i++) {
          if (window_was_decoding[camera_names[i]]) {
            visible_idx = i;
            break;
          }
        }
      }

      const auto buffered_frames =
          crimson::platform::nvidia::snapshotFrameBuffer(
              scene->cameras[visible_idx].display_buffer,
              scene->size_of_buffer);
      const auto buffer_model =
          crimson::playback::buildPlaybackBufferBrowserModel(
              buffered_frames, ps.to_display_frame_number);
      std::optional<crimson::gui::FrameBufferResumeSummary> last_resume;
      if (ps.last_resume_path != ResumePath::None) {
        last_resume = crimson::gui::FrameBufferResumeSummary{
            resumePathName(ps.last_resume_path), ps.last_resume_target_frame};
      }
      const auto buffer_window = crimson::gui::drawFrameBufferWindow(
          {buffer_model, scene->size_of_buffer,
           ps.paused_frame_on_toggle >= 0
               ? std::optional<int64_t>(ps.paused_frame_on_toggle)
               : std::nullopt,
           ps.buffer_browsed_since_pause, last_resume});
      frame_buffer_window_ui_ms += buffer_window.draw_ms;

      select_corr_head = buffer_model.preferred_slot.value_or(-1);
      if (buffer_window.selection) {
        const int previous_frame = ps.to_display_frame_number;
        const int selected_frame =
            static_cast<int>(buffer_window.selection->frame_number);
        ps.to_display_frame_number = selected_frame;
        ps.slider_frame_number = selected_frame;
        ps.pause_seeked = true;
        ps.buffer_browsed_since_pause =
            ps.paused_frame_on_toggle >= 0 &&
            selected_frame != ps.paused_frame_on_toggle;
        select_corr_head = buffer_window.selection->slot.value_or(-1);
        writeClippedPlaybackStateEvent("paused_buffer_select",
                                       json{{"previous_frame", previous_frame},
                                            {"target_frame", selected_frame},
                                            {"slot", select_corr_head}},
                                       true);
      }
      if (select_corr_head >= 0) {
        ps.read_head = select_corr_head;
        current_frame_num = ps.to_display_frame_number;
        // Keep paused seek target stable unless the user explicitly
        // selects/seeks a different frame.
      } else {
        current_frame_num = std::max(0, ps.to_display_frame_number);
      }
    }

    // Render a video frame
    if (video_loaded) {
      if (ps.play_video && scene->num_cams > 0 && scene->size_of_buffer > 0) {
        int live_frame =
            scene->cameras[0]
                .display_buffer[ps.read_head % scene->size_of_buffer]
                .frame_number;
        if (live_frame >= 0) {
          current_frame_num = live_frame;
        } else {
          current_frame_num = ps.to_display_frame_number;
        }
        (void)clipped_media_coordinator.onPresentedFrame(current_frame_num,
                                                         ps.play_video);
      }
      const bool freeze_stimulus_during_paused_browse =
          !ps.play_video && ps.pause_seeked && ps.buffer_browsed_since_pause;
      const bool freeze_stimulus_during_seek =
          seek_progress.state == SeekState::WaitingCameras ||
          seek_progress.state == SeekState::WaitingStimulus;
      if (zarr_loaded && stimulus_repository.hasMapping()) {
        if (!freeze_stimulus_during_paused_browse &&
            !freeze_stimulus_during_seek) {
          int stim_source_frame =
              ps.play_video ? current_frame_num : ps.to_display_frame_number;
          if (auto stim_frame = crimson::zarr::StimulusFrameForCamera(
                  &stimulus_repository, stim_source_frame)) {
            ps.current_stimulus_frame = *stim_frame;
          } else {
            ps.current_stimulus_frame = -1;
          }
        }
      } else {
        ps.current_stimulus_frame = -1;
      }
      if (stimulus_player.loaded) {
        const auto stimulus_presentation_result =
            updateStimulusPlaybackPresentation(
                StimulusPlaybackPresentationContext{
                    stimulus_player,
                    zarr_loaded ? &stimulus_repository : nullptr,
                    ps,
                    seek_progress,
                    current_frame_num,
                    video_fps,
                    latest_decoded_frame[stimulus_player.window_name].load(),
                    window_need_decoding[stimulus_player.window_name].load(),
                    &stimulus_catchup_seek_generation,
                },
                stimulus_playback_presentation_state);
        window_need_decoding[stimulus_player.window_name].store(
            stimulus_presentation_result.decoder_requested);
      }
      const int paused_visible_idx =
          ps.play_video ? -1
                        : playback_session_controller.getVisibleCameraIndex();
      for (int j = 0; j < scene->num_cams; j++) {
        const std::string &win_name = camera_names[j];

        // layout
        ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
        ImVec2 window_pos;

        if (scene->num_cams < 8) {
          if (j % 2 == 0) {
            window_pos.y = 200.0;
            window_pos.x = (j / 2.0) * 500;
          } else {
            window_pos.y = 600.0;
            window_pos.x = (j - 1) / 2.0 * 500;
          }
        } else {
          int row = j % 4;
          float base_x = (row == 0) ? j : (j - 1);
          float x_group = (base_x / 4.0f) * 500;
          switch (row) {
          case 0:
            window_pos = {x_group, 200.0f};
            break;
          case 1:
            window_pos = {x_group, 600.0f};
            break;
          case 2:
            window_pos = {x_group, 1000.0f};
            break;
          case 3:
            window_pos = {x_group, 1400.0f};
            break;
          }
        }

        ImGui::SetNextWindowPos(window_pos, ImGuiCond_FirstUseEver);
        // A clip handoff changes the decoder stream name, not the viewport.
        // Keep the clip in the visible title, but scope ImGui state (including
        // its active transport slider) to the stable recording and camera.
        std::string camera_window_label = win_name;
        if (clipped_media_state.source == ClippedMediaSource::RecordingClipIndex &&
            clipped_media_state.recording_clip_provider) {
          const auto &index = clipped_media_state.recording_clip_provider->index();
          const std::string recording_identity = index.recordingId().empty()
              ? "index:" + index.indexPath().generic_string()
              : "recording:" + index.recordingId();
          camera_window_label = crimson::gui::cameraViewWindowLabel(
              win_name, recording_identity, index.cameraSerial(), j);
        } else if (clipped_media_state.source == ClippedMediaSource::LegacyZarrCollection &&
                   zarr_loaded) {
          camera_window_label = crimson::gui::cameraViewWindowLabel(
              win_name, "archive:" + zarr_loader.getArchivePath(),
              clipped_media_state.camera_serial, j);
        }
        bool is_visible = ImGui::Begin(camera_window_label.c_str());

        if (!window_was_decoding[win_name] && is_visible && ps.play_video) {
          // seek if visibility has changed
          playback_session_controller.seekToFrame(current_frame_num, true);
        }

        if (!window_was_decoding[win_name] && is_visible && !ps.play_video &&
            !ps.pause_seeked) {
          // seek if visibility has changed
          playback_session_controller.seekToFrame(current_frame_num, true);
          for (auto &[key, value] : window_need_decoding) {
            value.store(true);
          }
        }

        if (ps.play_video) {
          window_need_decoding[win_name].store(is_visible);
        };

        if (is_visible) {
          const bool prewarm_playback_textures =
              !ps.play_video && video_loaded && !yolo_detection &&
              playbackLightweightRendererIsActive(true, playback_renderer_mode);
          const bool playback_upload_mode_active =
              ps.play_video || prewarm_playback_textures;
          const CameraViewPresenterContext camera_view_presenter_context{
              scene,
              j,
              current_frame_num,
              ps.play_video ? playback_presenter_target_frame
                            : ps.to_display_frame_number,
              ps.read_head,
              select_corr_head,
              ps.play_video,
              ps.pause_seeked,
              ps.rejected_seek_ring_quarantined,
              yolo_detection,
              playbackLightweightRendererIsActive(playback_upload_mode_active,
                                                  playback_renderer_mode),
              playbackPreviewIsActive(playback_upload_mode_active,
                                      yolo_detection,
                                      playback_preview_scale_mode),
              prewarm_playback_textures,
              playbackPreviewScaleFactor(playback_preview_scale_mode),
              playback_preview_scale_mode,
          };
          const CameraViewPresenterResult camera_view_presenter_result =
              presentCameraViewFrame(camera_view_presenter_context);

          unsigned char *presented_rgba_cuda_buffer =
              camera_view_presenter_result.presented_rgba_cuda_buffer;
          bool swap_playback_surface_after_draw =
              camera_view_presenter_result.swap_playback_surface_after_draw;
          int presented_slot = camera_view_presenter_result.presented_slot;
          int presented_frame = camera_view_presenter_result.presented_frame;
          current_frame_num =
              camera_view_presenter_result.resolved_current_frame_num;
          bool playback_surface_swapped_before_draw = false;
          if (swap_playback_surface_after_draw) {
            auto &camera = scene->cameras[j];
            const auto swap_start = std::chrono::steady_clock::now();
            std::swap(camera.image_texture, camera.playback_staging_texture);
            render_refresh_camera_presentation_texture(&camera);
            std::swap(camera.pbo_cuda, camera.playback_staging_pbo);
            std::swap(camera.applied_preview_sampling_mode,
                      camera.playback_staging_preview_sampling_mode);
            const int previous_front_frame = camera.last_uploaded_frame;
            const int previous_front_local_frame =
                camera.last_uploaded_local_frame;
            const int64_t previous_front_pts = camera.last_uploaded_pts;
            const bool previous_front_valid = camera.texture_has_valid_frame;
            camera.last_uploaded_frame = camera.playback_staging_frame;
            camera.last_uploaded_local_frame =
                camera.playback_staging_local_frame;
            camera.last_uploaded_pts = camera.playback_staging_pts;
            camera.texture_has_valid_frame = camera.playback_staging_valid;
            camera.playback_staging_frame = previous_front_frame;
            camera.playback_staging_local_frame = previous_front_local_frame;
            camera.playback_staging_pts = previous_front_pts;
            camera.playback_staging_valid = previous_front_valid;
            frame_camera_playback_swap_ms +=
                durationMs(std::chrono::steady_clock::now() - swap_start);
            playback_surface_swapped_before_draw = true;
            swap_playback_surface_after_draw = false;
            presented_frame = camera.last_uploaded_frame;
            if (presented_frame >= 0) {
              current_frame_num = presented_frame;
            }
            presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
          }
          playback_trace_presenter_view_idx = j;
          playback_trace_presenter_target_frame =
              camera_view_presenter_context.target_display_frame;
          playback_trace_presenter_preferred_paused_slot =
              camera_view_presenter_context.preferred_paused_slot;
          playback_trace_presented_slot = presented_slot;
          playback_trace_presented_frame = presented_frame;
          playback_trace_presenter_resolved_frame =
              camera_view_presenter_result.resolved_current_frame_num;
          playback_trace_prewarm_active = prewarm_playback_textures;

          frame_camera_upload_ms += camera_view_presenter_result.perf.upload_ms;
          frame_camera_upload_count +=
              camera_view_presenter_result.perf.upload_count;
          frame_camera_texture_resize_ms +=
              camera_view_presenter_result.perf.texture_resize_ms;
          frame_camera_preview_resize_ms +=
              camera_view_presenter_result.perf.preview_resize_ms;
          frame_camera_display_convert_ms +=
              camera_view_presenter_result.perf.display_convert_ms;
          frame_camera_pbo_copy_ms +=
              camera_view_presenter_result.perf.pbo_copy_ms;
          frame_camera_texture_upload_ms +=
              camera_view_presenter_result.perf.texture_upload_ms;
          frame_camera_playback_front_path_ms +=
              camera_view_presenter_result.perf.playback_front_path_ms;
          frame_camera_playback_stage_total_ms +=
              camera_view_presenter_result.perf.playback_stage_total_ms;
          frame_camera_playback_stage_upload_ms +=
              camera_view_presenter_result.perf.playback_stage_upload_ms;
          frame_camera_playback_prewarm_total_ms +=
              camera_view_presenter_result.perf.playback_prewarm_total_ms;
          frame_camera_playback_prewarm_upload_ms +=
              camera_view_presenter_result.perf.playback_prewarm_upload_ms;
          frame_camera_playback_prewarm_count +=
              camera_view_presenter_result.perf.playback_prewarm_count;

          // sync yolo detection
          if (yolo_detection) {
            std::unique_lock<std::mutex> lck(g_mutexes[j]);
            // std::cout << "main_thread: acquire lock" <<
            // std::endl;
            yolo_input_frames_rgba[j] = presented_rgba_cuda_buffer;
            g_ready[j] = true;
            g_cvs[j].notify_one();
          }

          const bool has_presented_camera_frame = presented_frame >= 0;
          const auto &clipped_handoff = clipped_media_state.handoff;
          bool presented_frame_uses_selected_clip = false;
          if (media_session_loader.hasMappedMedia() &&
              clipped_handoff.switch_in_progress &&
              clipped_handoff.pending_switch_parent_frame >= 0 &&
              has_presented_camera_frame) {
            presented_frame_uses_selected_clip =
                presented_frame >=
                    clipped_handoff.pending_switch_parent_frame &&
                clippedSelectedRunForFrame(presented_frame) ==
                    clipped_handoff.selected_run_index;
          }
          const crimson::platform::nvidia::CameraFrameQuery
              camera_frame_query =
                  crimson::platform::nvidia::selectCameraFrameQuery(
                      crimson::platform::nvidia::CameraFrameQueryInput{
                      current_frame_num,
                      presented_frame,
                      media_session_loader.hasMappedMedia(),
                      clipped_handoff.switch_in_progress,
                      clipped_handoff.pending_switch_parent_frame,
                      clipped_handoff.last_presented_parent_frame,
                      presented_frame_uses_selected_clip,
                  });
          const int zarr_bbox_query_frame = camera_frame_query.query_frame;
          if (canonical_detection_route && has_presented_camera_frame &&
              zarr_bbox_query_frame >= 0) {
            canonical_presented_request_this_frame = true;
            (void)requestCanonicalDetectionFrame(zarr_bbox_query_frame);
          }
          const bool camera_subject_shape_needs_contours =
              subject_shape_overlay_options.show_overlay &&
              (subject_shape_overlay_options.show_body_contour ||
               subject_shape_overlay_options.show_swim_bladder_contour ||
               subject_shape_overlay_options.show_eye_contours);
          const bool camera_details_include_subject_shapes =
              zarr_loaded && has_presented_camera_frame &&
              zarr_loader.hasSubjectShapeData() &&
              (subject_shape_overlay_options.show_overlay ||
               (zarr_loader.hasTailKinematicsData() &&
                tail_kinematics_overlay_options.show_overlay) ||
               (show_eye_masks && show_eye_angle_arcs &&
                zarr_loader.hasEyeAngleData()));
          const bool camera_details_include_eye_masks =
              zarr_loaded && has_presented_camera_frame &&
              (show_eye_masks || camera_subject_shape_needs_contours) &&
              zarr_loader.hasEyeMasks();
          const bool allow_blocking_eye_mask_load = !ps.play_video;
          crimson::platform::nvidia::CameraFrameData camera_frame_data =
              activeCameraFrameDataAdapter().resolve(
                  crimson::platform::nvidia::CameraFrameDataRequest{
                      zarr_loaded,
                      has_presented_camera_frame,
                      zarr_bbox_query_frame,
                      zarr_loader.getImageWidth(),
                      zarr_loader.getImageHeight(),
                      !canonical_detection_route,
                      !canonical_detection_route,
                      false,
                      false,
                      camera_details_include_eye_masks,
                      camera_details_include_subject_shapes,
                      allow_blocking_eye_mask_load,
                      kPlaybackMaskPrefetchLookaheadFrames,
                  });
          crimson::gui::CanonicalOverlayPresentation canonical_presentation;
          if (canonical_detection_route && zarr_bbox_query_frame >= 0) {
            requestCanonicalOverlayFrame(zarr_bbox_query_frame,
                show_keypoint_markers || show_heading_arrows,
                show_eye_masks,
                subject_shape_overlay_options.show_overlay || show_heading_arrows);
            crimson::overlay::ReadOnlyOverlayControlState controls;
            controls.show_keypoints = show_keypoint_markers;
            controls.show_headings = show_heading_arrows;
            controls.show_subject_masks = show_eye_masks;
            controls.show_subject_body_mask = show_subject_body_mask;
            controls.show_eye_left_mask = show_eye_left_mask;
            controls.show_eye_right_mask = show_eye_right_mask;
            controls.show_swim_bladder_mask = show_swim_bladder_mask;
            controls.show_eye_geometry = false;
            controls.show_subject_shape = subject_shape_overlay_options.show_overlay;
            controls.show_subject_shape_body_axes = subject_shape_overlay_options.show_body_frame_axes;
            controls.show_subject_shape_centerline = subject_shape_overlay_options.show_centerline;
            controls.show_subject_shape_bspline = subject_shape_overlay_options.show_bspline_sample;
            canonical_presentation = crimson::gui::makeCanonicalOverlayPresentation(
                canonical_overlay_session.snapshot(zarr_bbox_query_frame), j, zarr_bbox_query_frame,
                static_cast<int>(scene->cameras[j].image_width),
                static_cast<int>(scene->cameras[j].image_height), controls);
            last_canonical_overlay_snapshot = canonical_presentation.snapshot;
            camera_frame_data.keypoint_descriptor = canonical_presentation.snapshot.keypoints.descriptor;
            camera_frame_data.keypoint_frame = canonical_presentation.keypoints;
            camera_frame_data.keypoint_frame_requested = canonical_presentation.keypoints_ready;
          }
          const auto &camera_detection_descriptor =
              camera_frame_data.detection_descriptor;
          const auto &camera_keypoint_descriptor =
              camera_frame_data.keypoint_descriptor;
          const auto &presented_detection_frame =
              camera_frame_data.detection_frame;
          const auto &presented_keypoint_frame =
              camera_frame_data.keypoint_frame;
          const crimson::zarr::KeypointOverlayResolution*
              presented_keypoint_frame_ptr =
                  camera_frame_data.keypoint_frame_requested
                      ? &presented_keypoint_frame
                      : nullptr;
          auto &detection_details = camera_frame_data.legacy_details;
          const bool is_zarr_interpolated =
              camera_frame_data.frame_is_interpolated;
          const bool dataset_allows_bbox_edit =
              camera_frame_data.dataset_allows_bbox_edit;
          if (zarr_loaded && !dataset_allows_bbox_edit) {
            g_zarr_bbox_edit_state.draw_mode = false;
            g_zarr_bbox_edit_state.cancelDraw();
            g_zarr_bbox_edit_state.clearSelection();
          }
          const bool can_modify_boxes =
              dataset_allows_bbox_edit && g_zarr_bbox_edit_state.enabled &&
              (g_zarr_bbox_edit_state.allow_edit_while_playing ||
               !ps.play_video);
          const auto &loaded_zarr_boxes = camera_frame_data.source_boxes;
          std::vector<LoggedBoundingBox> zarr_boxes;
          if (zarr_loaded && has_presented_camera_frame) {
            frame_bbox_query_frame = zarr_bbox_query_frame;
            frame_bbox_get_boxes_ms +=
                camera_frame_data.metrics.detection_repository_ms +
                camera_frame_data.metrics.keypoint_repository_ms +
                camera_frame_data.metrics.bounding_box_conversion_ms;
            frame_bbox_loaded_count =
                static_cast<int>(loaded_zarr_boxes.size());
            const auto bbox_edit_resolve_start =
                std::chrono::steady_clock::now();
            if (canonical_detection_route) {
              zarr_boxes = loaded_zarr_boxes;
            } else {
              zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(
                  zarr_bbox_query_frame, loaded_zarr_boxes);
            }
            const double bbox_edit_resolve_ms = durationMs(
                std::chrono::steady_clock::now() - bbox_edit_resolve_start);
            frame_bbox_edit_resolve_ms += bbox_edit_resolve_ms;
            frame_bbox_display_count = static_cast<int>(zarr_boxes.size());
            frame_bbox_get_raw_detections_ms +=
                camera_frame_data.metrics.legacy_details_ms;
            if (camera_details_include_eye_masks) {
              frame_mask_data_load_ms +=
                  camera_frame_data.metrics.legacy_details_ms;
            }
            frame_bbox_load_total_ms +=
                camera_frame_data.metrics.total_ms + bbox_edit_resolve_ms;
          }

          auto deleteSelectedBoxOnCurrentFrame = [&]() -> bool {
            if (!can_modify_boxes) {
              return false;
            }
            if (g_zarr_bbox_edit_state.selected_frame != current_frame_num ||
                g_zarr_bbox_edit_state.selected_box < 0) {
              return false;
            }
            auto &editable_boxes = g_zarr_bbox_edit_state.ensureFrameOverride(
                current_frame_num, loaded_zarr_boxes, &detection_details);
            auto &added_flags = g_zarr_bbox_edit_state.ensureAddedFlags(
                current_frame_num, editable_boxes.size());
            auto &manual_flags = g_zarr_bbox_edit_state.ensureManualFlags(
                current_frame_num, editable_boxes.size());
            g_zarr_bbox_edit_state.ensureSourceMetadata(current_frame_num,
                                                        editable_boxes.size());
            auto &source_indices =
                g_zarr_bbox_edit_state.frame_source_indices[current_frame_num];
            auto &source_detection_source =
                g_zarr_bbox_edit_state
                    .frame_source_detection_source[current_frame_num];
            auto &source_reason =
                g_zarr_bbox_edit_state.frame_source_reason[current_frame_num];
            const int selected_idx = g_zarr_bbox_edit_state.selected_box;
            if (selected_idx < 0 ||
                selected_idx >= static_cast<int>(editable_boxes.size())) {
              g_zarr_bbox_edit_state.clearSelection();
              return false;
            }
            editable_boxes.erase(editable_boxes.begin() + selected_idx);
            if (selected_idx < static_cast<int>(added_flags.size())) {
              added_flags.erase(added_flags.begin() + selected_idx);
            } else {
              added_flags.assign(editable_boxes.size(), 0);
            }
            if (selected_idx < static_cast<int>(manual_flags.size())) {
              manual_flags.erase(manual_flags.begin() + selected_idx);
            } else {
              manual_flags.assign(editable_boxes.size(), 0);
            }
            if (selected_idx < static_cast<int>(source_indices.size())) {
              source_indices.erase(source_indices.begin() + selected_idx);
            } else {
              source_indices.assign(editable_boxes.size(), -1);
            }
            if (selected_idx <
                static_cast<int>(source_detection_source.size())) {
              source_detection_source.erase(source_detection_source.begin() +
                                            selected_idx);
            } else {
              source_detection_source.assign(editable_boxes.size(), 0);
            }
            if (selected_idx < static_cast<int>(source_reason.size())) {
              source_reason.erase(source_reason.begin() + selected_idx);
            } else {
              source_reason.assign(editable_boxes.size(), std::string{});
            }
            g_zarr_bbox_edit_state.dirty_frames.insert(current_frame_num);
            g_zarr_bbox_edit_state.drag_active = false;
            g_zarr_bbox_edit_state.drag_mouse_button = -1;
            if (editable_boxes.empty()) {
              g_zarr_bbox_edit_state.clearSelection();
            } else {
              g_zarr_bbox_edit_state.selected_frame = current_frame_num;
              g_zarr_bbox_edit_state.selected_box = std::min(
                  selected_idx, static_cast<int>(editable_boxes.size() - 1));
            }
            zarr_boxes = editable_boxes;
            return true;
          };

          FullFrameRectEditStateView full_frame_edit_state;
          full_frame_edit_state.selected_frame =
              g_zarr_bbox_edit_state.selected_frame;
          full_frame_edit_state.selected_box =
              g_zarr_bbox_edit_state.selected_box;
          full_frame_edit_state.drag_active =
              g_zarr_bbox_edit_state.drag_active;
          full_frame_edit_state.drag_mouse_button =
              g_zarr_bbox_edit_state.drag_mouse_button;
          full_frame_edit_state.drag_offset_x =
              g_zarr_bbox_edit_state.drag_offset_x;
          full_frame_edit_state.drag_offset_y =
              g_zarr_bbox_edit_state.drag_offset_y;
          full_frame_edit_state.draw_mode = g_zarr_bbox_edit_state.draw_mode;
          full_frame_edit_state.draw_active =
              g_zarr_bbox_edit_state.draw_active;
          full_frame_edit_state.draw_frame = g_zarr_bbox_edit_state.draw_frame;
          full_frame_edit_state.draw_anchor_x =
              g_zarr_bbox_edit_state.draw_anchor_x;
          full_frame_edit_state.draw_anchor_y =
              g_zarr_bbox_edit_state.draw_anchor_y;
          full_frame_edit_state.draw_current_x =
              g_zarr_bbox_edit_state.draw_current_x;
          full_frame_edit_state.draw_current_y =
              g_zarr_bbox_edit_state.draw_current_y;

          std::vector<ZarrDetectionLoader::ChaserBoundingBox> chaser_bboxes;
          std::vector<ZarrDetectionLoader::ChaserState> chaser_states;
          if (zarr_loaded) {
            chaser_bboxes =
                zarr_loader.getChaserBoundingBoxesForFrame(current_frame_num);
            chaser_states =
                zarr_loader.getChaserInterpolatedStatesForCameraFrame(
                    current_frame_num);
            if (chaser_states.empty() && ps.current_stimulus_frame >= 0 &&
                stimulus_repository.hasMapping()) {
              chaser_states = zarr_loader.getChaserStatesForStimulusFrame(
                  ps.current_stimulus_frame);
            }
            if (chaser_states.empty()) {
              chaser_states =
                  zarr_loader.getChaserStatesForFrame(current_frame_num);
            }
#if defined(CRIMSON_CHASER_DEBUG_LOGS)
            static bool debug_printed = false;
            static int frames_with_data = 0;
            if (!chaser_bboxes.empty() || !chaser_states.empty()) {
              frames_with_data++;
              if (!debug_printed) {
                std::cout << "\n=== CHASER DATA DEBUG ===" << std::endl;
                std::cout << "Camera frame " << current_frame_num << ": Found "
                          << chaser_bboxes.size() << " chaser bboxes, "
                          << chaser_states.size() << " chaser states"
                          << std::endl;
                if (!chaser_bboxes.empty()) {
                  std::cout
                      << "  First bbox: fish_id=" << chaser_bboxes[0].fish_id
                      << ", x=" << chaser_bboxes[0].x_px
                      << ", y=" << chaser_bboxes[0].y_px
                      << ", w=" << chaser_bboxes[0].width_px
                      << ", h=" << chaser_bboxes[0].height_px << std::endl;
                }
                if (!chaser_states.empty()) {
                  std::cout
                      << "  First state: stimulus_frame="
                      << chaser_states[0].stimulus_frame_num
                      << ", camera_frame=" << chaser_states[0].camera_frame_id
                      << std::endl;
                  std::cout << "    chaser=(" << chaser_states[0].chaser_pos_x
                            << "," << chaser_states[0].chaser_pos_y
                            << ") target=(" << chaser_states[0].target_pos_x
                            << "," << chaser_states[0].target_pos_y << ")"
                            << std::endl;
                  std::cout
                      << "  Camera params: has_homography="
                      << camera_params[j].has_valid_homography
                      << ", offsetX=" << camera_params[j].stimulus_offset_x
                      << ", offsetY=" << camera_params[j].stimulus_offset_y
                      << std::endl;
                }
                debug_printed = true;
              }
            }
            static int last_frame_checked = -1;
            if (current_frame_num > last_frame_checked + 1000) {
              std::cout << "Frames " << (last_frame_checked + 1) << "-"
                        << current_frame_num << ": " << frames_with_data
                        << " frames had chaser data" << std::endl;
              frames_with_data = 0;
              last_frame_checked = current_frame_num;
            }
#endif
          }

          const bool heading_overlay_enabled = show_heading_arrows;
          const bool heading_data_available =
              zarr_loaded && presented_keypoint_frame_ptr != nullptr &&
              std::any_of(presented_keypoint_frame.detections.begin(),
                          presented_keypoint_frame.detections.end(),
                          [](const auto& detection) {
                            return detection.heading_valid;
                          });
          const bool eye_mask_overlay_enabled = show_eye_masks;
          const bool eye_mask_data_available =
              zarr_loaded && zarr_loader.hasEyeMasks();
          const bool can_draw_headings =
              heading_overlay_enabled && heading_data_available;
          const bool can_draw_eye_masks =
              eye_mask_overlay_enabled && eye_mask_data_available;

          if (kHeadingDebugLoggingEnabled) {
            if (!heading_overlay_enabled) {
              if (!heading_debug_logged_toggle_disabled) {
                headingDebugLog("Heading overlay disabled via UI toggle; "
                                "skipping arrow drawing.");
                heading_debug_logged_toggle_disabled = true;
              }
            } else {
              if (heading_debug_logged_toggle_disabled) {
                headingDebugLog("Heading overlay toggle enabled; attempting to "
                                "draw arrows.");
                heading_debug_logged_toggle_disabled = false;
              }
              if (!heading_data_available) {
                if (!heading_debug_logged_no_data) {
                headingDebugLog("Keypoint repository reports no heading for "
                                "this frame; arrows will not be drawn.");
                  heading_debug_logged_no_data = true;
                }
              } else if (heading_debug_logged_no_data) {
                headingDebugLog(
                    "Heading data detected; resuming arrow attempts.");
                heading_debug_logged_no_data = false;
              }
              if (zarr_loaded &&
                  camera_detection_descriptor
                      .active_dataset_has_synthetic_observations) {
                if (!heading_debug_logged_interpolated) {
                  headingDebugLog("Dataset contains synthetic detections; "
                                  "headings render only for real boxes.");
                  heading_debug_logged_interpolated = true;
                }
              } else if (heading_debug_logged_interpolated) {
                headingDebugLog("Dataset now fully real; headings may render "
                                "for all boxes.");
                heading_debug_logged_interpolated = false;
              }
            }
          }

          if (kEyeMaskDebugLoggingEnabled) {
            if (!show_eye_masks) {
              if (!eye_mask_debug_logged_toggle_disabled) {
                eyeMaskDebugLog("Eye mask overlay disabled via UI toggle; "
                                "skipping mask drawing.");
                eye_mask_debug_logged_toggle_disabled = true;
              }
            } else if (eye_mask_debug_logged_toggle_disabled) {
              eyeMaskDebugLog(
                  "Eye mask overlay toggle enabled; attempting to draw masks.");
              eye_mask_debug_logged_toggle_disabled = false;
            }
            if (!(zarr_loaded && zarr_loader.hasEyeMasks())) {
              if (!eye_mask_debug_logged_no_data) {
                eyeMaskDebugLog("Zarr loader reports no eye mask data.");
                eye_mask_debug_logged_no_data = true;
              }
            } else if (eye_mask_debug_logged_no_data) {
              eyeMaskDebugLog("Eye mask data detected; masks may render.");
              eye_mask_debug_logged_no_data = false;
            }
          }

          int latest_decoded = -1;
          const auto latest_it = latest_decoded_frame.find(win_name);
          if (latest_it != latest_decoded_frame.end()) {
            latest_decoded = latest_it->second.load();
          }
          int total_recording_frames = -1;
          if (dc_context->estimated_num_frames > 0) {
            total_recording_frames = dc_context->estimated_num_frames;
          }
          if (dc_context->total_num_frame > 0 &&
              dc_context->total_num_frame != std::numeric_limits<int>::max()) {
            total_recording_frames =
                std::max(total_recording_frames, dc_context->total_num_frame);
          }

          CameraViewFrameContextInput camera_context_input;
          camera_context_input.scene = scene;
          camera_context_input.view_idx = j;
          camera_context_input.camera_name = win_name;
          camera_context_input.current_frame_num = current_frame_num;
          camera_context_input.presented_slot = presented_slot;
          camera_context_input.presented_frame = presented_frame;
          camera_context_input.swap_playback_surface_after_draw =
              swap_playback_surface_after_draw;
          camera_context_input.play_video = ps.play_video;
          camera_context_input.lightweight_playback_renderer_active =
              playbackLightweightRendererIsActive(ps.play_video,
                                                  playback_renderer_mode);
          camera_context_input.use_legacy_manual_keypoint_tools =
              use_legacy_manual_keypoint_tools;
          camera_context_input.legacy_labeling_state = &legacy_labeling_state;
          camera_context_input.zarr_loaded = zarr_loaded;
          camera_context_input.zarr_loader = &zarr_loader;
          camera_context_input.dataset_allows_bbox_edit =
              dataset_allows_bbox_edit;
          camera_context_input.bbox_edit_enabled =
              g_zarr_bbox_edit_state.enabled;
          camera_context_input.bbox_allow_edit_while_playing =
              g_zarr_bbox_edit_state.allow_edit_while_playing;
          camera_context_input.bbox_edit_state = &g_zarr_bbox_edit_state;
          camera_context_input.full_frame_edit_state = full_frame_edit_state;
          camera_context_input.zarr_boxes = &zarr_boxes;
          camera_context_input.detection_details =
              camera_frame_data.legacy_details_ready ? &detection_details
                                                     : nullptr;
          camera_context_input.keypoint_descriptor =
              !camera_keypoint_descriptor.run_name.empty()
                  ? &camera_keypoint_descriptor
                  : nullptr;
          camera_context_input.keypoint_frame = presented_keypoint_frame_ptr;
          camera_context_input.subject_mask_repository =
              canonical_detection_route ? nullptr : &subject_mask_repository;
          camera_context_input.frame_is_interpolated = is_zarr_interpolated;
          camera_context_input.latest_decoded_frame = latest_decoded;
          camera_context_input.total_recording_frames = total_recording_frames;
          camera_context_input.has_yolo_detections = yolo_detection;
          camera_context_input.yolo_boxes =
              yolo_detection ? &yolo_boxes.at(j) : nullptr;
          camera_context_input.yolo_labels =
              yolo_detection ? &yolo_labels.at(j) : nullptr;
          camera_context_input.yolo_class_ids =
              yolo_detection ? &yolo_classid.at(j) : nullptr;
          camera_context_input.show_keypoint_markers = show_keypoint_markers;
          camera_context_input.keypoint_tab_full_frame_edit_enabled =
              keypoint_tab_full_frame_edit_enabled;
          camera_context_input.visible_camera_index =
              playback_session_controller.getVisibleCameraIndex();
          camera_context_input.active_full_frame_keypoint_selection =
              &active_full_frame_keypoint_selection;
          camera_context_input.frame_debug_state = &frame_debug_window_state;
          camera_context_input.subject_mask_edit_session =
              &frame_debug_window_state.subject_mask_edit_session;
          camera_context_input.subject_mask_brush_state =
              &frame_debug_window_state.subject_mask_brush;
          camera_context_input.can_draw_headings = can_draw_headings;
          camera_context_input.can_draw_eye_masks = can_draw_eye_masks;
          camera_context_input.allow_blocking_eye_mask_load = !ps.play_video;
          camera_context_input.show_subject_body_mask = show_subject_body_mask;
          camera_context_input.show_eye_left_mask = show_eye_left_mask;
          camera_context_input.show_eye_right_mask = show_eye_right_mask;
          camera_context_input.show_swim_bladder_mask = show_swim_bladder_mask;
          camera_context_input.show_eye_direction_beams =
              show_eye_direction_beams;
          camera_context_input.show_eye_gaze_rays = show_eye_gaze_rays;
          camera_context_input.show_eye_angle_arcs = show_eye_angle_arcs;
          camera_context_input.show_eye_angle_labels = show_eye_angle_labels;
          camera_context_input.mask_overlay_mode = mask_overlay_mode;
          camera_context_input.subject_shape_overlay_options =
              subject_shape_overlay_options;
          camera_context_input.tail_kinematics_overlay_options =
              tail_kinematics_overlay_options;
          camera_context_input.show_movement_trail = show_movement_trail;
          camera_context_input.movement_trail_seconds = movement_trail_seconds;
          camera_context_input.movement_trail_valid_samples_only =
              movement_trail_valid_samples_only;
          camera_context_input.stimulus_inset_options = stimulus_inset_options;
          camera_context_input.chaser_distance_polar_inset_options =
              chaser_distance_polar_inset_options;
          camera_context_input.chaser_distance_polar_repository =
              chaser_distance_polar_repository.get();
          camera_context_input.stimulus_context_timeline =
              stimulus_context_timeline.get();
          camera_context_input.stimulus_player =
              stimulus_player.loaded ? &stimulus_player : nullptr;
          camera_context_input.target_stimulus_frame =
              ps.current_stimulus_frame;
          camera_context_input.chaser_bboxes = &chaser_bboxes;
          camera_context_input.chaser_states = &chaser_states;
          camera_context_input.camera_params = &camera_params[j];
          const int64_t transport_frame_count =
              std::max<int64_t>(0, currentPlaybackFrameCount());
          camera_context_input.transport_controls =
              CameraViewTransportControlsContext{
                  ps.play_video ? current_frame_num
                                : ps.to_display_frame_number,
                  transport_frame_count,
                  std::max<int64_t>(0, transport_frame_count - 1),
                  video_fps,
                  ps.play_video,
                  ps.slider_frame_number,
              };
          camera_context_input.capture_texture_draw_trace =
              diagnostics_session.clippedFrameTraceEnabled() && zarr_loaded &&
              zarr_loader.hasClippedCollection();
          PreparedCameraViewFrameContext prepared_camera_context;
          prepareCameraViewFrameContext(camera_context_input,
                                        prepared_camera_context);
          if (canonical_detection_route) {
            prepared_camera_context.context.can_draw_eye_masks =
                show_eye_masks && canonical_presentation.masks.ready();
            prepared_camera_context.context.subject_mask_scene =
                canonical_presentation.masks.ready() ? &canonical_presentation.masks : nullptr;
            prepared_camera_context.context.subject_shape_scene =
                canonical_presentation.shapes.ready() ? &canonical_presentation.shapes : nullptr;
            prepared_camera_context.context.mask_details = nullptr;
            prepared_camera_context.context.subject_shape_details = nullptr;
            prepared_camera_context.context.subject_mask_pick_enabled = false;
            prepared_camera_context.context.subject_mask_brush_input_enabled = false;
          }
          frame_mask_data_load_ms += prepared_camera_context.mask_data_load_ms;

          const auto &camera_before_draw = scene->cameras[j];
          const int frame_sync_front_frame_before_draw =
              camera_before_draw.last_uploaded_frame;
          const int frame_sync_front_local_before_draw =
              camera_before_draw.last_uploaded_local_frame;
          const int64_t frame_sync_front_pts_before_draw =
              camera_before_draw.last_uploaded_pts;
          const bool frame_sync_front_valid_before_draw =
              camera_before_draw.texture_has_valid_frame;
          const int frame_sync_staging_frame_before_draw =
              camera_before_draw.playback_staging_frame;
          const int frame_sync_staging_local_before_draw =
              camera_before_draw.playback_staging_local_frame;
          const int64_t frame_sync_staging_pts_before_draw =
              camera_before_draw.playback_staging_pts;
          const bool frame_sync_staging_valid_before_draw =
              camera_before_draw.playback_staging_valid;

          const CameraViewWindowResult camera_view_result =
              drawCameraViewWindowContents(prepared_camera_context.context);
          if (canonical_detection_route) {
            canonical_overlay_draw_frame = zarr_bbox_query_frame;
            canonical_keypoint_draw_count = camera_view_result.perf.keypoint_overlay_item_count;
            canonical_heading_draw_count = camera_view_result.perf.heading_overlay_item_count;
            canonical_shape_draw_count = camera_view_result.perf.subject_shape_overlay_item_count;
            canonical_expected_shape_draw_count = static_cast<int>(canonical_presentation.shapes.primitives.size());
            canonical_expected_mask_draw_count = static_cast<int>(canonical_presentation.masks.raster_masks.size());
            if (diagnostics_session.playbackTraceEnabled() && has_presented_camera_frame) {
              const auto& snapshot = canonical_presentation.snapshot;
              const auto& mask = snapshot.masks;
              const auto& mask_perf = camera_view_result.perf.mask_overlay;
              const bool mask_resolved = mask.state == crimson::gui::CanonicalOverlayState::Ready ||
                                         mask.state == crimson::gui::CanonicalOverlayState::Empty;
              const char* outcome = !show_eye_masks ? "disabled" :
                  !mask_resolved ? crimson::gui::canonicalOverlayStateName(mask.state) :
                  !canonical_presentation.masks.ready() ? "scene_rejected" :
                  canonical_expected_mask_draw_count == 0 ? "valid_absent" :
                  mask_perf.component_fill_count != canonical_expected_mask_draw_count ? "draw_failed" : "drawn";
              writePlaybackTraceEvent("canonical_overlay_present",
                  {{"generation", snapshot.generation},
                   {"query_frame", zarr_bbox_query_frame},
                   {"mask_frame", mask.frame ? mask.frame->camera_frame : -1},
                   {"mask_state", crimson::gui::canonicalOverlayStateName(mask.state)},
                   {"mask_outcome", outcome}, {"mask_error", mask.error},
                   {"mask_enabled", show_eye_masks},
                   {"mask_expected_fills", canonical_expected_mask_draw_count},
                   {"mask_actual_fills", mask_perf.component_fill_count},
                   {"mask_texture_uploads", mask_perf.texture_uploads},
                   {"mask_texture_upload_ms", mask_perf.texture_upload_ms},
                   {"mask_draw_ms", mask_perf.total_draw_ms},
                   {"mask_decoded_cache_bytes", snapshot.mask_buffer_metrics.cached_payload_bytes},
                   {"mask_decoded_cache_byte_budget", snapshot.mask_buffer_metrics.maximum_cached_payload_bytes},
                   {"mask_lookahead_frames", snapshot.mask_buffer_metrics.effective_lookahead_frames},
                   {"mask_contiguous_ready_ahead", snapshot.mask_buffer_metrics.contiguous_ready_ahead},
                   {"mask_average_resolve_ms", snapshot.mask_buffer_metrics.average_resolve_ms},
                   {"mask_maximum_resolve_ms", snapshot.mask_buffer_metrics.maximum_resolve_ms},
                   {"mask_pending_frames", snapshot.mask_buffer_metrics.pending_frames},
                   {"mask_speculative_queue_ms", snapshot.mask_buffer_metrics.speculative_average_queue_wait_ms},
                   {"mask_byte_budget_evictions", snapshot.mask_buffer_metrics.byte_budget_evictions},
                   {"mask_oversized_rejections", snapshot.mask_buffer_metrics.oversized_result_rejections},
                   {"mask_payload_cache_bytes", snapshot.mask_metrics.cached_payload_bytes},
                   {"mask_payload_read_calls", snapshot.mask_metrics.dense_mask_payload_reads},
                   {"mask_payload_read_ms", snapshot.mask_metrics.chunk_read_ms}},
                  j, camera_view_presenter_context.target_display_frame,
                  camera_view_presenter_context.preferred_paused_slot, presented_slot,
                  presented_frame, camera_view_presenter_result.resolved_current_frame_num,
                  prewarm_playback_textures);
            }
          }
          accumulateCameraViewMaskPerfMetrics(
              frame_mask_overlay_perf, camera_view_result.perf.mask_overlay);

          const auto &camera_after_draw = scene->cameras[j];
          const int frame_sync_front_frame_after_draw =
              camera_after_draw.last_uploaded_frame;
          const int frame_sync_front_local_after_draw =
              camera_after_draw.last_uploaded_local_frame;
          const int64_t frame_sync_front_pts_after_draw =
              camera_after_draw.last_uploaded_pts;
          const bool frame_sync_front_valid_after_draw =
              camera_after_draw.texture_has_valid_frame;
          const int frame_sync_staging_frame_after_draw =
              camera_after_draw.playback_staging_frame;
          const int frame_sync_staging_local_after_draw =
              camera_after_draw.playback_staging_local_frame;
          const int64_t frame_sync_staging_pts_after_draw =
              camera_after_draw.playback_staging_pts;
          const bool frame_sync_staging_valid_after_draw =
              camera_after_draw.playback_staging_valid;

          if (diagnostics_session.frameSyncTraceEnabled()) {
            const int zarr_box_count = static_cast<int>(zarr_boxes.size());
            nvidia_trace::FrameSyncTraceState &last_trace_state =
                frame_sync_trace_last_by_camera[win_name];
            const nvidia_trace::FrameSyncTraceState trace_state{
                true,
                has_presented_camera_frame,
                camera_view_presenter_context.target_display_frame,
                presented_frame,
                zarr_bbox_query_frame,
                latest_decoded,
                frame_sync_front_frame_before_draw,
                frame_sync_front_frame_after_draw,
                frame_sync_staging_frame_before_draw,
                frame_sync_staging_frame_after_draw,
                zarr_box_count};

            if (nvidia_trace::frameSyncTraceChanged(last_trace_state,
                                                    trace_state)) {
              auto makeClippedMapping = [&](int frame) -> json {
                if (!zarr_loaded || !zarr_loader.hasClippedCollection() ||
                    frame < 0) {
                  return nullptr;
                }
                const auto *row = zarr_loader.resolveClippedFrame(
                    static_cast<int64_t>(frame));
                if (row == nullptr) {
                  return nullptr;
                }
                json mapping = {
                    {"parent_frame_index", row->parent_frame_index},
                    {"recording_frame_id", row->recording_frame_id},
                    {"clip_id", row->clip_id},
                    {"clip_local_frame_index", row->clip_local_frame_index},
                    {"camera_serial", row->camera_serial},
                    {"selected_run_index", row->selected_run_index},
                };
                const auto *selected_run =
                    zarr_loader.getClippedResolver().selectedRun(
                        row->selected_run_index);
                if (selected_run != nullptr) {
                  mapping["work_unit_id"] = selected_run->work_unit_id;
                  mapping["detect_run"] = selected_run->detect_run;
                  mapping["refined_detect_run"] =
                      selected_run->refined_detect_run;
                }
                return mapping;
              };

              json details = {
                  {"camera_name", win_name},
                  {"view_idx", j},
                  {"has_presented_camera_frame", has_presented_camera_frame},
                  {"target_frame",
                   camera_view_presenter_context.target_display_frame},
                  {"current_frame_after_presenter", current_frame_num},
                  {"presented_slot", presented_slot},
                  {"presented_frame", presented_frame},
                  {"presenter_resolved_frame",
                   camera_view_presenter_result.resolved_current_frame_num},
                  {"playback_request",
                   {{"requested_frame", playback_requested_camera_frame},
                    {"presenter_target_frame", playback_presenter_target_frame},
                    {"presenter_target_slot", playback_presenter_target_slot},
                    {"target_clamped_to_buffer",
                     playback_target_clamped_to_buffer},
                    {"committed_frame_before_present",
                     ps.to_display_frame_number}}},
                  {"swap_playback_surface_after_draw",
                   swap_playback_surface_after_draw},
                  {"playback_surface_swapped_before_draw",
                   playback_surface_swapped_before_draw},
                  {"bbox_query_frame", zarr_bbox_query_frame},
                  {"bbox",
                   nvidia_trace::boundingBoxSummaryJson(
                       static_cast<int64_t>(zarr_boxes.size()),
                       nvidia_diagnostics::firstTraceBoundingBox(zarr_boxes))},
                  {"loaded_bbox",
                   nvidia_trace::boundingBoxSummaryJson(
                       static_cast<int64_t>(loaded_zarr_boxes.size()),
                       nvidia_diagnostics::firstTraceBoundingBox(
                           loaded_zarr_boxes))},
                  {"detection_details_frame_id", nullptr},
                  {"texture_before_draw",
                   nvidia_trace::textureStateJson(
                       {frame_sync_front_valid_before_draw,
                        frame_sync_front_frame_before_draw,
                        frame_sync_staging_valid_before_draw,
                        frame_sync_staging_frame_before_draw})},
                  {"texture_after_draw",
                   nvidia_trace::textureStateJson(
                       {frame_sync_front_valid_after_draw,
                        frame_sync_front_frame_after_draw,
                        frame_sync_staging_valid_after_draw,
                        frame_sync_staging_frame_after_draw})},
                  {"frame_sync_summary",
                   {{"valid_slots", camera_view_result.frame_sync.valid_slots},
                    {"empty_slots", camera_view_result.frame_sync.empty_slots},
                    {"latest_decoded",
                     camera_view_result.frame_sync.latest_decoded},
                    {"recording_remaining",
                     camera_view_result.frame_sync.recording_remaining},
                    {"recording_total",
                     camera_view_result.frame_sync.recording_total},
                    {"debug_line", camera_view_result.frame_sync.debug_line}}},
                  {"clipped_mapping",
                   {{"target",
                     makeClippedMapping(
                         camera_view_presenter_context.target_display_frame)},
                    {"presented", makeClippedMapping(presented_frame)},
                    {"bbox_query", makeClippedMapping(zarr_bbox_query_frame)},
                    {"front_before_draw",
                     makeClippedMapping(frame_sync_front_frame_before_draw)},
                    {"front_after_draw",
                     makeClippedMapping(frame_sync_front_frame_after_draw)}}},
                  {"clipped_state", clippedStateJson()},
              };
              details["bbox_frame_delta_from_presented"] =
                  has_presented_camera_frame
                      ? json(zarr_bbox_query_frame - presented_frame)
                      : json(nullptr);
              details["detection_details_frame_id"] =
                  camera_frame_data.legacy_details_ready
                      ? json(detection_details.frame_id)
                      : json(nullptr);
              details["bbox_matches_presented_frame"] =
                  has_presented_camera_frame &&
                  zarr_bbox_query_frame == presented_frame;
              details["bbox_matches_front_before_draw"] =
                  frame_sync_front_valid_before_draw &&
                  zarr_bbox_query_frame == frame_sync_front_frame_before_draw;
              details["bbox_matches_front_after_draw"] =
                  frame_sync_front_valid_after_draw &&
                  zarr_bbox_query_frame == frame_sync_front_frame_after_draw;

              writeFrameSyncTraceEvent(
                  details, j,
                  camera_view_presenter_context.target_display_frame,
                  camera_view_presenter_context.preferred_paused_slot,
                  presented_slot, presented_frame,
                  camera_view_presenter_result.resolved_current_frame_num,
                  prewarm_playback_textures);

              nvidia_trace::updateFrameSyncTraceState(last_trace_state,
                                                      trace_state);
            }
          }

          if (diagnostics_session.clippedFrameTraceEnabled() && zarr_loaded &&
              zarr_loader.hasClippedCollection() &&
              has_presented_camera_frame) {
            const int current_parent_frame_index = presented_frame;
            const int requested_parent_frame_index =
                camera_view_presenter_context.target_display_frame;
            const auto *current_row =
                zarr_loader.resolveClippedFrame(current_parent_frame_index);
            const auto *requested_row =
                zarr_loader.resolveClippedFrame(requested_parent_frame_index);
            const auto *bbox_row =
                zarr_loader.resolveClippedFrame(zarr_bbox_query_frame);
            int decoder_presented_local_frame = -1;
            int64_t decoder_presented_pts = -1;
            int decoder_frame_source_code = 0;
            if (presented_slot >= 0 &&
                presented_slot < static_cast<int>(scene->size_of_buffer)) {
              const auto &slot =
                  scene->cameras[j].display_buffer[presented_slot];
              if (!slot.available_to_write) {
                decoder_presented_local_frame = slot.local_frame_number;
                decoder_presented_pts = slot.frame_pts;
                decoder_frame_source_code = slot.frame_source_code;
              }
            }
            if (decoder_presented_local_frame < 0) {
              decoder_presented_local_frame =
                  frame_sync_front_local_before_draw;
              decoder_presented_pts = frame_sync_front_pts_before_draw;
            }

            const auto current_trace_resolver =
                nvidia_diagnostics::toTraceResolver(
                    clippedResolverTraceSource(current_row));
            const auto requested_trace_resolver =
                nvidia_diagnostics::toTraceResolver(
                    clippedResolverTraceSource(requested_row));
            const auto first_source_bbox =
                nvidia_diagnostics::firstTraceBoundingBox(loaded_zarr_boxes);
            const auto first_display_bbox =
                nvidia_diagnostics::firstTraceBoundingBox(zarr_boxes);
            const auto first_detection_source =
                nvidia_diagnostics::toTraceDetectionSource(
                    detection_details.detection_source,
                    detection_details.detection_reason);
            const auto texture_draw = nvidia_diagnostics::toTraceTextureDraw(
                scene->cameras[j].texture_draw_trace);
            const auto clipped_playback_state =
                nvidia_diagnostics::makeClippedFramePlaybackStateJson(
                    {has_presented_camera_frame, presented_slot, video_fps,
                     clipped_rebase_before_play, playbackSnapshot(),
                     cameraBufferSnapshot(j, requested_parent_frame_index,
                                          presented_frame,
                                          /*require_video_loaded=*/false)});

            nvidia_trace::ClippedFrameComparison comparison;
            comparison.current_parent_frame = current_parent_frame_index;
            comparison.bbox_query_parent_frame = zarr_bbox_query_frame;
            if (current_row != nullptr) {
              comparison.resolved_parent_frame =
                  current_row->parent_frame_index;
              comparison.current_clip_local_frame =
                  current_row->clip_local_frame_index;
            }
            if (bbox_row != nullptr) {
              comparison.bbox_clip_local_frame =
                  bbox_row->clip_local_frame_index;
            }
            if (decoder_presented_local_frame >= 0) {
              comparison.decoder_presented_local_frame =
                  decoder_presented_local_frame;
            }
            comparison.front_before_valid = frame_sync_front_valid_before_draw;
            if (frame_sync_front_local_before_draw >= 0) {
              comparison.front_before_local_frame =
                  frame_sync_front_local_before_draw;
            }
            comparison.front_after_valid = frame_sync_front_valid_after_draw;
            if (frame_sync_front_local_after_draw >= 0) {
              comparison.front_after_local_frame =
                  frame_sync_front_local_after_draw;
            }
            const auto sanity = nvidia_trace::evaluateClippedFrame(comparison);
            nvidia_trace::recordClippedFrame(clipped_frame_trace_stats, sanity);

            double active_timebase = 0.0;
            if (j < static_cast<int>(demuxers.size()) &&
                demuxers[j] != nullptr) {
              active_timebase = demuxers[j]->GetTimebase();
            }
            nvidia_trace::ClippedFrameTraceSnapshot trace_snapshot;
            trace_snapshot.current_parent_frame_index =
                current_parent_frame_index;
            trace_snapshot.requested_parent_frame_index =
                requested_parent_frame_index;
            trace_snapshot.playback_running = ps.play_video;
            trace_snapshot.playback_speed = set_playback_speed;
            trace_snapshot.playback_state = clipped_playback_state;
            trace_snapshot.resolver = current_trace_resolver;
            trace_snapshot.requested_resolver = requested_trace_resolver;
            trace_snapshot.active_video_path =
                clipped_media_state.current_video_path;
            trace_snapshot.active_clip_id = clipped_media_state.handoff.clip_id;
            if (requested_row != nullptr) {
              trace_snapshot.requested_decoder_local_frame =
                  requested_row->clip_local_frame_index;
            }
            if (decoder_presented_local_frame >= 0) {
              trace_snapshot.decoder_presented_local_frame =
                  decoder_presented_local_frame;
            }
            if (frame_sync_front_local_before_draw >= 0) {
              trace_snapshot.front_texture_local_frame_before_draw =
                  frame_sync_front_local_before_draw;
            }
            if (frame_sync_front_local_after_draw >= 0) {
              trace_snapshot.front_texture_local_frame_after_draw =
                  frame_sync_front_local_after_draw;
            }
            if (frame_sync_front_frame_before_draw >= 0) {
              trace_snapshot.front_texture_parent_frame_before_draw =
                  frame_sync_front_frame_before_draw;
            }
            if (frame_sync_front_frame_after_draw >= 0) {
              trace_snapshot.front_texture_parent_frame_after_draw =
                  frame_sync_front_frame_after_draw;
            }
            if (decoder_presented_pts >= 0) {
              trace_snapshot.presented_pts = decoder_presented_pts;
            }
            if (frame_sync_front_pts_before_draw >= 0) {
              trace_snapshot.front_texture_pts_before_draw =
                  frame_sync_front_pts_before_draw;
            }
            if (frame_sync_front_pts_after_draw >= 0) {
              trace_snapshot.front_texture_pts_after_draw =
                  frame_sync_front_pts_after_draw;
            }
            trace_snapshot.timebase = active_timebase;
            trace_snapshot.decoder_frame_source_code =
                decoder_frame_source_code;
            trace_snapshot.surface_swapped_before_draw =
                playback_surface_swapped_before_draw;
            trace_snapshot.upload_count =
                camera_view_presenter_result.perf.upload_count;
            trace_snapshot.presented_slot = presented_slot;
            trace_snapshot.latest_decoded_parent_frame = latest_decoded;
            trace_snapshot.texture_draw = texture_draw;
            trace_snapshot.bbox_query_parent_frame_index =
                zarr_bbox_query_frame;
            if (bbox_row != nullptr) {
              trace_snapshot.bbox_query_clip_local_frame_index =
                  bbox_row->clip_local_frame_index;
            }
            trace_snapshot.bbox_row_count =
                static_cast<int64_t>(loaded_zarr_boxes.size());
            trace_snapshot.first_bbox_source_image = first_source_bbox;
            trace_snapshot.first_bbox_display = first_display_bbox;
            trace_snapshot.first_bbox_source = first_detection_source;
            if (camera_frame_data.legacy_details_ready) {
              trace_snapshot.detection_details_frame_id =
                  static_cast<int64_t>(detection_details.frame_id);
            }
            trace_snapshot.sanity = sanity;
            diagnostics_session.writeClippedFrameTrace(
                nvidia_trace::clippedFrameJson(trace_snapshot),
                /*force_flush=*/false);

            if ((clipped_frame_trace_stats.frames_traced % 300) == 0) {
              diagnostics_session.writeClippedFrameTraceSummary("periodic");
            }
          }

          if (ui_reference_capture.waitingForStableFrame() &&
              j == playback_session_controller.getVisibleCameraIndex() &&
              has_presented_camera_frame &&
              presented_frame == ui_reference.target_frame) {
            ui_reference.exact_presented_this_frame = true;
            ui_reference.presented_frame = presented_frame;
            ui_reference.presented_slot = presented_slot;
            ui_reference.view_idx = j;
            ui_reference.bbox_query_frame = zarr_bbox_query_frame;
            ui_reference.presented_stimulus_frame =
                stimulus_player.last_displayed_frame;
          }

          if (playback_smoke.enabled && playback_smoke.started && playback_smoke.playback_started &&
              !playback_smoke.completed && has_presented_camera_frame) {
            playback_smoke.last_presented_frame = presented_frame;
            playback_smoke.max_presented_frame =
                std::max(playback_smoke.max_presented_frame, presented_frame);
            playback_smoke.last_presented_slot = presented_slot;
            playback_smoke.last_view_idx = j;
            ++playback_smoke.presented_count;
            if (presented_frame >= playback_smoke.end_frame) {
              playback_smoke.completed = true;
              const double elapsed_s = std::chrono::duration<double>(
                                           std::chrono::steady_clock::now() -
                                           playback_smoke.start_time)
                                           .count();
              std::cout << "[PlaybackSmoke] PASS "
                        << "start_frame=" << playback_smoke.start_frame
                        << " end_frame=" << playback_smoke.end_frame
                        << " presented_frame=" << presented_frame
                        << " presented_slot=" << presented_slot
                        << " view_idx=" << j
                        << " presented_count=" << playback_smoke.presented_count
                        << " elapsed_s=" << elapsed_s << std::endl;
              writePlaybackTraceEvent(
                  "playback_smoke_pass",
                  {{"start_frame", playback_smoke.start_frame},
                   {"end_frame", playback_smoke.end_frame},
                   {"presented_frame", presented_frame},
                   {"presented_slot", presented_slot},
                   {"presented_count", playback_smoke.presented_count},
                   {"elapsed_s", elapsed_s}},
                  j, camera_view_presenter_context.target_display_frame,
                  camera_view_presenter_context.preferred_paused_slot,
                  presented_slot, presented_frame,
                  camera_view_presenter_result.resolved_current_frame_num,
                  prewarm_playback_textures);
              app_exit_code = 0;
              glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
            }
          }

          if (clipped_boundary_smoke.enabled &&
              clipped_boundary_smoke.started &&
              !clipped_boundary_smoke.completed && has_presented_camera_frame &&
              presented_frame >= clipped_boundary_smoke.end_frame) {
            if (canonical_detection_route &&
                presented_frame != clipped_boundary_smoke.end_frame) {
              if (!clipped_boundary_smoke.endpoint_seek_requested) {
                if (ps.play_video) {
                  applyPlaybackToggleForPerf();
                }
                clipped_boundary_smoke.endpoint_seek_requested = true;
                const auto endpoint_seek =
                    playback_session_controller.seekToFrame(
                        clipped_boundary_smoke.end_frame,
                        /*prefer_buffer_when_paused=*/false,
                        /*force_inaccurate=*/false,
                        /*skip_stimulus_hard_seek=*/true);
                std::cout
                    << "[ClippedBoundarySmoke] canonical endpoint seek "
                    << "requested_frame=" << clipped_boundary_smoke.end_frame
                    << " overshoot_frame=" << presented_frame
                    << " status=" << static_cast<int>(endpoint_seek.status)
                    << std::endl;
              }
            }
            if (canonical_detection_route && ps.play_video) {
              applyPlaybackToggleForPerf();
            }
            const size_t expected_run =
                clippedSelectedRunForFrame(clipped_boundary_smoke.end_frame);
            const size_t presented_run =
                clippedSelectedRunForFrame(presented_frame);
            const bool bbox_matches_presented =
                zarr_bbox_query_frame == presented_frame;
            const bool run_matches =
                expected_run != std::numeric_limits<size_t>::max() &&
                presented_run == expected_run;
            const bool canonical_frame_ready =
                !canonical_detection_route ||
                (canonical_detection_repository.state() ==
                     crimson::platform::nvidia::NvidiaDetectionState::Ready &&
                 presented_detection_frame.ready() &&
                 presented_detection_frame.frame_id ==
                     static_cast<size_t>(clipped_boundary_smoke.end_frame));
            const size_t canonical_frame_rows =
                canonical_frame_ready && canonical_detection_route
                    ? presented_detection_frame.observations.size()
                    : 0;
            const bool canonical_scene_ready =
                !canonical_detection_route || canonical_frame_rows == 0 ||
                (loaded_zarr_boxes.size() == canonical_frame_rows &&
                 camera_view_result.perf.bbox_overlay_item_count > 0);
            if (bbox_matches_presented && run_matches &&
                canonical_frame_ready && canonical_scene_ready) {
              clipped_boundary_smoke.completed = true;
              const auto canonical_metrics =
                  canonical_detection_repository.metrics();
              std::cout << "[ClippedBoundarySmoke] PASS "
                        << "presented_frame=" << presented_frame
                        << " bbox_query_frame=" << zarr_bbox_query_frame
                        << " clip=" << clipped_media_state.handoff.clip_id
                        << " detection_source="
                        << (canonical_detection_route ? "canonical"
                                                      : "legacy")
                        << " detection_run="
                        << camera_detection_descriptor.run_name
                        << " frame_rows=" << canonical_frame_rows
                        << " boxes=" << loaded_zarr_boxes.size()
                        << " scene_boxes="
                        << camera_view_result.perf.bbox_overlay_item_count
                        << " first_box_xywh=";
              if (!loaded_zarr_boxes.empty()) {
                const auto &first_box = loaded_zarr_boxes.front();
                std::cout << first_box.x_min << "," << first_box.y_min << ","
                          << first_box.width << "," << first_box.height;
              } else {
                std::cout << "empty";
              }
              std::cout
                        << " cache_hits="
                        << canonical_metrics.buffer.cache_hits
                        << " cached_pages="
                        << canonical_metrics.buffer.peak_cached_pages
                        << " cached_bytes="
                        << canonical_metrics.buffer.cached_bytes
                        << " range_reads="
                        << canonical_metrics.repository.range_reads
                        << " resolved_rows="
                        << canonical_metrics.repository.resolved_rows
                        << " resident_range_reads="
                        << canonical_metrics.repository.resident_range_reads
                        << " residency_rows_read="
                        << canonical_metrics.repository.residency_rows_read
                        << " resident_bytes="
                        << canonical_metrics.repository.resident_retained_bytes
                        << std::endl;
              writeClippedHandoffTraceEvent(
                  "smoke_pass",
                  {{"presented_frame", presented_frame},
                   {"bbox_query_frame", zarr_bbox_query_frame},
                   {"expected_end_frame", clipped_boundary_smoke.end_frame},
                   {"selected_run_index", presented_run},
                   {"clip_id", clipped_media_state.handoff.clip_id},
                   {"detection_source",
                    canonical_detection_route ? "canonical" : "legacy"},
                   {"detection_run", camera_detection_descriptor.run_name},
                   {"detection_frame_ready", canonical_frame_ready},
                   {"detection_frame_rows", canonical_frame_rows},
                   {"detection_boxes", loaded_zarr_boxes.size()},
                   {"bbox_scene_items",
                    camera_view_result.perf.bbox_overlay_item_count},
                   {"first_box_xywh",
                    loaded_zarr_boxes.empty()
                        ? json(nullptr)
                        : json::array(
                              {loaded_zarr_boxes.front().x_min,
                               loaded_zarr_boxes.front().y_min,
                               loaded_zarr_boxes.front().width,
                               loaded_zarr_boxes.front().height})},
                   {"cache_hits", canonical_metrics.buffer.cache_hits},
                   {"cached_bytes", canonical_metrics.buffer.cached_bytes},
                   {"range_reads",
                    canonical_metrics.repository.range_reads},
                   {"resolved_rows",
                    canonical_metrics.repository.resolved_rows},
                   {"resident_range_reads",
                    canonical_metrics.repository.resident_range_reads},
                   {"residency_rows_read",
                    canonical_metrics.repository.residency_rows_read},
                   {"resident_bytes",
                    canonical_metrics.repository.resident_retained_bytes}});
              app_exit_code = 0;
              glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
            } else if (!canonical_detection_route ||
                       (canonical_frame_ready && !canonical_scene_ready)) {
              std::cerr << "[ClippedBoundarySmoke] FAIL "
                        << "presented_frame=" << presented_frame
                        << " bbox_query_frame=" << zarr_bbox_query_frame
                        << " expected_run=" << expected_run
                        << " presented_run=" << presented_run
                        << " canonical_frame_ready="
                        << canonical_frame_ready
                        << " detection_rows=" << canonical_frame_rows
                        << " boxes=" << loaded_zarr_boxes.size()
                        << " scene_boxes="
                        << camera_view_result.perf.bbox_overlay_item_count
                        << std::endl;
              writeClippedHandoffTraceEvent(
                  "smoke_fail",
                  {{"presented_frame", presented_frame},
                   {"bbox_query_frame", zarr_bbox_query_frame},
                   {"expected_end_frame", clipped_boundary_smoke.end_frame},
                   {"expected_run", expected_run},
                   {"presented_run", presented_run}});
              app_exit_code = 4;
              glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
            }
          }

          if (zarr_loaded) {
            applyCameraViewSubjectMaskPick(camera_view_result, zarr_loader,
                                           frame_debug_window_state);
            applyCameraViewSubjectMaskPaint(camera_view_result,
                                            frame_debug_window_state);
          }

          perf_camera_viewport_x_px = camera_view_result.perf.viewport_x_px;
          perf_camera_viewport_y_px = camera_view_result.perf.viewport_y_px;
          perf_camera_viewport_width_px =
              camera_view_result.perf.viewport_width_px;
          perf_camera_viewport_height_px =
              camera_view_result.perf.viewport_height_px;
          perf_camera_media_x_px = camera_view_result.perf.media_x_px;
          perf_camera_media_y_px = camera_view_result.perf.media_y_px;
          perf_camera_media_width_px = camera_view_result.perf.media_width_px;
          perf_camera_media_height_px = camera_view_result.perf.media_height_px;
          perf_camera_view_x_min = camera_view_result.perf.view_x_min;
          perf_camera_view_x_max = camera_view_result.perf.view_x_max;
          perf_camera_view_y_min = camera_view_result.perf.view_y_min;
          perf_camera_view_y_max = camera_view_result.perf.view_y_max;
          perf_camera_view_visible_fraction =
              camera_view_result.perf.visible_fraction;
          perf_camera_view_zoomed_in = camera_view_result.perf.zoomed_in;
          if (j == 0) {
            perf_stimulus_camera_overlay_scene =
                camera_view_result.stimulus_camera_overlay_scene;
            perf_stimulus_camera_overlay_origin_x_px =
                camera_view_result.stimulus_camera_overlay_origin_x_px;
            perf_stimulus_camera_overlay_origin_y_px =
                camera_view_result.stimulus_camera_overlay_origin_y_px;
            perf_chaser_distance_polar_scene =
                camera_view_result.chaser_distance_polar_scene;
            perf_chaser_distance_polar_origin_x_px =
                camera_view_result.chaser_distance_polar_origin_x_px;
            perf_chaser_distance_polar_origin_y_px =
                camera_view_result.chaser_distance_polar_origin_y_px;
          }
          frame_camera_plot_image_ui_ms +=
              camera_view_result.perf.plot_image_ui_ms;
          frame_camera_overlay_ui_ms += camera_view_result.perf.overlay_ui_ms;
          frame_bbox_overlay_build_ms +=
              camera_view_result.perf.bbox_overlay_build_ms;
          frame_bbox_overlay_draw_ms +=
              camera_view_result.perf.bbox_overlay_draw_ms;
          frame_bbox_overlay_item_count +=
              camera_view_result.perf.bbox_overlay_item_count;
          frame_subject_shape_overlay_ms +=
              camera_view_result.perf.subject_shape_overlay_ms;
          frame_tail_kinematics_overlay_ms +=
              camera_view_result.perf.tail_kinematics_overlay_ms;
          frame_camera_playback_swap_ms +=
              camera_view_result.perf.playback_swap_ms;
          frame_camera_scene_ui_ms += camera_view_result.perf.scene_ui_ms;
          frame_sync_valid_slots = camera_view_result.frame_sync.valid_slots;
          frame_sync_empty_slots = camera_view_result.frame_sync.empty_slots;
          frame_sync_latest_decoded =
              camera_view_result.frame_sync.latest_decoded;
          frame_sync_recording_remaining =
              camera_view_result.frame_sync.recording_remaining;
          frame_sync_recording_total =
              camera_view_result.frame_sync.recording_total;
          frame_sync_debug_line = camera_view_result.frame_sync.debug_line;

          const FullFrameRectEditResult &full_frame_edit_result =
              camera_view_result.full_frame_edit_result;
          frame_debug_window_state.keypoint_review_panel.full_frame_edit =
              camera_view_result.full_frame_keypoint_edit_state;

          g_zarr_bbox_edit_state.selected_frame =
              full_frame_edit_result.state.selected_frame;
          g_zarr_bbox_edit_state.selected_box =
              full_frame_edit_result.state.selected_box;
          g_zarr_bbox_edit_state.drag_active =
              full_frame_edit_result.state.drag_active;
          g_zarr_bbox_edit_state.drag_mouse_button =
              full_frame_edit_result.state.drag_mouse_button;
          g_zarr_bbox_edit_state.drag_offset_x =
              full_frame_edit_result.state.drag_offset_x;
          g_zarr_bbox_edit_state.drag_offset_y =
              full_frame_edit_result.state.drag_offset_y;
          g_zarr_bbox_edit_state.draw_mode =
              full_frame_edit_result.state.draw_mode;
          g_zarr_bbox_edit_state.draw_active =
              full_frame_edit_result.state.draw_active;
          g_zarr_bbox_edit_state.draw_frame =
              full_frame_edit_result.state.draw_frame;
          g_zarr_bbox_edit_state.draw_anchor_x =
              full_frame_edit_result.state.draw_anchor_x;
          g_zarr_bbox_edit_state.draw_anchor_y =
              full_frame_edit_result.state.draw_anchor_y;
          g_zarr_bbox_edit_state.draw_current_x =
              full_frame_edit_result.state.draw_current_x;
          g_zarr_bbox_edit_state.draw_current_y =
              full_frame_edit_result.state.draw_current_y;

          if (full_frame_edit_result.request_reset_frame) {
            g_zarr_bbox_edit_state.clearFrameEdits(current_frame_num);
            zarr_boxes = loaded_zarr_boxes;
          }
          if (full_frame_edit_result.request_delete_selected) {
            deleteSelectedBoxOnCurrentFrame();
          }
          if (full_frame_edit_result.request_add_rect) {
            auto &editable_boxes = g_zarr_bbox_edit_state.ensureFrameOverride(
                current_frame_num, loaded_zarr_boxes, &detection_details);
            auto &added_flags = g_zarr_bbox_edit_state.ensureAddedFlags(
                current_frame_num, editable_boxes.size());
            auto &manual_flags = g_zarr_bbox_edit_state.ensureManualFlags(
                current_frame_num, editable_boxes.size());
            g_zarr_bbox_edit_state.ensureSourceMetadata(current_frame_num,
                                                        editable_boxes.size());
            auto &source_indices =
                g_zarr_bbox_edit_state.frame_source_indices[current_frame_num];
            auto &source_detection_source =
                g_zarr_bbox_edit_state
                    .frame_source_detection_source[current_frame_num];
            auto &source_reason =
                g_zarr_bbox_edit_state.frame_source_reason[current_frame_num];

            uint16_t new_class_id = 0;
            float new_confidence = 1.0f;
            if (g_zarr_bbox_edit_state.selected_frame == current_frame_num &&
                g_zarr_bbox_edit_state.selected_box >= 0 &&
                g_zarr_bbox_edit_state.selected_box <
                    static_cast<int>(zarr_boxes.size())) {
              const auto &selected_box =
                  zarr_boxes[g_zarr_bbox_edit_state.selected_box];
              new_class_id = selected_box.class_id;
              new_confidence = selected_box.confidence;
            } else if (!zarr_boxes.empty()) {
              new_class_id = zarr_boxes.front().class_id;
              new_confidence = zarr_boxes.front().confidence;
            }

            LoggedBoundingBox new_box{};
            new_box.payload_timestamp_ns_epoch = 0;
            new_box.received_timestamp_ns_epoch = 0;
            new_box.payload_frame_id =
                static_cast<uint64_t>(std::max(0, current_frame_num));
            new_box.payload_camera_id = 0;
            new_box.box_index_in_payload =
                static_cast<uint8_t>(editable_boxes.size());
            new_box.x_min = full_frame_edit_result.new_rect.x_min;
            new_box.y_min = full_frame_edit_result.new_rect.y_min;
            new_box.width = full_frame_edit_result.new_rect.width;
            new_box.height = full_frame_edit_result.new_rect.height;
            new_box.class_id = new_class_id;
            new_box.confidence =
                std::isfinite(new_confidence) ? new_confidence : 1.0f;

            editable_boxes.push_back(new_box);
            added_flags.push_back(1);
            manual_flags.push_back(1);
            source_indices.push_back(-1);
            source_detection_source.push_back(0);
            source_reason.emplace_back("manual");
            g_zarr_bbox_edit_state.dirty_frames.insert(current_frame_num);
            g_zarr_bbox_edit_state.selected_frame = current_frame_num;
            g_zarr_bbox_edit_state.selected_box =
                static_cast<int>(editable_boxes.size() - 1);
          }
          if (full_frame_edit_result.request_move_selected) {
            auto &editable_boxes = g_zarr_bbox_edit_state.ensureFrameOverride(
                current_frame_num, loaded_zarr_boxes, &detection_details);
            auto &manual_flags = g_zarr_bbox_edit_state.ensureManualFlags(
                current_frame_num, editable_boxes.size());
            const int selected_idx = full_frame_edit_result.move_box_index;
            if (selected_idx >= 0 &&
                selected_idx < static_cast<int>(editable_boxes.size())) {
              LoggedBoundingBox &moving_box = editable_boxes[selected_idx];
              const float max_x = std::max(
                  0.0f, static_cast<float>(scene->cameras[j].image_width) -
                            moving_box.width);
              const float max_y = std::max(
                  0.0f, static_cast<float>(scene->cameras[j].image_height) -
                            moving_box.height);
              moving_box.x_min =
                  std::clamp(full_frame_edit_result.move_target_x, 0.0f, max_x);
              moving_box.y_min =
                  std::clamp(full_frame_edit_result.move_target_y, 0.0f, max_y);
              if (selected_idx < static_cast<int>(manual_flags.size())) {
                manual_flags[selected_idx] = 1;
              }
              g_zarr_bbox_edit_state.dirty_frames.insert(current_frame_num);
            } else {
              g_zarr_bbox_edit_state.clearSelection();
            }
          }

          if (use_legacy_manual_keypoint_tools) {
            legacy_labeling_state.keypoints_find =
                camera_view_result.legacy_manual_keypoints_find;
            is_view_focused[j] = camera_view_result.view_focused;
          }
          const CameraViewTransportControlsResult &camera_transport_result =
              camera_view_result.transport_result;
          ps.slider_frame_number = static_cast<int>(
              std::clamp<int64_t>(camera_transport_result.slider_frame_number,
                                  0, std::numeric_limits<int>::max()));
          ps.slider_just_changed = camera_transport_result.slider_just_changed;
          const auto &transport_intent = camera_transport_result.intent;
          if (camera_transport_result.action ==
                  CameraViewTransportAction::TogglePlayback &&
              transport_intent.has_value()) {
            applyPlaybackToggleForPerf();
            writePlaybackTraceEvent("toggle_playback",
                                    json{{"source", "camera_controls"}});
          }
          if (camera_transport_result.seek_request.has_value()) {
            executePlaybackSeek(*camera_transport_result.seek_request);
          }
        }
        ImGui::End();
      }

      const CameraViewPlaybackShortcutsResult playback_shortcuts =
          handleCameraViewPlaybackShortcuts();
      const int64_t shortcut_frame_count =
          std::max<int64_t>(1, currentPlaybackFrameCount());
      if (playback_shortcuts.toggle_playback) {
        const auto intent = crimson::workspace::makePlaybackIntent(
            crimson::workspace::Command::TogglePlayback,
            workspaceCapabilities(), current_frame_num, shortcut_frame_count);
        if (intent.has_value()) {
          applyPlaybackToggleForPerf();
          writePlaybackTraceEvent("toggle_playback",
                                  json{{"source", "shortcut"}});
        }
      }
      if (playback_shortcuts.step_delta != 0) {
        const int step_base =
            ps.play_video ? current_frame_num : ps.to_display_frame_number;
        const auto intent = crimson::workspace::makePlaybackIntent(
            playback_shortcuts.step_delta < 0
                ? crimson::workspace::Command::StepBackward
                : crimson::workspace::Command::StepForward,
            workspaceCapabilities(), step_base, shortcut_frame_count,
            std::nullopt, std::abs(playback_shortcuts.step_delta));
        if (intent.has_value()) {
          const auto request = crimson::playback::makePlaybackSeekRequest(
              crimson::playback::PlaybackSeekPhase::Discrete,
              crimson::playback::PlaybackSeekOrigin::KeyboardShortcut,
              intent->target_frame, shortcut_frame_count);
          if (request.has_value()) {
            executePlaybackSeek(*request);
          }
        }
      }

      for (const auto &[name, flag] : window_need_decoding) {
        window_was_decoding[name] = flag.load();
      }
    }

    if (canonical_detection_route && !ps.play_video &&
        !canonical_presented_request_this_frame && current_frame_num >= 0) {
      // A collapsed camera window has no presented-frame consumer. Keep Frame
      // Inspect useful without allowing its cursor to compete with playback.
      (void)requestCanonicalDetectionFrame(current_frame_num);
    }

    workspace_state.setWindowRequested(
        crimson::workspace::Window::AdvancedCropPreview,
        frame_debug_window_state.keypoint_review_panel
            .show_advanced_crop_preview);
    if (workspace_state.shouldSubmit(
            crimson::workspace::Window::AdvancedCropPreview,
            workspaceCapabilities())) {
      const auto crop_preview_ui_start = std::chrono::steady_clock::now();
      RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
      CropFrameSource live_crop_frame_source;
      int crop_preview_frame_num = current_frame_num;
      if (video_loaded) {
        const int visible_idx =
            playback_session_controller.getVisibleCameraIndex();
        if (visible_idx >= 0 && scene->size_of_buffer > 0) {
          const auto &camera = scene->cameras[visible_idx];
          if (ps.play_video && camera.texture_has_valid_frame &&
              camera.last_uploaded_frame >= 0) {
            crop_preview_frame_num = camera.last_uploaded_frame;
          }
          const int preferred_slot = ps.read_head % scene->size_of_buffer;
          const int slot_index = findCameraDisplaySlotForFrame(
              *scene, visible_idx, crop_preview_frame_num, preferred_slot);
          if (camera.texture_has_valid_frame &&
              camera.last_uploaded_frame == crop_preview_frame_num &&
              camera.image_texture != 0) {
            live_crop_frame_source.frame_number = crop_preview_frame_num;
            live_crop_frame_source.width = static_cast<int>(camera.image_width);
            live_crop_frame_source.height =
                static_cast<int>(camera.image_height);
            live_crop_frame_source.texture_id = camera.image_texture;
            live_crop_frame_source.texture_width =
                camera.display_texture_width > 0
                    ? camera.display_texture_width
                    : static_cast<int>(camera.image_width);
            live_crop_frame_source.texture_height =
                camera.display_texture_height > 0
                    ? camera.display_texture_height
                    : static_cast<int>(camera.image_height);
            live_crop_frame_source.texture_frame_number =
                camera.last_uploaded_frame;
          }
          if (slot_index >= 0) {
            const auto &slot = camera.display_buffer[slot_index];
            if (!slot.available_to_write &&
                slot.frame_number == crop_preview_frame_num &&
                slot.frame != nullptr) {
              live_crop_frame_source.frame = slot.frame;
              live_crop_frame_source.frame_number = slot.frame_number;
              live_crop_frame_source.width =
                  static_cast<int>(camera.image_width);
              live_crop_frame_source.height =
                  static_cast<int>(camera.image_height);
              live_crop_frame_source.pitch_bytes = slot.pitch_bytes;
              live_crop_frame_source.color_matrix = slot.color_matrix;
              live_crop_frame_source.color_range = slot.color_range;
              if (scene->use_cpu_buffer &&
                  slot.format == FramePixelFormat::RGBA8) {
                live_crop_frame_source.storage = CropFrameStorage::HostRGBA32;
              } else if (slot.format == FramePixelFormat::RGBA8) {
                live_crop_frame_source.storage = CropFrameStorage::DeviceRGBA32;
              } else if (slot.format == FramePixelFormat::NV12) {
                live_crop_frame_source.storage = CropFrameStorage::DeviceNV12;
              }
            }
          }
        }
      }
      ZarrPersistedCropProvider persisted_crop_image_provider(zarr_loader);
      LiveCropImageProvider live_crop_image_provider(zarr_loader,
                                                     live_crop_frame_source);
      ChainedCropImageProvider crop_image_provider(
          live_crop_image_provider, persisted_crop_image_provider);
      std::optional<CropSpec> selected_crop_spec;
      int selected_detection_index = -1;
      if (g_zarr_bbox_edit_state.selected_frame == crop_preview_frame_num &&
          g_zarr_bbox_edit_state.selected_box >= 0 &&
          activeDetectionRepository().descriptor().available) {
        const auto crop_detection_descriptor =
            activeDetectionRepository().descriptor();
        const auto crop_detection_frame = activeDetectionRepository().resolveFrame(
            static_cast<size_t>(crop_preview_frame_num), false);
        const auto loaded_crop_boxes =
            crimson::platform::nvidia::makeLegacyBoundingBoxes(
                crop_detection_descriptor, crop_detection_frame);
        const auto resolved_crop_boxes =
            g_zarr_bbox_edit_state.resolveFrameBoxes(crop_preview_frame_num,
                                                     loaded_crop_boxes);
        if (g_zarr_bbox_edit_state.selected_box <
            static_cast<int>(resolved_crop_boxes.size())) {
          const auto &selected_box = resolved_crop_boxes[static_cast<size_t>(
              g_zarr_bbox_edit_state.selected_box)];
          CropSpec crop_spec;
          crop_spec.offset_x = selected_box.x_min;
          crop_spec.offset_y = selected_box.y_min;
          crop_spec.width_px = selected_box.width;
          crop_spec.height_px = selected_box.height;
          crop_spec.valid =
              selected_box.width > 0.0f && selected_box.height > 0.0f;
          if (crop_spec.valid) {
            selected_crop_spec = crop_spec;
          }

          auto source_it = g_zarr_bbox_edit_state.frame_source_indices.find(
              crop_preview_frame_num);
          if (source_it != g_zarr_bbox_edit_state.frame_source_indices.end() &&
              g_zarr_bbox_edit_state.selected_box <
                  static_cast<int>(source_it->second.size())) {
            selected_detection_index = source_it->second[static_cast<size_t>(
                g_zarr_bbox_edit_state.selected_box)];
          } else if (g_zarr_bbox_edit_state.selected_box <
                     static_cast<int>(loaded_crop_boxes.size())) {
            selected_detection_index = g_zarr_bbox_edit_state.selected_box;
          }
        }
      }
      const CropPreviewWindowContext crop_preview_context{
          crop_image_provider,
          zarr_loader,
          keypoint_repository,
          refined_keypoint_repo,
          crop_preview_frame_num,
          g_zarr_bbox_edit_state.selected_frame,
          g_zarr_bbox_edit_state.selected_box,
          selected_detection_index,
          selected_crop_spec,
          ps.play_video,
          &frame_debug_window_state.keypoint_review_panel
               .show_advanced_crop_preview,
      };
      const auto crop_preview_result = drawCropPreviewWindow(
          crop_preview_context, crop_preview_window_state);
      frame_crop_preview_perf = crop_preview_result.perf;
      if (ui_reference_capture.waitingForStableFrame() &&
          ui_reference.state == UiReferenceState::CropPreview &&
          crop_preview_window_state.displayed_crop_source_frame ==
              ui_reference.target_frame &&
          crop_preview_window_state.last_width > 0 &&
          crop_preview_window_state.last_height > 0) {
        ui_reference.crop_ready = true;
        ui_reference.crop_source_frame =
            crop_preview_window_state.displayed_crop_source_frame;
        ui_reference.crop_source_label =
            crop_preview_window_state.displayed_crop_source_label;
      }

      startRefinedKeypointWrite(
          crop_preview_result.editor_action,
          crop_preview_result.selected_keypoint_selection);
      frame_crop_preview_ui_ms +=
          durationMs(std::chrono::steady_clock::now() - crop_preview_ui_start);
    }

    if (workspace_state.shouldSubmit(crimson::workspace::Window::Stimulus,
                                     workspaceCapabilities())) {
      const auto stimulus_debug_windows_result =
          drawStimulusPlaybackDebugWindows(StimulusPlaybackDebugWindowsContext{
              stimulus_player,
              zarr_loaded ? &stimulus_repository : nullptr,
              ps,
              seek_progress,
              current_frame_num,
              latest_decoded_frame[stimulus_player.window_name].load(),
          });
      frame_stimulus_window_ui_ms +=
          stimulus_debug_windows_result.stimulus_window_ui_ms;
      frame_stimulus_buffer_window_ui_ms +=
          stimulus_debug_windows_result.stimulus_buffer_window_ui_ms;
    }

    if (use_legacy_manual_keypoint_tools) {
      const auto keypoints_window_ui_start = std::chrono::steady_clock::now();
      const KeypointsWindowContext keypoints_window_context{
          static_cast<int>(scene->num_cams),
          legacy_labeling_state,
          current_frame_num,
          camera_names,
          is_view_focused,
      };
      drawKeypointsWindow(keypoints_window_context);
      frame_keypoints_window_ui_ms += durationMs(
          std::chrono::steady_clock::now() - keypoints_window_ui_start);
    }

    if (use_legacy_manual_keypoint_tools) {
      const auto labeling_tool_ui_start = std::chrono::steady_clock::now();
#if CRIMSON_ENABLE_SFM
      constexpr bool triangulation_supported = true;
#else
      constexpr bool triangulation_supported = false;
#endif
      const LabelingToolWindowContext labeling_tool_context{
          root_dir,
          legacy_labeling_state,
          static_cast<int>(scene->num_cams),
          current_frame_num,
          triangulation_supported,
          legacy_labeling_state.nextLabeledFrameAfter(current_frame_num),
      };
      const LabelingToolWindowResult labeling_tool_result =
          drawLabelingToolWindow(labeling_tool_context,
                                 labeling_tool_window_state);
      const LabelingToolWorkflowContext labeling_tool_workflow_context{
          legacy_labeling_state,
          current_frame_num,
          camera_params,
          scene,
          camera_names,
          &input_is_imgs,
          imgs_names,
          error_message,
          show_error,
      };
      const LabelingToolWorkflowResult labeling_tool_workflow_result =
          applyLabelingToolWindowActions(labeling_tool_result,
                                         labeling_tool_workflow_context);

      if (labeling_tool_workflow_result.jump_target_frame.has_value()) {
        playback_session_controller.seekToFrame(
            *labeling_tool_workflow_result.jump_target_frame, true);
      }

      frame_labeling_tool_ui_ms +=
          durationMs(std::chrono::steady_clock::now() - labeling_tool_ui_start);
    }

    static TimelineScrollState shared_timeline_scroll_state;
    static StimulusEventTimelineWindowState stimulus_timeline_window_state;
    static AnalysisTimelineWindowState analysis_timeline_window_state;
    static crimson::gui::CanonicalTimelineWindowState
        canonical_timeline_window_state;

    if (ui_reference_capture.waitingForStableFrame() &&
        !ui_reference.analysis_state_applied &&
        (ui_reference.state == UiReferenceState::AnalysisEye ||
         ui_reference.state == UiReferenceState::AnalysisTailStimulus)) {
      analysis_timeline_window_state.show_smoothed = false;
      analysis_timeline_window_state.show_instantaneous = false;
      analysis_timeline_window_state.show_heading_raw = false;
      analysis_timeline_window_state.show_heading_smoothed = false;
      analysis_timeline_window_state.show_heading_per_second = false;
      analysis_timeline_window_state.show_swim_bouts = false;
      analysis_timeline_window_state.show_detector_response = false;
      analysis_timeline_window_state.show_distance_trace = false;
      analysis_timeline_window_state.show_track_position = false;
      if (ui_reference.state == UiReferenceState::AnalysisEye) {
        analysis_timeline_window_state.show_eye_angle_traces = true;
        analysis_timeline_window_state.show_eye_left_trace = true;
        analysis_timeline_window_state.show_eye_right_trace = true;
        analysis_timeline_window_state.show_eye_vergence_trace = true;
        analysis_timeline_window_state.show_tail_tip_angle = false;
        analysis_timeline_window_state.show_tail_tip_lateral_deflection = false;
        analysis_timeline_window_state.show_tail_curvature = false;
        analysis_timeline_window_state.show_stimulus_context = false;

        const auto &eye_data = zarr_loader.getEyeAngleAnalysisData();
        int default_index = 0;
        for (size_t idx = 0; idx < eye_data.representations.size(); ++idx) {
          if (eye_data.representations[idx].key ==
              eye_data.default_representation) {
            default_index = static_cast<int>(idx);
            break;
          }
        }
        analysis_timeline_window_state.eye_angle_representation_index =
            eye_data.representations.size() > 1 ? (default_index == 0 ? 1 : 0)
                                                : default_index;
      } else {
        analysis_timeline_window_state.show_eye_angle_traces = false;
        analysis_timeline_window_state.show_tail_tip_angle = true;
        analysis_timeline_window_state.show_tail_tip_lateral_deflection = true;
        analysis_timeline_window_state.show_tail_curvature = true;
        analysis_timeline_window_state.show_stimulus_context = true;
      }
      ui_reference.analysis_state_applied = true;
    }

    // Stimulus Event Timeline Window
    if (workspace_state.shouldSubmit(
            crimson::workspace::Window::StimulusEventTimeline,
            workspaceCapabilities())) {
      const auto stimulus_timeline_ui_start = std::chrono::steady_clock::now();
      StimulusEventTimelineWindowContext stimulus_timeline_context{
          zarr_loader,
          shared_timeline_scroll_state,
          current_frame_num,
          video_fps,
      };
      StimulusEventTimelineWindowResult stimulus_timeline_result =
          drawStimulusEventTimelineWindow(stimulus_timeline_context,
                                          stimulus_timeline_window_state);
      if (stimulus_timeline_window_state.selected_event_idx >= 0) {
        workspace_state.selections().stimulus_event_index = static_cast<size_t>(
            stimulus_timeline_window_state.selected_event_idx);
      } else {
        workspace_state.selections().stimulus_event_index.reset();
      }
      if (stimulus_timeline_result.seek_target_frame.has_value()) {
        writeClippedPlaybackStateEvent(
            "seek_request",
            json{{"source", "stimulus_event_timeline"},
                 {"phase", "before"},
                 {"target_frame", *stimulus_timeline_result.seek_target_frame}},
            true);
        playback_session_controller.seekToFrame(
            *stimulus_timeline_result.seek_target_frame, true);
        writeClippedPlaybackStateEvent(
            "seek_request",
            json{{"source", "stimulus_event_timeline"},
                 {"phase", "after"},
                 {"target_frame", *stimulus_timeline_result.seek_target_frame}},
            true);
      }
      frame_stimulus_timeline_ui_ms += durationMs(
          std::chrono::steady_clock::now() - stimulus_timeline_ui_start);
    }

    // Analysis timeline window
    if (workspace_state.shouldSubmit(
            crimson::workspace::Window::AnalysisTimeline,
            workspaceCapabilities())) {
      const auto analysis_timeline_ui_start = std::chrono::steady_clock::now();
      if (canonical_detection_route) {
        static uint64_t logged_canonical_timeline_generation = 0;
        std::string timeline_request_error;
        (void)canonical_timeline_session.requestFrame(
            current_frame_num, ps.just_seeked, &timeline_request_error);
        const auto timeline_snapshot =
            canonical_timeline_session.snapshot(current_frame_num);
        if (timeline_snapshot.generation !=
                logged_canonical_timeline_generation &&
            timeline_snapshot.eye_angles.state ==
                crimson::gui::CanonicalTimelineProductState::Ready &&
            timeline_snapshot.motion.state ==
                crimson::gui::CanonicalTimelineProductState::Ready &&
            timeline_snapshot.swim_bouts.state ==
                crimson::gui::CanonicalTimelineProductState::Ready) {
          size_t eye_finite = 0;
          for (const auto &trace : timeline_snapshot.eye_angles.window->traces) {
            eye_finite += static_cast<size_t>(std::count_if(
                trace.values.begin(), trace.values.end(),
                [](double value) { return std::isfinite(value); }));
          }
          size_t motion_finite = 0;
          for (const auto &trace : timeline_snapshot.motion.window->traces) {
            motion_finite += static_cast<size_t>(std::count_if(
                trace.values.begin(), trace.values.end(),
                [](double value) { return std::isfinite(value); }));
          }
          const size_t detector_finite = static_cast<size_t>(std::count_if(
              timeline_snapshot.swim_bouts.window->detector_values.begin(),
              timeline_snapshot.swim_bouts.window->detector_values.end(),
              [](double value) { return std::isfinite(value); }));
          std::cout
              << "[CanonicalTimeline] windows=ready generation="
              << timeline_snapshot.generation
              << " frame=" << current_frame_num
              << " eye_source="
              << timeline_snapshot.eye_angles.source_identity
              << " eye_traces="
              << timeline_snapshot.eye_angles.window->traces.size()
              << " eye_finite=" << eye_finite
              << " motion_source=" << timeline_snapshot.motion.source_identity
              << " motion_traces="
              << timeline_snapshot.motion.window->traces.size()
              << " motion_finite=" << motion_finite
              << " bout_source="
              << timeline_snapshot.swim_bouts.source_identity
              << " bout_intervals="
              << timeline_snapshot.swim_bouts.window->intervals.size()
              << " detector_finite=" << detector_finite << std::endl;
          logged_canonical_timeline_generation = timeline_snapshot.generation;
        }
        crimson::gui::drawCanonicalTimelineWindow(
            timeline_snapshot, current_frame_num, video_fps,
            &canonical_timeline_window_state);
        workspace_state.selections().eye_angle_representation_key =
            timeline_snapshot.eye_angles.descriptor.default_representation;
        workspace_state.selections().motion_source_key =
            timeline_snapshot.motion.descriptor.default_source;
        workspace_state.selections().swim_bout_candidate_key =
            timeline_snapshot.swim_bouts.descriptor.default_candidate;
      } else {
        AnalysisTimelineWindowContext analysis_timeline_context{
            zarr_loader, shared_timeline_scroll_state, current_frame_num,
            video_fps, &frame_analysis_timeline_perf,
        };
        drawAnalysisTimelineWindow(analysis_timeline_context,
                                   analysis_timeline_window_state);
        const auto &eye_data = zarr_loader.getEyeAngleAnalysisData();
        const int eye_index =
            analysis_timeline_window_state.eye_angle_representation_index;
        if (eye_index >= 0 &&
            static_cast<size_t>(eye_index) < eye_data.representations.size()) {
          workspace_state.selections().eye_angle_representation_key =
              eye_data.representations[static_cast<size_t>(eye_index)].key;
        }
      }
      frame_movement_timeline_ui_ms += durationMs(
          std::chrono::steady_clock::now() - analysis_timeline_ui_start);
      frame_analysis_timeline_perf.total_window_ms =
          frame_movement_timeline_ui_ms;
    }

    bool detection_quality_requested = workspace_state.windowRequested(
        crimson::workspace::Window::DetectionQualityTimeline);
    bool keypoint_quality_requested = workspace_state.windowRequested(
        crimson::workspace::Window::KeypointQualityTimeline);
    quality_timeline_session.update(
        current_frame_num, ps.just_seeked, detection_quality_requested,
        detection_quality_timeline_controls, keypoint_quality_requested,
        keypoint_quality_timeline_controls);
    if (workspace_state.shouldSubmit(
            crimson::workspace::Window::DetectionQualityTimeline,
            workspaceCapabilities())) {
      crimson::gui::drawDetectionQualityTimelineWindow(
          &detection_quality_timeline_controls, &detection_quality_requested,
          quality_timeline_session.detectionState(),
          quality_timeline_session.detectionDescriptor(),
          quality_timeline_session.detectionWindow(),
          quality_timeline_session.detectionOverview(),
          quality_timeline_session.detectionError(), current_frame_num,
          video_fps,
          [&](int64_t frame) {
            playback_session_controller.seekToFrame(static_cast<int>(frame),
                                                    true);
            return true;
          },
          true);
      workspace_state.setWindowRequested(
          crimson::workspace::Window::DetectionQualityTimeline,
          detection_quality_requested);
    }
    if (workspace_state.shouldSubmit(
            crimson::workspace::Window::KeypointQualityTimeline,
            workspaceCapabilities())) {
      crimson::gui::drawKeypointQualityTimelineWindow(
          &keypoint_quality_timeline_controls, &keypoint_quality_requested,
          quality_timeline_session.keypointState(),
          quality_timeline_session.keypointDescriptor(),
          quality_timeline_session.keypointWindow(),
          quality_timeline_session.keypointOverview(),
          quality_timeline_session.keypointError(), current_frame_num,
          video_fps,
          [&](int64_t frame) {
            playback_session_controller.seekToFrame(static_cast<int>(frame),
                                                    true);
            return true;
          },
          true);
      workspace_state.setWindowRequested(
          crimson::workspace::Window::KeypointQualityTimeline,
          keypoint_quality_requested);
    }

    shared_timeline_scroll_state.prev_enabled =
        shared_timeline_scroll_state.enabled;

    show_help_window =
        workspace_state.windowRequested(crimson::workspace::Window::Help);
    processHelpMenuShortcut(show_help_window);
    workspace_state.setWindowRequested(crimson::workspace::Window::Help,
                                       show_help_window);

    if (workspace_state.shouldSubmit(crimson::workspace::Window::Help,
                                     workspaceCapabilities())) {
      const auto help_menu_ui_start = std::chrono::steady_clock::now();
      drawHelpMenuWindow(show_help_window);
      workspace_state.setWindowRequested(crimson::workspace::Window::Help,
                                         show_help_window);
      frame_help_menu_ui_ms +=
          durationMs(std::chrono::steady_clock::now() - help_menu_ui_start);
    }

    drawErrorPopup(show_error, error_message);

    if (ui_reference_capture.waitingForStableFrame()) {
      ui_reference.presented_stimulus_frame =
          stimulus_player.last_displayed_frame;
      ui_reference.camera_buffer_valid = 0;
      ui_reference.camera_buffer_capacity = 0;
      const int reference_view_idx =
          playback_session_controller.getVisibleCameraIndex();
      if (scene != nullptr && reference_view_idx >= 0 &&
          reference_view_idx < static_cast<int>(scene->num_cams)) {
        ui_reference.camera_buffer_capacity =
            static_cast<int>(scene->size_of_buffer);
        const auto &reference_camera = scene->cameras[reference_view_idx];
        for (int slot_idx = 0; slot_idx < ui_reference.camera_buffer_capacity;
             ++slot_idx) {
          const auto snapshot = frameSlotSnapshotReadable(
              reference_camera.display_buffer[slot_idx]);
          if (snapshot.has_value() && snapshot->frame_number >= 0) {
            ++ui_reference.camera_buffer_valid;
          }
        }
      }
      ui_reference.stimulus_buffer_valid = 0;
      ui_reference.stimulus_buffer_capacity =
          stimulus_player.loaded ? stimulus_player.buffer_size : 0;
      if (stimulus_player.loaded && stimulus_player.display_buffer != nullptr) {
        for (int slot_idx = 0; slot_idx < ui_reference.stimulus_buffer_capacity;
             ++slot_idx) {
          const auto snapshot = frameSlotSnapshotReadable(
              stimulus_player.display_buffer[slot_idx]);
          if (snapshot.has_value() && snapshot->frame_number >= 0) {
            ++ui_reference.stimulus_buffer_valid;
          }
        }
      }
      const bool camera_buffer_ready =
          ui_reference.camera_buffer_capacity > 0 &&
          ui_reference.camera_buffer_valid ==
              ui_reference.camera_buffer_capacity;
      const bool stimulus_buffer_ready =
          ui_reference.state != UiReferenceState::StimulusDebug ||
          ui_reference.stimulus_buffer_capacity > 0;
      const bool exact_camera_ready =
          ui_reference.exact_presented_this_frame && !ps.play_video &&
          current_frame_num == ui_reference.target_frame &&
          ps.to_display_frame_number == ui_reference.target_frame &&
          camera_buffer_ready && stimulus_buffer_ready;
      const bool optional_eye_overlays_ready =
          zarr_loader.getRefinedSubjectMaskOptionalOverlayStatus() ==
          "optional overlays ready";
      bool state_ready = false;
      switch (ui_reference.state) {
      case UiReferenceState::Workspace: {
        if (!canonical_detection_route) {
          state_ready = true;
          break;
        }
        const auto reference_detection_frame =
            canonical_detection_repository.resolveFrame(
                static_cast<size_t>(
                    std::max(0, ui_reference.target_frame)),
                false);
        const bool reference_detection_ready =
            ui_reference.target_frame >= 0 &&
            reference_detection_frame.ready() &&
            reference_detection_frame.frame_id ==
                static_cast<size_t>(ui_reference.target_frame);
        state_ready =
            reference_detection_ready &&
            ui_reference.bbox_query_frame == ui_reference.target_frame &&
            (reference_detection_frame.observations.empty() ||
             frame_bbox_overlay_item_count > 0);
        break;
      }
      case UiReferenceState::Overlays:
        if (canonical_detection_route) {
          const auto ready = crimson::gui::CanonicalOverlayState::Ready;
          const auto& overlays = last_canonical_overlay_snapshot;
          state_ready = show_eye_masks &&
              overlays.requested_frame == ui_reference.target_frame &&
              canonical_overlay_draw_frame == ui_reference.target_frame &&
              ui_reference.bbox_query_frame == ui_reference.target_frame &&
              overlays.keypoints.state == ready && overlays.masks.state == ready &&
              overlays.shapes.state == ready && canonical_keypoint_draw_count > 0 &&
              canonical_shape_draw_count == canonical_expected_shape_draw_count &&
              frame_mask_overlay_perf.component_fill_count == canonical_expected_mask_draw_count;
          break;
        }
        state_ready =
            show_eye_masks && optional_eye_overlays_ready &&
            ui_reference.bbox_query_frame == ui_reference.target_frame &&
            frame_mask_overlay_perf.attempted &&
            frame_mask_overlay_perf.visible_roi_count > 0 &&
            frame_mask_overlay_perf.component_fill_count > 0 &&
            frame_mask_overlay_perf.contours_drawn > 0 &&
            frame_mask_overlay_perf.axes_drawn > 0 &&
            (frame_mask_overlay_perf.gaze_rays_drawn > 0 ||
             frame_mask_overlay_perf.visual_cones_drawn > 0) &&
            frame_mask_overlay_perf.angle_labels_drawn > 0;
        break;
      case UiReferenceState::Polar:
        state_ready = perf_chaser_distance_polar_scene.ready() &&
                      perf_chaser_distance_polar_scene.requested_camera_frame ==
                          ui_reference.target_frame &&
                      perf_chaser_distance_polar_scene.source_camera_frame ==
                          ui_reference.target_frame &&
                      perf_chaser_distance_polar_scene.point_count > 0;
        break;
      case UiReferenceState::StimulusOverlay:
        state_ready =
            perf_stimulus_camera_overlay_scene.ready() &&
            perf_stimulus_camera_overlay_scene.requested_camera_frame ==
                ui_reference.target_frame &&
            perf_stimulus_camera_overlay_scene.source_camera_frame ==
                ui_reference.target_frame &&
            !perf_stimulus_camera_overlay_scene.primitives.empty() &&
            !perf_stimulus_camera_overlay_scene.text.empty();
        break;
      case UiReferenceState::StimulusDebug:
        state_ready = show_stimulus_debug_windows && stimulus_player.loaded &&
                      ui_reference.target_stimulus_frame >= 0 &&
                      ui_reference.presented_stimulus_frame ==
                          ui_reference.target_stimulus_frame;
        break;
      case UiReferenceState::CropPreview:
        state_ready = ui_reference.crop_ready;
        break;
      case UiReferenceState::AnalysisEye:
        if (canonical_detection_route) {
          (void)canonical_timeline_session.requestFrame(
              ui_reference.target_frame, false, nullptr);
          const auto timeline_reference =
              canonical_timeline_session.snapshot(ui_reference.target_frame);
          state_ready =
              ui_reference.analysis_state_applied &&
              timeline_reference.eye_angles.state ==
                  crimson::gui::CanonicalTimelineProductState::Ready &&
              timeline_reference.motion.state ==
                  crimson::gui::CanonicalTimelineProductState::Ready &&
              (timeline_reference.swim_bouts.state ==
                   crimson::gui::CanonicalTimelineProductState::Ready ||
               timeline_reference.swim_bouts.state ==
                   crimson::gui::CanonicalTimelineProductState::Empty);
        } else {
          state_ready = ui_reference.analysis_state_applied &&
                        zarr_loader.hasEyeAngleAnalysisData() &&
                        show_eye_masks && optional_eye_overlays_ready &&
                        frame_mask_overlay_perf.attempted &&
                        frame_mask_overlay_perf.visible_roi_count > 0 &&
                        frame_mask_overlay_perf.component_fill_count > 0 &&
                        frame_mask_overlay_perf.contours_drawn > 0 &&
                        frame_mask_overlay_perf.axes_drawn > 0 &&
                        (frame_mask_overlay_perf.gaze_rays_drawn > 0 ||
                         frame_mask_overlay_perf.visual_cones_drawn > 0) &&
                        frame_mask_overlay_perf.angle_labels_drawn > 0;
        }
        break;
      case UiReferenceState::AnalysisTailStimulus:
        state_ready =
            ui_reference.analysis_state_applied &&
            zarr_loader.hasTailKinematicsData() &&
            (zarr_loader.hasStimulusSteps() || zarr_loader.hasStimulusEvents());
        break;
      case UiReferenceState::Empty:
      case UiReferenceState::Keypoints:
      case UiReferenceState::Count:
        state_ready = false;
        break;
      }
      if (ui_reference_capture.observeFrame(exact_camera_ready, state_ready)) {
        std::cout << "[UiReference] render-ready state="
                  << uiReferenceStateName(ui_reference.state)
                  << " target_frame=" << ui_reference.target_frame
                  << " presented_frame=" << ui_reference.presented_frame
                  << " stable_frames="
                  << ui_reference_capture.stableFrameCount() << std::endl;
      }
    }

    // Rendering
    frame_ui_build_ms =
        durationMs(std::chrono::steady_clock::now() - ui_build_start);
    const auto imgui_render_start = std::chrono::steady_clock::now();
    ImGui::Render();
    if (ui_reference.enabled) {
      ui_semantic_snapshot =
          crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
    }
    frame_imgui_render_ms =
        durationMs(std::chrono::steady_clock::now() - imgui_render_start);
    ImDrawData *imgui_draw_data = ImGui::GetDrawData();
    if (imgui_draw_data != nullptr) {
      frame_imgui_draw_list_count = imgui_draw_data->CmdListsCount;
      frame_imgui_total_vtx_count = imgui_draw_data->TotalVtxCount;
      frame_imgui_total_idx_count = imgui_draw_data->TotalIdxCount;
      int draw_cmd_count = 0;
      for (int list_idx = 0; list_idx < imgui_draw_data->CmdListsCount;
           ++list_idx) {
        const ImDrawList *draw_list = imgui_draw_data->CmdLists[list_idx];
        if (draw_list != nullptr) {
          draw_cmd_count += draw_list->CmdBuffer.Size;
        }
      }
      frame_imgui_draw_cmd_count = draw_cmd_count;
    }
    int display_w, display_h;
    glfwGetFramebufferSize(window->render_target, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                 clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto gl_draw_start = std::chrono::steady_clock::now();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    frame_gl_draw_ms =
        durationMs(std::chrono::steady_clock::now() - gl_draw_start);
    if (diagnostics_session.clippedFrameTraceEnabled() && zarr_loaded &&
        zarr_loader.hasClippedCollection() && scene != nullptr) {
      for (int camera_idx = 0; camera_idx < static_cast<int>(scene->num_cams);
           ++camera_idx) {
        auto &trace = scene->cameras[camera_idx].texture_draw_trace;
        if (!trace.enabled || trace.queue_sequence == 0 ||
            trace.last_logged_sequence == trace.queue_sequence) {
          continue;
        }
        trace.last_logged_sequence = trace.queue_sequence;
        const auto texture_draw = nvidia_diagnostics::toTraceTextureDraw(trace);
        nvidia_trace::recordTextureDrawOutcome(clipped_frame_trace_stats,
                                               texture_draw);
        diagnostics_session.writeClippedFrameTrace(
            nvidia_trace::clippedTextureDrawEventJson(texture_draw),
            /*force_flush=*/false);

        if (clipped_texture_dump.enabled && !clipped_texture_dump.dumped &&
            trace.callback_observed && trace.callback_bound_texture_id != 0 &&
            trace.front_parent_frame == clipped_texture_dump.parent_frame) {
          clipped_texture_dump.dumped = true;
          const auto dump_result =
              crimson::platform::nvidia::dumpGlTextureToPng(
                  trace.callback_bound_texture_id,
                  clipped_texture_dump.output_path);

          std::optional<nvidia_trace::ClippedTextureDumpResolverSnapshot>
              dump_resolver;
          if (trace.front_parent_frame >= 0) {
            const auto *row =
                zarr_loader.resolveClippedFrame(trace.front_parent_frame);
            if (row != nullptr) {
              dump_resolver = nvidia_trace::ClippedTextureDumpResolverSnapshot{
                  row->parent_frame_index,
                  row->recording_frame_id,
                  row->clip_id,
                  row->clip_local_frame_index,
                  row->camera_serial,
                  static_cast<uint64_t>(row->selected_run_index)};
            }
          }

          const std::filesystem::path metadata_path =
              crimson::platform::nvidia::pathWithExtension(dump_result.raw_path,
                                                           ".json");
          nvidia_trace::ClippedTextureDumpSnapshot dump_snapshot;
          dump_snapshot.requested_parent_frame =
              clipped_texture_dump.parent_frame;
          dump_snapshot.ok = dump_result.ok;
          if (!dump_result.error.empty()) {
            dump_snapshot.error = dump_result.error;
          }
          dump_snapshot.raw_path = dump_result.raw_path.string();
          dump_snapshot.flip_y_path = dump_result.flip_y_path.string();
          dump_snapshot.metadata_path = metadata_path.string();
          dump_snapshot.width = dump_result.width;
          dump_snapshot.height = dump_result.height;
          dump_snapshot.texture_draw = texture_draw;
          dump_snapshot.resolver = std::move(dump_resolver);
          json dump_event =
              nvidia_trace::clippedTextureDumpEventJson(dump_snapshot);

          std::error_code metadata_ec;
          if (metadata_path.has_parent_path()) {
            std::filesystem::create_directories(metadata_path.parent_path(),
                                                metadata_ec);
          }
          if (!metadata_ec) {
            std::ofstream metadata_stream(metadata_path,
                                          std::ios::out | std::ios::trunc);
            if (metadata_stream.is_open()) {
              metadata_stream << dump_event.dump(2) << "\n";
            } else {
              dump_event["metadata_write_error"] =
                  "failed to open metadata file";
            }
          } else {
            dump_event["metadata_write_error"] = metadata_ec.message();
          }

          diagnostics_session.writeClippedFrameTrace(
              std::move(dump_event), /*force_flush=*/true);
          if (dump_result.ok) {
            std::cout << "[ClippedTextureDump] Wrote " << dump_result.raw_path
                      << " and " << dump_result.flip_y_path << std::endl;
          } else {
            std::cerr << "[ClippedTextureDump] Failed: " << dump_result.error
                      << std::endl;
          }
        }
      }
    }

    // Update and Render additional Platform Windows
    // (Platform functions may change the current OpenGL context, so we
    // save/restore it to make it easier to paste this code elsewhere.
    //  For this specific demo app we could also call
    //  glfwMakeContextCurrent(window) directly)
    //         if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    //             GLFWwindow *backup_current_context = glfwGetCurrentContext();
    //             ImGui::UpdatePlatformWindows();
    //             ImGui::RenderPlatformWindowsDefault();
    //             glfwMakeContextCurrent(backup_current_context);
    const auto swap_start = std::chrono::steady_clock::now();
    glfwSwapBuffers(window->render_target);
    frame_swap_ms = durationMs(std::chrono::steady_clock::now() - swap_start);

    if (ui_reference_capture.captureRequested()) {
      glFinish();
      const auto image_result = crimson::platform::nvidia::dumpGlBufferToPng(
          ui_reference.rendered_image_file, display_w, display_h, GL_FRONT);
      if (!image_result.ok) {
        std::cerr << "[UiReference] failed to capture rendered image: "
                  << image_result.error << std::endl;
        ui_reference_capture.fail(image_result.error);
        app_exit_code = 4;
        glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
      } else {
        ui_reference.rendered_image_width = image_result.width;
        ui_reference.rendered_image_height = image_result.height;
        (void)ui_reference_capture.markCaptureComplete();
      }
    }

    if (ui_reference_capture.readyToPublish()) {
      glFinish();
      int client_width = 0;
      int client_height = 0;
      int framebuffer_width = 0;
      int framebuffer_height = 0;
      glfwGetWindowSize(window->render_target, &client_width, &client_height);
      glfwGetFramebufferSize(window->render_target, &framebuffer_width,
                             &framebuffer_height);
      std::string eye_representation_key;
      const auto &eye_representations =
          zarr_loader.getEyeAngleAnalysisData().representations;
      const int eye_representation_index =
          analysis_timeline_window_state.eye_angle_representation_index;
      if (eye_representation_index >= 0 &&
          eye_representation_index <
              static_cast<int>(eye_representations.size())) {
        eye_representation_key =
            eye_representations[eye_representation_index].key;
      }
      json marker = crimson::ui_reference::makeMarkerEnvelope(
          {"linux-opengl",
           ui_reference.state,
           cli_zarr_override_path,
           ui_reference.target_frame,
           ui_reference.presented_frame,
           ui_reference_capture.stableFrameCount(),
           {client_width, client_height},
           {framebuffer_width, framebuffer_height},
           {ui_reference.rendered_image_file,
            ui_reference.rendered_image_width,
            ui_reference.rendered_image_height,
            "opengl_front_buffer"}});
      marker.update({
          {"presented_slot", ui_reference.presented_slot},
          {"view_idx", ui_reference.view_idx},
          {"current_frame", current_frame_num},
          {"slider_frame", ps.slider_frame_number},
          {"bbox_query_frame", ui_reference.bbox_query_frame},
          {"viewports",
           {{"camera",
             {{"x", perf_camera_viewport_x_px},
              {"y", perf_camera_viewport_y_px},
              {"width", perf_camera_viewport_width_px},
              {"height", perf_camera_viewport_height_px}}},
            {"camera_media",
             {{"x", perf_camera_media_x_px},
              {"y", perf_camera_media_y_px},
              {"width", perf_camera_media_width_px},
              {"height", perf_camera_media_height_px}}}}},
          {"semantic_snapshot",
           crimson::ui::semanticSnapshotJson(ui_semantic_snapshot)},
          {"buffers",
           {{"camera_valid", ui_reference.camera_buffer_valid},
            {"camera_capacity", ui_reference.camera_buffer_capacity},
            {"stimulus_valid", ui_reference.stimulus_buffer_valid},
            {"stimulus_capacity", ui_reference.stimulus_buffer_capacity}}},
          {"polar",
           crimson::ui_reference::polarSceneEvidenceJson(
               perf_chaser_distance_polar_scene,
               {perf_chaser_distance_polar_origin_x_px,
                perf_chaser_distance_polar_origin_y_px, 1.0, 1.0},
               chaser_distance_polar_repository != nullptr
                   ? &chaser_distance_polar_repository->descriptor()
                   : nullptr)},
          {"stimulus_camera_overlay",
           crimson::ui_reference::stimulusCameraOverlaySceneEvidenceJson(
               perf_stimulus_camera_overlay_scene,
               {perf_stimulus_camera_overlay_origin_x_px,
                perf_stimulus_camera_overlay_origin_y_px, 1.0, 1.0},
               stimulus_context_timeline_repository != nullptr
                   ? &stimulus_context_timeline_repository->descriptor()
                   : nullptr)},
          {"stimulus",
           {{"loaded", stimulus_player.loaded},
            {"debug_windows", show_stimulus_debug_windows},
            {"target_frame", ui_reference.target_stimulus_frame},
            {"presented_frame", ui_reference.presented_stimulus_frame}}},
          {"crop",
           {{"ready", ui_reference.crop_ready},
            {"source_frame", ui_reference.crop_source_frame},
            {"source_label", ui_reference.crop_source_label},
            {"width", crop_preview_window_state.last_width},
            {"height", crop_preview_window_state.last_height}}},
          {"overlays",
           {{"eye_masks", show_eye_masks},
            {"mask_draw_attempted", frame_mask_overlay_perf.attempted},
            {"visible_roi_count", frame_mask_overlay_perf.visible_roi_count},
            {"component_fill_count",
             frame_mask_overlay_perf.component_fill_count},
            {"contours_drawn", frame_mask_overlay_perf.contours_drawn},
            {"axes_drawn", frame_mask_overlay_perf.axes_drawn},
            {"gaze_rays_drawn", frame_mask_overlay_perf.gaze_rays_drawn},
            {"visual_cones_drawn", frame_mask_overlay_perf.visual_cones_drawn},
            {"visual_cone_overlaps_drawn",
             frame_mask_overlay_perf.visual_cone_overlaps_drawn},
            {"angle_labels_drawn", frame_mask_overlay_perf.angle_labels_drawn},
            {"optional_overlay_status",
             zarr_loader.getRefinedSubjectMaskOptionalOverlayStatus()},
            {"texture_cache_hits", frame_mask_overlay_perf.texture_cache_hits},
            {"texture_uploads", frame_mask_overlay_perf.texture_uploads}}},
          {"analysis",
           {{"state_applied", ui_reference.analysis_state_applied},
            {"eye_representation_index",
             analysis_timeline_window_state.eye_angle_representation_index},
            {"eye_representation_key", eye_representation_key},
            {"show_eye", analysis_timeline_window_state.show_eye_angle_traces},
            {"show_tail_angle",
             analysis_timeline_window_state.show_tail_tip_angle},
            {"show_tail_deflection",
             analysis_timeline_window_state.show_tail_tip_lateral_deflection},
            {"show_tail_curvature",
             analysis_timeline_window_state.show_tail_curvature},
            {"show_stimulus_context",
             analysis_timeline_window_state.show_stimulus_context}}}});
      if (canonical_detection_route) {
        const auto snapshot =
            canonical_timeline_session.snapshot(ui_reference.target_frame);
        auto traceCount = [](const auto& product) {
          size_t count = 0;
          if (product.window) {
            for (const auto& trace : product.window->traces) {
              count += static_cast<size_t>(std::count_if(
                  trace.values.begin(), trace.values.end(),
                  [](double value) { return std::isfinite(value); }));
            }
          }
          return count;
        };
        auto productMarker = [](const auto& product) {
          return json{
              {"state", crimson::gui::canonicalTimelineProductStateName(product.state)},
              {"source", product.source_identity},
              {"first_frame", product.window ? product.window->request.first_frame : -1},
              {"last_frame", product.window ? product.window->request.last_frame : -1}};
        };
        marker["analysis"]["canonical"] = true;
        marker["analysis"]["eye_representation_key"] =
            snapshot.eye_angles.descriptor.default_representation;
        marker["canonical_timelines"] = {
            {"frame", snapshot.requested_frame},
            {"eye", productMarker(snapshot.eye_angles)},
            {"motion", productMarker(snapshot.motion)},
            {"bouts", productMarker(snapshot.swim_bouts)}};
        marker["canonical_timelines"]["eye"]["finite_points"] =
            traceCount(snapshot.eye_angles);
        marker["canonical_timelines"]["motion"]["finite_points"] =
            traceCount(snapshot.motion);
        marker["canonical_timelines"]["bouts"]["interval_count"] =
            snapshot.swim_bouts.window
                ? snapshot.swim_bouts.window->intervals.size() : 0;
      }
      if (canonical_detection_route) {
        const auto& overlays = last_canonical_overlay_snapshot;
        const auto product = [](const auto& value) {
          json keys = json::array();
          if (value.frame) for (const auto& row : value.frame->detections) keys.push_back(row.instance_key);
          return json{{"state", crimson::gui::canonicalOverlayStateName(value.state)},
                      {"run", value.descriptor.run_name},
                      {"frame", value.frame ? value.frame->camera_frame : -1},
                      {"instance_keys", keys}, {"error", value.error}};
        };
        marker["canonical_overlays"] = {
            {"generation", overlays.generation},
            {"query_frame", overlays.requested_frame},
            {"draw_frame", canonical_overlay_draw_frame},
            {"keypoints", product(overlays.keypoints)},
            {"masks", product(overlays.masks)},
            {"shapes", product(overlays.shapes)},
            {"keypoint_primitives", canonical_keypoint_draw_count},
            {"heading_primitives", canonical_heading_draw_count},
            {"shape_primitives", canonical_shape_draw_count},
            {"shape_expected_primitives", canonical_expected_shape_draw_count},
            {"mask_expected_fills", canonical_expected_mask_draw_count},
            {"mask_cached_payload_bytes", overlays.mask_metrics.cached_payload_bytes},
            {"mask_peak_cached_payload_bytes", overlays.mask_metrics.peak_cached_payload_bytes},
            {"mask_logical_payload_bytes", overlays.mask_metrics.chunk_source_bytes_read},
            {"mask_mapping_retained_bytes", overlays.mask_metrics.metadata_retained_bytes}};
        if (overlays.selection) {
          marker["canonical_overlays"]["recording_id"] = overlays.selection->recording_id;
          marker["canonical_overlays"]["eye_run"] = overlays.selection->eye.run_id;
          marker["canonical_overlays"]["keypoint_identity"] = overlays.selection->keypoints.identity_digest;
          marker["canonical_overlays"]["mask_payload_digest"] = overlays.selection->mask.manifest_payload_digest;
          marker["canonical_overlays"]["shape_identity"] = overlays.selection->shape.identity_digest;
        }
      }
      std::string marker_error;
      if (!crimson::ui_reference::writeUiReferenceMarkerAtomically(
              ui_reference.ready_file, marker, &marker_error)) {
        std::cerr << "[UiReference] failed to write ready marker: "
                  << marker_error << std::endl;
        ui_reference_capture.fail(marker_error);
        app_exit_code = 4;
        glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
      } else {
        (void)ui_reference_capture.markPublished();
        std::cout << "[UiReference] READY state="
                  << uiReferenceStateName(ui_reference.state)
                  << " target_frame=" << ui_reference.target_frame
                  << " presented_frame=" << ui_reference.presented_frame
                  << " ready_file=" << ui_reference.ready_file << std::endl;
      }
    }

    const bool playback_was_just_seeked = ps.just_seeked;
    if (playback_trace_presented_frame >= 0) {
      camera_presentation_tracker.record(playback_requested_camera_frame,
                                         playback_trace_presented_frame,
                                         playback_was_just_seeked);
    }
    if (playback_was_just_seeked) {
      ps.just_seeked = false;
    }
    const auto playback_commit =
        playback_session_controller.commitPresentedFrame(
            playback_presenter_target_frame, playback_trace_presented_frame,
            playback_trace_presented_slot);
    if (playback_commit.eligible) {
      playback_commit_previous_frame = playback_commit.previous_committed_frame;
      playback_commit_frame = playback_commit.frame;
      playback_commit_slot = playback_commit.slot;
      playback_release_attempts = playback_commit.release_attempts;
      playback_release_count = playback_commit.release_count;
      playback_release_skip_count = playback_commit.release_skip_count;
      playback_release_deferred = playback_commit.release_deferred;
      const bool presented_from_slot = playback_commit.presented_from_slot;

      if (playback_commit_frame >= 0 || playback_release_attempts > 0 ||
          playback_release_deferred || playback_target_clamped_to_buffer ||
          playback_was_just_seeked) {
        json commit_details{
            {"requested_frame", playback_requested_camera_frame},
            {"presenter_target_frame", playback_presenter_target_frame},
            {"presenter_target_slot", playback_presenter_target_slot},
            {"target_clamped_to_buffer", playback_target_clamped_to_buffer},
            {"previous_committed_frame", playback_commit_previous_frame},
            {"committed_frame", playback_commit_frame},
            {"committed_slot", playback_commit_slot},
            {"presented_from_slot", presented_from_slot},
            {"presented_frame", playback_trace_presented_frame},
            {"presented_slot", playback_trace_presented_slot},
            {"release_attempts", playback_release_attempts},
            {"release_count", playback_release_count},
            {"release_skip_count", playback_release_skip_count},
            {"release_deferred", playback_release_deferred},
            {"was_just_seeked", playback_was_just_seeked},
        };
        writePlaybackTraceEvent(
            "playback_present_commit", commit_details,
            playback_trace_presenter_view_idx, playback_presenter_target_frame,
            playback_trace_presenter_preferred_paused_slot,
            playback_trace_presented_slot, playback_trace_presented_frame,
            playback_trace_presenter_resolved_frame,
            playback_trace_prewarm_active);
        if (clipped_collection_playback) {
          writeClippedPlaybackStateEvent("playback_present_commit",
                                         commit_details, false);
        }
      }
    }

    if (zarr_loaded) {
      zarr_loader.requestRefinedSubjectMaskOptionalOverlayPrefetch();
    }

    if (cli_frame_cap_fps > 0.0) {
      const auto target_period =
          std::chrono::duration<double>(1.0 / cli_frame_cap_fps);
      const auto target_end =
          frame_loop_start +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              target_period);
      const auto before_sleep = std::chrono::steady_clock::now();
      if (before_sleep < target_end) {
        std::this_thread::sleep_until(target_end);
        frame_cap_sleep_ms =
            durationMs(std::chrono::steady_clock::now() - before_sleep);
      }
    }

    const bool perf_playback_start_warmup_active =
        ps.play_video && perf_playback_start_frame >= 0 &&
        perf_frames_since_playback_start < kPlaybackWarmupPerfFrames;
    const int perf_frames_since_playback_start_value =
        perf_playback_start_frame >= 0
            ? static_cast<int>(std::min<uint64_t>(
                  perf_frames_since_playback_start,
                  static_cast<uint64_t>(std::numeric_limits<int>::max())))
            : -1;
    const PerfLogFrameContext perf_frame_context{
        camera_names,
        cwd,
        argv0_path,
        cli_recording_path,
        cli_zarr_override_path,
        ps.play_video,
        set_playback_speed,
        inst_speed,
        video_fps,
        perf_requested_camera_frame,
        ps.to_display_frame_number,
        current_frame_num,
        perf_min_decoded_camera_frame,
        perf_playback_start_warmup_active,
        perf_frames_since_playback_start_value,
        perf_playback_start_frame,
        perf_playback_resume_path,
        perf_playback_resume_target_frame,
        scene->use_cpu_buffer,
        static_cast<int>(scene->size_of_buffer),
        label_buffer_size,
        video_loaded,
        playbackPreviewScaleLabel(playback_preview_scale_mode),
        playbackPreviewIsActive(ps.play_video, yolo_detection,
                                playback_preview_scale_mode),
        playbackRendererModeLabel(playback_renderer_mode),
        static_cast<int>(perf_camera_viewport_width_px),
        static_cast<int>(perf_camera_viewport_height_px),
        perf_camera_view_x_min,
        perf_camera_view_x_max,
        perf_camera_view_y_min,
        perf_camera_view_y_max,
        perf_camera_view_visible_fraction,
        perf_camera_view_zoomed_in,
        frame_camera_upload_count,
        frame_camera_upload_ms,
        frame_camera_texture_resize_ms,
        frame_camera_preview_resize_ms,
        frame_camera_display_convert_ms,
        frame_camera_pbo_copy_ms,
        frame_camera_texture_upload_ms,
        frame_camera_playback_front_path_ms,
        frame_camera_playback_stage_total_ms,
        frame_camera_playback_stage_upload_ms,
        frame_camera_playback_prewarm_total_ms,
        frame_camera_playback_prewarm_upload_ms,
        frame_camera_playback_prewarm_count,
        frame_camera_playback_swap_ms,
        frame_camera_plot_image_ui_ms,
        frame_camera_overlay_ui_ms,
        frame_subject_shape_overlay_ms,
        frame_tail_kinematics_overlay_ms,
        frame_camera_scene_ui_ms,
        frame_file_browser_ui_ms,
        frame_frame_debug_ui_ms,
        frame_buffer_window_ui_ms,
        frame_crop_preview_ui_ms,
        frame_crop_preview_perf,
        frame_stimulus_buffer_window_ui_ms,
        frame_keypoints_window_ui_ms,
        frame_labeling_tool_ui_ms,
        frame_stimulus_window_ui_ms,
        frame_stimulus_timeline_ui_ms,
        frame_movement_timeline_ui_ms,
        frame_help_menu_ui_ms,
        frame_gl_draw_ms,
        frame_swap_ms,
        frame_cap_sleep_ms,
        cli_frame_cap_fps,
        frame_ui_build_ms,
        frame_imgui_render_ms,
        frame_imgui_draw_cmd_count,
        frame_imgui_draw_list_count,
        frame_imgui_total_vtx_count,
        frame_imgui_total_idx_count,
        &stimulus_player,
        stimulus_use_software_decode,
        stimulus_use_cpu_buffer,
        stimulus_buffer_size,
        ps.current_stimulus_frame,
        latest_decoded_frame[stimulus_player.window_name].load(),
        static_cast<int>(window->swap_interval),
        static_cast<int>(window->width),
        static_cast<int>(window->height),
        frame_loop_start,
        &frame_analysis_timeline_perf,
        frame_bbox_query_frame,
        frame_bbox_loaded_count,
        frame_bbox_display_count,
        frame_bbox_get_boxes_ms,
        frame_bbox_edit_resolve_ms,
        frame_bbox_get_raw_detections_ms,
        frame_bbox_load_total_ms,
        frame_bbox_overlay_build_ms,
        frame_bbox_overlay_draw_ms,
        frame_bbox_overlay_item_count,
    };
    maybeWritePerfLogSample(perf_log_writer, perf_frame_context,
                            kPerfLogSamplePeriod);
    if (diagnostics_session.playbackTraceEnabled() && video_loaded &&
        (!ps.play_video || seek_progress.state != SeekState::Idle)) {
      writePlaybackTraceEvent(
          "frame",
          json{{"frame_loop_ms", durationMs(std::chrono::steady_clock::now() -
                                            frame_loop_start)},
               {"camera_upload_count", frame_camera_upload_count},
               {"camera_upload_ms", frame_camera_upload_ms},
               {"prewarm_count", frame_camera_playback_prewarm_count},
               {"prewarm_ms", frame_camera_playback_prewarm_total_ms},
               {"frame_cap_sleep_ms", frame_cap_sleep_ms}},
          playback_trace_presenter_view_idx,
          playback_trace_presenter_target_frame,
          playback_trace_presenter_preferred_paused_slot,
          playback_trace_presented_slot, playback_trace_presented_frame,
          playback_trace_presenter_resolved_frame,
          playback_trace_prewarm_active);
    }

    int32_t selected_mask_roi_index = -1;
    std::string selected_mask_component_name;
    if (frame_debug_window_state.subject_mask_edit_session.active()) {
      const auto &target =
          frame_debug_window_state.subject_mask_edit_session.target();
      selected_mask_roi_index = target.roi_index;
      selected_mask_component_name = target.component_name;
    }
    const bool periodic_mask_perf_sample =
        (mask_perf_sample_index++ %
         static_cast<uint64_t>(cli_mask_perf_sample_every)) == 0;
    const bool playback_warmup_mask_perf_sample =
        perf_playback_start_warmup_active &&
        ((perf_frames_since_playback_start % kPlaybackWarmupPerfSampleStride) ==
         0);
    const bool playback_prewarm_mask_perf_sample =
        frame_camera_playback_prewarm_count > 0;
    const bool should_write_mask_perf_sample =
        periodic_mask_perf_sample || playback_warmup_mask_perf_sample ||
        playback_prewarm_mask_perf_sample;
    if (should_write_mask_perf_sample) {
      writeMaskPerfLogSample(
          mask_perf_log_writer,
          MaskPerfLogFrameContext{
              cwd,
              argv0_path,
              cli_recording_path,
              cli_zarr_override_path,
              zarr_loaded ? zarr_loader.getArchivePath() : std::string{},
              current_frame_num,
              ps.to_display_frame_number,
              ps.play_video,
              zarr_loaded && show_eye_masks && zarr_loader.hasEyeMasks(),
              zarr_loaded,
              cli_mask_perf_sample_every,
              playback_warmup_mask_perf_sample,
              zarr_loaded ? zarr_loader.getEyeMaskSourceLabel() : std::string{},
              zarr_loaded ? zarr_loader.getEyeMaskSourcePath() : std::string{},
              zarr_loaded ? zarr_loader.getEyeMaskRunName() : std::string{},
              selected_mask_roi_index,
              selected_mask_component_name,
              frame_mask_data_load_ms,
              frame_mask_overlay_perf,
              &perf_frame_context,
              frame_loop_start,
          });
    }
    if (ps.play_video && perf_playback_start_frame >= 0) {
      perf_frames_since_playback_start++;
    } else if (!ps.play_video && perf_playback_start_frame >= 0) {
      resetPlaybackStartPerf();
    }
  }

  diagnostics_session.close();

  // Cleanup
  session_lifecycle.beginClose();
  playback_transport.seekCoordinator().cancelActive();
  refined_keypoint_write_session.close();
  quality_timeline_session.close();
  canonical_timeline_session.close();
  canonical_overlay_session.shutdown();
  canonical_detection_repository.close();
  zarr_loader.setDataAccessScheduler(nullptr);
  analysis_data_scheduler->waitUntilIdle();
  const auto final_analysis_data_scheduler_metrics =
      analysis_data_scheduler->metrics();
  analysis_data_scheduler->shutdown();
  destroyStimulusPlayback(stimulus_player);
  clearCameraViewReadOnlyMaskTextureCache();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window->render_target);
  glfwTerminate();

  dc_context->stop_flag = true;
  // wait for threads to join
  for (auto &t : decoder_threads)
    t.join();

  session_lifecycle.completeClose();
  crimson::diagnostics::writeRuntimeDiagnostics(
      std::cout, "Nvidia",
      {session_lifecycle.snapshot(), session_loading_progress.snapshot(),
       camera_presentation_tracker.metrics()});
  crimson::data::writeDataAccessSchedulerDiagnostics(
      std::cout, "Nvidia", final_analysis_data_scheduler_metrics);
  crimson::playback::writePlaybackSeekDiagnostics(
      std::cout, "Nvidia", playback_transport.seekCoordinator().metrics());

  return app_exit_code;
}
