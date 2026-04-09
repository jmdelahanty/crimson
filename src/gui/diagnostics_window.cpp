#include "gui/diagnostics_window.h"

#include "imgui.h"

DiagnosticsWindowResult drawDiagnosticsWindow(
    const FrameDebugWindowContext& context) {
    DiagnosticsWindowResult result;

    if (!ImGui::Begin("Diagnostics")) {
        ImGui::End();
        return result;
    }

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

    if (context.zarr_loader.hasStimulusAlignment()) {
        ImGui::Separator();
        ImGui::Text("Stimulus Alignment:");
        if (context.zarr_loader.hasStimulusFrameMapping()) {
            ImGui::Text("Mapping variant: %s",
                        context.zarr_loader.hasCorrectedStimulusFrameMapping()
                            ? "corrected"
                            : "legacy");
            if (auto metadata_index =
                    context.zarr_loader.getStimulusMetadataIndexForCameraFrame(
                        context.current_frame_num)) {
                ImGui::Text("Frame metadata index: %d", *metadata_index);
            }
            if (auto first_cam =
                    context.zarr_loader.getFirstCameraFrameWithStimulus()) {
                if (auto first_stim =
                        context.zarr_loader.getFirstStimulusFrameNumber()) {
                    ImGui::Text("First mapped camera frame: %d -> Stim %d",
                                *first_cam,
                                *first_stim);
                } else {
                    ImGui::Text("First mapped camera frame: %d", *first_cam);
                }
            }
            ImGui::Text("Camera frame offset: %lld",
                        static_cast<long long>(
                            context.zarr_loader.getStimulusCameraFrameOffset()));
        } else {
            ImGui::Text("Mapping data not available");
        }
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
