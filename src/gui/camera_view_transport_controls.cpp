#include "gui/camera_view_transport_controls.h"

#include "IconsForkAwesome.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

std::string format_transport_time(double t_seconds) {
    int64_t seconds = static_cast<int64_t>(std::max(0.0, t_seconds));
    const int64_t hours = seconds / 3600;
    seconds -= hours * 3600;
    const int64_t minutes = seconds / 60;
    seconds -= minutes * 60;

    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld",
                      static_cast<long long>(hours),
                      static_cast<long long>(minutes),
                      static_cast<long long>(seconds));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld",
                      static_cast<long long>(minutes),
                      static_cast<long long>(seconds));
    }
    return std::string(buffer);
}

void show_item_tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

}  // namespace

CameraViewTransportControlsResult drawCameraViewTransportControls(
    const CameraViewTransportControlsContext& context) {
    CameraViewTransportControlsResult result;
    const int64_t frame_count = std::max<int64_t>(0, context.total_num_frames);
    const int64_t maximum_frame = context.maximum_frame_number > 0
                                      ? context.maximum_frame_number
                                      : std::max<int64_t>(0, frame_count - 1);
    result.slider_frame_number =
        std::clamp(context.slider_frame_number, int64_t{0}, maximum_frame);

    crimson::workspace::WorkspaceCapabilities capabilities;
    capabilities.video_loaded = frame_count > 0;
    const bool controls_enabled = context.enabled && frame_count > 0;
    capabilities.playback_ready = controls_enabled;
    capabilities.playing = context.play_video;

    const auto submit_action =
        [&](CameraViewTransportAction action,
            crimson::workspace::Command command, int64_t magnitude = 1,
            std::optional<int64_t> target = std::nullopt,
            std::optional<crimson::playback::PlaybackSeekPhase> seek_phase =
                std::nullopt) {
            const auto intent = crimson::workspace::makePlaybackIntent(
                command, capabilities, context.current_display_frame,
                frame_count, target, magnitude);
            if (intent.has_value()) {
                result.action = action;
                result.intent = intent;
                if (seek_phase.has_value() &&
                    intent->kind == crimson::workspace::PlaybackIntentKind::Seek) {
                    result.seek_request =
                        crimson::playback::makePlaybackSeekRequest(
                            *seek_phase,
                            crimson::playback::PlaybackSeekOrigin::CameraControls,
                            intent->target_frame, frame_count);
                }
            }
        };

    if (!controls_enabled) {
        ImGui::BeginDisabled();
    }

    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    if (ImGui::Button(ICON_FK_FAST_BACKWARD)) {
        result.step_delta = -10;
        submit_action(CameraViewTransportAction::StepBackward,
                      crimson::workspace::Command::StepBackward, 10,
                      std::nullopt,
                      crimson::playback::PlaybackSeekPhase::Discrete);
    }
    show_item_tooltip("Back 10 frames");
    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(ICON_FK_STEP_BACKWARD)) {
        result.step_delta = -1;
        submit_action(CameraViewTransportAction::StepBackward,
                      crimson::workspace::Command::StepBackward, 1,
                      std::nullopt,
                      crimson::playback::PlaybackSeekPhase::Discrete);
    }
    show_item_tooltip("Previous frame");
    ImGui::SameLine(0.0f, spacing);

    if (frame_count > 0 && context.current_display_frame >= frame_count - 1) {
        const ImVec4 repeat_normal = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
        const ImVec4 repeat_hover = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
        const ImVec4 repeat_active = ImVec4(1.0f, 0.9f, 0.1f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, repeat_normal);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, repeat_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, repeat_active);
        if (ImGui::Button(ICON_FK_REPEAT)) {
            submit_action(CameraViewTransportAction::Restart,
                          crimson::workspace::Command::Seek, 1, 0,
                          crimson::playback::PlaybackSeekPhase::Discrete);
        }
        ImGui::PopStyleColor(3);
        show_item_tooltip("Restart from first frame");
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
        if (ImGui::Button(context.play_video ? ICON_FK_PAUSE : ICON_FK_PLAY)) {
            submit_action(CameraViewTransportAction::TogglePlayback,
                          crimson::workspace::Command::TogglePlayback);
        }
        ImGui::PopStyleColor(3);
        show_item_tooltip(context.play_video ? "Pause" : "Play");
    }

    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(ICON_FK_STEP_FORWARD)) {
        result.step_delta = 1;
        submit_action(CameraViewTransportAction::StepForward,
                      crimson::workspace::Command::StepForward, 1,
                      std::nullopt,
                      crimson::playback::PlaybackSeekPhase::Discrete);
    }
    show_item_tooltip("Next frame");
    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(ICON_FK_FAST_FORWARD)) {
        result.step_delta = 10;
        submit_action(CameraViewTransportAction::StepForward,
                      crimson::workspace::Command::StepForward, 10,
                      std::nullopt,
                      crimson::playback::PlaybackSeekPhase::Discrete);
    }
    show_item_tooltip("Forward 10 frames");
    ImGui::SameLine();

    const double current_time_sec =
        context.video_fps > 0.0 ? result.slider_frame_number / context.video_fps
                                : 0.0;
    const double total_time_sec =
        context.video_fps > 0.0 ? maximum_frame / context.video_fps : 0.0;
    const std::string current_str = format_transport_time(current_time_sec);
    const std::string total_str = format_transport_time(total_time_sec);
    const std::string time_label = current_str + " / " + total_str;
    const float time_width = ImGui::CalcTextSize(time_label.c_str()).x;
    ImGui::SetNextItemWidth(
        std::max(60.0f, ImGui::GetContentRegionAvail().x - time_width -
                            ImGui::GetStyle().ItemSpacing.x));
    const int64_t minimum_frame = 0;
    result.slider_just_changed = ImGui::SliderScalar(
        "##camera-transport-frame", ImGuiDataType_S64,
        &result.slider_frame_number, &minimum_frame, &maximum_frame, "");
    result.slider_active = ImGui::IsItemActive();
    result.slider_released = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::TextUnformatted(time_label.c_str());

    if (result.slider_just_changed && result.slider_active) {
        submit_action(CameraViewTransportAction::SeekPreview,
                      crimson::workspace::Command::Seek, 1,
                      result.slider_frame_number,
                      crimson::playback::PlaybackSeekPhase::Preview);
    }
    if (result.slider_released) {
        submit_action(CameraViewTransportAction::SeekCommit,
                      crimson::workspace::Command::Seek, 1,
                      result.slider_frame_number,
                      crimson::playback::PlaybackSeekPhase::Commit);
    }

    if (!controls_enabled) {
        ImGui::EndDisabled();
        result = CameraViewTransportControlsResult{};
        result.slider_frame_number =
            std::clamp(context.slider_frame_number, int64_t{0}, maximum_frame);
    }

    return result;
}

CameraViewPlaybackShortcutsResult
handleCameraViewPlaybackShortcuts(bool enabled) {
    const ImGuiIO& io = ImGui::GetIO();
    return crimson::workspace::resolvePlaybackShortcut(
        crimson::workspace::PlaybackShortcutInput{
            enabled,
            io.WantTextInput,
            ImGui::IsKeyPressed(ImGuiKey_Space, false),
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false),
            ImGui::IsKeyPressed(ImGuiKey_RightArrow, false),
            ImGui::IsKeyPressed(ImGuiKey_Comma, false),
            ImGui::IsKeyPressed(ImGuiKey_Period, false),
            io.KeyShift,
        });
}
