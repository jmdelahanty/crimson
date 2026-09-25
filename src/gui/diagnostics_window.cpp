#include "gui/diagnostics_window.h"
#include "gui/frame_debug_window.h"

#include "imgui.h"

DiagnosticsWindowResult drawDiagnosticsWindow(
    const FrameDebugWindowContext* frame_context,
    const DiagnosticsRuntimeStatus& runtime_status) {
    DiagnosticsWindowResult result;

    if (!ImGui::Begin("Diagnostics")) {
        ImGui::End();
        return result;
    }

    if (runtime_status.swap_interval >= 0) {
        bool vsync_enabled = runtime_status.swap_interval != 0;
        if (ImGui::Checkbox("VSync", &vsync_enabled)) {
            result.requested_swap_interval = vsync_enabled ? 1 : 0;
        }
        if (vsync_enabled) {
            ImGui::TextDisabled("VSync is on. Turn it off only for playback diagnostics.");
        }
    }
    const auto& update = runtime_status.app_update;
    if (update.install_metadata_found) {
        if (update.update_available) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.2f, 1.0f),
                "Published app drop differs: %s -> %s",
                update.installed_release_name.c_str(),
                update.latest_release_name.c_str());
            if (!update.current_root.empty()) {
#ifdef _WIN32
                constexpr auto installer = "install_crimson.ps1";
#else
                constexpr auto installer = "install_crimson.sh";
#endif
                ImGui::TextWrapped("To update, rerun %s from %s", installer,
                                   update.current_root.c_str());
            }
        } else if (update.comparison_available) {
            ImGui::TextDisabled("Update status: up to date (%s)",
                                update.installed_release_name.c_str());
        } else {
            ImGui::TextWrapped("Update status: %s", update.status_detail.c_str());
        }
        result.request_refresh_update_check =
            ImGui::SmallButton("Refresh Update Check");
        ImGui::Separator();
    }
    if (!runtime_status.worker_errors.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Background worker errors detected:");
        for (const auto& [stream_name, message] : runtime_status.worker_errors) {
            ImGui::TextWrapped("%s: %s", stream_name.c_str(), message.c_str());
        }
        result.request_clear_worker_errors = ImGui::SmallButton("Clear Worker Errors");
        ImGui::Separator();
    }

    // Worker/update controls must remain available with no loaded recording or
    // when Frame Inspect is hidden. Only the frame-specific debug needs a context.
    if (!frame_context) {
        ImGui::End();
        return result;
    }
    const auto& context = *frame_context;
    ImGui::Text("Runtime State:");
    ImGui::Text("Display target frame: %d", context.display_target_frame);
    ImGui::Text("Slider frame: %d", context.slider_frame);
    if (context.frame_sync_valid_slots >= 0 &&
        context.frame_sync_empty_slots >= 0) {
        ImGui::Text("Buffer frames: valid=%d empty_remaining=%d total=%u",
                    context.frame_sync_valid_slots,
                    context.frame_sync_empty_slots,
                    context.scene_buffer_size);
    }
    if (context.frame_sync_recording_remaining >= 0 &&
        context.frame_sync_recording_total > 0) {
        ImGui::Text("Recording decode: latest=%d remaining=%d total=%d",
                    context.frame_sync_latest_decoded,
                    context.frame_sync_recording_remaining,
                    context.frame_sync_recording_total);
    }
    if (!context.frame_sync_debug_line.empty()) {
        ImGui::TextWrapped("Frame sync: %s",
                           context.frame_sync_debug_line.c_str());
    }

    if (context.stimulus_repository != nullptr &&
        context.stimulus_repository->hasMapping()) {
        ImGui::Separator();
        ImGui::Text("Stimulus Alignment:");
        ImGui::Text("Mapping variant: %s",
                    context.stimulus_repository->hasCorrectedMapping()
                        ? "corrected"
                        : "legacy");
        if (auto metadata_index =
                context.stimulus_repository->metadataIndexForCameraFrame(
                    context.current_frame_num)) {
            ImGui::Text("Frame metadata index: %d", *metadata_index);
        }
        if (auto first_cam =
                context.stimulus_repository->firstCameraFrameWithStimulus()) {
            if (auto first_stim =
                    context.stimulus_repository->firstStimulusFrame()) {
                ImGui::Text("First mapped camera frame: %d -> Stim %d",
                            *first_cam,
                            *first_stim);
            } else {
                ImGui::Text("First mapped camera frame: %d", *first_cam);
            }
        }
        ImGui::Text("Camera frame offset: %lld",
                    static_cast<long long>(
                        context.stimulus_repository->cameraFrameOffset()));
    }

    ImGui::Separator();
    ImGui::Text("Decode Debug:");
    if (ImGui::Button("Dump Decode Buffers")) {
        result.request_dump_decode_buffers = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Random Seek + Dump")) {
        result.request_random_seek_dump = true;
    }
    ImGui::TextWrapped("Output dir: CRIMSON_BUFFER_DUMP_DIR (default %s)",
                       context.default_buffer_dump_root.c_str());
    if (!context.decode_debug_status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                           "%s",
                           context.decode_debug_status.c_str());
    }
    ImGui::TextWrapped(
        "Box colors: clean=blue, interpolated=orange, manual=teal");

    ImGui::End();
    return result;
}
