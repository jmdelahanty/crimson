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
#include "h5_loader.h"
#include <ImGuiFileDialog.h>
#include <chrono>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include "zarr_loader.h"
#include "gui_interpolation.h"

#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

simplelogger::Logger *logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

std::vector<std::mutex> g_mutexes(MAX_VIEWS);
std::vector<std::condition_variable> g_cvs(MAX_VIEWS);
std::vector<bool> g_ready(MAX_VIEWS);
std::vector<std::vector<cv::Rect>> yolo_boxes(MAX_VIEWS);
std::vector<std::vector<std::string>> yolo_labels(MAX_VIEWS);
std::vector<std::vector<int>> yolo_classid(MAX_VIEWS);
std::vector<unsigned char *> yolo_input_frames_rgba(MAX_VIEWS);
std::unordered_map<std::string, std::atomic<bool>> window_need_decoding;
std::unordered_map<std::string, std::atomic<int>> latest_decoded_frame;

// 2. Add these variables near your other global variables
ZarrDetectionLoader* zarr_loader = nullptr;
H5SessionData* h5_session_data = nullptr;
bool show_interpolation_debug = false;
bool use_interpolated_detections = true;  // Toggle for using interpolated vs original

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

    H5SessionData h5_data;
    bool h5_loaded = false;

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
    int current_frame_num = 0;
    bool skeleton_chosen = false;
    std::vector<std::string> imgs_names;

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

            // 1. Check for H5 Bounding Boxes
            if (h5_loaded && h5_data.has_tracking_data) {
                auto boxes_for_frame = H5SessionLoader::getBoundingBoxesForFrame(h5_data, current_frame_num);
                if (!boxes_for_frame.empty()) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[H5] Bounding Boxes:  Found %zu", boxes_for_frame.size());
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "[H5] Bounding Boxes:  None");
                }
            }

            // 2. Check for H5 Chaser/Target States
            if (h5_loaded) {
                auto frame_meta = H5SessionLoader::getFrameMetadataByCameraID(h5_data, current_frame_num);
                if (frame_meta) {
                    auto chaser_states = H5SessionLoader::getChaserStatesForFrame(h5_data, frame_meta->stimulus_frame_num);
                    if (!chaser_states.empty()) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[H5] Chaser/Target:    Found %zu states", chaser_states.size());
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "[H5] Chaser/Target:    None for stimulus frame %llu", frame_meta->stimulus_frame_num);
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "[H5] Chaser/Target:    No frame metadata found for camera frame");
                }
            }

            // 3. Check for Labeled Keypoints
            if (plot_keypoints_flag) {
                if (keypoints_map.count(current_frame_num)) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[Manual] Keypoints:   Found");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "[Manual] Keypoints:   None");
                }
            }

            // 4. Check for YOLO Detections (for each camera view)
            if (yolo_detection) {
                ImGui::Separator();
                ImGui::Text("YOLO Detections:");
                for(int i = 0; i < scene->num_cams; ++i) {
                    if (!yolo_boxes.at(i).empty()) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "  - %s: Found %zu", camera_names[i].c_str(), yolo_boxes.at(i).size());
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "  - %s: None", camera_names[i].c_str());
                    }
                }
            }

            if (zarr_loaded) {
                int32_t n_dets = zarr_loader.getDetectionsForFrame(current_frame_num);
                if (n_dets > 0) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), 
                                    "[Zarr] Detections:    Found %d", n_dets);
                    
                    // Show additional info
                    if (zarr_loader.hasScores()) {
                        auto detections = zarr_loader.getRawDetections(current_frame_num);
                        if (!detections.scores.empty()) {
                            float max_score = *std::max_element(detections.scores.begin(), 
                                                            detections.scores.end());
                            ImGui::Text("  Max confidence: %.2f", max_score);
                        }
                    }
                    
                    if (zarr_loader.hasClassIDs()) {
                        ImGui::Text("  Has class IDs: Yes");
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                                    "[Zarr] Detections:    None");
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), 
                                "[Zarr] Detections:    Not loaded");
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
                    std::cout << "Successfully loaded Zarr detection file" << std::endl;
                    std::cout << "  Total frames: " << zarr_loader.getTotalFrames() << std::endl;
                    std::cout << "  FPS: " << zarr_loader.getFPS() << std::endl;
                    
                    if (zarr_loader.hasInterpolation()) {
                        std::cout << "  Interpolation data available!" << std::endl;
                        std::cout << "  Method: " << zarr_loader.getInterpolationMethod() << std::endl;
                    }
                } else {
                    zarr_loaded = false;
                    std::cout << "No Zarr detection file found (optional): " << zarr_error << std::endl;
                }

                // Try to load H5 file from the selected directory
                if (loadH5SessionFromDirectory(root_dir, h5_data, error_message)) {
                    h5_loaded = true;
                    std::cout << "Successfully loaded H5 session file" << std::endl;
                    std::cout << "  Session UUID: " << h5_data.session_info.session_uuid << std::endl;
                    std::cout << "  Protocol: " << h5_data.session_info.protocol_name_from_definition << std::endl;
                    std::cout << "  Total frames: " << h5_data.total_frames << std::endl;
                    std::cout << "  Events: " << h5_data.events.size() << std::endl;

                    if (h5_data.has_tracking_data) {
                        std::cout << "  Bounding boxes: " << h5_data.bounding_boxes.size() << std::endl;
                        std::cout << "  Chaser states: " << h5_data.chaser_states.size() << std::endl;
                    }

                    // Sync FPS with video if available
                    if (h5_data.fps > 0 && h5_data.has_video_metadata) {
                        std::cout << "  H5 FPS: " << h5_data.fps << ", Video FPS: " << video_fps << std::endl;
                        // Optionally sync: video_fps = h5_data.fps;
                    }
                } else {
                    // H5 file not found or failed to load - this is optional, not an error
                    std::cout << "No H5 session file found in directory (optional)" << std::endl;
                    h5_loaded = false;
                }

                if (loadZarrDetectionFromDirectory(root_dir, zarr_loader, error_message)) {
                    zarr_loaded = true;
                    std::cout << "Successfully loaded Zarr detection file" << std::endl;
                    std::cout << "  Total frames: " << zarr_loader.getTotalFrames() << std::endl;
                    std::cout << "  FPS: " << zarr_loader.getFPS() << std::endl;
                    
                    // Check if frame counts match between video and detections
                    if (video_loaded && zarr_loader.getTotalFrames() > 0) {
                        if (zarr_loader.getTotalFrames() != dc_context->total_num_frame) {
                            std::cout << "  WARNING: Zarr frames (" << zarr_loader.getTotalFrames()
                                      << ") != Video frames (" << dc_context->total_num_frame << ")" << std::endl;
                        }
                    }
                } else {
                    std::cout << "No Zarr detection file found (optional): " << error_message << std::endl;
                    zarr_loaded = false;
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

                // After loading video and H5, load camera calibration
                if (video_loaded) {
                    camera_params.resize(scene->num_cams); // Ensure vector is sized
                    if (h5_loaded) {
                        // Parse the arena config JSON to calculate the coordinate system offset
                        if (!h5_data.arena_config_json.empty()) {
                            try {
                                json config = json::parse(h5_data.arena_config_json);

                                // Extract the center of the stimulus (swimmable) area
                                float swim_x = config.value("swimmable_area_center_x_px", 0.0f);
                                float swim_y = config.value("swimmable_area_center_y_px", 0.0f);

                                std::cout << "[Offset Calc] Swimmable area center: (" << swim_x << ", " << swim_y << ")" << std::endl;

                                // Extract the calibration (sub_arena) dimensions and position
                                float sub_x = config.value("sub_arena_x_px", 0.0f);
                                float sub_y = config.value("sub_arena_y_px", 0.0f);
                                float sub_w = config.value("sub_arena_width_px", 0.0f);
                                float sub_h = config.value("sub_arena_height_px", 0.0f);
                                
                                // Correctly calculate the CENTER of the calibration area
                                float calib_center_x = sub_x + (sub_w / 2.0f);
                                float calib_center_y = sub_y + (sub_h / 2.0f);
                                
                                // **FIXED CALCULATION:**
                                // Calculate the final offset by finding the difference between the two centers
                                // and then adjusting by the swimmable area's center again.
                                float offsetX = (calib_center_x - swim_x) - swim_x;
                                float offsetY = (calib_center_y - swim_y) - swim_y;

                                std::cout << "[Offset Calc] Calibration center: (" << calib_center_x << ", " << calib_center_y << ")" << std::endl;

                                // Store the corrected offset in each camera's parameters
                                for (size_t i = 0; i < camera_params.size(); ++i) {
                                    camera_params[i].stimulus_offset_x = offsetX;
                                    camera_params[i].stimulus_offset_y = offsetY;
                                }
                                std::cout << "[Offset Calc] Corrected stimulus offset: (" << offsetX << ", " << offsetY << ")" << std::endl;

                            } catch (const json::exception& e) {
                                std::cerr << "Warning: Could not parse arena_config.json to calculate offset: " << e.what() << std::endl;
                            }
                        }
                        std::cout << "\n=== Loading Camera Calibrations from H5 ===" << std::endl;
                        for (size_t i = 0; i < camera_names.size(); ++i) {
                            std::string h5_camera_id = camera_names[i];

                            // ** FIX 1: Strip the "Cam" prefix to match H5 key **
                            if (h5_camera_id.rfind("Cam", 0) == 0) {
                                h5_camera_id = h5_camera_id.substr(3);
                            }

                            std::cout << "\nProcessing camera " << i << ": " << camera_names[i]
                                    << " (using ID: " << h5_camera_id << " for H5 lookup)" << std::endl;

                            // ** FIX 2: Use the enhanced loading function **
                            if (!camera_load_calibration_from_h5_enhanced(
                                    h5_data,          // Pass the entire H5 data structure
                                    h5_camera_id,     // Use the corrected camera ID
                                    camera_params[i], // Output parameters
                                    error_message)) {

                                std::cerr << "Warning: Failed to load calibration for camera " << camera_names[i]
                                        << " from H5 file: " << error_message << std::endl;

                                // Attempt to fall back to loading from a separate YAML file
                                std::string yaml_file = root_dir + "/calibration/" + camera_names[i] + ".yaml";
                                if (std::filesystem::exists(yaml_file)) {
                                    std::cout << "--> H5 failed, attempting fallback to YAML file: " << yaml_file << std::endl;
                                    if (!camera_load_params_from_yaml(yaml_file, camera_params[i], error_message)) {
                                        // If both H5 and YAML fail, show an error
                                        show_error = true;
                                        break;
                                    }
                                } else {
                                    std::cout << "--> No fallback YAML file found." << std::endl;
                                }
                            } else {
                                // Print debug info to confirm what was successfully loaded from H5
                                camera_print_calibration_details(camera_params[i], camera_names[i]);
                            }
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

                    if (ImPlot::BeginPlot("##no_plot_name", avail_size,
                                          ImPlotFlags_Equal |
                                          ImPlotAxisFlags_AutoFit |
                                          ImPlotFlags_Crosshairs)) {
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

                        // === ENHANCED H5 BOUNDING BOX RENDERING === //
                        if (h5_loaded && h5_data.has_tracking_data) {
                            // Get all bounding boxes for this camera's frame ID
                            auto boxes_for_frame = H5SessionLoader::getBoundingBoxesForFrame(h5_data, current_frame_num);
                            
                            if (!boxes_for_frame.empty()) {
                                // Check if this frame is interpolated (for H5 analysis files)
                                bool is_h5_interpolated = h5_data.is_analysis_file && 
                                                        h5_data.isFrameInterpolated(current_frame_num);
                                
                                // Draw the bounding boxes with interpolation indication
                                for (const auto& box : boxes_for_frame) {
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
                                    
                                    // Blue tones for H5 boxes
                                    ImVec4 color;
                                    if (is_h5_interpolated) {
                                        color = ImVec4(0.5f, 0.5f, 1.0f, 0.9f);  // Light blue for interpolated
                                    } else {
                                        color = ImVec4(0.2f, 0.2f, 1.0f, 1.0f);  // Blue for original
                                    }
                                    
                                    ImPlot::SetNextLineStyle(color, 2.0f);
                                    std::string label = "H5_" + std::to_string(box.class_id);
                                    if (is_h5_interpolated) label += " [I]";
                                    ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);
                                }
                            }

                            // Get the frame metadata to find the corresponding stimulus frame num
                            auto frame_meta = H5SessionLoader::getFrameMetadataByCameraID(h5_data, current_frame_num);
                            if (frame_meta) {
                                // Get chaser states for this stimulus frame
                                auto chaser_states = H5SessionLoader::getChaserStatesForFrame(h5_data, frame_meta->stimulus_frame_num);
                                if (!chaser_states.empty()) {
                                    gui_draw_chaser_state(chaser_states, scene->image_height[j], camera_params[j]);
                                }
                            }
                        }
                        
                        // === ENHANCED ZARR BOUNDING BOX RENDERING === //
                        if (zarr_loaded) {
                            // Check if this frame is interpolated
                            bool is_zarr_interpolated = zarr_loader.hasInterpolation() && 
                                                    zarr_loader.isFrameInterpolated(current_frame_num);
                            
                            // Get bounding boxes (using interpolated if available and enabled)
                            std::vector<LoggedBoundingBox> zarr_boxes;
                            if (zarr_loader.hasInterpolation() && use_interpolated_detections) {
                                // This will get interpolated boxes if available
                                zarr_boxes = zarr_loader.getBoundingBoxesForFrame(current_frame_num, true);
                            } else {
                                // This will get original boxes only
                                zarr_boxes = zarr_loader.getBoundingBoxesForFrame(current_frame_num);
                            }
                            
                            if (!zarr_boxes.empty()) {
                                for (const auto& box : zarr_boxes) {
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
                                    
                                    // Color based on interpolation status
                                    ImVec4 box_color;
                                    float line_width;
                                    
                                    if (is_zarr_interpolated && use_interpolated_detections) {
                                        // Orange/yellow for interpolated frames
                                        box_color = ImVec4(1.0f, 0.7f, 0.0f, 0.9f);
                                        line_width = 2.5f;
                                    } else {
                                        // Green for original detections
                                        box_color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
                                        line_width = 2.0f;
                                    }
                                    
                                    ImPlot::SetNextLineStyle(box_color, line_width);
                                    
                                    std::string label = "Zarr_" + std::to_string(box.class_id);
                                    if (is_zarr_interpolated && use_interpolated_detections) {
                                        label += " [I]";  // Mark as interpolated
                                    }
                                    
                                    ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);
                                }
                            }
                        }
                        
                        // === ADD INTERPOLATION STATUS OVERLAY === //
                        if ((zarr_loaded && zarr_loader.hasInterpolation()) || 
                            (h5_loaded && h5_data.is_analysis_file)) {
                            
                            // Determine interpolation status
                            bool any_interpolated = false;
                            std::string source = "";
                            
                            if (zarr_loaded && zarr_loader.hasInterpolation() && 
                                zarr_loader.isFrameInterpolated(current_frame_num)) {
                                any_interpolated = true;
                                source = "Zarr";
                            }
                            
                            if (h5_loaded && h5_data.is_analysis_file && 
                                h5_data.isFrameInterpolated(current_frame_num)) {
                                any_interpolated = true;
                                source = source.empty() ? "H5" : source + "+H5";
                            }
                            
                            // Create status text
                            std::string status_text;
                            ImVec4 status_color;
                            
                            if (any_interpolated) {
                                status_text = "INTERPOLATED";
                                if (!source.empty()) {
                                    status_text += " (" + source + ")";
                                }
                                status_color = ImVec4(1.0f, 0.7f, 0.0f, 0.9f);  // Orange
                            } else {
                                status_text = "ORIGINAL";
                                status_color = ImVec4(0.2f, 1.0f, 0.2f, 0.9f);  // Green
                            }
                            
                            // Draw status overlay in top-left corner of the plot
                            ImDrawList* draw_list = ImPlot::GetPlotDrawList();
                            ImVec2 text_size = ImGui::CalcTextSize(status_text.c_str());
                            
                            // Position in top-left, slightly offset from edge
                            ImVec2 plot_pos = ImPlot::PlotToPixels(ImPlotPoint(10, scene->image_height[j] - 30));
                            ImVec2 box_min = plot_pos;
                            ImVec2 box_max = ImVec2(box_min.x + text_size.x + 10, 
                                                    box_min.y + text_size.y + 6);
                            
                            // Semi-transparent background
                            draw_list->AddRectFilled(box_min, box_max, 
                                                    IM_COL32(0, 0, 0, 200), 3.0f);
                            // Colored border
                            draw_list->AddRect(box_min, box_max, 
                                            ImGui::ColorConvertFloat4ToU32(status_color), 3.0f);
                            
                            // Status text
                            draw_list->AddText(ImVec2(box_min.x + 5, box_min.y + 3), 
                                            ImGui::ColorConvertFloat4ToU32(status_color), 
                                            status_text.c_str());
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

                // Session Info Window
                if (h5_loaded) {
                    if (ImGui::Begin("H5 Session Info")) {
                        ImGui::Text("Session UUID: %s", h5_data.session_info.session_uuid.c_str());
                        ImGui::Text("Start Time: %s", h5_data.session_info.session_start_iso8601_utc.c_str());
                        ImGui::Text("Protocol: %s", h5_data.session_info.protocol_name_from_definition.c_str());
                        ImGui::Text("Rig ID: %s", h5_data.session_info.rig_id.c_str());
                        ImGui::Text("Arena ID: %s", h5_data.session_info.arena_id.c_str());
                        ImGui::Text("Output Size: %dx%d",
                                h5_data.session_info.stimulus_output_width,
                                h5_data.session_info.stimulus_output_height);

                        if (!h5_data.session_info.operator_notes.empty()) {
                            ImGui::Separator();
                            ImGui::TextWrapped("Notes: %s", h5_data.session_info.operator_notes.c_str());
                        }

                        if (!h5_data.session_info.subject_metadata.empty()) {
                            ImGui::Separator();
                            ImGui::Text("Subject Metadata:");
                            for (const auto& [key, value] : h5_data.session_info.subject_metadata) {
                                ImGui::Text("  %s: %s", key.c_str(), value.c_str());
                            }
                        }
                    }
                    ImGui::End();
                }


                // Events Window
                if (h5_loaded && !h5_data.events.empty()) {
                    if (ImGui::Begin("H5 Events")) {
                        // Find events near current frame time
                        if (video_loaded && h5_data.has_video_metadata) {
                            auto frame_meta = H5SessionLoader::getFrameMetadata(h5_data, current_frame_num);
                            if (frame_meta) {
                                ImGui::Text("Current Frame Timestamp: %.3f s", frame_meta->timestamp_ns / 1e9);
                                ImGui::Separator();
                            }
                        }

                        ImGui::Text("Total Events: %zu", h5_data.events.size());

                        // Show last few events
                        ImGui::Separator();
                        ImGui::Text("Recent Events:");
                        size_t start_idx = h5_data.events.size() > 10 ? h5_data.events.size() - 10 : 0;
                        for (size_t i = start_idx; i < h5_data.events.size(); i++) {
                            const auto& event = h5_data.events[i];
                            ImGui::Text("[%.3fs] Type:%d Step:%d %s",
                                    event.timestamp_ns_session / 1e9,
                                    event.event_type_id,
                                    event.current_step_index,
                                    event.name_or_context);
                        }
                    }
                    ImGui::End();
                }

                // Tracking Data Window
                if (h5_loaded && h5_data.has_tracking_data) {
                    if (ImGui::Begin("H5 Tracking Data")) {
                        ImGui::Text("Total Bounding Boxes: %zu", h5_data.bounding_boxes.size());
                        ImGui::Text("Total Chaser States: %zu", h5_data.chaser_states.size());

                        if (video_loaded) {
                            ImGui::Separator();
                            ImGui::Text("Current Frame: %d", current_frame_num);

                            // Get tracking data for current frame
                            auto boxes = H5SessionLoader::getBoundingBoxesForFrame(h5_data, current_frame_num);
                            auto chaser_states = H5SessionLoader::getChaserStatesForFrame(h5_data, current_frame_num);

                            if (!boxes.empty()) {
                                ImGui::Text("Bounding Boxes in Frame: %zu", boxes.size());
                                for (const auto& box : boxes) {
                                    ImGui::Text("  Camera %d: [%.1f,%.1f,%.1f,%.1f] Class:%d Conf:%.2f",
                                            box.payload_camera_id,
                                            box.x_min, box.y_min, box.width, box.height,
                                            box.class_id, box.confidence);
                                }
                            }

                            if (!chaser_states.empty()) {
                                ImGui::Text("Chaser States in Frame: %zu", chaser_states.size());
                                for (const auto& state : chaser_states) {
                                    ImGui::Text("  Chaser %d: %s Pos:(%.1f,%.1f) Target:(%.1f,%.1f)",
                                            state.chaser_index,
                                            state.is_chasing ? "Chasing" : "Not Chasing",
                                            state.chaser_pos_x, state.chaser_pos_y,
                                            state.target_pos_x, state.target_pos_y);
                                }
                            }
                        }
                    }
                    ImGui::End();
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