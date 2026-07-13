#include "gui/file_browser_window.h"

#include <ImGuiFileDialog.h>
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
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

template <size_t N>
void setPathBuffer(std::array<char, N>& buffer, const std::string& value) {
    buffer.fill('\0');
    const size_t copy_size = std::min(value.size(), N - 1);
    std::memcpy(buffer.data(), value.data(), copy_size);
}

void initializePathEditor(FileBrowserWindowState& state,
                          const UiPathConfig& config) {
    setPathBuffer(state.default_start_path_buffer,
                  config.default_start_path);
    state.preferred_root_buffers.clear();
    state.preferred_root_buffers.reserve(config.preferred_roots.size());
    for (const auto& root : config.preferred_roots) {
        std::array<char, FileBrowserWindowState::kPathBufferSize> buffer{};
        setPathBuffer(buffer, root);
        state.preferred_root_buffers.push_back(buffer);
    }
    state.path_browse_target = -2;
    state.path_editor_message.clear();
    state.path_editor_message_is_error = false;
}

UiPathConfig pathConfigFromEditor(const FileBrowserWindowState& state,
                                  const UiPathConfig& current_config) {
    UiPathConfig config;
    config.default_start_path = state.default_start_path_buffer.data();
    config.loaded_from = current_config.loaded_from;
    config.preferred_roots.reserve(state.preferred_root_buffers.size());
    for (const auto& buffer : state.preferred_root_buffers) {
        config.preferred_roots.emplace_back(buffer.data());
    }
    return config;
}

void openPathFolderDialog(FileBrowserWindowState& state,
                          int target,
                          const std::string& current_path,
                          const std::string& fallback_path) {
    state.path_browse_target = target;
    IGFD::FileDialogConfig config;
    config.countSelectionMax = 1;
    config.path = IsDirectoryNoThrow(current_path) ? current_path : fallback_path;
    config.flags = ImGuiFileDialogFlags_Modal;
    ImGuiFileDialog::Instance()->OpenDialog(
        "ChooseUiPathPresetFolder",
        "Choose Path Preset Directory",
        nullptr,
        config);
}

void textDisabledWrapped(const char* label, const std::string& value) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(label, value.c_str());
    ImGui::PopStyleColor();
}

void drawPathEditor(const FileBrowserWindowContext& context,
                    FileBrowserWindowState& state,
                    FileBrowserWindowResult& result) {
    if (state.request_open_path_editor) {
        ImGui::OpenPopup("Edit Path Presets");
        state.request_open_path_editor = false;
    }

    ImGui::SetNextWindowSize(ImVec2(900.0f, 500.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Edit Path Presets", nullptr)) {
        return;
    }

    ImGui::TextWrapped(
        "Edit the directories shown in File > Path Preset. Apply changes "
        "to this session, or save them to your user configuration.");
    if (!context.ui_path_config.loaded_from.empty()) {
        textDisabledWrapped("Loaded from: %s",
                            context.ui_path_config.loaded_from);
    }
    if (auto user_path = GetCrimsonUserUiPathConfigPath()) {
        textDisabledWrapped("User configuration: %s", user_path->string());
    }

    ImGui::SeparatorText("Default start path");
    ImGui::SetNextItemWidth(-100.0f);
    ImGui::InputText("##default_start_path",
                     state.default_start_path_buffer.data(),
                     state.default_start_path_buffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...##default_start_path")) {
        openPathFolderDialog(
            state,
            -1,
            state.default_start_path_buffer.data(),
            context.start_folder_name);
    }

    ImGui::SeparatorText("Preferred roots");
    int remove_index = -1;
    for (size_t index = 0; index < state.preferred_root_buffers.size(); ++index) {
        auto& buffer = state.preferred_root_buffers[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(-238.0f);
        ImGui::InputText("##preferred_root", buffer.data(), buffer.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            openPathFolderDialog(state,
                                 static_cast<int>(index),
                                 buffer.data(),
                                 context.start_folder_name);
        }
        ImGui::SameLine();
        if (ImGui::Button("Use as default")) {
            setPathBuffer(state.default_start_path_buffer,
                          std::string(buffer.data()));
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            remove_index = static_cast<int>(index);
        }
        ImGui::PopID();
    }
    if (remove_index >= 0) {
        state.preferred_root_buffers.erase(
            state.preferred_root_buffers.begin() + remove_index);
    }

    if (ImGui::Button("Add path")) {
        state.preferred_root_buffers.emplace_back();
    }

    if (!state.path_editor_message.empty()) {
        const ImVec4 color = state.path_editor_message_is_error
                                 ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                                 : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", state.path_editor_message.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    auto apply_editor_config = [&](bool save) {
        UiPathConfig candidate =
            pathConfigFromEditor(state, context.ui_path_config);
        UiPathConfig normalized;
        std::string error_message;
        if (!NormalizeUiPathConfig(candidate, normalized, error_message)) {
            state.path_editor_message = error_message;
            state.path_editor_message_is_error = true;
            return;
        }

        if (save) {
            std::filesystem::path saved_path;
            if (!SaveUserUiPathConfig(normalized,
                                      saved_path,
                                      error_message)) {
                state.path_editor_message = error_message;
                state.path_editor_message_is_error = true;
                return;
            }
            normalized.loaded_from = saved_path.string();
            state.path_editor_message = "Saved to " + saved_path.string();
            std::cout << "[UIPathConfig] Saved user preferences: "
                      << saved_path << std::endl;
        } else {
            state.path_editor_message = "Applied to this session.";
        }

        state.path_editor_message_is_error = false;
        initializePathEditor(state, normalized);
        state.path_editor_message =
            save ? "Saved to " + normalized.loaded_from
                 : "Applied to this session.";
        result.updated_path_config = std::move(normalized);
    };

    if (ImGui::Button("Apply")) {
        apply_editor_config(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        apply_editor_config(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset fields")) {
        initializePathEditor(state, context.ui_path_config);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
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
            if (ImGui::BeginMenu("Path Preset")) {
                for (const auto& preset_path :
                     context.ui_path_config.preferred_roots) {
                    bool selected = (preset_path == context.start_folder_name);
                    if (ImGui::MenuItem(preset_path.c_str(), nullptr, selected)) {
                        context.start_folder_name = preset_path;
                        std::cout << "[UIPathConfig] Start path set to: "
                                  << context.start_folder_name << std::endl;
                    }
                }
                if (!context.ui_path_config.preferred_roots.empty()) {
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Edit Path Presets...")) {
                    initializePathEditor(state, context.ui_path_config);
                    state.request_open_path_editor = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (context.video_loaded && context.can_offer_legacy_manual_labeling) {
            if (ImGui::BeginMenu("Legacy Manual Labeling")) {
                ImGui::TextDisabled("Legacy CSV/manual labeling workflow");
                ImGui::Separator();

                const bool legacy_tools_enabled =
                    state.enable_legacy_manual_labeling ||
                    context.legacy_manual_mode_active;
                if (!context.legacy_manual_mode_active) {
                    if (ImGui::MenuItem("Enable legacy manual labeling tools",
                                        nullptr,
                                        legacy_tools_enabled)) {
                        state.enable_legacy_manual_labeling =
                            !state.enable_legacy_manual_labeling;
                    }
                } else {
                    ImGui::TextDisabled("Legacy manual labeling mode is active.");
                }

                if (!context.active_skeleton_name.empty()) {
                    ImGui::Text("Active skeleton: %s",
                                context.active_skeleton_name.c_str());
                }

                if (legacy_tools_enabled) {
                    if (ImGui::BeginMenu("Skeleton")) {
                        for (const auto& element : context.skeleton_map) {
                            if (ImGui::MenuItem(
                                    element.first.c_str(),
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
                                        FileBrowserSkeletonSelection{
                                            element.first, element.second};
                                }
                            }
                        }
                        ImGui::EndMenu();
                    }
                } else {
                    ImGui::TextDisabled(
                        "Enable this menu to access the legacy manual "
                        "labeling tools.");
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

    drawPathEditor(context, state, result);

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

    if (ImGuiFileDialog::Instance()->Display("ChooseUiPathPresetFolder")) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string selected_path =
                ImGuiFileDialog::Instance()->GetCurrentPath();
            const auto selection = ImGuiFileDialog::Instance()->GetSelection();
            if (!selection.empty() &&
                IsDirectoryNoThrow(selection.begin()->second)) {
                selected_path = selection.begin()->second;
            }

            if (state.path_browse_target == -1) {
                setPathBuffer(state.default_start_path_buffer, selected_path);
            } else if (state.path_browse_target >= 0 &&
                       static_cast<size_t>(state.path_browse_target) <
                           state.preferred_root_buffers.size()) {
                setPathBuffer(
                    state.preferred_root_buffers[
                        static_cast<size_t>(state.path_browse_target)],
                    selected_path);
            }
        }
        state.path_browse_target = -2;
        ImGuiFileDialog::Instance()->Close();
    }
    return result;
}
