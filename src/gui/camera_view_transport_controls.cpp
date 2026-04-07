#include "gui/camera_view_transport_controls.h"

#include "IconsForkAwesome.h"
#include "imgui.h"

#include <cstdio>
#include <string>

namespace {

std::string format_transport_time(float t_seconds) {
    int seconds = static_cast<int>(t_seconds);
    const int hours = seconds / 3600;
    seconds -= hours * 3600;
    const int minutes = seconds / 60;
    seconds -= minutes * 60;

    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes,
                      seconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, seconds);
    }
    return std::string(buffer);
}

}  // namespace

CameraViewTransportControlsResult drawCameraViewTransportControls(
    const CameraViewTransportControlsContext& context) {
    CameraViewTransportControlsResult result;
    result.slider_frame_number = context.slider_frame_number;

    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    if (ImGui::Button(ICON_FK_FAST_BACKWARD)) {
        result.step_delta = -10;
    }
    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(ICON_FK_STEP_BACKWARD)) {
        result.step_delta = -1;
    }
    ImGui::SameLine(0.0f, spacing);

    if (context.current_display_frame == (context.total_num_frames - 1)) {
        const ImVec4 repeat_normal = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
        const ImVec4 repeat_hover = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
        const ImVec4 repeat_active = ImVec4(1.0f, 0.9f, 0.1f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, repeat_normal);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, repeat_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, repeat_active);
        if (ImGui::Button(ICON_FK_REPEAT)) {
            result.seek_target_frame = 0;
            result.force_inaccurate_seek = false;
        }
        ImGui::PopStyleColor(3);
    } else {
        ImVec4 normal;
        ImVec4 hover;
        ImVec4 active;
        if (context.play_video) {
            normal = ImVec4(0.8f, 0.3f, 0.3f, 1.0f);
            hover = ImVec4(0.9f, 0.4f, 0.4f, 1.0f);
            active = ImVec4(0.7f, 0.2f, 0.2f, 1.0f);
        } else {
            normal = ImVec4(0.2f, 0.6f, 0.2f, 1.0f);
            hover = ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
            active = ImVec4(0.3f, 0.75f, 0.3f, 1.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, normal);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
        if (ImGui::Button(context.play_video ? ICON_FK_PAUSE
                                             : ICON_FK_PLAY)) {
            result.toggle_playback = true;
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(ICON_FK_STEP_FORWARD)) {
        result.step_delta = 1;
    }
    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(ICON_FK_FAST_FORWARD)) {
        result.step_delta = 10;
    }
    ImGui::SameLine();

    result.slider_just_changed =
        ImGui::SliderInt("##frame count", &result.slider_frame_number, 0,
                         context.estimated_num_frames);
    const bool slider_active = ImGui::IsItemActive();
    const bool slider_released = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();

    const float current_time_sec =
        context.video_fps > 0.0f ? result.slider_frame_number / context.video_fps
                                 : 0.0f;
    const float total_time_sec =
        context.video_fps > 0.0f ? context.estimated_num_frames / context.video_fps
                                 : 0.0f;
    const std::string current_str = format_transport_time(current_time_sec);
    const std::string total_str = format_transport_time(total_time_sec);
    ImGui::Text("%s / %s", current_str.c_str(), total_str.c_str());

    if (result.slider_just_changed && slider_active) {
        result.seek_target_frame = result.slider_frame_number;
        result.force_inaccurate_seek = true;
    }
    if (slider_released) {
        result.seek_target_frame = result.slider_frame_number;
        result.force_inaccurate_seek = false;
    }

    return result;
}

CameraViewPlaybackShortcutsResult handleCameraViewPlaybackShortcuts() {
    CameraViewPlaybackShortcutsResult result;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        result.toggle_playback = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
        result.step_delta = ImGui::GetIO().KeyShift ? -10 : -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
        result.step_delta = ImGui::GetIO().KeyShift ? 10 : 1;
    }
    return result;
}
