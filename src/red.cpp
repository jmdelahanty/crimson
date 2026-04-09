#include "IconsForkAwesome.h"
#include "Logger.h"
#include "camera.h"
#include "filesystem"
#include "global.h"
#include "gui.h"
#include "legacy_labeling_state.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "render.h"
#include "skeleton.h"
#include "utils.h"
#include "debug_flags.h"
#include "yolo_detection.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <cmath>
#include <chrono>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <numeric>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <ctime>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include "perf_logging.h"
#include "media_session_loader.h"
#include "playback_session_controller.h"
#include "decode_debug_workflow.h"
#include "manual_detect_payload_preview.h"
#include "chained_crop_image_provider.h"
#include "live_crop_image_provider.h"
#include "refined_keypoint_repository.h"
#include "review_frame_index.h"
#include "zarr_persisted_crop_provider.h"
#include "zarr_loader.h"
#include "gui/file_browser_window.h"
#include "gui/crop_preview_window.h"
#include "gui/diagnostics_window.h"
#include "gui/frame_debug_window.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "gui/keypoints_window.h"
#include "gui/labeling_tool_window.h"
#include "gui/camera_view_overlay_renderer.h"
#include "gui/refined_keypoint_review_window.h"
#include "gui/refined_keypoint_write_workflow.h"
#include "gui/labeling_tool_workflow.h"
#include "gui/auxiliary_windows.h"
#include "gui/camera_view_manual_keypoint_input.h"
#include "gui/camera_view_presenter.h"
#include "gui/camera_view_window.h"
#include "gui/stimulus_playback_windows.h"
#include "gui/camera_view_transport_controls.h"
#include "gui_interpolation.h"
#include "gui/movement_timeline_window.h"
#include "gui/stimulus_event_timeline_window.h"
#include <opencv2/imgproc.hpp>

#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

simplelogger::Logger *logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

#include "ui_path_config.h"
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
std::unordered_map<std::string, std::shared_ptr<DecoderPerfSample>> decoder_perf_samples;
std::mutex g_seek_info_mutex;
std::mutex g_decoder_perf_mutex;

// Global variables
bool show_interpolation_debug = false;
std::vector<ZarrDetectionLoader::DetectionDataset> detection_dataset_ids;
std::vector<std::string> detection_dataset_labels;
int detection_dataset_choice = 0;
ZarrBBoxEditState g_zarr_bbox_edit_state;

#include "review_frame_state.h"

void refreshDetectionDatasetOptions(ZarrDetectionLoader& loader) {
    detection_dataset_ids.clear();
    detection_dataset_labels.clear();
    detection_dataset_choice = 0;
    auto options = loader.getAvailableDetectionDatasets();
    auto active = loader.getActiveDetectionDataset();
    for (size_t i = 0; i < options.size(); ++i) {
        detection_dataset_ids.push_back(options[i].first);
        detection_dataset_labels.push_back(options[i].second);
        if (options[i].first == active) {
            detection_dataset_choice = static_cast<int>(i);
        }
    }
}

#include "stimulus_playback.h"

static StimulusPlayback stimulus_player;

namespace {

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}



}  // namespace

int main(int argc, char **argv) {
    std::string cli_zarr_override_path;
    std::string cli_recording_path;
    std::filesystem::path cli_perf_log_path;
    const std::filesystem::path argv0_path = (argc > 0) ? argv[0] : "";
    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--zarr") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --zarr" << std::endl;
                return 1;
            }
            cli_zarr_override_path = argv[++i];
            continue;
        }
        if (arg == "--recording") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --recording" << std::endl;
                return 1;
            }
            cli_recording_path = argv[++i];
            continue;
        }
        if (arg == "--perf-log") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --perf-log" << std::endl;
                return 1;
            }
            cli_perf_log_path = argv[++i];
            continue;
        }
        std::cerr << "Ignoring unknown argument: " << arg << std::endl;
    }

    // Mutual exclusion: --recording takes precedence over --zarr
    if (!cli_recording_path.empty() && !cli_zarr_override_path.empty()) {
        std::cerr << "Warning: both --recording and --zarr specified; "
                  << "using --recording, ignoring --zarr" << std::endl;
        cli_zarr_override_path.clear();
    }

    // Validate --recording path early
    if (!cli_recording_path.empty() && !IsDirectoryNoThrow(cli_recording_path)) {
        std::cerr << "Error: --recording path is not a directory: "
                  << cli_recording_path << std::endl;
        cli_recording_path.clear();
    }

    gx_context *window = new gx_context();
    *window = gx_context{};
    window->swap_interval = 1;  // use vsync
    window->width = 1920;
    window->height = 1080;
    window->render_target_title = (char *)malloc(100);  // window title
    window->glsl_version = (char *)malloc(100);

    constexpr int kCudaDeviceIndex = 0;
    render_initialize_target(window, kCudaDeviceIndex, argv0_path);

    render_scene *scene = new render_scene();

    std::string root_dir;
    std::string skeleton_dir;
    std::vector<std::string> camera_names;
    std::vector<CameraParams> camera_params;
    std::vector<std::thread> decoder_threads;
    std::vector<std::unique_ptr<FFmpegDemuxer>> demuxers;

    // Zarr loading
    ZarrDetectionLoader zarr_loader;
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
    bool video_loaded = false;
    bool cpu_buffer_toggle = true;
    bool show_keypoint_markers = true;
    bool show_heading_arrows = true;
    bool show_eye_masks = false;
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
                std::cout << "[HEADING_DEBUG] Log limit reached, suppressing further messages"
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
                std::cout << "[EYE_MASK_DEBUG] Log limit reached, suppressing further messages"
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
    SeekProgress seek_progress;
    std::string frame_sync_debug_line;
    int frame_sync_valid_slots = -1;
    int frame_sync_empty_slots = -1;
    int frame_sync_latest_decoded = -1;
    int frame_sync_recording_remaining = -1;
    int frame_sync_recording_total = -1;
    PerfLogWriter perf_log_writer;
    constexpr auto kPerfLogSamplePeriod = std::chrono::milliseconds(250);
    if (!cli_perf_log_path.empty()) {
        (void)perf_log_writer.open(cli_perf_log_path);
    }

    window_need_decoding[stimulus_player.window_name].store(false);
    latest_decoded_frame[stimulus_player.window_name].store(-1);
    window_was_decoding[stimulus_player.window_name] = false;
    MediaSessionLoader media_session_loader(
        MediaSessionLoaderContext{
            scene,
            dc_context,
            &zarr_loader,
            &stimulus_player,
            &ps,
            &root_dir,
            &skeleton_dir,
            &camera_names,
            &camera_params,
            &decoder_threads,
            &demuxers,
            &is_view_focused,
            &window_need_decoding,
            &window_was_decoding,
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
            kCudaDeviceIndex,
        });

    media_session_loader.bootstrapFromCli(
        cli_zarr_override_path,
        cli_recording_path,
        [&]() { refreshDetectionDatasetOptions(zarr_loader); },
        [&]() { g_zarr_bbox_edit_state.clearAll(); });

    ReviewFrameFilters review_frame_filters;
    ReviewFrameCache review_frame_cache;
    std::string review_frame_status;
    std::string decode_debug_status;
    std::string bbox_payload_status;
    std::mt19937 debug_rng(
        static_cast<uint32_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    std::optional<ManualDetectPayloadPreview> manual_payload_preview;
    CropPreviewWindowState crop_preview_window_state;
    LabelingToolWindowState labeling_tool_window_state;
    FrameDebugWindowState frame_debug_window_state;
    PlaybackSessionController playback_session_controller(
        PlaybackSessionControllerContext{
            scene,
            dc_context,
            &zarr_loader,
            &stimulus_player,
            &ps,
            &seek_progress,
            &current_frame_num,
            &video_fps,
            &camera_names,
            &window_was_decoding,
            &window_need_decoding,
        });

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
        [&](std::string& reload_error,
            std::optional<ZarrDetectionLoader::DetectionDataset>
                preferred_dataset = std::nullopt) -> bool {
            const auto previous_dataset = zarr_loader.getActiveDetectionDataset();
            const std::string archive_path = zarr_loader.getArchivePath();
            if (archive_path.empty()) {
                reload_error = "No loaded Zarr archive.";
                return false;
            }

            if (!zarr_loader.loadZarrFile(archive_path, reload_error)) {
                zarr_loaded = false;
                return false;
            }

            zarr_loaded = true;
            const auto dataset_to_restore =
                preferred_dataset.value_or(previous_dataset);
            if (zarr_loader.isDatasetAvailable(dataset_to_restore)) {
                (void)zarr_loader.setActiveDetectionDataset(dataset_to_restore);
            }
            refreshDetectionDatasetOptions(zarr_loader);
            invalidateReviewFrameCache(review_frame_cache);
            review_frame_status.clear();
            if (zarr_loader.getTotalFrames() > 0 &&
                current_frame_num >=
                    static_cast<int>(zarr_loader.getTotalFrames())) {
                current_frame_num =
                    static_cast<int>(zarr_loader.getTotalFrames()) - 1;
            }
            return true;
        };

    while (!glfwWindowShouldClose(window->render_target)) {
        static FileBrowserWindowState file_browser_window_state;
        const auto frame_loop_start = std::chrono::steady_clock::now();
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
        double frame_camera_playback_swap_ms = 0.0;
        double frame_camera_plot_image_ui_ms = 0.0;
        double frame_camera_overlay_ui_ms = 0.0;
        double frame_camera_scene_ui_ms = 0.0;
        double frame_file_browser_ui_ms = 0.0;
        double frame_frame_debug_ui_ms = 0.0;
        double frame_buffer_window_ui_ms = 0.0;
        double frame_crop_preview_ui_ms = 0.0;
        double frame_stimulus_buffer_window_ui_ms = 0.0;
        double frame_keypoints_window_ui_ms = 0.0;
        double frame_labeling_tool_ui_ms = 0.0;
        double frame_stimulus_window_ui_ms = 0.0;
        double frame_stimulus_timeline_ui_ms = 0.0;
        double frame_movement_timeline_ui_ms = 0.0;
        double frame_help_menu_ui_ms = 0.0;
        double frame_gl_draw_ms = 0.0;
        double frame_swap_ms = 0.0;
        double frame_ui_build_ms = 0.0;
        double frame_imgui_render_ms = 0.0;
        int frame_imgui_draw_cmd_count = 0;
        int frame_imgui_draw_list_count = 0;
        int frame_imgui_total_vtx_count = 0;
        int frame_imgui_total_idx_count = 0;
        double perf_camera_viewport_width_px =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_viewport_height_px =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_x_min =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_x_max =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_y_min =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_y_max =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_visible_fraction =
            std::numeric_limits<double>::quiet_NaN();
        int perf_camera_view_zoomed_in = -1;
        int perf_requested_camera_frame = -1;
        int perf_min_decoded_camera_frame = -1;

        // Poll and handle events (inputs, window resize, etc.)
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const auto ui_build_start = std::chrono::steady_clock::now();

        playback_session_controller.pollSeekState();

        // --- Update playback time ---
        auto now = std::chrono::steady_clock::now();

        if (ps.play_video) {
            ps.accumulated_play_time +=
                std::chrono::duration<double>(now - ps.last_play_time_start)
                    .count() *
                set_playback_speed;
            ps.last_play_time_start = now;
        }
        double playback_time_now = ps.accumulated_play_time;

        const auto file_browser_ui_start = std::chrono::steady_clock::now();
        if (video_loaded && !legacy_labeling_state.skeleton_chosen) {
            legacy_labeling_state.ensureSkeletonResources();
        }
        const std::string active_skeleton_name =
            legacy_labeling_state.activeSkeletonName();
        const bool has_active_zarr_keypoint_review =
            zarr_loaded && zarr_loader.hasKeypointData();
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
        };
        FileBrowserWindowResult file_browser_result =
            drawFileBrowserWindow(file_browser_context,
                                  file_browser_window_state);
        if (file_browser_result.skeleton_selection.has_value()) {
            const auto& selection = *file_browser_result.skeleton_selection;
            bool load_calibration = true;
            if (scene->num_cams > 1) {
                for (u32 i = 0; i < scene->num_cams; i++) {
                    std::string cam_file = root_dir + "/calibration/" +
                                           camera_names[i] + ".yaml";

                    if (!std::filesystem::exists(cam_file)) {
                        load_calibration = false;
                        error_message = "Calibration file not found: " + cam_file;
                        show_error = true;
                        break;
                    }
                    if (!camera_load_params_from_yaml(cam_file,
                                                      camera_params[i],
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
                skeleton_initialize(selection.name,
                                    root_dir,
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
                yolo_threads.push_back(std::thread(&yolo_process,
                                                   yolov5_onnx,
                                                   &yolo_setting,
                                                   i));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::YOLOv8: {
            std::string engine_file_path =
                root_dir + "/yolo/yolorat_bbox/rat_bbox.engine";
            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process_trt,
                                                   engine_file_path,
                                                   i,
                                                   scene->cameras[i].image_width,
                                                   scene->cameras[i].image_height));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::YOLOv8Pose: {
            std::string engine_file_path =
                root_dir + "/yolo/yolopose/rat_pose.engine";
            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process_v8pose,
                                                   engine_file_path,
                                                   i,
                                                   scene->cameras[i].image_width,
                                                   scene->cameras[i].image_height));
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
            legacy_labeling_state.toolsEnabled(
                has_active_zarr_keypoint_review);

        if (video_loaded) {
            const auto frame_debug_ui_start = std::chrono::steady_clock::now();
            if (!use_legacy_manual_keypoint_tools) {
                legacy_labeling_state.keypoints_find = false;
            }
            std::vector<LoggedBoundingBox> zarr_boxes;
            bool frame_is_interpolated = false;
            const bool frame_has_bbox_edits =
                g_zarr_bbox_edit_state.isFrameDirty(current_frame_num);
            bool dataset_has_synthetic_boxes = false;
            bool dataset_allows_bbox_edit = false;
            ZarrDetectionLoader::FrameDetections detection_details;
            const ZarrDetectionLoader::FrameDetections* detection_details_ptr =
                nullptr;
            if (zarr_loaded) {
                dataset_has_synthetic_boxes =
                    zarr_loader.activeDatasetHasSyntheticDetections();
                if (zarr_loader.hasInterpolation()) {
                    frame_is_interpolated =
                        zarr_loader.isFrameInterpolated(current_frame_num);
                }
                std::vector<LoggedBoundingBox> loaded_zarr_boxes =
                    zarr_loader.getBoundingBoxesForFrame(current_frame_num);
                zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(
                    current_frame_num, loaded_zarr_boxes);
                const bool active_dataset_is_raw_detect =
                    zarr_loader.hasDetectionData() &&
                    (zarr_loader.getActiveDetectionDataset() ==
                     ZarrDetectionLoader::DetectionDataset::RawDetect);
                dataset_allows_bbox_edit =
                    zarr_loader.hasDetectionData() && !active_dataset_is_raw_detect;
                if (!dataset_allows_bbox_edit) {
                    g_zarr_bbox_edit_state.draw_mode = false;
                    g_zarr_bbox_edit_state.cancelDraw();
                    g_zarr_bbox_edit_state.clearSelection();
                }
                const bool need_details =
                    zarr_loader.hasScores() ||
                    zarr_loader.hasHeadingData() ||
                    zarr_loader.hasKeypointData() ||
                    zarr_loader.hasEyeMasks() || dataset_has_synthetic_boxes;
                if (need_details) {
                    detection_details =
                        zarr_loader.getRawDetections(current_frame_num,
                                                     false,
                                                     zarr_loader.hasEyeMasks());
                    detection_details_ptr = &detection_details;
                }
            }

            const FrameDebugWindowContext frame_debug_context{
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
            };
            const FrameDebugWindowResult frame_debug_result =
                drawFrameDebugWindow(frame_debug_context, frame_debug_window_state);
            const DiagnosticsWindowResult diagnostics_result =
                drawDiagnosticsWindow(frame_debug_context);

            show_keypoint_markers = frame_debug_result.show_keypoint_markers;
            show_heading_arrows = frame_debug_result.show_heading_arrows;
            show_eye_masks = frame_debug_result.show_eye_masks;

            if (frame_debug_result.requested_detection_dataset_index >= 0 &&
                frame_debug_result.requested_detection_dataset_index <
                    static_cast<int>(detection_dataset_ids.size()) &&
                zarr_loader.setActiveDetectionDataset(
                    detection_dataset_ids[frame_debug_result
                                              .requested_detection_dataset_index])) {
                detection_dataset_choice =
                    frame_debug_result.requested_detection_dataset_index;
                refreshDetectionDatasetOptions(zarr_loader);
                g_zarr_bbox_edit_state.clearAll();
                invalidateReviewFrameCache(review_frame_cache);
                review_frame_status.clear();
                if (zarr_loader.getTotalFrames() > 0 &&
                    current_frame_num >=
                        static_cast<int>(zarr_loader.getTotalFrames())) {
                    current_frame_num =
                        static_cast<int>(zarr_loader.getTotalFrames()) - 1;
                }
            }

            if (frame_debug_result.review_filters_changed) {
                review_frame_filters =
                    frame_debug_result.review_frame_filters;
                invalidateReviewFrameCache(review_frame_cache);
                review_frame_status.clear();
            }
            if (frame_debug_result.request_prev_review_frame) {
                auto jump_result = computeReviewFrameJump(
                    zarr_loaded, zarr_loader, review_frame_filters,
                    review_frame_cache, current_frame_num, false);
                review_frame_status = std::move(jump_result.status);
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_next_review_frame) {
                auto jump_result = computeReviewFrameJump(
                    zarr_loaded, zarr_loader, review_frame_filters,
                    review_frame_cache, current_frame_num, true);
                review_frame_status = std::move(jump_result.status);
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (diagnostics_result.request_dump_decode_buffers) {
                dumpDecodeBuffersToVideos(makeDecodeDebugDumpContext(),
                                         "manual_dump", decode_debug_status);
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
                            playback_session_controller.setCameraDecodeRequests(
                                enabled);
                        },
                    },
                    decode_debug_status);
            }
            if (frame_debug_result.request_reset_frame_bbox_edits) {
                g_zarr_bbox_edit_state.clearFrameEdits(current_frame_num);
            }
            if (frame_debug_result.request_clear_bbox_selection) {
                g_zarr_bbox_edit_state.clearSelection();
            }
            if (frame_debug_result.request_build_manual_payload_preview) {
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
                        "Manual payload preview failed: " +
                        manual_payload_preview->error;
                } else {
                    bbox_payload_status = summarizeManualDetectPayloadPreview(
                        *manual_payload_preview,
                        g_zarr_bbox_edit_state.dirtyFrameCount());
                }
            }
            if (frame_debug_result.request_write_manual_payload) {
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
                    if (zarr_loader.getActiveDetectionDataset() ==
                        ZarrDetectionLoader::DetectionDataset::RefinedFiltered) {
                        source_variant = "filtered";
                    }

                    std::string write_error;
                    std::string resolved_refined_run;
                    ManualWriteReviewOptions review_opts;
                    const auto review_metadata = resolveReviewMetadataValues(
                        frame_debug_window_state.manual_write_review);
                    review_opts.intended_use =
                        review_metadata.intended_use;
                    review_opts.state = review_metadata.review_state;
                    review_opts.method = review_metadata.method;
                    review_opts.reviewer = review_metadata.reviewer;
                    review_opts.notes = review_metadata.notes;
                    const bool write_ok =
                        zarr_loader.writeManualRefinedDetections(
                            manual_payload_preview->frame_indices,
                            manual_payload_preview->bbox_norm_coords,
                            manual_payload_preview->scores,
                            manual_payload_preview->class_ids,
                            manual_payload_preview->frame_counts,
                            manual_payload_preview->detection_source,
                            manual_payload_preview->reason,
                            "manual",
                            source_variant,
                            write_error,
                            &resolved_refined_run,
                            review_opts);
                    if (!write_ok) {
                        bbox_payload_status =
                            "Manual write failed: " + write_error;
                    } else {
                        const size_t written_detections =
                            manual_payload_preview->total_detections;
                        std::string reload_error;
                        const std::string archive_path =
                            zarr_loader.getArchivePath();
                        if (!archive_path.empty() &&
                            zarr_loader.loadZarrFile(archive_path,
                                                     reload_error)) {
                            zarr_loaded = true;
                            if (zarr_loader.isDatasetAvailable(
                                    ZarrDetectionLoader::DetectionDataset::
                                        RefinedManual)) {
                                (void)zarr_loader.setActiveDetectionDataset(
                                    ZarrDetectionLoader::DetectionDataset::
                                        RefinedManual);
                            }
                            refreshDetectionDatasetOptions(zarr_loader);
                            g_zarr_bbox_edit_state.clearAll();
                            manual_payload_preview.reset();
                            invalidateReviewFrameCache(review_frame_cache);
                            review_frame_status.clear();
                            if (zarr_loader.getTotalFrames() > 0 &&
                                current_frame_num >= static_cast<int>(
                                                         zarr_loader
                                                             .getTotalFrames())) {
                                current_frame_num =
                                    static_cast<int>(
                                        zarr_loader.getTotalFrames()) -
                                    1;
                            }
                            std::ostringstream payload_msg;
                            payload_msg
                                << "Manual write complete: run="
                                << (resolved_refined_run.empty() ? "<latest>"
                                                                : resolved_refined_run)
                                << " group=manual"
                                << " detections=" << written_detections;
                            bbox_payload_status = payload_msg.str();
                        } else {
                            zarr_loaded = false;
                            g_zarr_bbox_edit_state.clearAll();
                            bbox_payload_status =
                                "Manual write succeeded but reload failed: " +
                                reload_error;
                        }
                    }
                }
            }
            if (frame_debug_result.request_keypoint_review_write) {
                RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
                const RefinedKeypointReviewWriteWorkflowResult
                    review_write_result = applyRefinedKeypointReviewWrite(
                        refined_keypoint_repo,
                        RefinedKeypointReviewPanelResult{
                            frame_debug_result.selected_keypoint_selection,
                            frame_debug_result.request_keypoint_review_write,
                            frame_debug_result.keypoint_review_options,
                        },
                        frame_debug_window_state.keypoint_review_panel
                            .review_write_status,
                        reloadActiveZarrPreserveDataset);
                if (review_write_result.should_clear_zarr_loaded) {
                    zarr_loaded = false;
                }
            }
            frame_frame_debug_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - frame_debug_ui_start);
        }

        // file explorer display
        if (ImGuiFileDialog::Instance()->Display("ChooseMedia")) {
            if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
                auto selected_files =
                    ImGuiFileDialog::Instance()->GetSelection();
                root_dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                skeleton_dir = root_dir;

                // Reset any previously loaded stimulus video
                destroyStimulusPlayback(stimulus_player);
                window_need_decoding[stimulus_player.window_name].store(false);
                window_was_decoding[stimulus_player.window_name] = false;

                // Try to load Zarr detection file
                std::string zarr_error;
                if (loadZarrDetectionFromDirectory(root_dir, zarr_loader, zarr_error)) {
                    zarr_loaded = true;
                    refreshDetectionDatasetOptions(zarr_loader);
                    g_zarr_bbox_edit_state.clearAll();
                } else {
                    zarr_loaded = false;
                    g_zarr_bbox_edit_state.clearAll();
                    std::cout << "No Zarr detection file found (optional): " << zarr_error << std::endl;
                    detection_dataset_ids.clear();
                    detection_dataset_labels.clear();
                    detection_dataset_choice = 0;
                }

                // check if it is mp4, if it is mp4 files
                auto first_selection =
                    *selected_files.begin(); // Dereferencing iterator
                if (string_ends_with(first_selection.first, ".mp4")) {
                    for (const auto &elem : selected_files) {
                        std::string cam_string_full = elem.first;
                        std::size_t last_slash = cam_string_full.find_last_of("/\\");
                        if (last_slash != std::string::npos) {
                            cam_string_full = cam_string_full.substr(last_slash + 1);
                        }
                        std::size_t cam_string_mp4_position =
                            cam_string_full.find(".mp4");
                        std::string cam_string =
                            cam_string_full.substr(0, cam_string_mp4_position);
                        camera_names.push_back(cam_string);
                        std::cout << "camera names: " << cam_string
                                  << std::endl;
                        window_need_decoding[cam_string].store(true);
                        window_was_decoding[cam_string] = true;
                        std::map<std::string, std::string> m;
                        demuxers.push_back(
                            std::make_unique<FFmpegDemuxer>(elem.second.c_str(), m));
                    }
                    std::map<std::string, std::string> m;
                    FFmpegDemuxer dummy_dmuxer(
                        selected_files.begin()->second.c_str(), m);
                    dc_context->seek_interval =
                        (int)dummy_dmuxer
                            .FindKeyFrameInterval(); // get the seek interval
                    video_fps = dummy_dmuxer.GetFramerate();
                    scene->num_cams = selected_files.size();
                    scene->cameras.resize(scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        scene->cameras[j].image_width = demuxers[j]->GetWidth();
                        scene->cameras[j].image_height = demuxers[j]->GetHeight();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    // multiple threads for decoding for selected videos
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &decoder_process, dc_context, demuxers[i].get(),
                            camera_names[i], scene->cameras[i].display_buffer,
                            scene->size_of_buffer, &scene->cameras[i].seek_context,
                            scene->use_cpu_buffer));
                        is_view_focused.push_back(false);
                    }
                    video_loaded = true;
                } else {
                    input_is_imgs = true;
                    for (const auto &elem : selected_files) {
                        std::size_t cam_string_position = elem.first.find("_");
                        std::string cam_name =
                            elem.first.substr(0, cam_string_position);
                        std::string file_name =
                            elem.first.substr(cam_string_position + 1);

                        if (std::find(camera_names.begin(), camera_names.end(),
                                      cam_name) == camera_names.end()) {
                            camera_names.push_back(cam_name);
                        }

                        if (std::find(imgs_names.begin(), imgs_names.end(),
                                      file_name) == imgs_names.end()) {
                            imgs_names.push_back(file_name);
                        }
                    }

                    dc_context->seek_interval = 1;
                    scene->num_cams = camera_names.size();
                    scene->cameras.resize(scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        std::string file_name = root_dir + "/" +
                                                camera_names[j] + "_" +
                                                imgs_names[0];
                        cv::Mat image = cv::imread(file_name, cv::IMREAD_COLOR);
                        scene->cameras[j].image_width = image.cols;
                        scene->cameras[j].image_height = image.rows;
                    }
                    if (imgs_names.size() < label_buffer_size) {
                        label_buffer_size = imgs_names.size();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &image_loader, dc_context, imgs_names,
                            scene->cameras[i].display_buffer, scene->size_of_buffer,
                            &scene->cameras[i].seek_context, scene->use_cpu_buffer,
                            camera_names[i], root_dir));
                        is_view_focused.push_back(false);
                    }
                    video_loaded = true;
                }

                media_session_loader.loadCameraCalibrationsForCurrentMedia();
                if (video_loaded && !input_is_imgs) {
                    int initial_frame = std::max(0, ps.to_display_frame_number);
                    double seek_fps = (video_fps > 0.0) ? video_fps : 30.0;
                    seek_all_cameras(scene, initial_frame, seek_fps, ps, true,
                                     &zarr_loader, &stimulus_player);
                }
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

                std::string zarr_error;
                if (loadZarrDetectionFromPath(selected_zarr_path, zarr_loader, zarr_error)) {
                    zarr_loaded = true;
                    refreshDetectionDatasetOptions(zarr_loader);
                    g_zarr_bbox_edit_state.clearAll();
                    invalidateReviewFrameCache(review_frame_cache);
                    review_frame_status.clear();
                    std::cout << "Loaded Zarr archive override: "
                              << zarr_loader.getArchivePath() << std::endl;
                    media_session_loader.tryAutoLoadAffiliatedVideoFromZarr(
                        "Load Zarr Archive");
                    media_session_loader.tryAutoLoadStimulusVideo(
                        "file-dialog");
                } else {
                    zarr_loaded = false;
                    g_zarr_bbox_edit_state.clearAll();
                    invalidateReviewFrameCache(review_frame_cache);
                    review_frame_status.clear();
                    std::cout << "Failed to load Zarr archive override: " << zarr_error << std::endl;
                    detection_dataset_ids.clear();
                    detection_dataset_labels.clear();
                    detection_dataset_choice = 0;
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseStimulus")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                auto selection = ImGuiFileDialog::Instance()->GetSelection();
                if (!selection.empty()) {
                    std::string stimulus_path = selection.begin()->second;
                    int selected_stimulus_buffer_size =
                        std::max(1, stimulus_buffer_size);
                    if (!initializeStimulusPlayback(stimulus_player, stimulus_path,
                                                    selected_stimulus_buffer_size,
                                                    stimulus_use_cpu_buffer,
                                                    stimulus_use_software_decode,
                                                    kCudaDeviceIndex)) {
                        show_error = true;
                        error_message = "Failed to load stimulus video: " + stimulus_path;
                    } else {
                        window_was_decoding[stimulus_player.window_name] = false;
                        window_need_decoding[stimulus_player.window_name].store(false);
                        if (zarr_loaded) {
                            scheduleStimulusSeek(stimulus_player, &zarr_loader,
                                                 ps.to_display_frame_number,
                                                 !ps.play_video);
                        }
                    }
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseSkeleton")) {
            if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
                auto skeleton_file =
                    ImGuiFileDialog::Instance()->GetSelection();

                if (!skeleton_file.empty()) {

                    bool load_calibration = true;
                    if (scene->num_cams > 1) {
                        for (u32 i = 0; i < scene->num_cams; i++) {
                            std::string cam_file = root_dir + "/calibration/" +
                                                   camera_names[i] + ".yaml";
                            if (!camera_load_params_from_yaml(cam_file, camera_params[i], error_message)) {
                                load_calibration = false;
                                camera_params.clear();
                                camera_params.resize(scene->num_cams);
                                show_error = true;
                                break;
                            }
                        }
                    }

                        if (load_calibration) {
                        skeleton_dir =
                            ImGuiFileDialog::Instance()->GetCurrentPath();
                        skeleton_initialize("", skeleton_file.begin()->second,
                                            legacy_labeling_state.skeleton.get(),
                                            SP_LOAD);
                        legacy_labeling_state.activateManualMode(root_dir);
                    }
                }
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        static int select_corr_head = 0;
        if (video_loaded && (!ps.play_video)) {
            int visible_idx = 0;
            if (!ps.pause_seeked) {
                for (int i = 0; i < scene->num_cams; i++) {
                    if (window_was_decoding[camera_names[i]]) {
                        visible_idx = i;
                        break;
                    }
                }
            }

            struct PausedBufferListItem {
                int slot = -1;
                int frame = -1;
            };
            std::vector<PausedBufferListItem> paused_buffer_items;
            paused_buffer_items.reserve(scene->size_of_buffer);
            for (int i = 0; i < scene->size_of_buffer; ++i) {
                const auto& slot = scene->cameras[visible_idx].display_buffer[i];
                if (slot.available_to_write || slot.frame_number < 0) {
                    continue;
                }
                paused_buffer_items.push_back({i, slot.frame_number});
            }
            std::sort(paused_buffer_items.begin(), paused_buffer_items.end(),
                      [](const PausedBufferListItem& a,
                         const PausedBufferListItem& b) {
                          if (a.frame == b.frame) {
                              return a.slot < b.slot;
                          }
                          return a.frame < b.frame;
                      });

            struct PausedBufferSpan {
                int start_index = -1;
                int end_index = -1;
            };
            std::vector<PausedBufferSpan> paused_buffer_spans;
            paused_buffer_spans.reserve(paused_buffer_items.size());
            for (int i = 0; i < static_cast<int>(paused_buffer_items.size()); ++i) {
                if (paused_buffer_spans.empty() ||
                    paused_buffer_items[i].frame !=
                        paused_buffer_items[paused_buffer_spans.back().end_index]
                            .frame + 1) {
                    paused_buffer_spans.push_back({i, i});
                } else {
                    paused_buffer_spans.back().end_index = i;
                }
            }

            auto getPreferredPausedSlot = [&]() -> int {
                int exact_slot = -1;
                for (int i = 0; i < scene->size_of_buffer; ++i) {
                    const auto& slot = scene->cameras[visible_idx].display_buffer[i];
                    if (!slot.available_to_write &&
                        slot.frame_number == ps.to_display_frame_number) {
                        exact_slot = i;
                        break;
                    }
                }
                if (exact_slot >= 0) {
                    return exact_slot;
                }
                return playback_session_controller.findNearestPausedBufferSlot(
                    visible_idx, std::max(0, ps.to_display_frame_number));
            };

            ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
            const auto buffer_window_ui_start = std::chrono::steady_clock::now();
            if (ImGui::Begin("Frames in the buffer")) {
                ImGui::Text("Valid frames: %zu / %u",
                            paused_buffer_items.size(), scene->size_of_buffer);
                if (ps.paused_frame_on_toggle >= 0) {
                    ImGui::Text("Pause origin frame: %d, resume mode: %s",
                                ps.paused_frame_on_toggle,
                                ps.buffer_browsed_since_pause
                                    ? "buffered resume / camera re-anchor"
                                    : "smooth resume from pause frame");
                }
                if (ps.last_resume_path != ResumePath::None) {
                    ImGui::Text("Last resume: %s (target %d)",
                                resumePathName(ps.last_resume_path),
                                ps.last_resume_target_frame);
                }
                if (!paused_buffer_items.empty()) {
                    const int oldest_buffered_frame =
                        paused_buffer_items.front().frame;
                    const int newest_buffered_frame =
                        paused_buffer_items.back().frame;
                    int largest_gap = 0;
                    for (int span_idx = 1;
                         span_idx < static_cast<int>(paused_buffer_spans.size());
                         ++span_idx) {
                        const auto& previous_last_item =
                            paused_buffer_items[paused_buffer_spans[span_idx - 1]
                                                    .end_index];
                        const auto& current_first_item =
                            paused_buffer_items[paused_buffer_spans[span_idx]
                                                    .start_index];
                        largest_gap = std::max(
                            largest_gap,
                            current_first_item.frame - previous_last_item.frame - 1);
                    }
                    ImGui::Text("Selected/displayed frame: %d",
                                ps.to_display_frame_number);
                    ImGui::Text("Buffered spans: %zu, oldest: %d, newest: %d, largest gap: %d",
                                paused_buffer_spans.size(),
                                oldest_buffered_frame,
                                newest_buffered_frame,
                                largest_gap);
                    ImGui::Text("Newest buffered frame: %d",
                                newest_buffered_frame);
                }
                int selected_item = -1;
                int best_distance = std::numeric_limits<int>::max();
                int best_frame = std::numeric_limits<int>::min();
                for (int i = 0; i < static_cast<int>(paused_buffer_items.size()); ++i) {
                    const auto& item = paused_buffer_items[i];
                    if (item.frame == ps.to_display_frame_number) {
                        selected_item = i;
                        best_distance = 0;
                        best_frame = item.frame;
                        break;
                    }
                    const int distance = std::abs(item.frame - ps.to_display_frame_number);
                    if (distance < best_distance ||
                        (distance == best_distance && item.frame > best_frame)) {
                        best_distance = distance;
                        best_frame = item.frame;
                        selected_item = i;
                    }
                }
                if (paused_buffer_items.empty()) {
                    ImGui::TextDisabled("No decoded frames currently buffered.");
                } else {
                    const int newest_buffered_frame =
                        paused_buffer_items.back().frame;
                    for (int span_idx = 0;
                         span_idx < static_cast<int>(paused_buffer_spans.size());
                         ++span_idx) {
                        const auto& span = paused_buffer_spans[span_idx];
                        const auto& first_item =
                            paused_buffer_items[span.start_index];
                        const auto& last_item =
                            paused_buffer_items[span.end_index];

                        if (span_idx > 0) {
                            const auto& previous_last_item =
                                paused_buffer_items[paused_buffer_spans[span_idx - 1]
                                                        .end_index];
                            const int missing_frames =
                                first_item.frame - previous_last_item.frame - 1;
                            if (missing_frames > 0) {
                                const int missing_start =
                                    previous_last_item.frame + 1;
                                const int missing_end =
                                    first_item.frame - 1;
                                ImGui::Separator();
                                ImGui::TextDisabled(
                                    "Gap: %d missing frames (%d..%d)",
                                    missing_frames, missing_start,
                                    missing_end);
                            }
                        }

                        char span_label[192];
                        snprintf(span_label, sizeof(span_label),
                                 "Span %d-%d (%d frames, slots %d-%d, selected %+d..%+d, newest %+d..%+d)",
                                 first_item.frame, last_item.frame,
                                 span.end_index - span.start_index + 1,
                                 first_item.slot, last_item.slot,
                                 first_item.frame - ps.to_display_frame_number,
                                 last_item.frame - ps.to_display_frame_number,
                                 first_item.frame - newest_buffered_frame,
                                 last_item.frame - newest_buffered_frame);
                        ImGui::TextDisabled("%s", span_label);

                        for (int i = span.start_index; i <= span.end_index; ++i) {
                            const auto& item = paused_buffer_items[i];
                            char label[128];
                            const int selected_delta =
                                item.frame - ps.to_display_frame_number;
                            const int newest_delta =
                                item.frame - newest_buffered_frame;
                            snprintf(label, sizeof(label),
                                     "Frame %d (slot %d, selected %+d, newest %+d)",
                                     item.frame, item.slot, selected_delta,
                                     newest_delta);
                            ImGui::PushID(i);
                            if (ImGui::Selectable(label, selected_item == i)) {
                                selected_item = i;
                                ps.to_display_frame_number = item.frame;
                                ps.slider_frame_number = item.frame;
                                ps.pause_seeked = true;
                                ps.buffer_browsed_since_pause =
                                    (ps.paused_frame_on_toggle >= 0 &&
                                     item.frame != ps.paused_frame_on_toggle);
                            }
                            ImGui::PopID();
                        }
                    }
                }

                if (ImGui::IsKeyPressed(ImGuiKey_Comma, true)) {
                    if (selected_item > 0 &&
                        selected_item <= static_cast<int>(paused_buffer_items.size()) - 1) {
                        --selected_item;
                        ps.to_display_frame_number =
                            paused_buffer_items[selected_item].frame;
                        ps.slider_frame_number = ps.to_display_frame_number;
                        ps.pause_seeked = true;
                        ps.buffer_browsed_since_pause =
                            (ps.paused_frame_on_toggle >= 0 &&
                             ps.to_display_frame_number !=
                                 ps.paused_frame_on_toggle);
                    }
                };

                if (ImGui::IsKeyPressed(ImGuiKey_Period, true)) {
                    if (selected_item >= 0 &&
                        selected_item < static_cast<int>(paused_buffer_items.size()) - 1) {
                        ++selected_item;
                        ps.to_display_frame_number =
                            paused_buffer_items[selected_item].frame;
                        ps.slider_frame_number = ps.to_display_frame_number;
                        ps.pause_seeked = true;
                        ps.buffer_browsed_since_pause =
                            (ps.paused_frame_on_toggle >= 0 &&
                             ps.to_display_frame_number !=
                                 ps.paused_frame_on_toggle);
                    }
                };
            }
            ImGui::End();
            frame_buffer_window_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - buffer_window_ui_start);
            select_corr_head = getPreferredPausedSlot();
            if (select_corr_head >= 0) {
                ps.read_head = select_corr_head;
                current_frame_num =
                    scene->cameras[visible_idx].display_buffer[select_corr_head]
                        .frame_number;
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
                    scene->cameras[0].display_buffer[ps.read_head % scene->size_of_buffer].frame_number;
                if (live_frame >= 0) {
                    current_frame_num = live_frame;
                } else {
                    current_frame_num = ps.to_display_frame_number;
                }
            }
            const bool freeze_stimulus_during_paused_browse =
                !ps.play_video && ps.pause_seeked && ps.buffer_browsed_since_pause;
            const bool freeze_stimulus_during_seek =
                seek_progress.state == SeekState::WaitingCameras ||
                seek_progress.state == SeekState::WaitingStimulus;
            if (zarr_loaded && zarr_loader.hasStimulusAlignment()) {
                if (!freeze_stimulus_during_paused_browse &&
                    !freeze_stimulus_during_seek) {
                    int stim_source_frame = ps.play_video ? current_frame_num
                                                          : ps.to_display_frame_number;
                    if (auto stim_frame = zarr_loader.getStimulusFrameForCameraFrame(stim_source_frame)) {
                        ps.current_stimulus_frame = *stim_frame;
                    } else {
                        ps.current_stimulus_frame = -1;
                    }
                }
            } else {
                ps.current_stimulus_frame = -1;
            }
            const int paused_visible_idx =
                ps.play_video ? -1
                              : playback_session_controller
                                    .getVisibleCameraIndex();
            for (int j = 0; j < scene->num_cams; j++) {
                const std::string &win_name = camera_names[j];

                // layout
                ImGui::SetNextWindowSize(ImVec2(500, 400),
                                         ImGuiCond_FirstUseEver);
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
                bool is_visible = ImGui::Begin(win_name.c_str());

                if (!window_was_decoding[win_name] && is_visible &&
                    ps.play_video) {
                    // seek if visibility has changed
                    playback_session_controller.seekToFrame(current_frame_num,
                                                            true);
                }

                if (!window_was_decoding[win_name] && is_visible &&
                    !ps.play_video && !ps.pause_seeked) {
                    // seek if visibility has changed
                    playback_session_controller.seekToFrame(current_frame_num,
                                                            true);
                    for (auto &[key, value] : window_need_decoding) {
                        value.store(true);
                    }
                }

                if (ps.play_video) {
                    window_need_decoding[win_name].store(is_visible);
                };

                if (is_visible) {
                    const CameraViewPresenterContext camera_view_presenter_context{
                        scene,
                        j,
                        current_frame_num,
                        ps.to_display_frame_number,
                        ps.read_head,
                        select_corr_head,
                        ps.play_video,
                        ps.pause_seeked,
                        yolo_detection,
                        playbackLightweightRendererIsActive(
                            ps.play_video, playback_renderer_mode),
                        playbackPreviewIsActive(
                            ps.play_video, yolo_detection,
                            playback_preview_scale_mode),
                        playbackPreviewScaleFactor(
                            playback_preview_scale_mode),
                        playback_preview_scale_mode,
                    };
                    const CameraViewPresenterResult camera_view_presenter_result =
                        presentCameraViewFrame(camera_view_presenter_context);

                    unsigned char *presented_rgba_cuda_buffer =
                        camera_view_presenter_result.presented_rgba_cuda_buffer;
                    bool swap_playback_surface_after_draw =
                        camera_view_presenter_result.swap_playback_surface_after_draw;
                    int presented_slot =
                        camera_view_presenter_result.presented_slot;
                    int presented_frame =
                        camera_view_presenter_result.presented_frame;
                    current_frame_num =
                        camera_view_presenter_result.resolved_current_frame_num;

                    frame_camera_upload_ms +=
                        camera_view_presenter_result.perf.upload_ms;
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

                    // sync yolo detection
                    if (yolo_detection) {
                        std::unique_lock<std::mutex> lck(g_mutexes[j]);
                        // std::cout << "main_thread: acquire lock" <<
                        // std::endl;
                        yolo_input_frames_rgba[j] = presented_rgba_cuda_buffer;
                        g_ready[j] = true;
                        g_cvs[j].notify_one();
                    }

                    ZarrDetectionLoader::FrameDetections detection_details;
                    const int zarr_bbox_query_frame = current_frame_num;
                    const bool is_zarr_interpolated =
                        zarr_loaded && zarr_loader.hasInterpolation() &&
                        zarr_loader.isFrameInterpolated(zarr_bbox_query_frame);
                    const bool active_dataset_is_raw_detect =
                        zarr_loaded && zarr_loader.hasDetectionData() &&
                        (zarr_loader.getActiveDetectionDataset() ==
                         ZarrDetectionLoader::DetectionDataset::RawDetect);
                    const bool dataset_allows_bbox_edit =
                        zarr_loaded && zarr_loader.hasDetectionData() &&
                        !active_dataset_is_raw_detect;
                    if (zarr_loaded && !dataset_allows_bbox_edit) {
                        g_zarr_bbox_edit_state.draw_mode = false;
                        g_zarr_bbox_edit_state.cancelDraw();
                        g_zarr_bbox_edit_state.clearSelection();
                    }
                    const bool can_modify_boxes =
                        dataset_allows_bbox_edit &&
                        g_zarr_bbox_edit_state.enabled &&
                        (g_zarr_bbox_edit_state.allow_edit_while_playing ||
                         !ps.play_video);
                    std::vector<LoggedBoundingBox> loaded_zarr_boxes;
                    std::vector<LoggedBoundingBox> zarr_boxes;
                    if (zarr_loaded) {
                        loaded_zarr_boxes =
                            zarr_loader.getBoundingBoxesForFrame(
                                zarr_bbox_query_frame);
                        zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(
                            zarr_bbox_query_frame, loaded_zarr_boxes);
                        detection_details =
                            zarr_loader.getRawDetections(zarr_bbox_query_frame,
                                                         false);
                    }

                    auto deleteSelectedBoxOnCurrentFrame = [&]() -> bool {
                        if (!can_modify_boxes) {
                            return false;
                        }
                        if (g_zarr_bbox_edit_state.selected_frame != current_frame_num ||
                            g_zarr_bbox_edit_state.selected_box < 0) {
                            return false;
                        }
                        auto& editable_boxes =
                            g_zarr_bbox_edit_state.ensureFrameOverride(
                                current_frame_num, loaded_zarr_boxes,
                                &detection_details);
                        auto& added_flags =
                            g_zarr_bbox_edit_state.ensureAddedFlags(
                                current_frame_num, editable_boxes.size());
                        auto& manual_flags =
                            g_zarr_bbox_edit_state.ensureManualFlags(
                                current_frame_num, editable_boxes.size());
                        g_zarr_bbox_edit_state.ensureSourceMetadata(
                            current_frame_num, editable_boxes.size());
                        auto& source_indices =
                            g_zarr_bbox_edit_state
                                .frame_source_indices[current_frame_num];
                        auto& source_detection_source =
                            g_zarr_bbox_edit_state
                                .frame_source_detection_source[current_frame_num];
                        auto& source_reason =
                            g_zarr_bbox_edit_state
                                .frame_source_reason[current_frame_num];
                        const int selected_idx =
                            g_zarr_bbox_edit_state.selected_box;
                        if (selected_idx < 0 ||
                            selected_idx >=
                                static_cast<int>(editable_boxes.size())) {
                            g_zarr_bbox_edit_state.clearSelection();
                            return false;
                        }
                        editable_boxes.erase(editable_boxes.begin() +
                                             selected_idx);
                        if (selected_idx <
                            static_cast<int>(added_flags.size())) {
                            added_flags.erase(added_flags.begin() +
                                              selected_idx);
                        } else {
                            added_flags.assign(editable_boxes.size(), 0);
                        }
                        if (selected_idx <
                            static_cast<int>(manual_flags.size())) {
                            manual_flags.erase(manual_flags.begin() +
                                               selected_idx);
                        } else {
                            manual_flags.assign(editable_boxes.size(), 0);
                        }
                        if (selected_idx <
                            static_cast<int>(source_indices.size())) {
                            source_indices.erase(source_indices.begin() +
                                                 selected_idx);
                        } else {
                            source_indices.assign(editable_boxes.size(), -1);
                        }
                        if (selected_idx < static_cast<int>(
                                               source_detection_source.size())) {
                            source_detection_source.erase(
                                source_detection_source.begin() + selected_idx);
                        } else {
                            source_detection_source.assign(editable_boxes.size(),
                                                          0);
                        }
                        if (selected_idx <
                            static_cast<int>(source_reason.size())) {
                            source_reason.erase(source_reason.begin() +
                                                selected_idx);
                        } else {
                            source_reason.assign(editable_boxes.size(),
                                                 std::string{});
                        }
                        g_zarr_bbox_edit_state.dirty_frames.insert(
                            current_frame_num);
                        g_zarr_bbox_edit_state.drag_active = false;
                        g_zarr_bbox_edit_state.drag_mouse_button = -1;
                        if (editable_boxes.empty()) {
                            g_zarr_bbox_edit_state.clearSelection();
                        } else {
                            g_zarr_bbox_edit_state.selected_frame =
                                current_frame_num;
                            g_zarr_bbox_edit_state.selected_box = std::min(
                                selected_idx,
                                static_cast<int>(editable_boxes.size() - 1));
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
                    full_frame_edit_state.draw_mode =
                        g_zarr_bbox_edit_state.draw_mode;
                    full_frame_edit_state.draw_active =
                        g_zarr_bbox_edit_state.draw_active;
                    full_frame_edit_state.draw_frame =
                        g_zarr_bbox_edit_state.draw_frame;
                    full_frame_edit_state.draw_anchor_x =
                        g_zarr_bbox_edit_state.draw_anchor_x;
                    full_frame_edit_state.draw_anchor_y =
                        g_zarr_bbox_edit_state.draw_anchor_y;
                    full_frame_edit_state.draw_current_x =
                        g_zarr_bbox_edit_state.draw_current_x;
                    full_frame_edit_state.draw_current_y =
                        g_zarr_bbox_edit_state.draw_current_y;

                    std::vector<ZarrDetectionLoader::ChaserBoundingBox>
                        chaser_bboxes;
                    std::vector<ZarrDetectionLoader::ChaserState> chaser_states;
                    if (zarr_loaded) {
                        chaser_bboxes =
                            zarr_loader.getChaserBoundingBoxesForFrame(
                                current_frame_num);
                        chaser_states =
                            zarr_loader.getChaserInterpolatedStatesForCameraFrame(
                                current_frame_num);
                        if (chaser_states.empty() &&
                            ps.current_stimulus_frame >= 0 &&
                            zarr_loader.hasStimulusFrameMapping()) {
                            chaser_states =
                                zarr_loader.getChaserStatesForStimulusFrame(
                                    ps.current_stimulus_frame);
                        }
                        if (chaser_states.empty()) {
                            chaser_states =
                                zarr_loader.getChaserStatesForFrame(
                                    current_frame_num);
                        }
#if defined(CRIMSON_CHASER_DEBUG_LOGS)
                        static bool debug_printed = false;
                        static int frames_with_data = 0;
                        if (!chaser_bboxes.empty() || !chaser_states.empty()) {
                            frames_with_data++;
                            if (!debug_printed) {
                                std::cout << "\n=== CHASER DATA DEBUG ==="
                                          << std::endl;
                                std::cout
                                    << "Camera frame " << current_frame_num
                                    << ": Found " << chaser_bboxes.size()
                                    << " chaser bboxes, "
                                    << chaser_states.size()
                                    << " chaser states" << std::endl;
                                if (!chaser_bboxes.empty()) {
                                    std::cout
                                        << "  First bbox: fish_id="
                                        << chaser_bboxes[0].fish_id
                                        << ", x=" << chaser_bboxes[0].x_px
                                        << ", y=" << chaser_bboxes[0].y_px
                                        << ", w="
                                        << chaser_bboxes[0].width_px
                                        << ", h="
                                        << chaser_bboxes[0].height_px
                                        << std::endl;
                                }
                                if (!chaser_states.empty()) {
                                    std::cout
                                        << "  First state: stimulus_frame="
                                        << chaser_states[0].stimulus_frame_num
                                        << ", camera_frame="
                                        << chaser_states[0].camera_frame_id
                                        << std::endl;
                                    std::cout
                                        << "    chaser=("
                                        << chaser_states[0].chaser_pos_x
                                        << ","
                                        << chaser_states[0].chaser_pos_y
                                        << ") target=("
                                        << chaser_states[0].target_pos_x
                                        << ","
                                        << chaser_states[0].target_pos_y
                                        << ")" << std::endl;
                                    std::cout
                                        << "  Camera params: has_homography="
                                        << camera_params[j].has_valid_homography
                                        << ", offsetX="
                                        << camera_params[j].stimulus_offset_x
                                        << ", offsetY="
                                        << camera_params[j].stimulus_offset_y
                                        << std::endl;
                                }
                                debug_printed = true;
                            }
                        }
                        static int last_frame_checked = -1;
                        if (current_frame_num > last_frame_checked + 1000) {
                            std::cout << "Frames " << (last_frame_checked + 1)
                                      << "-" << current_frame_num << ": "
                                      << frames_with_data
                                      << " frames had chaser data"
                                      << std::endl;
                            frames_with_data = 0;
                            last_frame_checked = current_frame_num;
                        }
#endif
                    }

                    const bool heading_overlay_enabled = show_heading_arrows;
                    const bool heading_data_available =
                        zarr_loaded && zarr_loader.hasHeadingData();
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
                                headingDebugLog("Heading overlay disabled via UI toggle; skipping arrow drawing.");
                                heading_debug_logged_toggle_disabled = true;
                            }
                        } else {
                            if (heading_debug_logged_toggle_disabled) {
                                headingDebugLog("Heading overlay toggle enabled; attempting to draw arrows.");
                                heading_debug_logged_toggle_disabled = false;
                            }
                            if (!heading_data_available) {
                                if (!heading_debug_logged_no_data) {
                                    headingDebugLog("Zarr loader reports no heading data; arrows will not be drawn.");
                                    heading_debug_logged_no_data = true;
                                }
                            } else if (heading_debug_logged_no_data) {
                                headingDebugLog("Heading data detected; resuming arrow attempts.");
                                heading_debug_logged_no_data = false;
                            }
                            if (zarr_loaded &&
                                zarr_loader.activeDatasetHasSyntheticDetections()) {
                                if (!heading_debug_logged_interpolated) {
                                    headingDebugLog("Dataset contains synthetic detections; headings render only for real boxes.");
                                    heading_debug_logged_interpolated = true;
                                }
                            } else if (heading_debug_logged_interpolated) {
                                headingDebugLog("Dataset now fully real; headings may render for all boxes.");
                                heading_debug_logged_interpolated = false;
                            }
                        }
                    }

                    if (kEyeMaskDebugLoggingEnabled) {
                        if (!show_eye_masks) {
                            if (!eye_mask_debug_logged_toggle_disabled) {
                                eyeMaskDebugLog("Eye mask overlay disabled via UI toggle; skipping mask drawing.");
                                eye_mask_debug_logged_toggle_disabled = true;
                            }
                        } else if (eye_mask_debug_logged_toggle_disabled) {
                            eyeMaskDebugLog("Eye mask overlay toggle enabled; attempting to draw masks.");
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

                    std::optional<ZarrDetectionLoader::FrameDetections>
                        heading_details;
                    std::optional<ZarrDetectionLoader::FrameDetections>
                        mask_details;
                    if (can_draw_headings) {
                        heading_details = zarr_loader.getRawDetections(
                            current_frame_num,
                            /*use_interpolated=*/false,
                            /*include_eye_masks=*/false);
                    }
                    if (can_draw_eye_masks) {
                        mask_details = zarr_loader.getRawDetections(
                            current_frame_num,
                            /*use_interpolated=*/false,
                            /*include_eye_masks=*/true);
                    }

                    std::vector<std::string> frame_events;
                    if (zarr_loaded && zarr_loader.hasStimulusEvents()) {
                        frame_events =
                            zarr_loader.getStimulusEventsForFrame(
                                current_frame_num);
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
                        dc_context->total_num_frame !=
                            std::numeric_limits<int>::max()) {
                        total_recording_frames = std::max(
                            total_recording_frames,
                            dc_context->total_num_frame);
                    }

                    const CameraViewWindowContext camera_view_context{
                        scene,
                        j,
                        win_name,
                        current_frame_num,
                        presented_slot,
                        presented_frame,
                        swap_playback_surface_after_draw,
                        ps.play_video,
                        playbackLightweightRendererIsActive(
                            ps.play_video, playback_renderer_mode),
                        use_legacy_manual_keypoint_tools,
                        &legacy_labeling_state,
                        zarr_loaded,
                        dataset_allows_bbox_edit,
                        g_zarr_bbox_edit_state.enabled,
                        g_zarr_bbox_edit_state.allow_edit_while_playing,
                        &g_zarr_bbox_edit_state,
                        full_frame_edit_state,
                        zarr_loaded ? &zarr_boxes : nullptr,
                        zarr_loaded ? &detection_details : nullptr,
                        zarr_loaded &&
                            zarr_loader.activeDatasetHasSyntheticDetections(),
                        is_zarr_interpolated,
                        latest_decoded,
                        total_recording_frames,
                        yolo_detection,
                        yolo_detection ? &yolo_boxes.at(j) : nullptr,
                        yolo_detection ? &yolo_labels.at(j) : nullptr,
                        yolo_detection ? &yolo_classid.at(j) : nullptr,
                        show_keypoint_markers,
                        can_draw_headings,
                        can_draw_eye_masks,
                        heading_details ? &*heading_details : nullptr,
                        mask_details ? &*mask_details : nullptr,
                        zarr_loaded
                            ? (zarr_loader.getEyeMaskRunName() + "|" +
                               zarr_loader.getEyeAngleRunName())
                            : std::string{},
                        zarr_loaded ? &chaser_bboxes : nullptr,
                        zarr_loaded ? &chaser_states : nullptr,
                        &camera_params[j],
                        !frame_events.empty() ? &frame_events : nullptr,
                        CameraViewTransportControlsContext{
                            ps.to_display_frame_number,
                            dc_context->total_num_frame,
                            dc_context->estimated_num_frames,
                            video_fps,
                            ps.play_video,
                            ps.slider_frame_number,
                        },
                    };
                    const CameraViewWindowResult camera_view_result =
                        drawCameraViewWindowContents(camera_view_context);

                    perf_camera_viewport_width_px =
                        camera_view_result.perf.viewport_width_px;
                    perf_camera_viewport_height_px =
                        camera_view_result.perf.viewport_height_px;
                    perf_camera_view_x_min =
                        camera_view_result.perf.view_x_min;
                    perf_camera_view_x_max =
                        camera_view_result.perf.view_x_max;
                    perf_camera_view_y_min =
                        camera_view_result.perf.view_y_min;
                    perf_camera_view_y_max =
                        camera_view_result.perf.view_y_max;
                    perf_camera_view_visible_fraction =
                        camera_view_result.perf.visible_fraction;
                    perf_camera_view_zoomed_in =
                        camera_view_result.perf.zoomed_in;
                    frame_camera_plot_image_ui_ms +=
                        camera_view_result.perf.plot_image_ui_ms;
                    frame_camera_overlay_ui_ms +=
                        camera_view_result.perf.overlay_ui_ms;
                    frame_camera_playback_swap_ms +=
                        camera_view_result.perf.playback_swap_ms;
                    frame_camera_scene_ui_ms +=
                        camera_view_result.perf.scene_ui_ms;
                    frame_sync_valid_slots =
                        camera_view_result.frame_sync.valid_slots;
                    frame_sync_empty_slots =
                        camera_view_result.frame_sync.empty_slots;
                    frame_sync_latest_decoded =
                        camera_view_result.frame_sync.latest_decoded;
                    frame_sync_recording_remaining =
                        camera_view_result.frame_sync.recording_remaining;
                    frame_sync_recording_total =
                        camera_view_result.frame_sync.recording_total;
                    frame_sync_debug_line =
                        camera_view_result.frame_sync.debug_line;

                    const FullFrameRectEditResult& full_frame_edit_result =
                        camera_view_result.full_frame_edit_result;

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
                        g_zarr_bbox_edit_state.clearFrameEdits(
                            current_frame_num);
                        zarr_boxes = loaded_zarr_boxes;
                    }
                    if (full_frame_edit_result.request_delete_selected) {
                        deleteSelectedBoxOnCurrentFrame();
                    }
                    if (full_frame_edit_result.request_add_rect) {
                        auto& editable_boxes =
                            g_zarr_bbox_edit_state.ensureFrameOverride(
                                current_frame_num, loaded_zarr_boxes,
                                &detection_details);
                        auto& added_flags =
                            g_zarr_bbox_edit_state.ensureAddedFlags(
                                current_frame_num, editable_boxes.size());
                        auto& manual_flags =
                            g_zarr_bbox_edit_state.ensureManualFlags(
                                current_frame_num, editable_boxes.size());
                        g_zarr_bbox_edit_state.ensureSourceMetadata(
                            current_frame_num, editable_boxes.size());
                        auto& source_indices =
                            g_zarr_bbox_edit_state
                                .frame_source_indices[current_frame_num];
                        auto& source_detection_source =
                            g_zarr_bbox_edit_state
                                .frame_source_detection_source[current_frame_num];
                        auto& source_reason =
                            g_zarr_bbox_edit_state
                                .frame_source_reason[current_frame_num];

                        uint16_t new_class_id = 0;
                        float new_confidence = 1.0f;
                        if (g_zarr_bbox_edit_state.selected_frame ==
                                current_frame_num &&
                            g_zarr_bbox_edit_state.selected_box >= 0 &&
                            g_zarr_bbox_edit_state.selected_box <
                                static_cast<int>(zarr_boxes.size())) {
                            const auto& selected_box =
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
                        new_box.payload_frame_id = static_cast<uint64_t>(
                            std::max(0, current_frame_num));
                        new_box.payload_camera_id = 0;
                        new_box.box_index_in_payload =
                            static_cast<uint8_t>(editable_boxes.size());
                        new_box.x_min = full_frame_edit_result.new_rect.x_min;
                        new_box.y_min = full_frame_edit_result.new_rect.y_min;
                        new_box.width = full_frame_edit_result.new_rect.width;
                        new_box.height = full_frame_edit_result.new_rect.height;
                        new_box.class_id = new_class_id;
                        new_box.confidence =
                            std::isfinite(new_confidence) ? new_confidence
                                                          : 1.0f;

                        editable_boxes.push_back(new_box);
                        added_flags.push_back(1);
                        manual_flags.push_back(1);
                        source_indices.push_back(-1);
                        source_detection_source.push_back(0);
                        source_reason.emplace_back("manual");
                        g_zarr_bbox_edit_state.dirty_frames.insert(
                            current_frame_num);
                        g_zarr_bbox_edit_state.selected_frame =
                            current_frame_num;
                        g_zarr_bbox_edit_state.selected_box =
                            static_cast<int>(editable_boxes.size() - 1);
                    }
                    if (full_frame_edit_result.request_move_selected) {
                        auto& editable_boxes =
                            g_zarr_bbox_edit_state.ensureFrameOverride(
                                current_frame_num, loaded_zarr_boxes,
                                &detection_details);
                        auto& manual_flags =
                            g_zarr_bbox_edit_state.ensureManualFlags(
                                current_frame_num, editable_boxes.size());
                        const int selected_idx =
                            full_frame_edit_result.move_box_index;
                        if (selected_idx >= 0 &&
                            selected_idx <
                                static_cast<int>(editable_boxes.size())) {
                            LoggedBoundingBox& moving_box =
                                editable_boxes[selected_idx];
                            const float max_x = std::max(
                                0.0f,
                                static_cast<float>(scene->cameras[j].image_width) -
                                    moving_box.width);
                            const float max_y = std::max(
                                0.0f,
                                static_cast<float>(scene->cameras[j].image_height) -
                                    moving_box.height);
                            moving_box.x_min = std::clamp(
                                full_frame_edit_result.move_target_x, 0.0f,
                                max_x);
                            moving_box.y_min = std::clamp(
                                full_frame_edit_result.move_target_y, 0.0f,
                                max_y);
                            if (selected_idx <
                                static_cast<int>(manual_flags.size())) {
                                manual_flags[selected_idx] = 1;
                            }
                            g_zarr_bbox_edit_state.dirty_frames.insert(
                                current_frame_num);
                        } else {
                            g_zarr_bbox_edit_state.clearSelection();
                        }
                    }

                    if (use_legacy_manual_keypoint_tools) {
                        legacy_labeling_state.keypoints_find =
                            camera_view_result.legacy_manual_keypoints_find;
                        is_view_focused[j] = camera_view_result.view_focused;
                    }
                    const CameraViewTransportControlsResult&
                        camera_transport_result =
                            camera_view_result.transport_result;
                    ps.slider_frame_number =
                        camera_transport_result.slider_frame_number;
                    ps.slider_just_changed =
                        camera_transport_result.slider_just_changed;
                    if (camera_transport_result.toggle_playback) {
                        playback_session_controller.applyPlaybackToggle();
                    }
                    if (camera_transport_result.step_delta != 0) {
                        playback_session_controller.stepFrames(
                            camera_transport_result.step_delta);
                    }
                    if (camera_transport_result.seek_target_frame.has_value()) {
                        playback_session_controller.seekToFrame(
                            *camera_transport_result.seek_target_frame, true,
                            camera_transport_result.force_inaccurate_seek);
                    }
                }
                ImGui::End();
            }

            const CameraViewPlaybackShortcutsResult playback_shortcuts =
                handleCameraViewPlaybackShortcuts();
            if (playback_shortcuts.toggle_playback) {
                playback_session_controller.applyPlaybackToggle();
            }
            if (playback_shortcuts.step_delta != 0) {
                playback_session_controller.stepFrames(
                    playback_shortcuts.step_delta);
            }

            for (const auto &[name, flag] : window_need_decoding) {
                window_was_decoding[name] = flag.load();
            }
        }

        if (zarr_loaded &&
            (zarr_loader.hasCropImages() || zarr_loader.hasKeypointData() ||
             zarr_loader.hasEyeMasks())) {
            const auto crop_preview_ui_start = std::chrono::steady_clock::now();
            RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
            CropFrameSource live_crop_frame_source;
            int crop_preview_frame_num = current_frame_num;
            if (video_loaded) {
                const int visible_idx =
                    playback_session_controller.getVisibleCameraIndex();
                if (visible_idx >= 0 && scene->size_of_buffer > 0) {
                    const auto& camera = scene->cameras[visible_idx];
                    if (ps.play_video && camera.texture_has_valid_frame &&
                        camera.last_uploaded_frame >= 0) {
                        crop_preview_frame_num = camera.last_uploaded_frame;
                    }
                    const int preferred_slot = ps.read_head % scene->size_of_buffer;
                    const int slot_index = findCameraDisplaySlotForFrame(
                        *scene, visible_idx, crop_preview_frame_num,
                        preferred_slot);
                    if (camera.texture_has_valid_frame &&
                        camera.last_uploaded_frame == crop_preview_frame_num &&
                        camera.image_texture != 0) {
                        live_crop_frame_source.frame_number = crop_preview_frame_num;
                        live_crop_frame_source.width =
                            static_cast<int>(camera.image_width);
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
                        const auto& slot = camera.display_buffer[slot_index];
                        if (!slot.available_to_write &&
                            slot.frame_number == crop_preview_frame_num &&
                            slot.frame != nullptr) {
                            live_crop_frame_source.frame = slot.frame;
                            live_crop_frame_source.frame_number =
                                slot.frame_number;
                            live_crop_frame_source.width =
                                static_cast<int>(camera.image_width);
                            live_crop_frame_source.height =
                                static_cast<int>(camera.image_height);
                            live_crop_frame_source.pitch_bytes =
                                slot.pitch_bytes;
                            live_crop_frame_source.color_matrix =
                                slot.color_matrix;
                            if (scene->use_cpu_buffer &&
                                slot.format == PictureBufferFormat::RGBA32) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::HostRGBA32;
                            } else if (slot.format ==
                                       PictureBufferFormat::RGBA32) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::DeviceRGBA32;
                            } else if (slot.format ==
                                       PictureBufferFormat::NV12) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::DeviceNV12;
                            }
                        }
                    }
                }
            }
            ZarrPersistedCropProvider persisted_crop_image_provider(zarr_loader);
            LiveCropImageProvider live_crop_image_provider(
                zarr_loader, live_crop_frame_source);
            ChainedCropImageProvider crop_image_provider(
                live_crop_image_provider, persisted_crop_image_provider);
            std::optional<CropSpec> selected_crop_spec;
            int selected_detection_index = -1;
            if (g_zarr_bbox_edit_state.selected_frame == crop_preview_frame_num &&
                g_zarr_bbox_edit_state.selected_box >= 0 &&
                zarr_loader.hasDetectionData()) {
                const auto loaded_crop_boxes =
                    zarr_loader.getBoundingBoxesForFrame(crop_preview_frame_num);
                const auto resolved_crop_boxes =
                    g_zarr_bbox_edit_state.resolveFrameBoxes(crop_preview_frame_num,
                                                             loaded_crop_boxes);
                if (g_zarr_bbox_edit_state.selected_box <
                    static_cast<int>(resolved_crop_boxes.size())) {
                    const auto& selected_box =
                        resolved_crop_boxes[static_cast<size_t>(
                            g_zarr_bbox_edit_state.selected_box)];
                    CropSpec crop_spec;
                    crop_spec.offset_x = selected_box.x_min;
                    crop_spec.offset_y = selected_box.y_min;
                    crop_spec.width_px = selected_box.width;
                    crop_spec.height_px = selected_box.height;
                    crop_spec.valid = selected_box.width > 0.0f &&
                                      selected_box.height > 0.0f;
                    if (crop_spec.valid) {
                        selected_crop_spec = crop_spec;
                    }

                    auto source_it =
                        g_zarr_bbox_edit_state.frame_source_indices.find(
                            crop_preview_frame_num);
                    if (source_it !=
                            g_zarr_bbox_edit_state.frame_source_indices.end() &&
                        g_zarr_bbox_edit_state.selected_box <
                            static_cast<int>(source_it->second.size())) {
                        selected_detection_index =
                            source_it->second[static_cast<size_t>(
                                g_zarr_bbox_edit_state.selected_box)];
                    } else if (g_zarr_bbox_edit_state.selected_box <
                               static_cast<int>(loaded_crop_boxes.size())) {
                        selected_detection_index =
                            g_zarr_bbox_edit_state.selected_box;
                    }
                }
            }
            const CropPreviewWindowContext crop_preview_context{
                crop_image_provider,
                zarr_loader,
                refined_keypoint_repo,
                crop_preview_frame_num,
                g_zarr_bbox_edit_state.selected_frame,
                g_zarr_bbox_edit_state.selected_box,
                selected_detection_index,
                selected_crop_spec,
                ps.play_video,
            };
            const auto crop_preview_result = drawCropPreviewWindow(
                crop_preview_context, crop_preview_window_state);

            applyCropPreviewKeypointWriteAction(
                refined_keypoint_repo,
                crop_preview_result.editor_action,
                crop_preview_result.selected_keypoint_selection,
                crop_preview_window_state.editor_state,
                frame_debug_window_state.keypoint_review_panel
                    .manual_write_status,
                reloadActiveZarrPreserveDataset);
            frame_crop_preview_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - crop_preview_ui_start);
        }

        static StimulusPlaybackWindowsState stimulus_playback_windows_state;
        if (stimulus_player.loaded) {
            const auto stimulus_playback_windows_result =
                drawStimulusPlaybackWindows(
                    StimulusPlaybackWindowsContext{
                        stimulus_player,
                        zarr_loaded ? &zarr_loader : nullptr,
                        ps,
                        seek_progress,
                        current_frame_num,
                        video_fps,
                        latest_decoded_frame[stimulus_player.window_name].load(),
                        window_need_decoding[stimulus_player.window_name].load(),
                        &stimulus_catchup_seek_generation,
                    },
                    stimulus_playback_windows_state);
            window_need_decoding[stimulus_player.window_name].store(
                stimulus_playback_windows_result.decoder_requested);
            frame_stimulus_window_ui_ms +=
                stimulus_playback_windows_result.stimulus_window_ui_ms;
            frame_stimulus_buffer_window_ui_ms +=
                stimulus_playback_windows_result.stimulus_buffer_window_ui_ms;
        }

        if (use_legacy_manual_keypoint_tools) {
            const auto keypoints_window_ui_start =
                std::chrono::steady_clock::now();
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
            const auto labeling_tool_ui_start =
                std::chrono::steady_clock::now();
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

            frame_labeling_tool_ui_ms += durationMs(
                std::chrono::steady_clock::now() - labeling_tool_ui_start);
        }

        static TimelineScrollState shared_timeline_scroll_state;
        static StimulusEventTimelineWindowState stimulus_timeline_window_state;
        static MovementTimelineWindowState movement_timeline_window_state;

        // Stimulus Event Timeline Window
        if (zarr_loaded) {
            const auto stimulus_timeline_ui_start =
                std::chrono::steady_clock::now();
            StimulusEventTimelineWindowContext stimulus_timeline_context{
                zarr_loader,
                shared_timeline_scroll_state,
                current_frame_num,
                video_fps,
            };
            StimulusEventTimelineWindowResult stimulus_timeline_result =
                drawStimulusEventTimelineWindow(stimulus_timeline_context,
                                               stimulus_timeline_window_state);
            if (stimulus_timeline_result.seek_target_frame.has_value()) {
                playback_session_controller.seekToFrame(
                    *stimulus_timeline_result.seek_target_frame, true);
            }
            frame_stimulus_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - stimulus_timeline_ui_start);
        }

        // Movement timeline windows
        if (zarr_loaded && zarr_loader.hasMovementData()) {
            const auto movement_timeline_ui_start =
                std::chrono::steady_clock::now();
            MovementTimelineWindowContext movement_timeline_context{
                zarr_loader,
                shared_timeline_scroll_state,
                current_frame_num,
                video_fps,
            };
            drawMovementTimelineWindow(movement_timeline_context,
                                       movement_timeline_window_state);
            frame_movement_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - movement_timeline_ui_start);
        }

        shared_timeline_scroll_state.prev_enabled =
            shared_timeline_scroll_state.enabled;

        processHelpMenuShortcut(show_help_window);

        if (show_help_window) {
            const auto help_menu_ui_start = std::chrono::steady_clock::now();
            drawHelpMenuWindow(show_help_window);
            frame_help_menu_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - help_menu_ui_start);
        }

        drawErrorPopup(show_error, error_message);

        // Rendering
        frame_ui_build_ms =
            durationMs(std::chrono::steady_clock::now() - ui_build_start);
        const auto imgui_render_start = std::chrono::steady_clock::now();
        ImGui::Render();
        frame_imgui_render_ms = durationMs(
            std::chrono::steady_clock::now() - imgui_render_start);
        ImDrawData* imgui_draw_data = ImGui::GetDrawData();
        if (imgui_draw_data != nullptr) {
            frame_imgui_draw_list_count = imgui_draw_data->CmdListsCount;
            frame_imgui_total_vtx_count = imgui_draw_data->TotalVtxCount;
            frame_imgui_total_idx_count = imgui_draw_data->TotalIdxCount;
            int draw_cmd_count = 0;
            for (int list_idx = 0; list_idx < imgui_draw_data->CmdListsCount;
                 ++list_idx) {
                const ImDrawList* draw_list = imgui_draw_data->CmdLists[list_idx];
                if (draw_list != nullptr) {
                    draw_cmd_count += draw_list->CmdBuffer.Size;
                }
            }
            frame_imgui_draw_cmd_count = draw_cmd_count;
        }
        int display_w, display_h;
        glfwGetFramebufferSize(window->render_target, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w,
                     clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        const auto gl_draw_start = std::chrono::steady_clock::now();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        frame_gl_draw_ms = durationMs(std::chrono::steady_clock::now() -
                                      gl_draw_start);

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
        frame_swap_ms =
            durationMs(std::chrono::steady_clock::now() - swap_start);

        if (ps.just_seeked) {
            ps.just_seeked = false;
        } else {
            if (dc_context->decoding_flag && ps.play_video) {
                // always round up, mimimum 1
                int frame_to_show =
                    static_cast<int>(std::ceil(playback_time_now * video_fps));
                perf_requested_camera_frame = frame_to_show;

                int min_decoded_frame = INT_MAX;
                bool have_decode_bound = false;
                auto considerDecodeBound = [&](const std::string &stream_name) {
                    auto need_it = window_need_decoding.find(stream_name);
                    if (need_it == window_need_decoding.end() ||
                        !need_it->second.load()) {
                        return;
                    }
                    auto latest_it = latest_decoded_frame.find(stream_name);
                    if (latest_it == latest_decoded_frame.end()) {
                        return;
                    }
                    int decoded = latest_it->second.load();
                    if (decoded < 0) {
                        return;
                    }
                    min_decoded_frame = std::min(min_decoded_frame, decoded);
                    have_decode_bound = true;
                };
                for (const auto &cam_name : camera_names) {
                    considerDecodeBound(cam_name);
                }
                perf_min_decoded_camera_frame =
                    have_decode_bound ? min_decoded_frame : -1;
                if (have_decode_bound) {
                    frame_to_show = std::min(frame_to_show, min_decoded_frame);
                } else {
                    frame_to_show = ps.to_display_frame_number;
                }
                if (kPlaybackDebugLoggingEnabled) {
                    static int last_logged_display = -1;
                    if (frame_to_show != last_logged_display) {
                        int stim_latest =
                            latest_decoded_frame[stimulus_player.window_name].load();
                        bool stim_decode =
                            window_need_decoding[stimulus_player.window_name].load();
                        std::cout << "[Playback] request frame=" << frame_to_show
                                  << " min_decoded=" << min_decoded_frame
                                  << " stim_latest=" << stim_latest
                                  << " stim_decoder_active="
                                  << (stim_decode ? "true" : "false")
                                  << std::endl;
                        last_logged_display = frame_to_show;
                    }
                }

                int frame_delta = frame_to_show - ps.to_display_frame_number;
                if (frame_delta > 0) {
                    // Update frame number
                    ps.to_display_frame_number = frame_to_show;

                    // Mark all intermediate frames as available
                    for (int offset = 0; offset < frame_delta; ++offset) {
                        int index =
                            (ps.read_head + offset) % scene->size_of_buffer;
                        for (int j = 0; j < scene->num_cams; j++) {
                            scene->cameras[j].display_buffer[index].available_to_write =
                                true;
                        }
                    }

                    // Advance the read head
                    ps.read_head =
                        (ps.read_head + frame_delta) % scene->size_of_buffer;

                    // Optional: update slider/UI sync
                    ps.slider_frame_number = ps.to_display_frame_number;
                }
            }
        }

        maybeWritePerfLogSample(
            perf_log_writer,
            PerfLogFrameContext{
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
                frame_camera_playback_swap_ms,
                frame_camera_plot_image_ui_ms,
                frame_camera_overlay_ui_ms,
                frame_camera_scene_ui_ms,
                frame_file_browser_ui_ms,
                frame_frame_debug_ui_ms,
                frame_buffer_window_ui_ms,
                frame_crop_preview_ui_ms,
                frame_stimulus_buffer_window_ui_ms,
                frame_keypoints_window_ui_ms,
                frame_labeling_tool_ui_ms,
                frame_stimulus_window_ui_ms,
                frame_stimulus_timeline_ui_ms,
                frame_movement_timeline_ui_ms,
                frame_help_menu_ui_ms,
                frame_gl_draw_ms,
                frame_swap_ms,
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
            },
            kPerfLogSamplePeriod);
    }

    // Cleanup
    destroyStimulusPlayback(stimulus_player);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window->render_target);
    glfwTerminate();

    dc_context->stop_flag = true;
    // wait for threads to join
    for (auto &t : decoder_threads)
        t.join();

    return 0;
}
