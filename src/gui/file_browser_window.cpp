#include "gui/file_browser_window.h"

#include <ImGuiFileDialog.h>
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace {

const char* playbackPreviewScaleLabel(int mode) {
    switch (mode) {
    case 1:
        return "1/2";
    case 2:
        return "1/4";
    default:
        return "1x";
    }
}

const char* playbackRendererModeLabel(int mode) {
    switch (mode) {
    case 1:
        return "lightweight";
    default:
        return "standard";
    }
}

}  // namespace

FileBrowserWindowResult drawFileBrowserWindow(const FileBrowserWindowContext& context,
                                              FileBrowserWindowState& state) {
    FileBrowserWindowResult result;
    if (!ImGui::Begin("File Browser", nullptr, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return result;
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open")) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 0;
                config.path = context.start_folder_name;
                config.flags = ImGuiFileDialogFlags_Modal;
                ImGuiFileDialog::Instance()->OpenDialog(
                    "ChooseMedia",
                    "Choose Media",
                    ".mp4,.tiff,.jpeg,.jpg,.png",
                    config);
            }
            if (ImGui::MenuItem("Load Zarr Archive")) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = context.root_dir.empty() ? context.start_folder_name
                                                       : context.root_dir;
                config.flags = ImGuiFileDialogFlags_Modal;
                ImGuiFileDialog::Instance()->OpenDialog(
                    "ChooseZarrArchive",
                    "Choose Zarr Archive Directory",
                    nullptr,
                    config);
            }
            if (context.video_loaded && ImGui::MenuItem("Load Stimulus Video")) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = context.root_dir;
                config.flags = ImGuiFileDialogFlags_Modal;
                ImGuiFileDialog::Instance()->OpenDialog(
                    "ChooseStimulus",
                    "Choose Stimulus Video",
                    ".mp4",
                    config);
            }
            if (!context.ui_path_config.preferred_roots.empty() &&
                ImGui::BeginMenu("Path Preset")) {
                for (const auto& preset_path :
                     context.ui_path_config.preferred_roots) {
                    bool selected = (preset_path == context.start_folder_name);
                    if (ImGui::MenuItem(preset_path.c_str(), nullptr, selected)) {
                        context.start_folder_name = preset_path;
                        std::cout << "[UIPathConfig] Start path set to: "
                                  << context.start_folder_name << std::endl;
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (context.video_loaded && context.show_legacy_skeleton_menu) {
            if (ImGui::BeginMenu("Legacy Skeleton")) {
                for (const auto& element : context.skeleton_map) {
                    if (ImGui::MenuItem(element.first.c_str(),
                                        nullptr,
                                        context.active_skeleton_name ==
                                            element.first,
                                        !context.skeleton_chosen)) {
                        if (element.second == SP_LOAD) {
                            IGFD::FileDialogConfig config;
                            config.countSelectionMax = 1;
                            config.path = context.skeleton_dir;
                            config.flags = ImGuiFileDialogFlags_Modal;
                            ImGuiFileDialog::Instance()->OpenDialog(
                                "ChooseSkeleton",
                                "Choose Skeleton",
                                ".json",
                                config);
                        } else {
                            result.skeleton_selection =
                                FileBrowserSkeletonSelection{element.first,
                                                             element.second};
                        }
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Detection")) {
                if (context.cpu_buffer_toggle) {
                    if (ImGui::MenuItem("YOLOv5")) {
                        result.detection_action =
                            FileBrowserDetectionAction::YOLOv5;
                    }
                } else {
                    if (ImGui::MenuItem("YOLOv8")) {
                        result.detection_action =
                            FileBrowserDetectionAction::YOLOv8;
                    }

                    if (ImGui::MenuItem("YOLOv8Pose")) {
                        result.detection_action =
                            FileBrowserDetectionAction::YOLOv8Pose;
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

    if (!context.video_loaded) {
        const char* items[] = {"CPU Buffer", "GPU Buffer"};
        int buffer_type_index = context.scene_use_cpu_buffer ? 0 : 1;
        ImGui::Combo("Buffer Type",
                     &buffer_type_index,
                     items,
                     IM_ARRAYSIZE(items));
        context.scene_use_cpu_buffer = (buffer_type_index == 0);

        ImGui::InputInt("Buffer Size", &context.label_buffer_size);
        context.label_buffer_size = std::max(1, context.label_buffer_size);
    }

    {
        const char* items[] = {"Full Resolution (1x)",
                               "Half-Resolution Preview (1/2)",
                               "Quarter-Resolution Preview (1/4)"};
        ImGui::Combo("Playback Preview Scale",
                     &context.playback_preview_scale_mode,
                     items,
                     IM_ARRAYSIZE(items));
        if (context.playback_preview_scale_mode != 0) {
            if (context.yolo_detection) {
                ImGui::TextDisabled(
                    "Preview scaling is temporarily disabled while YOLO inference is active.");
            } else if (!context.play_video) {
                ImGui::TextDisabled(
                    "Preview scaling applies only during playback; paused inspection remains full resolution.");
            } else if (context.scene_use_cpu_buffer) {
                ImGui::Text("Effective preview scale: %s (CPU resized preview)",
                            playbackPreviewScaleLabel(
                                context.playback_preview_scale_mode));
            } else {
                ImGui::Text("Effective preview scale: %s (GPU mip preview)",
                            playbackPreviewScaleLabel(
                                context.playback_preview_scale_mode));
            }
        }
    }

    {
        const char* items[] = {"Standard Renderer",
                               "Lightweight Playback Renderer"};
        ImGui::Combo("Playback Renderer",
                     &context.playback_renderer_mode,
                     items,
                     IM_ARRAYSIZE(items));
        if (context.playback_renderer_mode == 1) {
            if (!context.play_video) {
                ImGui::TextDisabled(
                    "The lightweight renderer applies only during playback; paused inspection keeps the full plot path.");
            } else {
                ImGui::Text("Active playback renderer: %s",
                            playbackRendererModeLabel(
                                context.playback_renderer_mode));
            }
        }
    }

    if (!context.stimulus_loaded) {
        ImGui::InputInt("Stimulus Buffer Size", &context.stimulus_buffer_size);
        context.stimulus_buffer_size =
            std::max(1, context.stimulus_buffer_size);
        {
            const char* items[] = {"Stimulus GPU Buffer",
                                   "Stimulus CPU Buffer"};
            int stimulus_buffer_mode = context.stimulus_use_cpu_buffer ? 1 : 0;
            ImGui::Combo("Stimulus Buffer Type",
                         &stimulus_buffer_mode,
                         items,
                         IM_ARRAYSIZE(items));
            context.stimulus_use_cpu_buffer = (stimulus_buffer_mode == 1);
        }
        {
            const char* items[] = {"Stimulus Software Decode",
                                   "Stimulus GPU Decode"};
            int stimulus_decode_mode =
                context.stimulus_use_software_decode ? 0 : 1;
            ImGui::Combo("Stimulus Decode Backend",
                         &stimulus_decode_mode,
                         items,
                         IM_ARRAYSIZE(items));
            context.stimulus_use_software_decode =
                (stimulus_decode_mode == 0);
        }
        ImGui::Text("Stimulus Buffer Size: %d", context.stimulus_buffer_size);
        ImGui::Text("Stimulus Decode Backend: %s",
                    context.stimulus_use_software_decode ? "Software" : "GPU");
        ImGui::Text("Stimulus Buffer Mode: %s",
                    context.stimulus_use_cpu_buffer ? "CPU" : "GPU");
    } else {
        ImGui::Text("Stimulus Buffer Size: %d",
                    context.loaded_stimulus_buffer_size);
        ImGui::Text("Stimulus Decode Backend: %s",
                    context.loaded_stimulus_use_software_decode ? "Software"
                                                                : "GPU");
        ImGui::Text("Stimulus Buffer Mode: %s",
                    context.loaded_stimulus_use_cpu_buffer ? "CPU" : "GPU");
    }

    if (context.video_loaded) {
        ImGui::InputInt("Seek Step", &context.seek_interval, 10, 100);
        ImGui::InputInt("Seek Accurate", &state.seek_accurate_frame_num, 1, 100);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            result.accurate_seek_target_frame = state.seek_accurate_frame_num;
        }

        auto now_wall = std::chrono::steady_clock::now();
        double wall_seconds =
            std::chrono::duration<double>(now_wall -
                                          context.last_wall_time_playspeed)
                .count();
        int frame_delta = context.current_frame_num - context.last_frame_num_playspeed;
        if (wall_seconds > 0.5 && context.play_video) {
            context.inst_speed =
                frame_delta / (context.video_fps * wall_seconds);
            context.last_frame_num_playspeed = context.current_frame_num;
            context.last_wall_time_playspeed = now_wall;
        }

        if (context.play_video) {
            ImGui::Text("Video FPS: %.1f", context.video_fps);
            ImGui::SliderFloat("Set Playback Speed",
                               &context.set_playback_speed,
                               0.1f,
                               1.0f,
                               "%.1fx");
            ImGui::Text("Current Playback Speed: %.2fx", context.inst_speed);
            ImGui::Text("Tip: If playback is slower than real-time, \n"
                        "collapse camera views to improve speed.");
        }
    }

    ImGui::End();
    return result;
}
