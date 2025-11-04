#include "IconsForkAwesome.h"
#include "Logger.h"
#include "camera.h"
#include "filesystem"
#include "global.h"
#include "gui.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "render.h"
#include "skeleton.h"
#include "utils.h"
#include "yolo_detection.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <cmath>
#include <chrono>
#include <array>
#include <limits>
#include <sstream>
#include <numeric>
#include <unordered_map>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <cstdint>
#include <cstring>
#include "zarr_loader.h"
#include "gui_interpolation.h"

#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

simplelogger::Logger *logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {
inline bool IsFiniteFloat(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}
} // namespace

std::vector<std::mutex> g_mutexes(MAX_VIEWS);
std::vector<std::condition_variable> g_cvs(MAX_VIEWS);
std::vector<bool> g_ready(MAX_VIEWS);
std::vector<std::vector<cv::Rect>> yolo_boxes(MAX_VIEWS);
std::vector<std::vector<std::string>> yolo_labels(MAX_VIEWS);
std::vector<std::vector<int>> yolo_classid(MAX_VIEWS);
std::vector<unsigned char *> yolo_input_frames_rgba(MAX_VIEWS);
std::unordered_map<std::string, std::atomic<bool>> window_need_decoding;
std::unordered_map<std::string, std::atomic<int>> latest_decoded_frame;

// Global variables
bool show_interpolation_debug = false;
std::vector<ZarrDetectionLoader::DetectionDataset> detection_dataset_ids;
std::vector<std::string> detection_dataset_labels;
int detection_dataset_choice = 0;

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

struct PlaybackState {
    int pause_selected = 0;
    bool slider_just_changed = false;
    bool play_video = false;
    int to_display_frame_number = 0;
    int read_head = 0;
    bool just_seeked = false;
    bool pause_seeked = false;
    int slider_frame_number = 0;
    double accumulated_play_time = 0.0;
    std::chrono::steady_clock::time_point last_play_time_start =
        std::chrono::steady_clock::now();
    int last_frame_num_playspeed = 0;
    std::chrono::steady_clock::time_point last_wall_time_playspeed =
        std::chrono::steady_clock::now();
};

struct EyeOrientationSmoother {
    struct History {
        std::deque<double> angles;
        double last_unwrapped = std::numeric_limits<double>::quiet_NaN();
    };

    static constexpr size_t kMaxSamples = 60;

    void resetIfRunChanged(const std::string& run_id) {
        if (run_id != current_run_) {
            histories_.clear();
            current_run_ = run_id;
        }
    }

    ImVec2 smoothDirection(int32_t roi_index, int eye, const ImVec2& raw_dir) {
        if (roi_index < 0 || eye < 0 || eye >= 2) {
            return raw_dir;
        }
        double raw_angle = std::atan2(raw_dir.y, raw_dir.x);
        if (!std::isfinite(raw_angle)) {
            return raw_dir;
        }

        auto& history = histories_[roi_index][eye];

        double unwrapped = raw_angle;
        if (std::isfinite(history.last_unwrapped)) {
            while (unwrapped - history.last_unwrapped > M_PI) {
                unwrapped -= 2.0 * M_PI;
            }
            while (unwrapped - history.last_unwrapped < -M_PI) {
                unwrapped += 2.0 * M_PI;
            }
        }
        history.last_unwrapped = unwrapped;

        history.angles.push_back(unwrapped);
        if (history.angles.size() > kMaxSamples) {
            history.angles.pop_front();
        }

        std::vector<double> sorted(history.angles.begin(), history.angles.end());
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];
        if ((sorted.size() % 2) == 0 && sorted.size() >= 2) {
            median = 0.5 * (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]);
        }

        double wrapped = std::fmod(median, 2.0 * M_PI);
        if (wrapped <= -M_PI) wrapped += 2.0 * M_PI;
        if (wrapped > M_PI) wrapped -= 2.0 * M_PI;

        ImVec2 smoothed(static_cast<float>(std::cos(wrapped)),
                        static_cast<float>(std::sin(wrapped)));
        float len = std::sqrt(smoothed.x * smoothed.x + smoothed.y * smoothed.y);
        if (len > 1e-6f) {
            smoothed.x /= len;
            smoothed.y /= len;
            return smoothed;
        }
        return raw_dir;
    }

private:
    std::unordered_map<int32_t, std::array<History, 2>> histories_;
    std::string current_run_;
};

static EyeOrientationSmoother g_eye_orientation_smoother;

void seek_all_cameras(render_scene *scene, int frame_number, double video_fps,
                      PlaybackState &state, bool seek_accurate) {
    // Trigger seek request
    for (int i = 0; i < scene->num_cams; i++) {
        scene->seek_context[i].seek_frame = (uint64_t)frame_number;
        scene->seek_context[i].use_seek = true;
        scene->seek_context[i].seek_accurate = seek_accurate;
    }

    // Wait for seek to complete
    for (int i = 0; i < scene->num_cams; i++) {
        while (!scene->seek_context[i].seek_done) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    // Reset seek_done flags
    for (int i = 0; i < scene->num_cams; i++) {
        scene->seek_context[i].seek_done = false;
    }

    // Update playback state
    state.to_display_frame_number = scene->seek_context[0].seek_frame;
    state.read_head = 0;
    state.just_seeked = true;
    state.slider_frame_number = state.to_display_frame_number;

    state.accumulated_play_time = frame_number / video_fps;
    state.last_play_time_start = std::chrono::steady_clock::now();
    state.last_frame_num_playspeed = frame_number;
    state.last_wall_time_playspeed = std::chrono::steady_clock::now();
}

int main(int, char **) {
    gx_context *window = (gx_context *)malloc(sizeof(gx_context));
    *window =
        (gx_context){.swap_interval = 1, // use vsync
                     .width = 1920,
                     .height = 1080,
                     .render_target_title = (char *)malloc(100), // window title
                     .glsl_version = (char *)malloc(100)};

    render_initialize_target(window);

    render_scene *scene = (render_scene *)malloc(sizeof(render_scene));

    std::string root_dir;
    std::string skeleton_dir;
    std::vector<std::string> camera_names;
    std::vector<CameraParams> camera_params;
    std::vector<std::thread> decoder_threads;
    std::vector<FFmpegDemuxer *> demuxers;

    // Zarr loading
    ZarrDetectionLoader zarr_loader;
    bool zarr_loaded = false;

    DecoderContext *dc_context =
        (DecoderContext *)malloc(sizeof(DecoderContext));
    *dc_context = (DecoderContext){.decoding_flag = false,
                                   .stop_flag = false,
                                   .total_num_frame = int(INT_MAX),
                                   .estimated_num_frames = 0,
                                   .gpu_index = 0,
                                   .seek_interval = 250};

    // gui states, todo: bundle this later
    std::time_t last_saved = static_cast<std::time_t>(-1);
    bool video_loaded = false;
    bool cpu_buffer_toggle = true;
    bool plot_keypoints_flag = false;
    bool show_keypoint_markers = true;
    bool show_heading_arrows = true;
    bool show_eye_masks = false;
    int current_frame_num = 0;
    bool skeleton_chosen = false;
    std::vector<std::string> imgs_names;

    constexpr bool kHeadingDebugLoggingEnabled = false;
    constexpr int kHeadingDebugMaxMessages = 400;
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
    SkeletonContext *skeleton;
    std::map<u32, KeyPoints *> keypoints_map;
    bool keypoints_find = false;
    std::map<std::string, SkeletonPrimitive> skeleton_map;

    // others
    std::filesystem::path cwd = std::filesystem::current_path();
    std::string delimiter = "/";
    std::vector<std::string> tokenized_path = string_split(cwd, delimiter);
    std::string start_folder_name = "/home/" + tokenized_path[2] + "/data";
    start_folder_name = "/nfs/exports/ratlv";
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    ImGuiIO &io = ImGui::GetIO();

    ImPlotStyle &style = ImPlot::GetStyle();
    ImVec4 *colors = style.Colors;
    colors[ImPlotCol_Crosshairs] = ImVec4(0.3f, 0.10f, 0.64f, 1.00f);

    bool yolo_detection = false;
    std::vector<std::thread> yolo_threads;
    yolo_param yolo_setting = yolo_param();
    std::string keypoints_root_folder;
    int label_buffer_size = 64;
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

    while (!glfwWindowShouldClose(window->render_target)) {
        // Poll and handle events (inputs, window resize, etc.)
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

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

        if (ImGui::Begin("File Browser", NULL, ImGuiWindowFlags_MenuBar)) {
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("Open")) {
                        IGFD::FileDialogConfig config;
                        config.countSelectionMax = 0;
                        config.path = start_folder_name;
                        config.flags = ImGuiFileDialogFlags_Modal;
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "ChooseMedia", "Choose Media",
                            ".mp4,.tiff,.jpeg,.jpg,.png", config);
                    };
                    ImGui::EndMenu();
                }

                if (video_loaded) {
                    if (ImGui::BeginMenu("Skeleton")) {
                        if (!skeleton_chosen) {
                            skeleton = new SkeletonContext;
                            skeleton_map = skeleton_get_all();
                        }

                        for (auto &element : skeleton_map) {

                            if (ImGui::MenuItem(element.first.c_str(), NULL,
                                                skeleton->name == element.first,
                                                !skeleton_chosen)) {
                                if (element.second == SP_LOAD) {
                                    IGFD::FileDialogConfig config;
                                    config.countSelectionMax = 1;
                                    config.path = skeleton_dir;
                                    config.flags = ImGuiFileDialogFlags_Modal;
                                    ImGuiFileDialog::Instance()->OpenDialog(
                                        "ChooseSkeleton", "Choose Skeleton",
                                        ".json", config);
                                } else {

                                    bool load_calibration = true;
                                    if (scene->num_cams > 1) {
                                        for (u32 i = 0; i < scene->num_cams;
                                             i++) {
                                            std::string cam_file =
                                                root_dir + "/calibration/" +
                                                camera_names[i] + ".yaml";

                                            if (!std::filesystem::exists(cam_file)) {
                                                load_calibration = false;
                                                error_message = "Calibration file not found: " + cam_file;
                                                show_error = true;
                                                break;
                                            }
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
                                        skeleton_initialize(element.first,
                                                            root_dir, skeleton,
                                                            element.second);
                                        plot_keypoints_flag = true;
                                        keypoints_root_folder =
                                            root_dir + "/labeled_data/";
                                        // create folders
                                        std::filesystem::create_directory(
                                            keypoints_root_folder);
                                        skeleton_chosen = true;
                                    }
                                }
                            }

                        }
                        ImGui::EndMenu();
                    }

                    if (ImGui::BeginMenu("Detection")) {
                        if (cpu_buffer_toggle) {
                            if (ImGui::MenuItem("YOLOv5")) {
                                std::string yolov5_onnx =
                                    root_dir + "/yolo/v5/best.onnx";
                                std::string yolov5_labelname =
                                    root_dir + "/yolo/v5/label.names";
                                read_yolo_labels(yolov5_labelname,
                                                 &yolo_setting);

                                for (int i = 0; i < scene->num_cams; i++) {
                                    yolo_threads.push_back(
                                        std::thread(&yolo_process, yolov5_onnx,
                                                    &yolo_setting, i));
                                }
                                yolo_detection = true;
                            }
                        } else {
                            if (ImGui::MenuItem("YOLOv8")) {
                                std::string engine_file_path =
                                    root_dir +
                                    "/yolo/yolorat_bbox/rat_bbox.engine";
                                for (int i = 0; i < scene->num_cams; i++) {
                                    yolo_threads.push_back(std::thread(
                                        &yolo_process_trt, engine_file_path, i,
                                        scene->image_width[i],
                                        scene->image_height[i]));
                                }
                                yolo_detection = true;
                            }

                            if (ImGui::MenuItem("YOLOv8Pose")) {
                                std::string engine_file_path =
                                    root_dir + "/yolo/yolopose/rat_pose.engine";
                                for (int i = 0; i < scene->num_cams; i++) {
                                    yolo_threads.push_back(std::thread(
                                        &yolo_process_v8pose, engine_file_path,
                                        i, scene->image_width[i],
                                        scene->image_height[i]));
                                }
                                yolo_detection = true;
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
                ImGui::EndMenuBar();
            }
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                        1000.0f / ImGui::GetIO().Framerate,
                        ImGui::GetIO().Framerate);

            if (!video_loaded) {
                {
                    const char *items[] = {"CPU Buffer", "GPU Buffer"};
                    static int item_current = 0;
                    ImGui::Combo("Buffer Type", &item_current, items,
                                 IM_ARRAYSIZE(items));
                    if (item_current == 0) {
                        scene->use_cpu_buffer = true;
                    } else {
                        scene->use_cpu_buffer = false;
                    }
                }

                ImGui::InputInt("Buffer Size", &label_buffer_size);
            }
            if (video_loaded) {
                ImGui::InputInt("Seek Step", &dc_context->seek_interval, 10,
                                100);
                static int seek_accurate_frame_num = 0;
                ImGui::InputInt("Seek Accurate", &seek_accurate_frame_num, 1,
                                100);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    seek_all_cameras(scene, seek_accurate_frame_num, video_fps,
                                     ps, true);
                }

                auto now_wall = std::chrono::steady_clock::now();
                double wall_seconds =
                    std::chrono::duration<double>(now_wall -
                                                  ps.last_wall_time_playspeed)
                        .count();
                int frame_delta =
                    current_frame_num - ps.last_frame_num_playspeed;
                if (wall_seconds > 0.5 && ps.play_video) {
                    inst_speed =
                        frame_delta /
                        (video_fps * wall_seconds); // Real-time normalized
                    ps.last_frame_num_playspeed = current_frame_num;
                    ps.last_wall_time_playspeed = now_wall;
                }

                // Always draw the latest value
                if (ps.play_video) {
                    ImGui::Text("Video FPS: %.1f", video_fps);
                    ImGui::SliderFloat("Set Playback Speed",
                                       &set_playback_speed, 0.1f, 1.0f,
                                       "%.1fx");
                    ImGui::Text("Current Playback Speed: %.2fx", inst_speed);
                    ImGui::Text("Tip: If playback is slower than real-time, \n"
                                "collapse camera views to improve speed.");
                }
            }
        }
        ImGui::End();

        if (video_loaded) {
            ImGui::Begin("Frame Debug");
            ImGui::Text("Inspecting Frame: %d", current_frame_num);
            ImGui::Separator();

            // Check for Labeled Keypoints
            if (plot_keypoints_flag) {
                if (keypoints_map.count(current_frame_num)) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[Manual] Keypoints:   Found");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "[Manual] Keypoints:   None");
                }
            }

            if (zarr_loaded) {
                const bool dataset_has_synthetic_boxes = zarr_loader.activeDatasetHasSyntheticDetections();
                if (!detection_dataset_labels.empty()) {
                    ImGui::Text("Detection dataset:");
                    const char* current_label =
                        detection_dataset_labels[std::min<int>(detection_dataset_choice,
                                                               static_cast<int>(detection_dataset_labels.size()) - 1)].c_str();
                    bool dataset_changed = false;
                    if (ImGui::BeginCombo("##detection_dataset_combo", current_label)) {
                        for (int i = 0; i < static_cast<int>(detection_dataset_labels.size()); ++i) {
                            bool selected = (i == detection_dataset_choice);
                            if (ImGui::Selectable(detection_dataset_labels[i].c_str(), selected)) {
                                if (zarr_loader.setActiveDetectionDataset(detection_dataset_ids[i])) {
                                    detection_dataset_choice = i;
                                    dataset_changed = true;
                                }
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                        if (dataset_changed) {
                            refreshDetectionDatasetOptions(zarr_loader);
                            if (zarr_loader.getTotalFrames() > 0 &&
                                current_frame_num >= static_cast<int>(zarr_loader.getTotalFrames())) {
                                current_frame_num = static_cast<int>(zarr_loader.getTotalFrames()) - 1;
                            }
                        }
                    }
                }

                std::vector<LoggedBoundingBox> zarr_boxes;
                bool frame_is_interpolated = false;

                if (zarr_loader.hasInterpolation()) {
                    frame_is_interpolated = zarr_loader.isFrameInterpolated(current_frame_num);
                }

                zarr_boxes = zarr_loader.getBoundingBoxesForFrame(current_frame_num);

                const bool need_details =
                    zarr_loader.hasScores() ||
                    zarr_loader.hasHeadingData() ||
                    zarr_loader.hasKeypointData() ||
                    dataset_has_synthetic_boxes;
                ZarrDetectionLoader::FrameDetections detection_details;
                if (need_details) {
                    detection_details = zarr_loader.getRawDetections(current_frame_num, false);
                }

                if (!zarr_boxes.empty()) {
                    if (frame_is_interpolated && dataset_has_synthetic_boxes) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    Found %zu (INTERPOLATED)",
                                           zarr_boxes.size());
                    } else if (frame_is_interpolated && !dataset_has_synthetic_boxes) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                           "[Zarr] Detections:    Found %zu (original, interp available)",
                                           zarr_boxes.size());
                    } else {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    Found %zu", zarr_boxes.size());
                    }

                    if (zarr_loader.hasScores() && !detection_details.scores.empty()) {
                        float max_score = *std::max_element(
                            detection_details.scores.begin(), detection_details.scores.end());
                        ImGui::Text("  Max confidence: %.2f", max_score);
                    }

                    if (zarr_loader.hasClassIDs()) {
                        ImGui::Text("  Has class IDs: Yes");
                    }

                    if (zarr_loader.hasHeadingData()) {
                        if (!dataset_has_synthetic_boxes &&
                            !detection_details.heading_valid.empty()) {
                            size_t valid_headings =
                                std::count(detection_details.heading_valid.begin(),
                                           detection_details.heading_valid.end(), 1);
                            ImGui::Text("  Heading vectors: %zu valid", valid_headings);
                        } else {
                            ImGui::Text("  Heading vectors available (use original detections)");
                        }
                    }

                    if (zarr_loader.hasInterpolation()) {
                        ImGui::Text("  Interpolation available: Yes");
                        ImGui::Text("  Current frame interpolated: %s",
                                    frame_is_interpolated ? "Yes" : "No");
                        ImGui::Text("  Using interpolation: %s",
                                    dataset_has_synthetic_boxes ? "Yes" : "No");
                        ImGui::Text("  Method: %s",
                                    zarr_loader.getInterpolationMethod().c_str());
                    }
                } else {
                    if (zarr_loader.hasInterpolation() && frame_is_interpolated) {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    None (frame is interpolated)");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    None");
                    }
                }

                if (zarr_loader.hasKeypointData() || zarr_loader.hasHeadingData()) {
                    ImGui::Separator();
                    if (zarr_loader.hasKeypointData()) {
                        ImGui::Text("Keypoint Overlay:");
                        ImGui::Checkbox("Show keypoint markers", &show_keypoint_markers);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Overlay swim bladder and eye keypoints on the video frame.");
                        }
                        if (!zarr_loader.getKeypointsRunName().empty()) {
                            ImGui::Text("  Keypoints run: %s",
                                        zarr_loader.getKeypointsRunName().c_str());
                        }
                        if (detection_details.keypoints_per_detection > 0 &&
                            !detection_details.keypoint_labels.empty()) {
                            std::string label_list;
                            for (size_t i = 0; i < detection_details.keypoint_labels.size(); ++i) {
                                if (i > 0) {
                                    label_list += ", ";
                                }
                                label_list += detection_details.keypoint_labels[i];
                                if (label_list.size() > 72 &&
                                    i + 1 < detection_details.keypoint_labels.size()) {
                                    label_list += "...";
                                    break;
                                }
                            }
                            if (!label_list.empty()) {
                                ImGui::TextWrapped("  Labels: %s", label_list.c_str());
                            }
                        }
                        if (zarr_loader.activeDatasetHasSyntheticDetections()) {
                            ImGui::TextWrapped("Synthetic detections are present; interpolated boxes draw with hollow keypoint markers.");
                        }
                    }
                    if (zarr_loader.hasHeadingData()) {
                        if (zarr_loader.hasKeypointData()) {
                            ImGui::Spacing();
                        }
                        ImGui::Text("Heading Overlay:");
                        ImGui::Checkbox("Show heading arrows", &show_heading_arrows);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Visualize swim bladder headings from the keypoints run.");
                        }
                        if (!zarr_loader.getKeypointsRunName().empty() &&
                            !zarr_loader.hasKeypointData()) {
                            ImGui::Text("  Keypoints run: %s",
                                        zarr_loader.getKeypointsRunName().c_str());
                        }
                        if (zarr_loader.activeDatasetHasSyntheticDetections()) {
                            ImGui::TextWrapped("Synthetic detections are present; arrows render only for real boxes.");
                        }
                    }
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "[Zarr] Detections:    Not loaded");
            }

            if (zarr_loaded && zarr_loader.hasInterpolation()) {
                ImGui::Separator();
                ImGui::Text("Interpolation Status:");
                bool current_interpolated = zarr_loader.isFrameInterpolated(current_frame_num);
                ImGui::Text("  Current frame interpolated: %s", current_interpolated ? "Yes" : "No");
                ImGui::Text("  Dataset uses interpolation: %s",
                            zarr_loader.activeDatasetHasSyntheticDetections() ? "Yes" : "No");
                ImGui::Text("  Method: %s", zarr_loader.getInterpolationMethod().c_str());
            }

            if (zarr_loaded && zarr_loader.hasEyeMasks()) {
                ImGui::Separator();
                ImGui::Text("Eye Mask Overlay:");
                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_logged_toggle_disabled) {
                    eyeMaskDebugLog("Eye mask overlay toggle re-enabled; attempting to draw masks.");
                    eye_mask_debug_logged_toggle_disabled = false;
                }
                ImGui::Checkbox("Show refined eye masks", &show_eye_masks);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Visualize refined eye masks as semi-transparent overlays.");
                }
                if (!zarr_loader.getEyeMaskRunName().empty()) {
                    ImGui::Text("  Eye mask run: %s",
                                zarr_loader.getEyeMaskRunName().c_str());
                }
                if (zarr_loader.activeDatasetHasSyntheticDetections()) {
                    ImGui::TextWrapped("Synthetic detections are present; masks are skipped for interpolated boxes.");
                }
            }

            ImGui::End();
        }

        // file explorer display
        if (ImGuiFileDialog::Instance()->Display("ChooseMedia")) {
            if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
                auto selected_files =
                    ImGuiFileDialog::Instance()->GetSelection();
                root_dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                skeleton_dir = root_dir;

                // Try to load Zarr detection file
                std::string zarr_error;
                if (loadZarrDetectionFromDirectory(root_dir, zarr_loader, zarr_error)) {
                    zarr_loaded = true;
                    refreshDetectionDatasetOptions(zarr_loader);
                } else {
                    zarr_loaded = false;
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
                        FFmpegDemuxer *demuxer =
                            new FFmpegDemuxer(elem.second.c_str(), m);
                        demuxers.push_back(demuxer);
                    }
                    std::map<std::string, std::string> m;
                    FFmpegDemuxer dummy_dmuxer(
                        selected_files.begin()->second.c_str(), m);
                    dc_context->seek_interval =
                        (int)dummy_dmuxer
                            .FindKeyFrameInterval(); // get the seek interval
                    video_fps = dummy_dmuxer.GetFramerate();
                    scene->num_cams = selected_files.size();
                    scene->image_width =
                        (u32 *)malloc(sizeof(u32) * scene->num_cams);
                    scene->image_height =
                        (u32 *)malloc(sizeof(u32) * scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        scene->image_width[j] = demuxers[j]->GetWidth();
                        scene->image_height[j] = demuxers[j]->GetHeight();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    // multiple threads for decoding for selected videos
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &decoder_process, dc_context, demuxers[i],
                            camera_names[i], scene->display_buffer[i],
                            scene->size_of_buffer, &scene->seek_context[i],
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
                    scene->image_width =
                        (u32 *)malloc(sizeof(u32) * scene->num_cams);
                    scene->image_height =
                        (u32 *)malloc(sizeof(u32) * scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        std::string file_name = root_dir + "/" +
                                                camera_names[j] + "_" +
                                                imgs_names[0];
                        cv::Mat image = cv::imread(file_name, cv::IMREAD_COLOR);
                        scene->image_width[j] = image.cols;
                        scene->image_height[j] = image.rows;
                    }
                    if (imgs_names.size() < label_buffer_size) {
                        label_buffer_size = imgs_names.size();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &image_loader, dc_context, imgs_names,
                            scene->display_buffer[i], scene->size_of_buffer,
                            &scene->seek_context[i], scene->use_cpu_buffer,
                            camera_names[i], root_dir));
                        is_view_focused.push_back(false);
                    }
                    video_loaded = true;
                }

                // After loading video, load camera calibration
                if (video_loaded) {
                    camera_params.resize(scene->num_cams);
                    std::cout << "\n=== Loading Camera Calibrations from YAML ===" << std::endl;
                    for (size_t i = 0; i < camera_names.size(); ++i) {
                        std::cout << "\nProcessing camera " << i << ": " << camera_names[i] << std::endl;

                        // Load calibration from YAML file
                        std::string yaml_file = root_dir + "/calibration/" + camera_names[i] + ".yaml";
                        if (std::filesystem::exists(yaml_file)) {
                            std::cout << "Loading homography from YAML for camera: " << camera_names[i] << std::endl;
                            if (!camera_load_params_from_yaml(yaml_file, camera_params[i], error_message)) {
                                std::cerr << "Error: Failed to load calibration from YAML: " << error_message << std::endl;
                                show_error = true;
                                break;
                            } else {
                                camera_print_calibration_details(camera_params[i], camera_names[i]);
                            }
                        } else {
                            std::cerr << "Warning: No calibration YAML file found at: " << yaml_file << std::endl;
                        }
                    }
                }
            }
            // close
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
                                            skeleton, SP_LOAD);
                        plot_keypoints_flag = true;
                        keypoints_root_folder = root_dir + "/labeled_data/";
                        skeleton_chosen = true;
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

            ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Frames in the buffer")) {
                {
                    for (u32 i = 0; i < scene->size_of_buffer; i++) {
                        int seletable_frame_id =
                            (i + ps.read_head) % scene->size_of_buffer;
                        char label[32];
                        if (input_is_imgs) {
                            snprintf(label, sizeof(label), "%d: %s",
                                     scene
                                         ->display_buffer[visible_idx]
                                                         [seletable_frame_id]
                                         .frame_number,
                                     imgs_names[i].c_str());
                        } else {
                            sprintf(label, "Frame %d",
                                    scene
                                        ->display_buffer[visible_idx]
                                                        [seletable_frame_id]
                                        .frame_number);
                        }
                        if (ImGui::Selectable(label, ps.pause_selected == i)) {
                            // start from the lowest frame
                            ps.pause_selected = i;
                        }
                    }
                }

                if (ImGui::IsKeyPressed(ImGuiKey_Comma, true)) {
                    if (ps.pause_selected > 0) {
                        ps.pause_selected--;
                    }
                };

                if (ImGui::IsKeyPressed(ImGuiKey_Period, true)) {
                    if (ps.pause_selected < (scene->size_of_buffer - 1)) {
                        ps.pause_selected++;
                    }
                };
            }
            ImGui::End();
            select_corr_head =
                (ps.pause_selected + ps.read_head) % scene->size_of_buffer;
            current_frame_num =
                scene->display_buffer[visible_idx][select_corr_head]
                    .frame_number;
        }

        // Render a video frame
        if (video_loaded) {
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
                    seek_all_cameras(scene, current_frame_num, video_fps, ps,
                                     true);
                }

                if (!window_was_decoding[win_name] && is_visible &&
                    !ps.play_video && !ps.pause_seeked) {
                    // seek if visibility has changed
                    seek_all_cameras(scene, current_frame_num, video_fps, ps,
                                     true);
                    for (auto &[key, value] : window_need_decoding) {
                        value.store(true);
                    }
                }

                if (ps.play_video) {
                    window_need_decoding[win_name].store(is_visible);
                };

                if (is_visible) {
                    if (ps.play_video) {
                        // if the current frame is ready, upload for display,
                        // otherwise wait for the frame to get ready
                        // while (scene->display_buffer[j][ps.read_head]
                        //            .frame_number !=
                        //        ps.to_display_frame_number) {
                        //     std::cout
                        //         << win_name << " , read head: " <<
                        //         ps.read_head
                        //         << ", frame_number: "
                        //         << scene->display_buffer[j][ps.read_head]
                        //                .frame_number
                        //         << ", to_display_frame_number: "
                        //         << ps.to_display_frame_number << std::endl;
                        //     std::this_thread::sleep_for(
                        //         std::chrono::milliseconds(1));
                        // }

                        current_frame_num = ps.to_display_frame_number;
                        if (scene->use_cpu_buffer) {
                            // upload_texture(&scene->image_texture[j],
                            // scene->display_buffer[j][read_head].frame,
                            // scene->image_width[j], scene->image_height[j]);
                            // // 2x slower than pbo copy frame to cuda buffer
                            ck(cudaMemcpy(
                                scene->pbo_cuda[j].cuda_buffer,
                                scene->display_buffer[j][ps.read_head].frame,
                                scene->image_width[j] * scene->image_height[j] *
                                    4,
                                cudaMemcpyHostToDevice));
                        } else {
                            ck(cudaMemcpy(
                                scene->pbo_cuda[j].cuda_buffer,
                                scene->display_buffer[j][ps.read_head].frame,
                                scene->image_width[j] * scene->image_height[j] *
                                    4,
                                cudaMemcpyDeviceToDevice));
                        }
                    } else {
                        if (scene->use_cpu_buffer) {
                            // upload_texture(&scene->image_texture[j],
                            // scene->display_buffer[j][select_corr_head].frame,
                            // scene->image_width[j], scene->image_height[j]);
                            ck(cudaMemcpy(
                                scene->pbo_cuda[j].cuda_buffer,
                                scene->display_buffer[j][select_corr_head]
                                    .frame,
                                scene->image_width[j] * scene->image_height[j] *
                                    4,
                                cudaMemcpyHostToDevice));
                        } else {
                            ck(cudaMemcpy(
                                scene->pbo_cuda[j].cuda_buffer,
                                scene->display_buffer[j][select_corr_head]
                                    .frame,
                                scene->image_width[j] * scene->image_height[j] *
                                    4,
                                cudaMemcpyDeviceToDevice));
                        }
                    }
                    bind_pbo(&scene->pbo_cuda[j].pbo);
                    bind_texture(&scene->image_texture[j]);
                    upload_image_pbo_to_texture(scene->image_width[j],
                                                scene->image_height[j]);
                    unbind_pbo();
                    unbind_texture();

                    // sync yolo detection
                    if (yolo_detection) {
                        std::unique_lock<std::mutex> lck(g_mutexes[j]);
                        // std::cout << "main_thread: acquire lock" <<
                        // std::endl;
                        yolo_input_frames_rgba[j] =
                            scene->pbo_cuda[j].cuda_buffer;
                        g_ready[j] = true;
                        g_cvs[j].notify_one();
                    }

                    ImGui::BeginGroup();
                    std::string scene_name = "scene view" + std::to_string(j);
                    ImGui::BeginChild(
                        scene_name.c_str(),
                        ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
                    ImVec2 avail_size = ImGui::GetContentRegionAvail();

                    // ImGui::Image((void*)(intptr_t)image_texture[j],
                    // avail_size);
                    //
                    if (plot_keypoints_flag) {
                        if (keypoints_map.find(current_frame_num) ==
                            keypoints_map.end()) {
                            keypoints_find = false;
                        } else {
                            keypoints_find = true;
                        }
                    }

                    ImPlot::PushStyleVar(ImPlotStyleVar_LegendPadding, ImVec2(12.0f, 12.0f));
                    if (ImPlot::BeginPlot("##no_plot_name", avail_size,
                                          ImPlotFlags_Equal |
                                          ImPlotAxisFlags_AutoFit |
                                          ImPlotFlags_Crosshairs)) {
                        ImPlot::SetupLegend(ImPlotLocation_SouthWest, ImPlotLegendFlags_None);
                        ImPlot::PlotImage(
                            "##no_image_name",
                            (ImTextureID)(intptr_t)scene->image_texture[j],
                            ImVec2(0, 0),
                            ImVec2(scene->image_width[j],
                                scene->image_height[j]));

                        if (yolo_detection) {
                            draw_cv_contours(
                                yolo_boxes.at(j), yolo_labels.at(j),
                                yolo_classid.at(j), scene->image_height[j]);
                        }

                        // === ZARR BOUNDING BOX RENDERING === //
                        if (zarr_loaded) {
                            // Check interpolation status for this frame
                            bool is_zarr_interpolated = zarr_loader.hasInterpolation() &&
                                                        zarr_loader.isFrameInterpolated(current_frame_num);
                            
                            // Get bounding boxes from the active dataset
                            std::vector<LoggedBoundingBox> zarr_boxes =
                                zarr_loader.getBoundingBoxesForFrame(current_frame_num);
                            ZarrDetectionLoader::FrameDetections detection_details =
                                zarr_loader.getRawDetections(current_frame_num, false);
                            auto draw_keypoint_markers = [&]() {
                                if (!(show_keypoint_markers &&
                                      detection_details.has_keypoints &&
                                      !detection_details.keypoints_pixels.empty() &&
                                      detection_details.keypoints_per_detection > 0)) {
                                    return;
                                }

                                const size_t kp_per_det = detection_details.keypoints_per_detection;
                                std::vector<std::string> lowered_labels(kp_per_det);
                                for (size_t kp_idx = 0; kp_idx < kp_per_det; ++kp_idx) {
                                    if (kp_idx < detection_details.keypoint_labels.size()) {
                                        lowered_labels[kp_idx] = detection_details.keypoint_labels[kp_idx];
                                        std::transform(lowered_labels[kp_idx].begin(),
                                                       lowered_labels[kp_idx].end(),
                                                       lowered_labels[kp_idx].begin(),
                                                       [](unsigned char c) {
                                                           return static_cast<char>(std::tolower(c));
                                                       });
                                    } else {
                                        lowered_labels[kp_idx].clear();
                                    }
                                }

                                auto chooseColor = [&](size_t kp_idx) -> ImVec4 {
                                    const std::string& label = lowered_labels[kp_idx];
                                    if (label.find("swim") != std::string::npos ||
                                        label.find("bladder") != std::string::npos) {
                                        return ImVec4(1.0f, 0.85f, 0.15f, 1.0f);
                                    }
                                    if (label.find("left") != std::string::npos) {
                                        return ImVec4(0.3f, 0.95f, 0.4f, 1.0f);
                                    }
                                    if (label.find("right") != std::string::npos) {
                                        return ImVec4(0.75f, 0.4f, 0.95f, 1.0f);
                                    }
                                    static const ImVec4 fallback_colors[] = {
                                        ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                        ImVec4(0.35f, 0.85f, 0.55f, 1.0f),
                                        ImVec4(0.6f, 0.5f, 0.95f, 1.0f),
                                        ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
                                        ImVec4(0.4f, 0.75f, 0.95f, 1.0f)
                                    };
                                    return fallback_colors[kp_idx % (sizeof(fallback_colors) / sizeof(fallback_colors[0]))];
                                };

                                auto chooseMarker = [&](size_t kp_idx) -> ImPlotMarker {
                                    const std::string& label = lowered_labels[kp_idx];
                                    if (label.find("swim") != std::string::npos ||
                                        label.find("bladder") != std::string::npos) {
                                        return ImPlotMarker_Circle;
                                    }
                                    if (label.find("left") != std::string::npos) {
                                        return ImPlotMarker_Square;
                                    }
                                    if (label.find("right") != std::string::npos) {
                                        return ImPlotMarker_Diamond;
                                    }
                                    static const ImPlotMarker fallback_markers[] = {
                                        ImPlotMarker_Circle,
                                        ImPlotMarker_Square,
                                        ImPlotMarker_Diamond,
                                        ImPlotMarker_Cross,
                                        ImPlotMarker_Plus,
                                        ImPlotMarker_Up,
                                        ImPlotMarker_Down
                                    };
                                    return fallback_markers[kp_idx % (sizeof(fallback_markers) / sizeof(fallback_markers[0]))];
                                };

                                auto chooseSize = [&](size_t kp_idx) -> float {
                                    const std::string& label = lowered_labels[kp_idx];
                                    if (label.find("swim") != std::string::npos ||
                                        label.find("bladder") != std::string::npos) {
                                        return 4.5f;
                                    }
                                    if (label.find("left") != std::string::npos ||
                                        label.find("right") != std::string::npos) {
                                        return 4.5f;
                                    }
                                    return 7.0f;
                                };

                                size_t detection_count = std::min(detection_details.keypoints_pixels.size(),
                                                                  detection_details.boxes.size());
                                for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
                                    const auto& keypoints = detection_details.keypoints_pixels[det_idx];
                                    if (keypoints.size() != kp_per_det) {
                                        continue;
                                    }
                                    bool detection_is_interp = false;
                                    if (!detection_details.detection_source.empty() &&
                                        det_idx < detection_details.detection_source.size()) {
                                        detection_is_interp = detection_details.detection_source[det_idx] != 0;
                                    }
                                    uint8_t heading_valid_flag = 1;
                                    if (!detection_details.heading_valid.empty() &&
                                        det_idx < detection_details.heading_valid.size()) {
                                        heading_valid_flag = detection_details.heading_valid[det_idx];
                                    }

                                    for (size_t kp_idx = 0; kp_idx < kp_per_det; ++kp_idx) {
                                        const auto& kp = keypoints[kp_idx];
                                        float kp_x = kp[0];
                                        float kp_y = kp[1];
                                        if (!std::isfinite(kp_x) || !std::isfinite(kp_y)) {
                                            continue;
                                        }

                                        double plot_x = static_cast<double>(kp_x);
                                        double plot_y = static_cast<double>(scene->image_height[j]) -
                                                        static_cast<double>(kp_y);

                                        ImVec4 base_color = chooseColor(kp_idx);
                                        float alpha_scale = 1.0f;
                                        if (heading_valid_flag == 0) {
                                            alpha_scale *= 0.4f;
                                        }
                                        if (detection_is_interp) {
                                            alpha_scale *= 0.65f;
                                        }
                                        alpha_scale = std::clamp(alpha_scale, 0.25f, 1.0f);

                                        ImVec4 fill_color = base_color;
                                        fill_color.w *= alpha_scale;
                                        ImVec4 outline_color = base_color;
                                        outline_color.w = std::max(alpha_scale, 0.6f);

                                        ImPlot::SetNextMarkerStyle(chooseMarker(kp_idx),
                                                                   chooseSize(kp_idx),
                                                                   fill_color,
                                                                   2.0f,
                                                                   outline_color);
                                        std::string label = "##kp_" + std::to_string(det_idx) + "_" +
                                                            std::to_string(kp_idx);
                                        ImPlot::PlotScatter(label.c_str(), &plot_x, &plot_y, 1);
                                    }
                                }
                            };
                            
                            // DEBUG: Add this to see what's happening
                            if (!zarr_boxes.empty()) {
                                // std::cout << "Drawing " << zarr_boxes.size() << " zarr boxes for frame " 
                                //         << current_frame_num << " (interpolated=" << is_zarr_interpolated << ")" << std::endl;
                                // for (const auto& box : zarr_boxes) {
                                //     std::cout << "  Box: x_min=" << box.x_min << ", y_min=" << box.y_min 
                                //             << ", width=" << box.width << ", height=" << box.height 
                                //             << ", class_id=" << box.class_id << std::endl;
                                // }
                            } else {
                                // std::cout << "No zarr boxes to draw for frame " << current_frame_num << std::endl;
                            }
                            
                            // Draw the boxes
                            if (!zarr_boxes.empty()) {
                                for (size_t box_idx = 0; box_idx < zarr_boxes.size(); ++box_idx) {
                                    const auto& box = zarr_boxes[box_idx];
                                    double x_coords[5] = {
                                        box.x_min, 
                                        box.x_min + box.width, 
                                        box.x_min + box.width, 
                                        box.x_min, 
                                        box.x_min
                                    };
                                    
                                    double y_coords[5] = {
                                        (double)scene->image_height[j] - box.y_min,
                                        (double)scene->image_height[j] - box.y_min,
                                        (double)scene->image_height[j] - (box.y_min + box.height),
                                        (double)scene->image_height[j] - (box.y_min + box.height),
                                        (double)scene->image_height[j] - box.y_min
                                    };
                                    
                                    bool detection_is_interp = zarr_loader.activeDatasetHasSyntheticDetections();
                                    if (!detection_details.detection_source.empty()) {
                                        if (box_idx < detection_details.detection_source.size()) {
                                            detection_is_interp = detection_details.detection_source[box_idx] != 0;
                                        } else {
                                            detection_is_interp = false;
                                        }
                                    }

                                    ImVec4 box_color = detection_is_interp
                                                           ? ImVec4(1.0f, 0.7f, 0.0f, 0.9f)
                                                           : ImVec4(0.2f, 0.6f, 1.0f, 1.0f);
                                    float line_width = detection_is_interp ? 2.5f : 2.0f;

                                    if (is_zarr_interpolated && zarr_loader.activeDatasetHasSyntheticDetections() &&
                                        detection_details.detection_source.empty()) {
                                        box_color = ImVec4(1.0f, 0.7f, 0.0f, 0.9f);
                                        line_width = 2.5f;
                                    }
                                    
                                    ImPlot::SetNextLineStyle(box_color, line_width);
                                    
                                    std::string label = "Zarr_" + std::to_string(box.class_id);
                                    if (detection_is_interp) {
                                        label += " [I]";  // Mark as interpolated
                                    }
                                    
                                    ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);
                                }
                            }

                            // Draw chaser bounding boxes and target positions
                            if (zarr_loaded) {
                                auto chaser_bboxes = zarr_loader.getChaserBoundingBoxesForFrame(current_frame_num);
                                auto chaser_states = zarr_loader.getChaserStatesForFrame(current_frame_num);

                                #if defined(CRIMSON_CHASER_DEBUG_LOGS)
                                // Debug: Print what we found
                                static bool debug_printed = false;
                                static int frames_with_data = 0;
                                if (chaser_bboxes.size() > 0 || chaser_states.size() > 0) {
                                    frames_with_data++;
                                    if (!debug_printed) {
                                        std::cout << "\n=== CHASER DATA DEBUG ===" << std::endl;
                                        std::cout << "Camera frame " << current_frame_num << ": Found " << chaser_bboxes.size()
                                                  << " chaser bboxes, " << chaser_states.size() << " chaser states" << std::endl;

                                        if (chaser_bboxes.size() > 0) {
                                            std::cout << "  First bbox: fish_id=" << chaser_bboxes[0].fish_id
                                                      << ", x=" << chaser_bboxes[0].x_px << ", y=" << chaser_bboxes[0].y_px
                                                      << ", w=" << chaser_bboxes[0].width_px << ", h=" << chaser_bboxes[0].height_px << std::endl;
                                        }

                                        if (chaser_states.size() > 0) {
                                            std::cout << "  First state: stimulus_frame=" << chaser_states[0].stimulus_frame_num
                                                      << ", camera_frame=" << chaser_states[0].camera_frame_id << std::endl;
                                            std::cout << "    chaser=(" << chaser_states[0].chaser_pos_x << "," << chaser_states[0].chaser_pos_y << ")"
                                                      << " target=(" << chaser_states[0].target_pos_x << "," << chaser_states[0].target_pos_y << ")" << std::endl;
                                            std::cout << "  Camera params: has_homography=" << camera_params[j].has_valid_homography
                                                      << ", offsetX=" << camera_params[j].stimulus_offset_x
                                                      << ", offsetY=" << camera_params[j].stimulus_offset_y << std::endl;
                                        }
                                        debug_printed = true;
                                    }
                                }

                                // Print summary after a while
                                static int last_frame_checked = -1;
                                if (current_frame_num > last_frame_checked + 1000) {
                                    std::cout << "Frames " << (last_frame_checked + 1) << "-" << current_frame_num
                                              << ": " << frames_with_data << " frames had chaser data" << std::endl;
                                    frames_with_data = 0;
                                    last_frame_checked = current_frame_num;
                                }
                                #endif

struct StateOverlay {
                                   int chaser_index = -1;
                                   double target_plot_x = 0.0;
                                   double target_plot_y = 0.0;
                                   double target_world_x = std::numeric_limits<double>::quiet_NaN();
                                   double target_world_y = std::numeric_limits<double>::quiet_NaN();
                                   double chaser_plot_x = 0.0;
                                   double chaser_plot_y = 0.0;
                                   double chaser_world_x = std::numeric_limits<double>::quiet_NaN();
                                   double chaser_world_y = std::numeric_limits<double>::quiet_NaN();
                                   size_t target_bbox_index = std::numeric_limits<size_t>::max();
                                   bool has_target = false;
                                   bool has_chaser = false;
                               };

                                std::vector<uint8_t> target_bbox_usage(chaser_bboxes.size(), 0);
                                std::vector<StateOverlay> state_overlays;
                                state_overlays.reserve(chaser_states.size());

                                const size_t kInvalidBBoxIndex = std::numeric_limits<size_t>::max();

                                auto selectBoundingBoxForTarget = [&](double cam_x, double cam_y) -> size_t {
                                    auto choose = [&](auto predicate) -> size_t {
                                        double best_distance = std::numeric_limits<double>::infinity();
                                        size_t best_index = kInvalidBBoxIndex;
                                        for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
                                            const auto& box = chaser_bboxes[idx];
                                            if (!predicate(box)) {
                                                continue;
                                            }
                                            if (!std::isfinite(box.centroid_x) || !std::isfinite(box.centroid_y)) {
                                                continue;
                                            }
                                            double dx = cam_x - static_cast<double>(box.centroid_x);
                                            double dy = cam_y - static_cast<double>(box.centroid_y);
                                            double dist_sq = dx * dx + dy * dy;
                                            if (dist_sq < best_distance) {
                                                best_distance = dist_sq;
                                                best_index = idx;
                                            }
                                        }
                                        return best_index;
                                    };

                                    size_t idx = choose([](const auto& box) { return box.is_target; });
                                    if (idx != kInvalidBBoxIndex) {
                                        return idx;
                                    }
                                    idx = choose([](const auto& box) { return box.fish_id < 0; });
                                    if (idx != kInvalidBBoxIndex) {
                                        return idx;
                                    }
                                    return choose([](const auto&) { return true; });
                                };

                                if (!chaser_states.empty()) {
                                    auto projectStimulusToCamera = [&](const ZarrDetectionLoader::ChaserState& state,
                                                                       float stim_x, float stim_y,
                                                                       bool is_target,
                                                                       double& out_x, double& out_y) -> bool {
                                        if (state.has_camera_coords) {
                                            double cx = is_target ? state.target_camera_x : state.chaser_camera_x;
                                            double cy = is_target ? state.target_camera_y : state.chaser_camera_y;
                                            if (!std::isfinite(cx) || !std::isfinite(cy)) {
                                                return false;
                                            }
                                            out_x = cx;
                                            out_y = static_cast<double>(scene->image_height[j]) - cy;
                                            return true;
                                        }

                                        if (!std::isfinite(stim_x) || !std::isfinite(stim_y)) {
                                            return false;
                                        }

                                        float offsetX = camera_params[j].stimulus_offset_x;
                                        float offsetY = camera_params[j].stimulus_offset_y;

                                        if (camera_params[j].has_valid_homography) {
                                            std::vector<cv::Point2f> src_points(1);
                                            std::vector<cv::Point2f> dst_points;
                                            src_points[0] = cv::Point2f(stim_x + offsetX, stim_y + offsetY);
                                            cv::perspectiveTransform(src_points, dst_points, camera_params[j].inverse_homography_matrix);
                                            if (dst_points.empty()) {
                                                return false;
                                            }
                                            out_x = dst_points[0].x;
                                            out_y = static_cast<double>(scene->image_height[j]) - dst_points[0].y;
                                            return true;
                                        }

                                        constexpr double kProjectorExtent = 358.0;
                                        double projector_w = kProjectorExtent;
                                        double projector_h = kProjectorExtent;
                                        double tex_x = stim_x + offsetX;
                                        double tex_y = stim_y + offsetY;
                                        out_x = (tex_x / projector_w) * static_cast<double>(scene->image_width[j]);
                                        double y = (tex_y / projector_h) * static_cast<double>(scene->image_height[j]);
                                        out_y = static_cast<double>(scene->image_height[j]) - y;
                                        return true;
                                    };

                                    for (const auto& state : chaser_states) {
                                        StateOverlay overlay;
                                        overlay.chaser_index = state.chaser_index;

                                        double target_plot_x = 0.0;
                                        double target_plot_y = 0.0;
                                        if (projectStimulusToCamera(state,
                                                                    state.target_pos_x,
                                                                    state.target_pos_y,
                                                                    true,
                                                                    target_plot_x,
                                                                    target_plot_y)) {
                                            overlay.has_target = true;
                                            double target_cam_x = target_plot_x;
                                            double target_cam_y = static_cast<double>(scene->image_height[j]) - target_plot_y;
                                            size_t bbox_idx = chaser_bboxes.empty()
                                                                 ? kInvalidBBoxIndex
                                                                 : selectBoundingBoxForTarget(target_cam_x, target_cam_y);
                                            if (bbox_idx != kInvalidBBoxIndex) {
                                                auto& bbox = chaser_bboxes[bbox_idx];
                                                overlay.target_plot_x = static_cast<double>(bbox.centroid_x);
                                                overlay.target_plot_y =
                                                    static_cast<double>(scene->image_height[j]) - static_cast<double>(bbox.centroid_y);
                                                overlay.target_bbox_index = bbox_idx;
                                                target_bbox_usage[bbox_idx] = 1;
                                                bbox.is_target = true;
                                                if (bbox.chaser_index < 0 && state.chaser_index >= 0) {
                                                    bbox.chaser_index = state.chaser_index;
                                                }
                                                overlay.target_world_x = bbox.centroid_x;
                                                overlay.target_world_y = static_cast<double>(scene->image_height[j]) - bbox.centroid_y;
                                            } else {
                                                overlay.target_plot_x = target_plot_x;
                                                overlay.target_plot_y = target_plot_y;
                                                overlay.target_world_x = target_plot_x;
                                                overlay.target_world_y = target_plot_y;
                                            }
                                        }

                                        double chaser_plot_x = 0.0;
                                        double chaser_plot_y = 0.0;
                                        if (projectStimulusToCamera(state,
                                                                    state.chaser_pos_x,
                                                                    state.chaser_pos_y,
                                                                    false,
                                                                    chaser_plot_x,
                                                                    chaser_plot_y)) {
                                            overlay.has_chaser = true;
                                            overlay.chaser_plot_x = chaser_plot_x;
                                            overlay.chaser_plot_y = chaser_plot_y;
                                            overlay.chaser_world_x = chaser_plot_x;
                                            overlay.chaser_world_y = chaser_plot_y;
                                        }

                                        state_overlays.push_back(overlay);
                                    }
                                }

                                for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
                                    const auto& bbox = chaser_bboxes[idx];
                                    if (!std::isfinite(bbox.x_px) || !std::isfinite(bbox.y_px) ||
                                        !std::isfinite(bbox.width_px) || !std::isfinite(bbox.height_px)) {
                                        continue;
                                    }

                                    bool highlight_target =
                                        bbox.is_target ||
                                        (idx < target_bbox_usage.size() && target_bbox_usage[idx] != 0);

                                    ImVec4 box_color = highlight_target
                                                           ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                                                           : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                                    float line_width = highlight_target ? 2.75f : 2.0f;

                                    double x0 = static_cast<double>(bbox.x_px);
                                    double x1 = static_cast<double>(bbox.x_px + bbox.width_px);
                                    double y0 = static_cast<double>(bbox.y_px);
                                    double y1 = static_cast<double>(bbox.y_px + bbox.height_px);

                                    double x_coords[5] = {x0, x1, x1, x0, x0};
                                    double y_coords[5] = {
                                        static_cast<double>(scene->image_height[j]) - y0,
                                        static_cast<double>(scene->image_height[j]) - y0,
                                        static_cast<double>(scene->image_height[j]) - y1,
                                        static_cast<double>(scene->image_height[j]) - y1,
                                        static_cast<double>(scene->image_height[j]) - y0};

                                    ImPlot::SetNextLineStyle(box_color, line_width);
                                    auto format_label_id = [&](int32_t candidate, size_t fallback) -> int32_t {
                                        if (candidate >= 0) {
                                            return candidate;
                                        }
                                        if (bbox.fish_id >= 0) {
                                            return bbox.fish_id;
                                        }
                                        return static_cast<int32_t>(fallback);
                                    };
                                    int32_t label_id = format_label_id(bbox.chaser_index, idx);
                                    std::string label = highlight_target
                                                            ? "Target BBox " + std::to_string(label_id) +
                                                                  "##target_bbox_" + std::to_string(idx)
                                                            : "Chaser BBox " + std::to_string(label_id) +
                                                                  "##chaser_bbox_" + std::to_string(idx);
                                    ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);

                                    if (!highlight_target) {
                                        if (std::isfinite(bbox.centroid_x) && std::isfinite(bbox.centroid_y)) {
                                            double centroid_x = static_cast<double>(bbox.centroid_x);
                                            double centroid_y =
                                                static_cast<double>(scene->image_height[j]) - static_cast<double>(bbox.centroid_y);
                                            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                                                       5.0f,
                                                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                                                       IMPLOT_AUTO,
                                                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
                                            std::string centroid_label = highlight_target
                                                                            ? "Target BBox " + std::to_string(label_id) + " Centroid##target_centroid_" + std::to_string(idx)
                                                                            : "Chaser BBox " + std::to_string(label_id) + " Centroid##chaser_centroid_" + std::to_string(idx);
                                            ImPlot::PlotScatter(centroid_label.c_str(), &centroid_x, &centroid_y, 1);
                                        }
                                    }
                                }

                                for (const auto& overlay : state_overlays) {
                                    if (overlay.has_target) {
                                        double plot_x = overlay.target_plot_x;
                                        double plot_y = overlay.target_plot_y;
                                        ImVec4 target_color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                                        float outline = overlay.target_bbox_index != kInvalidBBoxIndex ? 2.5f : 2.0f;
                                        float target_marker_size = 3.0f;
                                        float target_outline = outline * 0.25f;
                                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                                                   target_marker_size,
                                                                   target_color,
                                                                   target_outline,
                                                                   target_color);
                                        std::string target_label = "Target_" + std::to_string(overlay.chaser_index);
                                        ImPlot::PlotScatter(target_label.c_str(), &plot_x, &plot_y, 1);
                                    }

                                    if (overlay.has_chaser && overlay.has_target) {
                                        double line_x[2] = {overlay.chaser_plot_x, overlay.target_plot_x};
                                        double line_y[2] = {overlay.chaser_plot_y, overlay.target_plot_y};
                                        double dx = overlay.target_plot_x - overlay.chaser_plot_x;
                                        double dy = overlay.target_plot_y - overlay.chaser_plot_y;
                                        double dist = std::sqrt(dx * dx + dy * dy);
                                        double max_dim = static_cast<double>(std::max(scene->image_width[j], scene->image_height[j]));
                                        double max_dist = (max_dim > 0.0) ? (max_dim * (2.0 / 3.0)) : 200.0;
                                        double t = std::clamp(dist / max_dist, 0.0, 1.0);
                                        ImVec4 close_color(1.0f, 0.15f, 0.1f, 1.0f);
                                        ImVec4 far_color(0.15f, 0.9f, 0.2f, 1.0f);
                                        ImVec4 line_color(
                                            close_color.x * static_cast<float>(1.0 - t) + far_color.x * static_cast<float>(t),
                                            close_color.y * static_cast<float>(1.0 - t) + far_color.y * static_cast<float>(t),
                                            close_color.z * static_cast<float>(1.0 - t) + far_color.z * static_cast<float>(t),
                                            1.0f);
                                        ImPlot::SetNextLineStyle(line_color, 2.5f);
                                        std::string line_label = "ChaserTargetLine##" +
                                                                 std::to_string(reinterpret_cast<uintptr_t>(&overlay));
                                        ImPlot::PlotLine(line_label.c_str(), line_x, line_y, 2);
                                    }

                                    if (overlay.has_chaser) {
                                        double plot_x = overlay.chaser_plot_x;
                                        double plot_y = overlay.chaser_plot_y;
                                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                                                   8.0f,
                                                                   ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                                                   2.0f,
                                                                   ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
                                        std::string chaser_label = "Chaser_state_" + std::to_string(overlay.chaser_index);
                                        ImPlot::PlotScatter(chaser_label.c_str(), &plot_x, &plot_y, 1);
                                    }
                                }

                                }

                            const bool heading_overlay_enabled = show_heading_arrows;
                            const bool heading_data_available = zarr_loader.hasHeadingData();
                            const bool eye_mask_overlay_enabled = show_eye_masks;
                            const bool eye_mask_data_available = zarr_loader.hasEyeMasks();
                            const bool can_draw_headings =
                                heading_overlay_enabled && heading_data_available;
                            const bool can_draw_eye_masks =
                                eye_mask_overlay_enabled && eye_mask_data_available;
                            const float scene_height_f =
                                static_cast<float>(scene->image_height[j]);

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

                                    if (zarr_loader.activeDatasetHasSyntheticDetections()) {
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

                                if (!zarr_loader.hasEyeMasks()) {
                                    if (!eye_mask_debug_logged_no_data) {
                                        eyeMaskDebugLog("Zarr loader reports no eye mask data.");
                                        eye_mask_debug_logged_no_data = true;
                                    }
                                } else if (eye_mask_debug_logged_no_data) {
                                    eyeMaskDebugLog("Eye mask data detected; masks may render.");
                                    eye_mask_debug_logged_no_data = false;
                                }
                            }

                            if (can_draw_headings) {
                                if (kHeadingDebugLoggingEnabled &&
                                    heading_debug_entry_log_count < 200 &&
                                    heading_debug_last_frame_logged != current_frame_num) {
                                    headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                    ": entering heading draw path (overlay_enabled=" +
                                                    (heading_overlay_enabled ? "1" : "0") +
                                                    ", heading_data=" +
                                                    (heading_data_available ? "1" : "0") +
                                                    ", dataset_interp=" +
                                                    (zarr_loader.activeDatasetHasSyntheticDetections() ? "1" : "0") + ").");
                                    heading_debug_last_frame_logged = current_frame_num;
                                    heading_debug_entry_log_count++;
                                }

                                ZarrDetectionLoader::FrameDetections heading_details =
                                    zarr_loader.getRawDetections(
                                        current_frame_num,
                                        /*use_interpolated=*/false,
                                        /*include_eye_masks=*/false);

                                const size_t det_count = heading_details.boxes.size();
                                const size_t valid_count = heading_details.heading_valid.size();
                                const size_t heading_count = heading_details.headings_deg.size();
                                const size_t swim_bladder_count = heading_details.swim_bladder_pixels.size();

                                if (det_count > 0 &&
                                    valid_count == det_count &&
                                    heading_count == det_count) {
                                    if (kHeadingDebugLoggingEnabled) {
                                        headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                        ": processing " + std::to_string(det_count) +
                                                        " detections (heading_valid=" + std::to_string(valid_count) +
                                                        ", headings_deg=" + std::to_string(heading_count) +
                                                        ", swim_bladder=" + std::to_string(swim_bladder_count) + ").");
                                    }

                                    ImDrawList* plot_draw_list = ImPlot::GetPlotDrawList();
                                    const ImU32 arrow_color =
                                        ImGui::GetColorU32(ImVec4(1.0f, 0.25f, 0.1f, 0.95f));
                                    const float arrow_thickness = 2.0f;

                                    for (size_t det_idx = 0; det_idx < det_count; ++det_idx) {
                                        if (heading_details.heading_valid[det_idx] == 0) {
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " skipped (heading_valid == 0).");
                                                heading_debug_draw_log_count++;
                                            }
                                            continue;
                                        }

                                        if (!heading_details.detection_source.empty() &&
                                            det_idx < heading_details.detection_source.size() &&
                                            heading_details.detection_source[det_idx] != 0) {
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " skipped (synthetic detection). ");
                                                heading_debug_draw_log_count++;
                                            }
                                            continue;
                                        }

                                        const auto& box = heading_details.boxes[det_idx];
                                        if (det_idx >= heading_details.swim_bladder_pixels.size()) {
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " lacks swim bladder pixel data (available=" +
                                                                std::to_string(swim_bladder_count) + ").");
                                                heading_debug_draw_log_count++;
                                            }
                                            continue;
                                        }

                                        float box_width = std::max(0.0f, box[2] - box[0]);
                                        float box_height = std::max(0.0f, box[3] - box[1]);
                                        float base_x = heading_details.swim_bladder_pixels[det_idx][0];
                                        float base_y = heading_details.swim_bladder_pixels[det_idx][1];

                                        if (!std::isfinite(base_x) || !std::isfinite(base_y)) {
                                            float fallback_x = 0.5f * (box[0] + box[2]);
                                            float fallback_y = 0.5f * (box[1] + box[3]);
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 120) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " had invalid swim bladder coords (" +
                                                                std::to_string(base_x) + ", " +
                                                                std::to_string(base_y) +
                                                                "); using bounding box center (" +
                                                                std::to_string(fallback_x) + ", " +
                                                                std::to_string(fallback_y) + ").");
                                                heading_debug_draw_log_count++;
                                            }
                                            base_x = fallback_x;
                                            base_y = fallback_y;
                                        }

                                        if (kHeadingDebugLoggingEnabled &&
                                            heading_debug_draw_log_count < 120) {
                                            headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                            ": detection " + std::to_string(det_idx) +
                                                            " bbox=[" + std::to_string(box[0]) + ", " +
                                                            std::to_string(box[1]) + ", " +
                                                            std::to_string(box[2]) + ", " +
                                                            std::to_string(box[3]) + "] base=(" +
                                                            std::to_string(base_x) + ", " +
                                                            std::to_string(base_y) + ").");
                                            heading_debug_draw_log_count++;
                                        }

                                        float heading_deg = heading_details.headings_deg[det_idx];
                                        float heading_rad =
                                            heading_deg * static_cast<float>(M_PI) / 180.0f;

                                        float bbox_scale =
                                            std::max(box_width, box_height) * 1.25f;
                                        float frame_scale = scene_height_f * 0.02f;
                                        float arrow_len =
                                            std::max(60.0f, std::max(bbox_scale, frame_scale));
                                        float end_x = base_x + std::cos(heading_rad) * arrow_len;
                                        float end_y = base_y - std::sin(heading_rad) * arrow_len;

                                        if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                            headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                            ": drawing heading arrow det " +
                                                            std::to_string(det_idx) + " base=(" +
                                                            std::to_string(base_x) + ", " +
                                                            std::to_string(base_y) + ") heading_deg=" +
                                                            std::to_string(heading_deg) + " arrow_len=" +
                                                            std::to_string(arrow_len) + " end=(" +
                                                            std::to_string(end_x) + ", " +
                                                            std::to_string(end_y) + ").");
                                            heading_debug_draw_log_count++;
                                        }

                                        ImPlotPoint plot_start(base_x, scene_height_f - base_y);
                                        ImPlotPoint plot_end(end_x, scene_height_f - end_y);
                                        ImVec2 p0 = ImPlot::PlotToPixels(plot_start);
                                        ImVec2 p1 = ImPlot::PlotToPixels(plot_end);

                                        plot_draw_list->AddLine(p0, p1, arrow_color, arrow_thickness);

                                        ImVec2 dir = ImVec2(p0.x - p1.x, p0.y - p1.y);
                                        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                                        if (len > 1e-3f) {
                                            dir.x /= len;
                                            dir.y /= len;
                                            float head_size = 8.0f;
                                            ImVec2 left = ImVec2(
                                                p1.x + dir.x * head_size + dir.y * head_size * 0.5f,
                                                p1.y + dir.y * head_size - dir.x * head_size * 0.5f);
                                            ImVec2 right = ImVec2(
                                                p1.x + dir.x * head_size - dir.y * head_size * 0.5f,
                                                p1.y + dir.y * head_size + dir.x * head_size * 0.5f);
                                            plot_draw_list->AddTriangleFilled(p1, left, right, arrow_color);
                                        }
                                    }
                                } else if (kHeadingDebugLoggingEnabled && can_draw_headings) {
                                    headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                    ": heading data mismatch (boxes=" + std::to_string(det_count) +
                                                    ", heading_valid=" + std::to_string(valid_count) +
                                                    ", headings_deg=" + std::to_string(heading_count) +
                                                    ", swim_bladder=" + std::to_string(swim_bladder_count) + ").");
                                }
                            }

                            if (can_draw_eye_masks) {
                                g_eye_orientation_smoother.resetIfRunChanged(
                                    zarr_loader.getEyeMaskRunName() + "|" +
                                    zarr_loader.getEyeAngleRunName());
                                ZarrDetectionLoader::FrameDetections mask_details =
                                    zarr_loader.getRawDetections(
                                        current_frame_num,
                                        /*use_interpolated=*/false,
                                        /*include_eye_masks=*/true);
                                if (mask_details.includes_eye_masks) {
                                    size_t mask_count =
                                        std::min(mask_details.eye_masks.size(),
                                                 mask_details.boxes.size());
                                    if (mask_count > 0) {
                                        if (kEyeMaskDebugLoggingEnabled &&
                                            eye_mask_debug_entry_log_count < 200 &&
                                            eye_mask_debug_last_frame_logged != current_frame_num) {
                                            eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                            ": entering eye mask draw path (masks=" +
                                                            std::to_string(mask_count) + ").");
                                            eye_mask_debug_last_frame_logged = current_frame_num;
                                            eye_mask_debug_entry_log_count++;
                                        }
                                        for (size_t det_idx = 0; det_idx < mask_count; ++det_idx) {
                                            const auto& mask_info = mask_details.eye_masks[det_idx];
                                            if (!mask_details.detection_source.empty() &&
                                                det_idx < mask_details.detection_source.size() &&
                                                mask_details.detection_source[det_idx] != 0) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " marked synthetic; skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (!mask_info.valid) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " marked invalid; skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (!std::isfinite(mask_info.offset_x) ||
                                                !std::isfinite(mask_info.offset_y)) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has non-finite offsets; skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (mask_info.roi_width <= 0.0f || mask_info.roi_height <= 0.0f) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has invalid ROI size (" +
                                                                    std::to_string(mask_info.roi_width) + "x" +
                                                                    std::to_string(mask_info.roi_height) + "); skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (mask_info.rows <= 0 || mask_info.cols <= 0) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has invalid matrix dimensions (" +
                                                                    std::to_string(mask_info.rows) + "x" +
                                                                    std::to_string(mask_info.cols) + "); skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (mask_info.rows <= 0 || mask_info.cols <= 0) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has invalid matrix dimensions (" +
                                                                    std::to_string(mask_info.rows) + "x" +
                                                                    std::to_string(mask_info.cols) + "); skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            double cell_w = mask_info.roi_width / static_cast<double>(mask_info.cols);
                                            double cell_h = mask_info.roi_height / static_cast<double>(mask_info.rows);
                                            for (int eye = 0; eye < 2; ++eye) {
                                                const auto& pixel_indices = mask_info.pixel_indices[eye];
                                                bool has_pixels = !pixel_indices.empty();
                                                if (!has_pixels) {
                                                    if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                        eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                        ": mask entry " + std::to_string(det_idx) +
                                                                        " eye " + std::to_string(eye) +
                                                                        " has no non-zero pixels.");
                                                        eye_mask_debug_draw_log_count++;
                                                    }
                                                } else if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": drawing eye mask det " + std::to_string(det_idx) +
                                                                    " eye=" + std::to_string(eye) +
                                                                    " offset=(" + std::to_string(mask_info.offset_x) + ", " +
                                                                    std::to_string(mask_info.offset_y) + ") size=(" +
                                                                    std::to_string(mask_info.roi_width) + ", " +
                                                                    std::to_string(mask_info.roi_height) + ").");
                                                    eye_mask_debug_draw_log_count++;
                                                }

                                                std::string base_id = (eye == 0)
                                                                          ? "##eye_mask_left_" + std::to_string(det_idx)
                                                                          : "##eye_mask_right_" + std::to_string(det_idx);
                                                ImVec4 base_color = (eye == 0)
                                                                        ? ImVec4(0.2f, 0.6f, 1.0f, 0.35f)
                                                                        : ImVec4(1.0f, 0.3f, 0.6f, 0.35f);

                                                std::vector<double> xs;
                                                std::vector<double> ys;
                                                if (has_pixels) {
                                                    xs.reserve(pixel_indices.size());
                                                    ys.reserve(pixel_indices.size());
                                                    for (uint16_t linear : pixel_indices) {
                                                        uint16_t row = linear / static_cast<uint16_t>(mask_info.cols);
                                                        uint16_t col = linear % static_cast<uint16_t>(mask_info.cols);
                                                        double px = mask_info.offset_x +
                                                                    (static_cast<double>(col) + 0.5) * cell_w;
                                                        double py = mask_info.offset_y +
                                                                    (static_cast<double>(row) + 0.5) * cell_h;
                                                        xs.push_back(px);
                                                        ys.push_back(scene_height_f - py);
                                                    }
                                                    if (!xs.empty()) {
                                                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, base_color, 1.0f,
                                                                                    base_color);
                                                        ImPlot::PlotScatter((base_id + "_pts").c_str(), xs.data(), ys.data(),
                                                                            static_cast<int>(xs.size()));
                                                    }
                                                }

                                                if (mask_info.has_feret_axes && cell_w > 0.0 && cell_h > 0.0) {
                                                    auto roiToWorld = [&](float roi_x, float roi_y) -> std::pair<double, double> {
                                                        double px = mask_info.offset_x +
                                                                    static_cast<double>(roi_x) * cell_w;
                                                        double py = mask_info.offset_y +
                                                                    static_cast<double>(roi_y) * cell_h;
                                                        return {px, py};
                                                    };
                                                    auto worldToScene = [&](double world_x, double world_y) -> std::pair<double, double> {
                                                        return {world_x, scene_height_f - world_y};
                                                    };
                                                    auto roiToScene = [&](float roi_x, float roi_y) -> std::pair<double, double> {
                                                        auto world = roiToWorld(roi_x, roi_y);
                                                        return worldToScene(world.first, world.second);
                                                    };
                                                    auto draw_axis =
                                                        [&](const ZarrDetectionLoader::FrameDetections::EyeMask::AxisSegment& axis,
                                                            const std::string& label, const ImVec4& color, float thickness) {
                                                            if (!axis.valid) {
                                                                return;
                                                            }
                                                            auto p0 = roiToScene(axis.x0, axis.y0);
                                                            auto p1 = roiToScene(axis.x1, axis.y1);
                                                            double x_vals[2] = {p0.first, p1.first};
                                                            double y_vals[2] = {p0.second, p1.second};
                                                            ImPlot::SetNextLineStyle(color, thickness);
                                                            ImPlot::PlotLine(label.c_str(), x_vals, y_vals, 2);
                                                        };

                                                    ImVec4 major_color = base_color;
                                                    major_color.w = 0.9f;
                                                        ImVec4 minor_color = base_color;
                                                        minor_color.x = std::min(1.0f, minor_color.x + 0.15f);
                                                        minor_color.y = std::min(1.0f, minor_color.y + 0.15f);
                                                        minor_color.z = std::min(1.0f, minor_color.z + 0.15f);
                                                        minor_color.w = 0.75f;

                                                    draw_axis(mask_info.feret_major[eye],
                                                              base_id + "_feret_major",
                                                              major_color,
                                                              2.5f);
                                                    draw_axis(mask_info.feret_minor[eye],
                                                              base_id + "_feret_minor",
                                                              minor_color,
                                                              1.8f);

                                                    const auto& minor_axis = mask_info.feret_minor[eye];
                                                    if (minor_axis.valid) {
                                                        auto endpoint0_world = roiToWorld(minor_axis.x0, minor_axis.y0);
                                                        auto endpoint1_world = roiToWorld(minor_axis.x1, minor_axis.y1);
                                                        auto center_world = roiToWorld(
                                                            0.5f * (minor_axis.x0 + minor_axis.x1),
                                                            0.5f * (minor_axis.y0 + minor_axis.y1));
                                                        if (mask_info.has_eye_angles && mask_info.feret_angle_valid[eye]) {
                                                            auto center_scene = worldToScene(center_world.first, center_world.second);
                                                            char angle_label[32];
                                                            std::snprintf(angle_label, sizeof(angle_label), "%+.1f°",
                                                                          mask_info.feret_minor_angle_deg[eye]);
                                                            ImPlot::PlotText(angle_label,
                                                                             center_scene.first,
                                                                             center_scene.second,
                                                                             ImVec2(0.0f, -12.0f));
                                                        }

                                                        double det_center_x =
                                                            0.5 * (mask_details.boxes[det_idx][0] + mask_details.boxes[det_idx][2]);
                                                        double det_center_y =
                                                            0.5 * (mask_details.boxes[det_idx][1] + mask_details.boxes[det_idx][3]);

                                                        auto squaredDistance = [](double ax, double ay, double bx, double by) -> double {
                                                            double dx = ax - bx;
                                                            double dy = ay - by;
                                                            return dx * dx + dy * dy;
                                                        };

                                                        double dist0 = squaredDistance(endpoint0_world.first, endpoint0_world.second,
                                                                                       det_center_x, det_center_y);
                                                        double dist1 = squaredDistance(endpoint1_world.first, endpoint1_world.second,
                                                                                       det_center_x, det_center_y);
                                                        auto outward_endpoint = (dist0 >= dist1) ? endpoint0_world : endpoint1_world;

                                                        ImVec2 dir_world = ImVec2(
                                                            static_cast<float>(outward_endpoint.first - center_world.first),
                                                            static_cast<float>(outward_endpoint.second - center_world.second));
                                                        float dir_len = std::sqrt(dir_world.x * dir_world.x +
                                                                                  dir_world.y * dir_world.y);
                                                        if (dir_len > 1e-3f) {
                                                            dir_world.x /= dir_len;
                                                            dir_world.y /= dir_len;

                                                            ImVec2 smoothed_dir =
                                                                g_eye_orientation_smoother.smoothDirection(
                                                                    mask_info.roi_index, eye, dir_world);
                                                            float smooth_len = std::sqrt(smoothed_dir.x * smoothed_dir.x +
                                                                                          smoothed_dir.y * smoothed_dir.y);
                                                            if (smooth_len > 1e-3f) {
                                                                smoothed_dir.x /= smooth_len;
                                                                smoothed_dir.y /= smooth_len;
                                                                dir_world = smoothed_dir;
                                                            }

                                                            float roi_span = std::max(mask_info.roi_width, mask_info.roi_height);
                                                            float beam_length = std::max(roi_span * 3.5f, 80.0f);
                                                            float beam_width = std::max(roi_span * 0.75f, 25.0f);

                                                            ImVec2 apex_world = ImVec2(
                                                                static_cast<float>(center_world.first - dir_world.x * (roi_span * 0.15f)),
                                                                static_cast<float>(center_world.second - dir_world.y * (roi_span * 0.15f)));
                                                            ImVec2 base_center_world = ImVec2(
                                                                apex_world.x + dir_world.x * beam_length,
                                                                apex_world.y + dir_world.y * beam_length);

                                                            ImVec2 perp_world = ImVec2(-dir_world.y, dir_world.x);
                                                            float perp_len = std::sqrt(perp_world.x * perp_world.x +
                                                                                        perp_world.y * perp_world.y);
                                                            if (perp_len > 1e-3f) {
                                                                perp_world.x /= perp_len;
                                                                perp_world.y /= perp_len;
                                                            }

                                                            ImVec2 left_world = ImVec2(
                                                                base_center_world.x + perp_world.x * (beam_width * 0.5f),
                                                                base_center_world.y + perp_world.y * (beam_width * 0.5f));
                                                            ImVec2 right_world = ImVec2(
                                                                base_center_world.x - perp_world.x * (beam_width * 0.5f),
                                                                base_center_world.y - perp_world.y * (beam_width * 0.5f));

                                                            auto left_scene_pair = worldToScene(left_world.x, left_world.y);
                                                            auto right_scene_pair = worldToScene(right_world.x, right_world.y);
                                                            auto apex_scene_pair = worldToScene(apex_world.x, apex_world.y);

                                                            ImVec2 tri_points[3];
                                                            tri_points[0] = ImPlot::PlotToPixels(
                                                                ImPlotPoint(left_scene_pair.first, left_scene_pair.second));
                                                            tri_points[1] = ImPlot::PlotToPixels(
                                                                ImPlotPoint(right_scene_pair.first, right_scene_pair.second));
                                                            tri_points[2] = ImPlot::PlotToPixels(
                                                                ImPlotPoint(apex_scene_pair.first, apex_scene_pair.second));

                                                            ImVec4 beam_color = base_color;
                                                            beam_color.w = 0.16f;

                                                            ImDrawList* beam_draw_list = ImPlot::GetPlotDrawList();
                                                            beam_draw_list->AddConvexPolyFilled(
                                                                tri_points,
                                                                3,
                                                                ImGui::ColorConvertFloat4ToU32(beam_color));
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                } else if (kEyeMaskDebugLoggingEnabled) {
                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                    ": eye mask data unavailable in detection results.");
                                }
                            }
                            draw_keypoint_markers();
                        }

                        if (zarr_loaded && zarr_loader.hasStimulusEvents()) {
                            struct StimulusOverlayState {
                                std::string text;
                                int last_event_frame = std::numeric_limits<int>::min();
                            };
                            static std::array<StimulusOverlayState, MAX_VIEWS> s_overlay_cache;

                            auto frame_events = zarr_loader.getStimulusEventsForFrame(current_frame_num);
                            if (!frame_events.empty()) {
                                std::string events_text;
                                for (size_t i = 0; i < frame_events.size(); ++i) {
                                    if (i > 0) {
                                        events_text += "\n";
                                    }
                                    events_text += frame_events[i];
                                }
                                s_overlay_cache[j].text = std::move(events_text);
                                s_overlay_cache[j].last_event_frame = current_frame_num;
                            }

                            if (!s_overlay_cache[j].text.empty() &&
                                current_frame_num >= s_overlay_cache[j].last_event_frame) {
                                ImVec2 plot_pos = ImPlot::GetPlotPos();
                                ImDrawList* draw_list = ImPlot::GetPlotDrawList();
                                ImVec2 overlay_origin = ImVec2(plot_pos.x + 12.0f, plot_pos.y + 12.0f);
                                ImVec2 text_size = ImGui::CalcTextSize(s_overlay_cache[j].text.c_str(), nullptr, false, -1.0f);
                                ImVec2 box_min = overlay_origin;
                                ImVec2 box_max = ImVec2(box_min.x + text_size.x + 12.0f,
                                                        box_min.y + text_size.y + 8.0f);

                                draw_list->AddRectFilled(box_min, box_max,
                                                         IM_COL32(0, 0, 0, 180), 4.0f);
                                draw_list->AddRect(box_min, box_max,
                                                   IM_COL32(80, 180, 255, 220), 4.0f);
                                draw_list->AddText(ImVec2(box_min.x + 6.0f, box_min.y + 4.0f),
                                                   IM_COL32(200, 220, 255, 255),
                                                   s_overlay_cache[j].text.c_str());
                            }
                        }

                        if (j == 0 && zarr_loader.hasMovementData()) {
                            ImGui::SetNextWindowSizeConstraints(ImVec2(120.0f, 120.0f),
                                                                ImVec2(420.0f, 420.0f));
                            bool crop_window_open = ImGui::Begin("Crop Preview");
                            if (crop_window_open) {
                                if (zarr_loader.hasCropImages()) {
                                    const auto& movement_frames = zarr_loader.getMovementFrameIndices();
                                    const auto& detection_indices = zarr_loader.getMovementDetectionIndices();
                                    int32_t crop_roi_index = -1;
                                    if (!movement_frames.empty() &&
                                        movement_frames.size() == detection_indices.size()) {
                                        auto it = std::lower_bound(movement_frames.begin(),
                                                                   movement_frames.end(),
                                                                   current_frame_num);
                                        if (it != movement_frames.end() && *it == current_frame_num) {
                                            size_t idx = static_cast<size_t>(std::distance(movement_frames.begin(), it));
                                            if (idx < detection_indices.size()) {
                                                crop_roi_index = detection_indices[idx];
                                            }
                                        }
                                    }

                                    static GLuint crop_texture = 0;
                                    static std::vector<uint8_t> crop_rgba_buffer;
                                    static int last_roi_index = -1;
                                    static size_t last_width = 0;
                                    static size_t last_height = 0;
                                    static size_t last_channels = 0;

                                    if (crop_roi_index >= 0) {
                                        ZarrDetectionLoader::CropImageView crop_view;
                                        if (zarr_loader.getCropImageForIndex(crop_roi_index, crop_view)) {
                                            bool needs_upload =
                                                crop_roi_index != last_roi_index ||
                                                crop_view.width != last_width ||
                                                crop_view.height != last_height ||
                                                crop_view.channels != last_channels;

                                            if (crop_texture == 0) {
                                                create_texture(&crop_texture);
                                                needs_upload = true;
                                            }

                                            if (needs_upload) {
                                                size_t pixel_count = crop_view.width * crop_view.height;
                                                crop_rgba_buffer.resize(pixel_count * 4);
                                                const uint8_t* src = crop_view.data;
                                                uint8_t* dst = crop_rgba_buffer.data();
                                                if (crop_view.channels == 4) {
                                                    std::memcpy(dst, src, pixel_count * 4);
                                                } else if (crop_view.channels == 3) {
                                                    for (size_t p = 0; p < pixel_count; ++p) {
                                                        dst[4 * p + 0] = src[3 * p + 0];
                                                        dst[4 * p + 1] = src[3 * p + 1];
                                                        dst[4 * p + 2] = src[3 * p + 2];
                                                        dst[4 * p + 3] = 255;
                                                    }
                                                } else {
                                                    for (size_t p = 0; p < pixel_count; ++p) {
                                                        uint8_t v = src[p];
                                                        dst[4 * p + 0] = v;
                                                        dst[4 * p + 1] = v;
                                                        dst[4 * p + 2] = v;
                                                        dst[4 * p + 3] = 255;
                                                    }
                                                }
                                                upload_texture(&crop_texture,
                                                               crop_rgba_buffer.data(),
                                                               static_cast<unsigned int>(crop_view.width),
                                                               static_cast<unsigned int>(crop_view.height));
                                                last_roi_index = crop_roi_index;
                                                last_width = crop_view.width;
                                                last_height = crop_view.height;
                                                last_channels = crop_view.channels;
                                            }

                                            if (crop_texture != 0) {
                                                ImVec2 img_size(static_cast<float>(crop_view.width),
                                                                static_cast<float>(crop_view.height));
                                                float max_dim = std::max(img_size.x, img_size.y);
                                                const float preview_max = 260.0f;
                                                if (max_dim > preview_max && max_dim > 0.0f) {
                                                    float scale = preview_max / max_dim;
                                                    img_size.x *= scale;
                                                    img_size.y *= scale;
                                                }
                                                ImGui::Image((ImTextureID)(intptr_t)crop_texture, img_size);
                                                ImGui::Text("ROI #%d", crop_roi_index);
                                            }
                                        } else {
                                            ImGui::TextUnformatted("No crop available for current frame.");
                                            last_roi_index = -1;
                                        }
                                    } else {
                                        ImGui::TextUnformatted("No crop available for current frame.");
                                            last_roi_index = -1;
                                    }
                                } else {
                                    ImGui::TextUnformatted("Crop images not loaded.");
                                }
                            }
                            ImGui::End();
                        }

                        if (plot_keypoints_flag) {
                            // plot arena for testing camera parameters
                            // gui_plot_perimeter(&camera_params[j],
                            // scene->image_height[j]); if (scene->num_cams > 1)
                            // {
                            //     gui_plot_world_coordinates(&camera_params[j],
                            //     j, scene->image_height[j]);
                            // }

                            // labeling
                            if (ImPlot::IsPlotHovered()) {
                                is_view_focused[j] = true;

                                if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                                    // create keypoints
                                    if (!keypoints_find) {
                                        // not found
                                        KeyPoints *keypoints =
                                            (KeyPoints *)malloc(
                                                sizeof(KeyPoints));
                                        allocate_keypoints(keypoints, scene,
                                                           skeleton);
                                        keypoints_map[current_frame_num] =
                                            keypoints;
                                    }
                                }

                                if (keypoints_find) {
                                    u32 *kp = &(keypoints_map[current_frame_num]
                                                    ->active_id[j]);
                                    if (ImGui::IsKeyPressed(ImGuiKey_W,
                                                            false)) {
                                        // labeling sequentially each view
                                        ImPlotPoint mouse =
                                            ImPlot::GetPlotMousePos();
                                        keypoints_map[current_frame_num]
                                            ->keypoints2d[j][*kp]
                                            .position = {mouse.x, mouse.y};
                                        keypoints_map[current_frame_num]
                                            ->keypoints2d[j][*kp]
                                            .is_labeled = true;
                                        keypoints_map[current_frame_num]
                                            ->keypoints2d[j][*kp]
                                            .is_triangulated = false;
                                        if (*kp < (skeleton->num_nodes - 1)) {
                                            (*kp)++;
                                        }
                                    }

                                    if (ImGui::IsKeyPressed(ImGuiKey_A, true)) {
                                        if (*kp <= 0) {
                                            *kp = 0;
                                        } else
                                            (*kp)--;
                                    }

                                    if (ImGui::IsKeyPressed(ImGuiKey_D, true)) {
                                        if (*kp >= skeleton->num_nodes - 1) {
                                            *kp = skeleton->num_nodes - 1;
                                        } else
                                            (*kp)++;
                                    }

                                    if (ImGui::IsKeyPressed(
                                            ImGuiKey_E,
                                            false)) // skip to the last keypoint
                                    {
                                        *kp = skeleton->num_nodes - 1;
                                    }

                                    if (ImGui::IsKeyPressed(
                                            ImGuiKey_Q,
                                            false)) // go to the first keypoint
                                    {
                                        *kp = 0;
                                    }

                                    // delete all keypoint on a frame
                                    if (ImGui::IsKeyPressed(ImGuiKey_Backspace,
                                                            false)) {
                                        free_keypoints(
                                            keypoints_map[current_frame_num],
                                            scene);
                                        keypoints_map.erase(current_frame_num);
                                        keypoints_find = false;
                                    }
                                }
                            } else {
                                is_view_focused[j] = false;
                            }

                            if (keypoints_find) {
                                gui_plot_keypoints(
                                    keypoints_map.at(current_frame_num),
                                    skeleton, j, scene->num_cams);
                                // think more general solution of multiple sets
                                // of keypoints
                                if (skeleton->name == "Rat4Box" ||
                                    skeleton->name == "Rat4Box3Ball") {
                                    gui_plot_bbox_from_keypoints(
                                        keypoints_map.at(current_frame_num),
                                        skeleton, j, 4, 5);
                                }
                            }
                        }     
                        ImPlot::EndPlot();
                    }
                    ImPlot::PopStyleVar();

                    ImGui::EndChild();

                    float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                    if (ImGui::Button(ICON_FK_FAST_BACKWARD)) {
                        int clamped_frame =
                            std::max(0, current_frame_num -
                                            10 * dc_context->seek_interval);
                        seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                         false);
                    }
                    ImGui::SameLine(0.0f, spacing);
                    if (ImGui::Button(ICON_FK_STEP_BACKWARD)) {
                        int clamped_frame = std::max(
                            0, current_frame_num - dc_context->seek_interval);
                        seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                         false);
                    }
                    ImGui::SameLine(0.0f, spacing);

                    if (ps.to_display_frame_number ==
                        (dc_context->total_num_frame - 1)) {
                        ImVec4 repeat_normal = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
                        ImVec4 repeat_hover = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
                        ImVec4 repeat_active = ImVec4(1.0f, 0.9f, 0.1f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, repeat_normal);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                              repeat_hover);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                              repeat_active);

                        if (ImGui::Button(ICON_FK_REPEAT)) {
                            // seek to zero
                            seek_all_cameras(scene, 0, video_fps, ps, false);
                        }
                        ImGui::PopStyleColor(3);
                    } else {
                        ImVec4 normal, hover, active;
                        if (ps.play_video) {
                            normal = ImVec4(0.8f, 0.3f, 0.3f, 1.0f);
                            hover = ImVec4(0.9f, 0.4f, 0.4f, 1.0f);
                            active = ImVec4(0.7f, 0.2f, 0.2f, 1.0f);
                        } else {
                            // green
                            normal = ImVec4(0.2f, 0.6f, 0.2f, 1.0f);
                            hover = ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
                            active = ImVec4(0.3f, 0.75f, 0.3f, 1.0f);
                        }
                        ImGui::PushStyleColor(ImGuiCol_Button, normal);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
                        if (ImGui::Button(ps.play_video ? ICON_FK_PAUSE
                                                        : ICON_FK_PLAY)) {
                            ps.play_video = !ps.play_video;
                            if (ps.play_video) {
                                ps.pause_seeked = false;
                                ps.last_play_time_start =
                                    std::chrono::steady_clock::now();
                            } else {
                                ps.pause_selected = 0;
                            }
                        }
                        ImGui::PopStyleColor(3);
                    }

                    ImGui::SameLine(0.0f, spacing);
                    if (ImGui::Button(ICON_FK_STEP_FORWARD)) {
                        int clamped_frame = std::min(
                            dc_context->total_num_frame,
                            current_frame_num + dc_context->seek_interval);
                        seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                         false);
                    }
                    ImGui::SameLine(0.0f, spacing);
                    if (ImGui::Button(ICON_FK_FAST_FORWARD)) {
                        int clamped_frame = std::min(
                            dc_context->total_num_frame,
                            current_frame_num + 10 * dc_context->seek_interval);
                        seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                         false);
                    }
                    ImGui::SameLine();
                    ps.slider_just_changed = ImGui::SliderInt(
                        "##frame count", &ps.slider_frame_number, 0,
                        dc_context->estimated_num_frames);
                    ImGui::SameLine();
                    float current_time_sec = ps.slider_frame_number / video_fps;
                    float total_time_sec =
                        dc_context->estimated_num_frames / video_fps;

                    std::string current_str = format_time(current_time_sec);
                    std::string total_str = format_time(total_time_sec);
                    ImGui::Text("%s / %s", current_str.c_str(),
                                total_str.c_str());

                    if (ps.slider_just_changed) {
                        // std::cout << "main, seeking: " <<
                        // ps.slider_frame_number
                        //           << std::endl;
                        seek_all_cameras(scene, ps.slider_frame_number,
                                         video_fps, ps, false);
                    }

                    ImGui::EndGroup();
                }
                ImGui::End();
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                ps.play_video = !ps.play_video;
                if (ps.play_video) {
                    ps.pause_seeked = false;
                    ps.last_play_time_start = std::chrono::steady_clock::now();
                } else {
                    ps.pause_selected = 0;
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
                if (ImGui::GetIO().KeyShift) {
                    int clamped_frame = std::max(
                        0, current_frame_num - 10 * dc_context->seek_interval);
                    seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                     false);
                } else {
                    int clamped_frame = std::max(
                        0, current_frame_num - dc_context->seek_interval);
                    seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                     false);
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
                if (ImGui::GetIO().KeyShift) {
                    int clamped_frame = std::min(
                        dc_context->total_num_frame,
                        current_frame_num + 10 * dc_context->seek_interval);
                    seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                     false);
                } else {
                    int clamped_frame =
                        std::min(dc_context->total_num_frame,
                                 current_frame_num + dc_context->seek_interval);
                    seek_all_cameras(scene, clamped_frame, video_fps, ps,
                                     false);
                }
            }

            for (const auto &[name, flag] : window_need_decoding) {
                window_was_decoding[name] = flag.load();
            }
        }

        if (plot_keypoints_flag) {
            if (ImGui::Begin("Keypoints")) {

                const float TEXT_BASE_HEIGHT =
                    ImGui::GetTextLineHeightWithSpacing();
                {
                    const int rows_count = scene->num_cams;
                    const int columns_count = skeleton->num_nodes + 1;

                    static ImGuiTableFlags table_flags =
                        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingFixedFit |
                        ImGuiTableFlags_BordersOuter |
                        ImGuiTableFlags_BordersInnerH |
                        ImGuiTableFlags_Hideable | ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_HighlightHoveredColumn;

                    if (ImGui::BeginTable(
                            "table_angled_headers", columns_count, table_flags,
                            ImVec2(0.0f, TEXT_BASE_HEIGHT * 12))) {
                        ImGui::TableSetupColumn(
                            "Name", ImGuiTableColumnFlags_NoHide |
                                        ImGuiTableColumnFlags_NoReorder);
                        for (int column = 1; column < columns_count; column++)
                            ImGui::TableSetupColumn(
                                skeleton->node_names[column - 1].c_str(),
                                ImGuiTableColumnFlags_AngledHeader |
                                    ImGuiTableColumnFlags_WidthFixed);
                        ImGui::TableSetupScrollFreeze(1, 2);

                        ImGui::
                            TableAngledHeadersRow(); // Draw angled headers
                                                     // for all columns with
                                                     // the
                                                     // ImGuiTableColumnFlags_AngledHeader
                                                     // flag.
                        ImGui::TableHeadersRow(); // Draw remaining headers
                                                  // and allow access to
                                                  // context-menu and other
                                                  // functions.

                        for (int row = 0; row < rows_count; row++) {
                            ImGui::PushID(row);
                            ImGui::TableNextRow();

                            if (is_view_focused[row] && keypoints_find) {
                                ImU32 row_bg_color = ImGui::GetColorU32(
                                    ImVec4(0.7f, 0.3f, 0.3f, 0.65f));
                                ImGui::TableSetBgColor(
                                    ImGuiTableBgTarget_RowBg0, row_bg_color);
                            }

                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::Text("%s", camera_names[row].c_str());
                            for (int column = 1; column < columns_count;
                                 column++)
                                if (ImGui::TableSetColumnIndex(column)) {
                                    if (keypoints_find) {
                                        ImVec4 node_color;
                                        if (keypoints_map[current_frame_num]
                                                ->active_id[row] ==
                                            column - 1) {
                                            node_color = (ImVec4)ImColor::HSV(
                                                0.8, 1.0f, 1.0f);
                                        } else {
                                            if (keypoints_map[current_frame_num]
                                                    ->keypoints2d[row]
                                                                 [column - 1]
                                                    .is_labeled) {
                                                node_color =
                                                    skeleton
                                                        ->node_colors[column -
                                                                      1];
                                                node_color.w = 0.9;
                                            }
                                        }

                                        if (keypoints_map[current_frame_num]
                                                ->keypoints2d[row][column - 1]
                                                .is_triangulated) {
                                            ImGui::TextColored(
                                                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                                "T");
                                        }

                                        ImU32 cell_bg_color =
                                            ImGui::GetColorU32(node_color);
                                        ImGui::TableSetBgColor(
                                            ImGuiTableBgTarget_CellBg,
                                            cell_bg_color);
                                    }
                                }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
            }
            ImGui::End();
        }

        if (plot_keypoints_flag) {
            if (ImGui::Begin("Labeling Tool")) {

                if (scene->num_cams > 1) {
                    bool keypoint_triangulated_all = true;
                    if (keypoints_find) {
                        for (int i = 0; i < scene->num_cams; i++) {
                            for (int j = 0; j < skeleton->num_nodes; j++) {
                                if (!keypoints_map.at(current_frame_num)
                                         ->keypoints2d[i][j]
                                         .is_triangulated) {
                                    keypoint_triangulated_all = false;
                                }
                            }
                        }
                    } else {
                        keypoint_triangulated_all = false;
                    }

                    bool enabled = keypoints_find;
                    bool apply_color = !keypoint_triangulated_all && enabled;
                    if (apply_color) {
                        ImGui::PushStyleColor(
                            ImGuiCol_Button,
                            (ImVec4)ImColor::HSV(0.8, 1.0f, 1.0f));
                        ImGui::PushStyleColor(
                            ImGuiCol_ButtonHovered,
                            (ImVec4)ImColor::HSV(0.8, 0.9f, 0.8f));
                        ImGui::PushStyleColor(
                            ImGuiCol_ButtonActive,
                            (ImVec4)ImColor::HSV(0.8, 0.9f, 0.5f));
                    }

                    ImGui::BeginDisabled(!enabled);
                    if (ImGui::Button("Triangulate")) {
                        reprojection(keypoints_map.at(current_frame_num),
                                     skeleton, camera_params, scene);
                    }
                    ImGui::EndDisabled();

                    if (apply_color) {
                        ImGui::PopStyleColor(3);
                    }

                    if (keypoints_find) {
                        if (ImGui::IsKeyPressed(ImGuiKey_T,
                                                false)) // triangulate
                        {
                            reprojection(keypoints_map.at(current_frame_num),
                                         skeleton, camera_params, scene);
                        }
                    }
                }

                if (ImGui::Button("Update keypoints working directory")) {
                    IGFD::FileDialogConfig config;
                    config.countSelectionMax = 1;
                    config.path = root_dir;
                    config.flags = ImGuiFileDialogFlags_Modal;
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "ChooseKeypointsFolder",
                        "Choose keypoints working directory", nullptr, config);
                }
                ImGui::SameLine();
                ImGui::Text("%s", keypoints_root_folder.c_str());

                if (ImGui::Button("Save Labeled Data") ||
                    (ImGui::GetIO().KeyCtrl &&
                     ImGui::IsKeyPressed(ImGuiKey_S, false))) {
                    save_keypoints(keypoints_map, skeleton,
                                   keypoints_root_folder, scene->num_cams,
                                   camera_names, &input_is_imgs, imgs_names);
                    last_saved = time(NULL);
                }
                if (last_saved != static_cast<std::time_t>(-1)) {
                    ImGui::SameLine();
                    ImGui::Text("Last saved: %s", ctime(&last_saved));
                }

                static bool load_old_format = false;
                if (ImGui::Button("Load Most Recent Labels")) {
                    free_all_keypoints(keypoints_map, scene);
                    if (load_old_format) {
                        if (load_keypoints_depreciated(
                                keypoints_map, skeleton, keypoints_root_folder,
                                scene, camera_names, error_message)) {
                            free_all_keypoints(keypoints_map, scene);
                            show_error = true;
                        }

                    } else {
                        std::string most_recent_folder;
                        if (find_most_recent_labels(keypoints_root_folder,
                                                    most_recent_folder,
                                                    error_message)) {
                            show_error = true;
                        } else {
                            if (load_keypoints(most_recent_folder,
                                               keypoints_map, skeleton, scene,
                                               camera_names, error_message)) {
                                free_all_keypoints(keypoints_map, scene);
                                show_error = true;
                            }
                        }
                    }
                }
                ImGui::SameLine();
                ImGui::Checkbox("Old format", &load_old_format);

                if (ImGui::Button("Load From Selected")) {
                    IGFD::FileDialogConfig config;
                    config.countSelectionMax = 1;
                    config.path = keypoints_root_folder;
                    config.flags = ImGuiFileDialogFlags_Modal;
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "LoadFromSelected", "Load from selected", nullptr,
                        config);
                }

                auto upper_it = keypoints_map.upper_bound(current_frame_num);
                if (upper_it == keypoints_map.end()) {
                    upper_it = keypoints_map.begin();
                }

                ImGui::Separator();
                ImGui::Text("Next labeled frame : %d", (*upper_it).first);
                if (ImGui::Button("Jump to Next Labeled Frame")) {
                    seek_all_cameras(scene, (*upper_it).first, video_fps, ps,
                                     true);
                }
                ImGui::Text("Total labeled frames : %zu", keypoints_map.size());
            }
            ImGui::End();
        }

        // Stimulus Event Timeline Window
        if (zarr_loaded) {
            if (ImGui::Begin("Stimulus Event Timeline")) {
                auto timeline = zarr_loader.getStimulusEventTimeline();
                static size_t last_logged_timeline_count = std::numeric_limits<size_t>::max();
                if (timeline.size() != last_logged_timeline_count) {
                    size_t missing_camera = 0;
                    for (const auto& evt : timeline) {
                        if (evt.camera_frame_id < 0) {
                            ++missing_camera;
                        }
                    }
                    std::cout << "  [StimulusTimeline] Entries=" << timeline.size()
                              << ", missing_camera_ids=" << missing_camera << std::endl;
                    const size_t preview = std::min<size_t>(timeline.size(), 5);
                    for (size_t i = 0; i < preview; ++i) {
                        const auto& evt = timeline[i];
                        std::cout << "    [" << i << "] stim_frame=" << evt.stimulus_frame_num
                                  << ", cam_frame=" << evt.camera_frame_id
                                  << ", type=" << evt.event_type_id
                                  << ", label='" << evt.label << "'" << std::endl;
                    }
                    last_logged_timeline_count = timeline.size();
                }
                if (timeline.empty()) {
                    ImGui::TextUnformatted("No stimulus events found.");
                } else {
                    // Helper function to generate consistent colors for event types
                    auto getEventTypeColor = [](int32_t event_type_id) -> ImVec4 {
                        // Use hash to generate consistent color for each event type
                        uint32_t hash = static_cast<uint32_t>(event_type_id) * 2654435761u;
                        float h = (hash % 360) / 360.0f;  // Hue
                        float s = 0.7f + 0.25f * ((hash >> 8) % 100) / 100.0f;  // Saturation 0.7-0.95
                        float v = 0.8f + 0.2f * ((hash >> 16) % 100) / 100.0f;  // Value 0.8-1.0

                        // Convert HSV to RGB
                        float c = v * s;
                        float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
                        float m = v - c;
                        float r, g, b;
                        if (h < 1.0f/6.0f) { r = c; g = x; b = 0; }
                        else if (h < 2.0f/6.0f) { r = x; g = c; b = 0; }
                        else if (h < 3.0f/6.0f) { r = 0; g = c; b = x; }
                        else if (h < 4.0f/6.0f) { r = 0; g = x; b = c; }
                        else if (h < 5.0f/6.0f) { r = x; g = 0; b = c; }
                        else { r = c; g = 0; b = x; }
                        return ImVec4(r + m, g + m, b + m, 0.9f);
                    };

                    // Collect unique event types and create filter state
                    static std::unordered_map<int32_t, bool> event_type_filter;
                    static bool filter_initialized = false;
                    std::unordered_map<int32_t, std::string> event_type_labels;
                    for (const auto& evt : timeline) {
                        if (event_type_labels.find(evt.event_type_id) == event_type_labels.end()) {
                            // Extract just the event type name (before any " - " context)
                            std::string type_name = evt.label;
                            size_t dash_pos = type_name.find(" - ");
                            if (dash_pos != std::string::npos) {
                                type_name = type_name.substr(0, dash_pos);
                            }
                            event_type_labels[evt.event_type_id] = type_name;

                            if (!filter_initialized) {
                                event_type_filter[evt.event_type_id] = true;  // All enabled by default
                            }
                        }
                    }
                    filter_initialized = true;

                    static int selected_event_idx = -1;
                    if (selected_event_idx >= static_cast<int>(timeline.size())) {
                        selected_event_idx = -1;
                    }

                    std::vector<double> x_values(timeline.size());
                    std::vector<double> y_values(timeline.size());
                    std::vector<int32_t> display_frames(timeline.size());
                    for (size_t i = 0; i < timeline.size(); ++i) {
                        const auto& evt = timeline[i];
                        int32_t frame = evt.camera_frame_id >= 0 ? evt.camera_frame_id
                                                                 : evt.stimulus_frame_num;
                        display_frames[i] = frame;
                        int32_t clamped = frame >= 0 ? frame : 0;
                        if (video_fps > 0.0) {
                            x_values[i] = static_cast<double>(clamped) / video_fps;
                        } else {
                            x_values[i] = static_cast<double>(clamped);
                        }
                        y_values[i] = 0.0;  // All events on same line
                    }

                    size_t timeline_signature = timeline.size();
                    if (!timeline.empty()) {
                        auto signature_value = [&](int32_t frame) -> size_t {
                            return static_cast<size_t>(std::max(frame, 0));
                        };
                        timeline_signature = timeline_signature * 1315423911u +
                                             signature_value(display_frames.front());
                        timeline_signature = timeline_signature * 2654435761u +
                                             signature_value(display_frames.back());
                    }
                    static size_t cached_timeline_signature = 0;

                    double default_max_time =
                        (video_fps > 0.0)
                            ? static_cast<double>(std::max<size_t>(1, zarr_loader.getTotalFrames())) / video_fps
                            : static_cast<double>(std::max<size_t>(1, zarr_loader.getTotalFrames()));
                    for (int32_t frame : display_frames) {
                        if (frame >= 0) {
                            double candidate = (video_fps > 0.0)
                                                   ? static_cast<double>(frame + 1) / video_fps
                                                   : static_cast<double>(frame + 1);
                            default_max_time = std::max(default_max_time, candidate);
                        }
                    }

                    ImVec2 plot_size = ImVec2(ImGui::GetContentRegionAvail().x, 170.0f);
                    if (ImPlot::BeginPlot("##stimulus_timeline_plot", plot_size,
                                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                        ImPlot::SetupAxes("Time (s)", nullptr, ImPlotAxisFlags_NoHighlight,
                                          ImPlotAxisFlags_NoDecorations);
                        ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                                          ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock);
                        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, 0.5, ImGuiCond_Always);
                        if (timeline_signature != cached_timeline_signature) {
                            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, default_max_time, ImGuiCond_Always);
                            cached_timeline_signature = timeline_signature;
                        }

                        // Plot events grouped by type with consistent colors
                        for (const auto& [event_type_id, type_label] : event_type_labels) {
                            // Skip filtered out event types
                            if (!event_type_filter[event_type_id]) {
                                continue;
                            }

                            // Collect events of this type
                            std::vector<double> type_x_values;
                            std::vector<double> type_y_values;
                            for (size_t i = 0; i < timeline.size(); ++i) {
                                if (timeline[i].event_type_id == event_type_id) {
                                    type_x_values.push_back(x_values[i]);
                                    type_y_values.push_back(y_values[i]);
                                }
                            }

                            if (!type_x_values.empty()) {
                                ImVec4 color = getEventTypeColor(event_type_id);
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6.0f, color, 1.5f,
                                                           ImVec4(0, 0, 0, 0));
                                ImPlot::PlotScatter(type_label.c_str(), type_x_values.data(),
                                                   type_y_values.data(),
                                                   static_cast<int>(type_x_values.size()));
                            }
                        }

                        double current_time = (video_fps > 0.0)
                                                  ? static_cast<double>(current_frame_num) / video_fps
                                                  : static_cast<double>(current_frame_num);
                        double current_line_x[2] = {current_time, current_time};
                        double current_line_y[2] = {-1.0, 1.0};
                        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 2.0f);
                        ImPlot::PlotLine("Current Frame", current_line_x, current_line_y, 2);

                        int hovered_event_idx = -1;
                        constexpr float kSelectionRadiusPx = 12.0f;
                        if (ImPlot::IsPlotHovered()) {
                            ImVec2 mouse_pos = ImGui::GetIO().MousePos;
                            float best_distance = kSelectionRadiusPx;
                            for (size_t i = 0; i < x_values.size(); ++i) {
                                ImVec2 event_pixels =
                                    ImPlot::PlotToPixels(ImPlotPoint(x_values[i], y_values[i]));
                                float dx = mouse_pos.x - event_pixels.x;
                                float dy = mouse_pos.y - event_pixels.y;
                                float distance = std::sqrt(dx * dx + dy * dy);
                                if (distance < best_distance) {
                                    best_distance = distance;
                                    hovered_event_idx = static_cast<int>(i);
                                }
                            }

                            if (hovered_event_idx != -1 &&
                                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                selected_event_idx = hovered_event_idx;
                                const int target_frame = display_frames[hovered_event_idx];
                                if (target_frame >= 0) {
                                    ps.slider_frame_number = target_frame;
                                    seek_all_cameras(scene, target_frame, video_fps, ps, false);
                                }
                            }
                        }

                        if (hovered_event_idx != -1) {
                            const auto& hovered_evt = timeline[hovered_event_idx];
                            const int32_t camera_frame = hovered_evt.camera_frame_id;
                            const int32_t stim_frame = hovered_evt.stimulus_frame_num;
                            if (camera_frame >= 0) {
                                double event_time = x_values[hovered_event_idx];
                                if (stim_frame >= 0 && stim_frame != camera_frame) {
                                    ImGui::SetTooltip("Camera Frame %d\nStimulus Frame %d\nTime %.3f s\n%s",
                                                      camera_frame,
                                                      stim_frame,
                                                      event_time,
                                                      hovered_evt.label.c_str());
                                } else {
                                    ImGui::SetTooltip("Camera Frame %d\nTime %.3f s\n%s",
                                                      camera_frame,
                                                      event_time,
                                                      hovered_evt.label.c_str());
                                }
                            } else {
                            ImGui::SetTooltip("Frame %d\nTime %.3f s\n%s",
                                             std::max(stim_frame, 0),
                                             x_values[hovered_event_idx],
                                             hovered_evt.label.c_str());
                            }
                        }

                        if (selected_event_idx >= 0 &&
                            selected_event_idx < static_cast<int>(x_values.size())) {
                            double selected_x = x_values[selected_event_idx];
                            double selected_y = y_values[selected_event_idx];
                            ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond, 9.0f,
                                                       ImVec4(1.0f, 0.5f, 0.2f, 1.0f), 2.0f,
                                                       ImVec4(0, 0, 0, 0));
                            ImPlot::PlotScatter("Selected Event", &selected_x, &selected_y, 1);
                        }
                        ImPlot::EndPlot();
                    }

                    // Legend and Filter UI
                    ImGui::SeparatorText("Event Type Legend & Filter");
                    ImGui::BeginChild("##event_type_legend", ImVec2(0, 120), true);

                    // Add "Show All" / "Hide All" buttons
                    if (ImGui::SmallButton("Show All")) {
                        for (auto& [type_id, enabled] : event_type_filter) {
                            enabled = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Hide All")) {
                        for (auto& [type_id, enabled] : event_type_filter) {
                            enabled = false;
                        }
                    }

                    ImGui::Separator();

                    // Display legend with checkboxes for filtering
                    int col_count = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 250.0f));
                    if (ImGui::BeginTable("##legend_table", col_count, ImGuiTableFlags_SizingStretchSame)) {
                        int col_idx = 0;
                        for (const auto& [event_type_id, type_label] : event_type_labels) {
                            if (col_idx % col_count == 0) {
                                ImGui::TableNextRow();
                            }
                            ImGui::TableNextColumn();

                            // Push unique ID for this event type
                            ImGui::PushID(event_type_id);

                            ImVec4 color = getEventTypeColor(event_type_id);

                            // Color swatch
                            ImGui::PushStyleColor(ImGuiCol_Button, color);
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
                            ImGui::SmallButton("  ");
                            ImGui::PopStyleColor(3);

                            ImGui::SameLine();

                            // Checkbox for filtering
                            bool& enabled = event_type_filter[event_type_id];
                            ImGui::Checkbox(type_label.c_str(), &enabled);

                            ImGui::PopID();

                            col_idx++;
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndChild();

                    ImGui::SeparatorText("Event List");
                    ImGui::BeginChild("##stimulus_event_list", ImVec2(0, 200), true);
                    for (size_t i = 0; i < timeline.size(); ++i) {
                        const auto& evt = timeline[i];
                        std::ostringstream row_label;
                        if (evt.camera_frame_id >= 0) {
                            row_label << "Cam " << evt.camera_frame_id;
                            if (evt.stimulus_frame_num >= 0 &&
                                evt.stimulus_frame_num != evt.camera_frame_id) {
                                row_label << " (Stim " << evt.stimulus_frame_num << ")";
                            }
                        } else {
                            row_label << "Stim " << evt.stimulus_frame_num;
                        }
                        row_label << "  " << evt.label;
                        ImGui::PushID(static_cast<int>(i));
                        bool is_selected = (selected_event_idx == static_cast<int>(i));
                        if (ImGui::Selectable(row_label.str().c_str(), is_selected)) {
                            selected_event_idx = static_cast<int>(i);
                            int target_frame = evt.camera_frame_id >= 0
                                                   ? evt.camera_frame_id
                                                   : evt.stimulus_frame_num;
                            if (target_frame >= 0) {
                                ps.slider_frame_number = target_frame;
                                seek_all_cameras(scene, target_frame, video_fps, ps, false);
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    if (selected_event_idx >= 0 &&
                        selected_event_idx < static_cast<int>(timeline.size())) {
                        const auto& evt = timeline[selected_event_idx];
                        ImGui::Separator();
                        ImGui::Text("Selected Event:");
                        if (evt.camera_frame_id >= 0) {
                            ImGui::BulletText("Camera Frame: %d", evt.camera_frame_id);
                        }
                        if (evt.stimulus_frame_num >= 0) {
                            ImGui::BulletText("Stimulus Frame: %d", evt.stimulus_frame_num);
                        }
                        ImGui::BulletText("Type ID: %d", evt.event_type_id);
                        ImGui::BulletText("%s", evt.label.c_str());
                    }
                }
            }
            ImGui::End();
        }

        // Movement timeline windows
        if (zarr_loaded && zarr_loader.hasMovementData()) {
            auto renderMovementDatasetUI = [&](const char* combo_label) -> const ZarrDetectionData::MovementSeries* {
                size_t series_count = zarr_loader.getMovementSeriesCount();
                size_t selected_index = zarr_loader.getSelectedMovementSeriesIndex();
                const auto* selected_series = zarr_loader.getMovementSeries(selected_index);

                if (series_count > 1) {
                    std::ostringstream summary;
                    if (selected_series) {
                        summary << selected_series->category << "/" << selected_series->run_name
                                << " (track " << selected_series->track_id << ")";
                    } else {
                        summary << "Select dataset";
                    }
                    if (ImGui::BeginCombo(combo_label, summary.str().c_str())) {
                        for (size_t i = 0; i < series_count; ++i) {
                            const auto* series = zarr_loader.getMovementSeries(i);
                            if (!series) {
                                continue;
                            }
                            std::ostringstream label;
                            label << series->category << "/" << series->run_name
                                  << " (track " << series->track_id << ")";
                            bool is_selected = (i == selected_index);
                            if (ImGui::Selectable(label.str().c_str(), is_selected)) {
                                if (zarr_loader.selectMovementSeries(i)) {
                                    selected_index = i;
                                    selected_series = zarr_loader.getMovementSeries(i);
                                }
                            }
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else if (selected_series) {
                    ImGui::Text("Dataset: %s/%s (track %s)",
                                selected_series->category.c_str(),
                                selected_series->run_name.c_str(),
                                selected_series->track_id.c_str());
                }

                if (selected_series) {
                    if (!selected_series->detection_variant.empty() ||
                        !selected_series->source_detect_run.empty()) {
                        if (!selected_series->source_detect_run.empty()) {
                            ImGui::Text("Variant: %s | Source run: %s",
                                        selected_series->detection_variant.empty()
                                            ? "unknown"
                                            : selected_series->detection_variant.c_str(),
                                        selected_series->source_detect_run.c_str());
                        } else {
                            ImGui::Text("Variant: %s",
                                        selected_series->detection_variant.empty()
                                            ? "unknown"
                                            : selected_series->detection_variant.c_str());
                        }
                    }
                    if (selected_series->fps > 0.0 || selected_series->smoothing_seconds > 0.0) {
                        if (selected_series->fps > 0.0 && selected_series->smoothing_seconds > 0.0) {
                            ImGui::Text("FPS: %.2f | Smoothing: %.2f s",
                                        selected_series->fps,
                                        selected_series->smoothing_seconds);
                        } else if (selected_series->fps > 0.0) {
                            ImGui::Text("FPS: %.2f", selected_series->fps);
                        } else {
                            ImGui::Text("Smoothing: %.2f s", selected_series->smoothing_seconds);
                        }
                    }
                    if (selected_series->video_width > 0 && selected_series->video_height > 0) {
                        ImGui::Text("Camera size: %dx%d",
                                    selected_series->video_width,
                                    selected_series->video_height);
                    }
                }

                return selected_series;
            };

            if (ImGui::Begin("Speed & Distance Timeline")) {
                const auto* selected_series = renderMovementDatasetUI("Dataset");

                const auto& time_data = zarr_loader.getMovementTimeSeconds();
                const auto& smoothed_speed = zarr_loader.getMovementSmoothedSpeedMm();
                const auto& instant_speed = zarr_loader.getMovementInstantaneousSpeedMm();
                const auto& distance_mm = zarr_loader.getMovementDistanceToTargetMm();
                const auto& heading_degrees = zarr_loader.getMovementHeadingDegrees();
                const auto& smoothed_heading_degrees = zarr_loader.getMovementSmoothedHeadingDegrees();
                const auto& heading_keypoint_success = zarr_loader.getMovementHeadingKeypointSuccess();
                const auto& heading_per_second_degrees = zarr_loader.getMovementHeadingPerSecondDegrees();
                const auto& heading_per_second_resultant = zarr_loader.getMovementHeadingPerSecondResultant();
                const auto& heading_per_second_time = zarr_loader.getMovementHeadingPerSecondTimeSeconds();
                const auto& frame_indices = zarr_loader.getMovementFrameIndices();
                static bool show_smoothed = true;
                static bool show_instantaneous = false;
                static bool show_heading_raw = false;
                static bool show_heading_smoothed = true;
                static bool show_heading_per_second = false;

                bool smoothed_available = !smoothed_speed.empty();
                bool instant_available = !instant_speed.empty();
                bool distance_available = !distance_mm.empty();
                bool heading_sample_available = !heading_degrees.empty() || !smoothed_heading_degrees.empty();
                bool heading_per_second_available = !heading_per_second_degrees.empty() &&
                                                    !heading_per_second_time.empty() &&
                                                    heading_per_second_degrees.size() == heading_per_second_time.size();

                if (!selected_series || time_data.empty() ||
                    (!smoothed_available && !instant_available && !distance_available &&
                     !heading_sample_available && !heading_per_second_available)) {
                    ImGui::TextUnformatted("No movement data available.");
                } else {
                    if (!smoothed_available && show_smoothed) {
                        show_smoothed = false;
                    }
                    if (!instant_available && show_instantaneous) {
                        show_instantaneous = false;
                    }
                    if (!heading_sample_available) {
                        show_heading_raw = false;
                        show_heading_smoothed = false;
                    }
                    if (!heading_per_second_available) {
                        show_heading_per_second = false;
                    }

                    if (!zarr_loader.getMovementCategory().empty()) {
                        ImGui::Text("Movement Run: %s/%s | Track: %s",
                                    zarr_loader.getMovementCategory().c_str(),
                                    zarr_loader.getMovementRunName().c_str(),
                                    zarr_loader.getMovementTrackId().c_str());
                    } else {
                        ImGui::Text("Movement Run: %s | Track: %s",
                                    zarr_loader.getMovementRunName().c_str(),
                                    zarr_loader.getMovementTrackId().c_str());
                    }
                    ImGui::Text("Data points: %zu", time_data.size());

                    ImGui::Checkbox("Show Smoothed Speed", &show_smoothed);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Instantaneous Speed", &show_instantaneous);

                    ImGui::SeparatorText("Heading Options");
                    ImGui::BeginDisabled(!heading_sample_available);
                    ImGui::Checkbox("Show Raw Heading", &show_heading_raw);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Smoothed Heading", &show_heading_smoothed);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!heading_per_second_available);
                    ImGui::Checkbox("Show Heading (per-second)", &show_heading_per_second);
                    ImGui::EndDisabled();

                    std::vector<double> time_plot;
                    std::vector<double> smoothed_plot;
                    std::vector<double> instant_plot;
                    time_plot.reserve(time_data.size());
                    smoothed_plot.reserve(smoothed_speed.size());
                    if (!instant_speed.empty()) {
                        instant_plot.reserve(instant_speed.size());
                    }

                    for (size_t i = 0; i < time_data.size(); ++i) {
                        time_plot.push_back(static_cast<double>(time_data[i]));
                        if (smoothed_available && i < smoothed_speed.size()) {
                            smoothed_plot.push_back(static_cast<double>(smoothed_speed[i]));
                        }
                        if (instant_available && i < instant_speed.size()) {
                            instant_plot.push_back(static_cast<double>(instant_speed[i]));
                        }
                    }

                    constexpr double kMmPerPlotUnit = 10.0;
                    std::vector<double> distance_time;
                    std::vector<double> distance_units;
                    double sum_distance = 0.0;
                    double max_distance_mm = 0.0;
                    double min_distance_mm = std::numeric_limits<double>::infinity();
                    size_t valid_distance_count = 0;

                    size_t distance_samples = std::min(time_data.size(), distance_mm.size());
                    distance_time.reserve(distance_samples);
                    distance_units.reserve(distance_samples);
                    for (size_t i = 0; i < distance_samples; ++i) {
                        float raw_distance = distance_mm[i];
                        if (!IsFiniteFloat(raw_distance)) {
                            continue;
                        }
                        double t = static_cast<double>(time_data[i]);
                        double value_mm = static_cast<double>(raw_distance);
                        distance_time.push_back(t);
                        distance_units.push_back(value_mm / kMmPerPlotUnit);
                        sum_distance += value_mm;
                        max_distance_mm = std::max(max_distance_mm, value_mm);
                        min_distance_mm = std::min(min_distance_mm, value_mm);
                        ++valid_distance_count;
                    }

                    std::vector<double> heading_time_raw;
                    std::vector<double> heading_raw_plot;
                    std::vector<double> heading_time_smoothed;
                    std::vector<double> heading_smoothed_plot;
                    heading_time_raw.reserve(std::min(time_data.size(), heading_degrees.size()));
                    heading_raw_plot.reserve(std::min(time_data.size(), heading_degrees.size()));
                    heading_time_smoothed.reserve(std::min(time_data.size(), smoothed_heading_degrees.size()));
                    heading_smoothed_plot.reserve(std::min(time_data.size(), smoothed_heading_degrees.size()));

                    auto headingSampleAllowed = [&](size_t index) {
                        if (!heading_keypoint_success.empty() && index < heading_keypoint_success.size()) {
                            return heading_keypoint_success[index] != 0;
                        }
                        return true;
                    };

                    double heading_y_min = std::numeric_limits<double>::infinity();
                    double heading_y_max = -std::numeric_limits<double>::infinity();
                    auto extend_heading_range = [&](double value) {
                        heading_y_min = std::min(heading_y_min, value);
                        heading_y_max = std::max(heading_y_max, value);
                    };

                    size_t raw_samples = std::min(time_data.size(), heading_degrees.size());
                    for (size_t i = 0; i < raw_samples; ++i) {
                        if (!headingSampleAllowed(i)) {
                            continue;
                        }
                        float heading_raw_val = heading_degrees[i];
                        if (!IsFiniteFloat(heading_raw_val)) {
                            continue;
                        }
                        double t = static_cast<double>(time_data[i]);
                        double v = static_cast<double>(heading_raw_val);
                        heading_time_raw.push_back(t);
                        heading_raw_plot.push_back(v);
                        extend_heading_range(v);
                    }

                    size_t smoothed_samples = std::min(time_data.size(), smoothed_heading_degrees.size());
                    for (size_t i = 0; i < smoothed_samples; ++i) {
                        if (!headingSampleAllowed(i)) {
                            continue;
                        }
                        float heading_smooth_val = smoothed_heading_degrees[i];
                        if (!IsFiniteFloat(heading_smooth_val)) {
                            continue;
                        }
                        double t = static_cast<double>(time_data[i]);
                        double v = static_cast<double>(heading_smooth_val);
                        heading_time_smoothed.push_back(t);
                        heading_smoothed_plot.push_back(v);
                        extend_heading_range(v);
                    }

                    std::vector<double> heading_per_second_time_plot;
                    std::vector<double> heading_per_second_plot;
                    std::vector<double> heading_per_second_resultant_plot;
                    if (heading_per_second_available) {
                        size_t per_samples = std::min(heading_per_second_degrees.size(), heading_per_second_time.size());
                        heading_per_second_time_plot.reserve(per_samples);
                        heading_per_second_plot.reserve(per_samples);
                        if (!heading_per_second_resultant.empty()) {
                            heading_per_second_resultant_plot.reserve(per_samples);
                        }
                        for (size_t i = 0; i < per_samples; ++i) {
                            float heading_val = heading_per_second_degrees[i];
                            float heading_time_val = heading_per_second_time[i];
                            if (!IsFiniteFloat(heading_val) || !IsFiniteFloat(heading_time_val)) {
                                continue;
                            }
                            double t = static_cast<double>(heading_time_val);
                            double v = static_cast<double>(heading_val);
                            heading_per_second_time_plot.push_back(t);
                            heading_per_second_plot.push_back(v);
                            extend_heading_range(v);
                            if (!heading_per_second_resultant.empty() && i < heading_per_second_resultant.size()) {
                                float resultant = heading_per_second_resultant[i];
                                if (IsFiniteFloat(resultant)) {
                                    heading_per_second_resultant_plot.push_back(static_cast<double>(resultant));
                                } else {
                                    heading_per_second_resultant_plot.push_back(std::numeric_limits<double>::quiet_NaN());
                                }
                            }
                        }
                        if (!heading_per_second_resultant.empty() &&
                            heading_per_second_resultant_plot.size() != heading_per_second_plot.size()) {
                            heading_per_second_resultant_plot.resize(
                                heading_per_second_plot.size(),
                                std::numeric_limits<double>::quiet_NaN());
                        }
                    }

                    auto compute_heading_axis = [&](double& min_out, double& max_out) {
                        if (heading_y_min == std::numeric_limits<double>::infinity() ||
                            heading_y_max == -std::numeric_limits<double>::infinity()) {
                            min_out = -180.0;
                            max_out = 180.0;
                        } else {
                            min_out = heading_y_min;
                            max_out = heading_y_max;
                            double span = std::max(10.0, max_out - min_out);
                            double padding = std::max(5.0, span * 0.1);
                            min_out -= padding;
                            max_out += padding;
                            if (min_out >= max_out) {
                                min_out -= 1.0;
                                max_out += 1.0;
                            }
                        }
                    };

                    auto computeCurrentTime = [&]() -> double {
                        if (current_frame_num < 0) {
                            return -1.0;
                        }
                        double resolved_time = -1.0;

                        if (!frame_indices.empty()) {
                            auto it = std::find(frame_indices.begin(), frame_indices.end(), current_frame_num);
                            if (it != frame_indices.end()) {
                                size_t idx = std::distance(frame_indices.begin(), it);
                                if (idx < time_data.size()) {
                                    resolved_time = static_cast<double>(time_data[idx]);
                                }
                            } else {
                                auto upper = std::lower_bound(frame_indices.begin(), frame_indices.end(), current_frame_num);
                                if (upper != frame_indices.end() && upper != frame_indices.begin()) {
                                    auto lower = upper - 1;
                                    size_t lower_idx = std::distance(frame_indices.begin(), lower);
                                    size_t upper_idx = std::distance(frame_indices.begin(), upper);

                                    if (upper_idx < time_data.size() && lower_idx < time_data.size()) {
                                        int32_t f0 = *lower;
                                        int32_t f1 = *upper;
                                        float t0 = time_data[lower_idx];
                                        float t1 = time_data[upper_idx];
                                        float delta_f = static_cast<float>(f1 - f0);
                                        if (delta_f != 0.0f) {
                                            float alpha = static_cast<float>(current_frame_num - f0) / delta_f;
                                            resolved_time = static_cast<double>(t0 + alpha * (t1 - t0));
                                        }
                                    }
                                } else if (upper == frame_indices.begin() && !time_data.empty()) {
                                    resolved_time = static_cast<double>(time_data.front());
                                } else if (upper == frame_indices.end() && !time_data.empty()) {
                                    resolved_time = static_cast<double>(time_data.back());
                                }
                            }
                        }

                        if (resolved_time < 0.0 && video_fps > 0.0) {
                            double estimated_time = static_cast<double>(current_frame_num) / video_fps;
                            if (!time_data.empty()) {
                                double min_time = static_cast<double>(time_data.front());
                                double max_time = static_cast<double>(time_data.back());
                                resolved_time = std::clamp(estimated_time, min_time, max_time);
                            } else {
                                resolved_time = estimated_time;
                            }
                        }

                        return resolved_time;
                    };

                    double current_time_line = computeCurrentTime();

                    ImVec2 subplot_size = ImVec2(-1, 720);
                    if (!time_plot.empty() &&
                        ImPlot::BeginSubplots("##movement_plots", 3, 1, subplot_size,
                                              ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle)) {
                        if (ImPlot::BeginPlot("##speed_plot")) {
                            ImPlot::SetupAxes(nullptr, "Speed (mm/s)");
                            ImPlot::SetupAxisLimits(ImAxis_X1, time_plot.front(), time_plot.back(), ImGuiCond_Once);

                            double max_speed = 0.0;
                            if (show_smoothed && !smoothed_plot.empty()) {
                                max_speed = std::max(max_speed, *std::max_element(smoothed_plot.begin(), smoothed_plot.end()));
                            }
                            if (show_instantaneous && !instant_plot.empty()) {
                                max_speed = std::max(max_speed, *std::max_element(instant_plot.begin(), instant_plot.end()));
                            }
                            double y_max_speed = (max_speed > 0.0) ? max_speed * 1.1 : 1.0;
                            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max_speed, ImGuiCond_Once);

                            if (show_smoothed && !smoothed_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Smoothed Speed",
                                                 time_plot.data(),
                                                 smoothed_plot.data(),
                                                 static_cast<int>(time_plot.size()));
                            }

                            if (show_instantaneous && !instant_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.5f, 0.2f, 0.6f), 1.0f);
                                ImPlot::PlotLine("Instantaneous Speed",
                                                 time_plot.data(),
                                                 instant_plot.data(),
                                                 static_cast<int>(instant_plot.size()));
                            }

                            if (current_time_line >= 0.0) {
                                ImPlotRect limits = ImPlot::GetPlotLimits();
                                double current_line_x[2] = {current_time_line, current_time_line};
                                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                                ImPlot::PlotLine("##current_time_speed", current_line_x, current_line_y, 2);
                            }

                            ImPlot::EndPlot();
                        }

                        if (ImPlot::BeginPlot("##heading_plot")) {
                            ImPlot::SetupAxes(nullptr, "Heading (deg)");
                            if (!time_plot.empty()) {
                                ImPlot::SetupAxisLimits(ImAxis_X1, time_plot.front(), time_plot.back(), ImGuiCond_Once);
                            }
                            double heading_axis_min;
                            double heading_axis_max;
                            compute_heading_axis(heading_axis_min, heading_axis_max);
                            ImPlot::SetupAxisLimits(ImAxis_Y1, heading_axis_min, heading_axis_max, ImGuiCond_Once);

                            bool drew_heading = false;
                            if (show_heading_raw && !heading_raw_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.35f, 0.2f, 0.9f), 1.5f);
                                ImPlot::PlotLine("Heading (raw)",
                                                 heading_time_raw.data(),
                                                 heading_raw_plot.data(),
                                                 static_cast<int>(heading_raw_plot.size()));
                                drew_heading = true;
                            }
                            if (show_heading_smoothed && !heading_smoothed_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.7f, 0.4f, 1.0f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Heading (smoothed)",
                                                 heading_time_smoothed.data(),
                                                 heading_smoothed_plot.data(),
                                                 static_cast<int>(heading_smoothed_plot.size()));
                                drew_heading = true;
                            }

                            bool drew_per_second = show_heading_per_second && !heading_per_second_plot.empty();
                            if (drew_per_second) {
                                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.8f, 0.8f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Heading (per-second)",
                                                 heading_per_second_time_plot.data(),
                                                 heading_per_second_plot.data(),
                                                 static_cast<int>(heading_per_second_plot.size()));
                            }

                            bool drew_resultant = drew_per_second && !heading_per_second_resultant_plot.empty();
                            if (drew_resultant) {
                                ImPlot::SetupAxis(ImAxis_Y2, "Resultant");
                                ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 1.0, ImGuiCond_Once);
                                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                                ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.6f, 0.6f, 0.7f), 1.5f);
                                ImPlot::PlotLine("Resultant",
                                                 heading_per_second_time_plot.data(),
                                                 heading_per_second_resultant_plot.data(),
                                                 static_cast<int>(heading_per_second_resultant_plot.size()));
                                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                            }

                            if ((drew_heading || drew_per_second) && current_time_line >= 0.0) {
                                ImPlotRect limits = ImPlot::GetPlotLimits();
                                double current_line_x[2] = {current_time_line, current_time_line};
                                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                                ImPlot::PlotLine("##current_time_heading", current_line_x, current_line_y, 2);
                            }

                            ImPlot::EndPlot();
                        }

                        if (ImPlot::BeginPlot("##distance_plot")) {
                            ImPlot::SetupAxes("Time (s)", "Distance (10 mm)");
                            if (!time_plot.empty()) {
                                ImPlot::SetupAxisLimits(ImAxis_X1, time_plot.front(), time_plot.back(), ImGuiCond_Once);
                            }
                            double y_max_units = (max_distance_mm > 0.0)
                                                     ? (max_distance_mm * 1.1) / kMmPerPlotUnit
                                                     : 1.0;
                            if (y_max_units <= 0.0) {
                                y_max_units = 1.0;
                            }
                            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max_units, ImGuiCond_Once);

                            if (!distance_time.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Distance to Target (10 mm)",
                                                 distance_time.data(),
                                                 distance_units.data(),
                                                 static_cast<int>(distance_time.size()));
                            }

                            if (current_time_line >= 0.0) {
                                ImPlotRect limits = ImPlot::GetPlotLimits();
                                double current_line_x[2] = {current_time_line, current_time_line};
                                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                                ImPlot::PlotLine("##current_time_distance", current_line_x, current_line_y, 2);
                            }

                            ImPlot::EndPlot();
                        }

                        ImPlot::EndSubplots();
                    }

                    ImGui::SeparatorText("Speed Statistics");
                    if (!smoothed_speed.empty()) {
                        float avg_speed = std::accumulate(smoothed_speed.begin(), smoothed_speed.end(), 0.0f) /
                                          smoothed_speed.size();
                        float max_spd = *std::max_element(smoothed_speed.begin(), smoothed_speed.end());
                        ImGui::BulletText("Average Speed (smoothed): %.2f mm/s", avg_speed);
                        ImGui::BulletText("Max Speed (smoothed): %.2f mm/s", max_spd);
                    } else {
                        ImGui::TextUnformatted("No smoothed speed data available.");
                    }

                    ImGui::SeparatorText("Distance Statistics");
                    if (valid_distance_count == 0) {
                        ImGui::TextUnformatted("No valid distance samples available.");
                    } else {
                        ImGui::Text("Valid points: %zu / %zu", valid_distance_count, distance_samples);
                        double avg_distance = sum_distance / static_cast<double>(valid_distance_count);
                        ImGui::BulletText("Average Distance: %.2f mm", avg_distance);
                        ImGui::BulletText("Max Distance: %.2f mm", max_distance_mm);
                        if (min_distance_mm < std::numeric_limits<double>::infinity()) {
                            ImGui::BulletText("Min Distance: %.2f mm", min_distance_mm);
                        }
                    }

                    ImGui::SeparatorText("Heading Statistics");
                    const std::vector<double>* heading_for_stats = nullptr;
                    if (!heading_smoothed_plot.empty()) {
                        heading_for_stats = &heading_smoothed_plot;
                    } else if (!heading_raw_plot.empty()) {
                        heading_for_stats = &heading_raw_plot;
                    }
                    if (heading_for_stats && !heading_for_stats->empty()) {
                        double sum_cos = 0.0;
                        double sum_sin = 0.0;
                        for (double deg : *heading_for_stats) {
                            double rad = deg * static_cast<double>(M_PI) / 180.0;
                            sum_cos += std::cos(rad);
                            sum_sin += std::sin(rad);
                        }
                        size_t count = heading_for_stats->size();
                        double mean_rad = std::atan2(sum_sin, sum_cos);
                        double mean_deg = mean_rad * 180.0 / static_cast<double>(M_PI);
                        double resultant =
                            std::sqrt(sum_cos * sum_cos + sum_sin * sum_sin) / static_cast<double>(count);
                        ImGui::Text("Valid samples: %zu", count);
                        ImGui::BulletText("Circular Mean: %.1f deg", mean_deg);
                        ImGui::BulletText("Mean Resultant Length: %.2f", resultant);
                    } else {
                        ImGui::TextUnformatted("No valid heading samples available.");
                    }
                    if (!heading_per_second_resultant_plot.empty()) {
                        size_t finite_count = 0;
                        double sum_res = 0.0;
                        for (double value : heading_per_second_resultant_plot) {
                            if (value == value) {
                                sum_res += value;
                                ++finite_count;
                            }
                        }
                        if (finite_count > 0) {
                            ImGui::BulletText("Average per-second resultant: %.2f",
                                              sum_res / static_cast<double>(finite_count));
                        }
                    }
                }
            }
            ImGui::End();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseKeypointsFolder")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                keypoints_root_folder =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("LoadFromSelected")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                auto selected_folder =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
                free_all_keypoints(keypoints_map, scene);
                if (load_keypoints(selected_folder, keypoints_map, skeleton,
                                   scene, camera_names, error_message)) {
                    free_all_keypoints(keypoints_map, scene);
                    show_error = true;
                }
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
            show_help_window = !show_help_window;
        }

        if (show_help_window) {
            if (ImGui::Begin("Help Menu")) {
                ImGui::Text("<Space>: toggle play and pause");
                ImGui::Text("<Left Arrow>    : Seek backward");
                ImGui::Text("<Shift+Left>    : Seek backward (×10)");
                ImGui::Text("<Right Arrow>   : Seek forward");
                ImGui::Text("<Shift+Right>   : Seek forward (×10)");

                ImGui::SeparatorText("When paused");
                ImGui::Text("<,>: previous image in buffer");
                ImGui::Text("<.>: next image in buffer");

                ImGui::SeparatorText("While hovering image");
                ImGui::Text("<c>: create keypoints on frame");
                ImGui::Text("<w>: drop active keypoint");
                ImGui::Text("<a>: active keypoint++ ");
                ImGui::Text("<d>: active keypoint--");
                ImGui::Text("<q>: active keypoint set to first node");
                ImGui::Text("<e>: active keypoint set to last node");
                ImGui::Text("<t> -> triangulate");
                ImGui::Text("<Backspace>: delete all keypoints");
                ImGui::Text("<Ctrl+s>         : Save labels");

                ImGui::SeparatorText("While hovering keypoints");
                ImGui::Text("<r>: delete active keypoint");
                ImGui::Text("<f>: delete active keypoint on all cameras");
                ImGui::Text("Click keypoint to active it");
            }
            ImGui::End();
        }

        if (show_error) {
            ImGui::OpenPopup("Error");
            show_error = false; // Reset the flag so it only opens once
        }

        if (ImGui::BeginPopupModal("Error", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", error_message.c_str());
            ImGui::Separator();

            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
                show_error = false;
            }

            ImGui::EndPopup();
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window->render_target, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w,
                     clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

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
        glfwSwapBuffers(window->render_target);

        if (ps.just_seeked) {
            ps.just_seeked = false;
        } else {
            if (dc_context->decoding_flag && ps.play_video) {
                // always round up, mimimum 1
                int frame_to_show =
                    static_cast<int>(std::ceil(playback_time_now * video_fps));

                int min_decoded_frame = INT_MAX;
                for (const auto &[cam_name, visible] : window_need_decoding) {
                    if (visible.load()) {
                        int decoded = latest_decoded_frame[cam_name].load();
                        min_decoded_frame =
                            std::min(min_decoded_frame, decoded);
                    }
                }
                frame_to_show = std::min(frame_to_show, min_decoded_frame);

                int frame_delta = frame_to_show - ps.to_display_frame_number;
                if (frame_delta > 0) {
                    // Update frame number
                    ps.to_display_frame_number = frame_to_show;

                    // Mark all intermediate frames as available
                    for (int offset = 0; offset < frame_delta; ++offset) {
                        int index =
                            (ps.read_head + offset) % scene->size_of_buffer;
                        for (int j = 0; j < scene->num_cams; j++) {
                            scene->display_buffer[j][index].available_to_write =
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
    }

    // Cleanup
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
