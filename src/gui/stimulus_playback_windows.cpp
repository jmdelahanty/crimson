#include "gui/stimulus_playback_windows.h"

#include "debug_flags.h"
#include "frame_slot.h"
#include "global.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

namespace {

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

constexpr int kStimulusDiscardSlackFrames = 1;

bool isStimulusFrameClose(int candidate_frame, int target_frame) {
    if (candidate_frame < 0 || target_frame < 0) {
        return false;
    }
    return std::abs(candidate_frame - target_frame) <=
           kStimulusDiscardSlackFrames;
}

}  // namespace

StimulusPlaybackPresentationResult updateStimulusPlaybackPresentation(
    const StimulusPlaybackPresentationContext& context,
    StimulusPlaybackPresentationState& state) {
    const auto update_start = std::chrono::steady_clock::now();
    StimulusPlaybackPresentationResult result;
    auto& stimulus_player = context.stimulus_player;
    auto& playback_state = context.playback_state;
    auto& seek_progress = context.seek_progress;

    const bool mapping_available = context.stimulus_repository != nullptr &&
                                   context.stimulus_repository->hasMapping();
    const bool seek_needs_stimulus =
        (seek_progress.state == SeekState::WaitingStimulus);
    const bool base_decode_request =
        playback_state.play_video || !playback_state.pause_seeked ||
        seek_needs_stimulus;
    bool decoder_requested = base_decode_request;

    int target_stimulus_frame = playback_state.current_stimulus_frame;
    result.target_stimulus_frame = target_stimulus_frame;
    int effective_target_frame = target_stimulus_frame;
    if (effective_target_frame < 0) {
        std::optional<int32_t> first_stim;
        if (mapping_available) {
            auto opt_first = context.stimulus_repository->firstStimulusFrame();
            if (opt_first) {
                first_stim = *opt_first;
            }
        }
        effective_target_frame = first_stim.value_or(0);
    }

    int discard_threshold = effective_target_frame - kStimulusDiscardSlackFrames;
    if (discard_threshold < 0) {
        discard_threshold = 0;
    }
    discardStimulusFramesOlderThan(stimulus_player, discard_threshold);

    if (stimulus_player.playback_catchup_seek_in_flight) {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        if (stimulus_player.seek.seek_done &&
            stimulus_player.seek.settled_seek_id ==
                stimulus_player.playback_catchup_seek_id) {
            stimulus_player.playback_catchup_seek_in_flight = false;
        }
    }

    if (playback_state.play_video && mapping_available &&
        target_stimulus_frame >= 0 &&
        seek_progress.state != SeekState::WaitingCameras &&
        seek_progress.state != SeekState::WaitingStimulus) {
        const int stimulus_progress_frame =
            std::max(context.latest_decoded_frame,
                     stimulus_player.last_displayed_frame);
        const int stimulus_lag_frames =
            (stimulus_progress_frame >= 0)
                ? (target_stimulus_frame - stimulus_progress_frame)
                : target_stimulus_frame;
        const int catchup_threshold =
            std::max(8, static_cast<int>(std::ceil(stimulus_player.fps * 0.35)));
        const int catchup_seek_backoff =
            std::max(1, static_cast<int>(std::ceil(stimulus_player.fps * 0.05)));
        const auto now = std::chrono::steady_clock::now();
        const bool catchup_cooldown_elapsed =
            !stimulus_player.playback_catchup_seek_in_flight ||
            (now - stimulus_player.playback_catchup_last_request) >=
                std::chrono::milliseconds(200);
        const bool target_has_advanced =
            stimulus_player.playback_catchup_target_frame < 0 ||
            target_stimulus_frame >
                (stimulus_player.playback_catchup_target_frame +
                 std::max(2, catchup_threshold / 2));

        if (stimulus_lag_frames > catchup_threshold &&
            catchup_cooldown_elapsed && target_has_advanced &&
            context.catchup_seek_generation != nullptr) {
            const int catchup_seek_frame =
                std::max(0, target_stimulus_frame - catchup_seek_backoff);
            const uint64_t catchup_seek_id = (*context.catchup_seek_generation)++;
            {
                std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                stimulus_player.seek.seek_frame =
                    static_cast<uint64_t>(catchup_seek_frame);
                stimulus_player.seek.seek_id = catchup_seek_id;
                stimulus_player.seek.use_seek = true;
                stimulus_player.seek.seek_done = false;
                stimulus_player.seek.seek_accurate = false;
            }
            stimulus_player.last_displayed_frame = -1;
            stimulus_player.throttled = false;
            stimulus_player.throttle_resume_frame = -1;
            stimulus_player.playback_catchup_seek_in_flight = true;
            stimulus_player.playback_catchup_seek_id = catchup_seek_id;
            stimulus_player.playback_catchup_target_frame = catchup_seek_frame;
            stimulus_player.playback_catchup_last_request = now;
            decoder_requested = true;
            if (crimson_stimulus_debug_logs_enabled()) {
                std::cout << "[Stimulus] catch-up seek target="
                          << target_stimulus_frame
                          << " progress=" << stimulus_progress_frame
                          << " lag=" << stimulus_lag_frames
                          << " seek_frame=" << catchup_seek_frame
                          << " seek_id=" << catchup_seek_id << std::endl;
            }
        }
    }

    if (base_decode_request) {
        const double fps_ratio =
            (context.video_fps > 0.0) ? (stimulus_player.fps / context.video_fps)
                                      : 1.0;
        const double high_headroom =
            fps_ratio * 6.0 + stimulus_player.fps * 0.20;
        const double low_headroom =
            fps_ratio * 2.0 + stimulus_player.fps * 0.05;
        const int high_threshold =
            effective_target_frame + static_cast<int>(high_headroom);
        const int low_threshold =
            effective_target_frame + static_cast<int>(low_headroom);
        const int newest_frame = getNewestStimulusFrame(stimulus_player);

        if (context.decoder_active_now && newest_frame >= 0 &&
            newest_frame > high_threshold) {
            if (!stimulus_player.throttled &&
                crimson_stimulus_debug_logs_enabled()) {
                std::cout << "[Stimulus] throttling decode: newest="
                          << newest_frame
                          << " high_threshold=" << high_threshold << std::endl;
            }
            decoder_requested = false;
            stimulus_player.throttled = true;
            stimulus_player.throttle_resume_frame = low_threshold;
        } else if (!context.decoder_active_now && stimulus_player.throttled) {
            const int resume_target =
                std::max(low_threshold, stimulus_player.throttle_resume_frame);
            if (newest_frame <= resume_target) {
                if (crimson_stimulus_debug_logs_enabled()) {
                    std::cout << "[Stimulus] resuming decode: newest="
                              << newest_frame
                              << " resume_target=" << resume_target
                              << std::endl;
                }
                decoder_requested = true;
                stimulus_player.throttled = false;
                stimulus_player.throttle_resume_frame = -1;
            } else {
                decoder_requested = false;
            }
        }
    } else {
        stimulus_player.throttled = false;
        stimulus_player.throttle_resume_frame = -1;
        decoder_requested = false;
    }

    if (!decoder_requested && target_stimulus_frame >= 0) {
        int candidate_index =
            findStimulusBuffer(stimulus_player, target_stimulus_frame);
        int candidate_frame = -1;
        if (candidate_index >= 0 && stimulus_player.display_buffer) {
            auto metadata = frameSlotSnapshotReadable(
                stimulus_player.display_buffer[candidate_index]);
            if (metadata.has_value()) {
                candidate_frame = metadata->frame_number;
            }
        }
        if (!isStimulusFrameClose(candidate_frame, target_stimulus_frame)) {
            decoder_requested = true;
        }
    }

    if (decoder_requested != state.last_decoder_logged) {
        if (crimson_stimulus_debug_logs_enabled()) {
            std::cout << "[Stimulus] decoder_should_run="
                      << (decoder_requested ? "true" : "false")
                      << " (play=" << (playback_state.play_video ? "true" : "false")
                      << " pause_seeked="
                      << (playback_state.pause_seeked ? "true" : "false")
                      << ")" << std::endl;
        }
        state.last_decoder_logged = decoder_requested;
    }
    result.decoder_requested = decoder_requested;

    if (mapping_available && target_stimulus_frame >= 0 &&
        target_stimulus_frame != stimulus_player.last_displayed_frame) {
        int buffer_index =
            findStimulusBuffer(stimulus_player, target_stimulus_frame);
        if (buffer_index != -1) {
            int buffered_frame = -1;
            auto metadata = frameSlotSnapshotReadable(
                stimulus_player.display_buffer[buffer_index]);
            if (metadata.has_value()) {
                buffered_frame = metadata->frame_number;
            }
            if (isStimulusFrameClose(buffered_frame, target_stimulus_frame)) {
                uploadStimulusFrameToTexture(stimulus_player, buffer_index);
                stimulus_player.last_displayed_frame = buffered_frame;
                result.uploaded_frame = true;
                if (crimson_stimulus_debug_logs_enabled()) {
                    std::cout << "[Stimulus] uploaded frame "
                              << buffered_frame
                              << " for target " << target_stimulus_frame
                              << " (buffer " << buffer_index << ")"
                              << std::endl;
                }
            }
        }
    }

    result.displayed_stimulus_frame = stimulus_player.last_displayed_frame;
    result.update_ms =
        durationMs(std::chrono::steady_clock::now() - update_start);
    if (crimson_stimulus_debug_logs_enabled()) {
        const bool changed =
            result.target_stimulus_frame !=
                state.last_logged_target_stimulus_frame ||
            result.displayed_stimulus_frame !=
                state.last_logged_displayed_stimulus_frame ||
            result.decoder_requested != state.last_logged_decoder_requested ||
            stimulus_player.throttled != state.last_logged_throttled ||
            result.uploaded_frame;
        if (state.presentation_debug_logs < 12 || changed) {
            std::cout << "[StimulusPresentation] update"
                      << " camera_frame=" << context.current_frame_num
                      << " target_stimulus_frame="
                      << result.target_stimulus_frame
                      << " displayed_stimulus_frame="
                      << result.displayed_stimulus_frame
                      << " latest_decoded_frame="
                      << context.latest_decoded_frame
                      << " decoder_requested="
                      << (result.decoder_requested ? "true" : "false")
                      << " uploaded_frame="
                      << (result.uploaded_frame ? "true" : "false")
                      << " throttled="
                      << (stimulus_player.throttled ? "true" : "false")
                      << " seek_state=" << seekStateName(seek_progress.state)
                      << " update_ms=" << result.update_ms << std::endl;
            state.last_logged_target_stimulus_frame =
                result.target_stimulus_frame;
            state.last_logged_displayed_stimulus_frame =
                result.displayed_stimulus_frame;
            state.last_logged_decoder_requested = result.decoder_requested;
            state.last_logged_throttled = stimulus_player.throttled;
            state.presentation_debug_logs++;
        }
    }
    return result;
}

StimulusPlaybackDebugWindowsResult drawStimulusPlaybackDebugWindows(
    const StimulusPlaybackDebugWindowsContext& context) {
    StimulusPlaybackDebugWindowsResult result;
    auto& stimulus_player = context.stimulus_player;
    auto& playback_state = context.playback_state;
    auto& seek_progress = context.seek_progress;

    const bool mapping_available = context.stimulus_repository != nullptr &&
                                   context.stimulus_repository->hasMapping();
    const int target_stimulus_frame = playback_state.current_stimulus_frame;

    ImGui::SetNextWindowSize(ImVec2(480.0f, 360.0f),
                             ImGuiCond_FirstUseEver);
    const auto stimulus_window_ui_start = std::chrono::steady_clock::now();
    bool stimulus_visible = ImGui::Begin(stimulus_player.window_name.c_str());
    if (stimulus_visible) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspect = (stimulus_player.width > 0 && stimulus_player.height > 0)
                           ? static_cast<float>(stimulus_player.height) /
                                 static_cast<float>(stimulus_player.width)
                           : 1.0f;
        float display_width = avail.x;
        float display_height = display_width * aspect;
        if (display_height > avail.y && avail.y > 0.0f) {
            display_height = avail.y;
            display_width = display_height / std::max(aspect, 1e-3f);
        }
        if (display_width <= 0.0f || display_height <= 0.0f) {
            display_width = static_cast<float>(stimulus_player.width);
            display_height = static_cast<float>(stimulus_player.height);
        }

        if (stimulus_player.last_displayed_frame >= 0) {
            ImGui::Image((ImTextureID)(intptr_t)stimulus_player.texture,
                         ImVec2(display_width, display_height));
        } else {
            ImGui::Dummy(ImVec2(display_width, display_height));
            ImGui::TextUnformatted("Waiting for stimulus frame...");
        }

        ImGui::Separator();
        const bool displayed_frame_is_close =
            isStimulusFrameClose(stimulus_player.last_displayed_frame,
                                 target_stimulus_frame);
        if (mapping_available && target_stimulus_frame >= 0 &&
            !displayed_frame_is_close) {
            ImGui::TextUnformatted("Awaiting stimulus frame decode...");
        }
        if (!mapping_available) {
            ImGui::TextUnformatted("Stimulus alignment not available.");
        } else if (target_stimulus_frame < 0) {
            ImGui::Text("Stimulus inactive for camera frame %d",
                        context.current_frame_num);
        } else {
            ImGui::Text("Camera frame %d -> Stimulus frame %d",
                        context.current_frame_num, target_stimulus_frame);
        }
        ImGui::Text("Latest decoded stimulus frame: %d",
                    context.latest_decoded_frame);
        ImGui::Separator();
        ImGui::Text("Seek id: %lu  State: %s", seek_progress.seek_id,
                    seekStateName(seek_progress.state));
        if (seek_progress.state == SeekState::WaitingCameras) {
            ImGui::Text("Cameras: %d / %d settled",
                        seek_progress.cameras_settled,
                        seek_progress.cameras_total);
        }
        if (seek_progress.state == SeekState::WaitingStimulus) {
            ImGui::Text("Stimulus target: %d",
                        seek_progress.target_stimulus_frame);
        }
        ImGui::Separator();
        ImGui::Text("Stimulus video: %s", stimulus_player.video_path.c_str());
        ImGui::Text("Resolution: %u x %u  |  %.2f fps",
                    stimulus_player.width, stimulus_player.height,
                    stimulus_player.fps);
    }
    ImGui::End();
    result.stimulus_window_ui_ms =
        durationMs(std::chrono::steady_clock::now() - stimulus_window_ui_start);

    ImGui::SetNextWindowSize(ImVec2(500.0f, 440.0f), ImGuiCond_FirstUseEver);
    const auto stimulus_buffer_window_ui_start =
        std::chrono::steady_clock::now();
    if (ImGui::Begin("Stimulus Frames in Buffer")) {
        struct StimulusBufferListItem {
            int slot = -1;
            int frame = -1;
        };
        std::vector<StimulusBufferListItem> stimulus_buffer_items;
        if (stimulus_player.display_buffer && stimulus_player.buffer_size > 0) {
            stimulus_buffer_items.reserve(stimulus_player.buffer_size);
            for (int i = 0; i < stimulus_player.buffer_size; ++i) {
                auto metadata =
                    frameSlotSnapshotReadable(stimulus_player.display_buffer[i]);
                if (!metadata.has_value() || metadata->frame_number < 0) {
                    continue;
                }
                stimulus_buffer_items.push_back({i, metadata->frame_number});
            }
        }
        std::sort(stimulus_buffer_items.begin(), stimulus_buffer_items.end(),
                  [](const StimulusBufferListItem& a,
                     const StimulusBufferListItem& b) {
                      if (a.frame == b.frame) {
                          return a.slot < b.slot;
                      }
                      return a.frame < b.frame;
                  });

        ImGui::Text("Valid frames: %zu / %d", stimulus_buffer_items.size(),
                    std::max(0, stimulus_player.buffer_size));
        if (target_stimulus_frame >= 0) {
            ImGui::Text("Target stimulus frame: %d", target_stimulus_frame);
        } else {
            ImGui::TextDisabled("Target stimulus frame: (none)");
        }
        ImGui::Text("Last displayed frame: %d",
                    stimulus_player.last_displayed_frame);
        ImGui::Text("Latest decoded frame: %d", context.latest_decoded_frame);
        ImGui::Separator();

        int selected_item = -1;
        if (target_stimulus_frame >= 0) {
            int best_distance = std::numeric_limits<int>::max();
            int best_frame = std::numeric_limits<int>::min();
            for (int i = 0; i < static_cast<int>(stimulus_buffer_items.size());
                 ++i) {
                const auto& item = stimulus_buffer_items[i];
                if (item.frame == target_stimulus_frame) {
                    selected_item = i;
                    break;
                }
                const int distance = std::abs(item.frame - target_stimulus_frame);
                if (distance < best_distance ||
                    (distance == best_distance && item.frame > best_frame)) {
                    best_distance = distance;
                    best_frame = item.frame;
                    selected_item = i;
                }
            }
        }

        if (stimulus_buffer_items.empty()) {
            ImGui::TextDisabled(
                "No decoded stimulus frames currently buffered.");
        } else {
            ImGui::TextDisabled(
                "Click an item to upload that buffered frame.");
            for (int i = 0; i < static_cast<int>(stimulus_buffer_items.size());
                 ++i) {
                const auto& item = stimulus_buffer_items[i];
                char label[128];
                if (target_stimulus_frame >= 0) {
                    const int delta = item.frame - target_stimulus_frame;
                    snprintf(label, sizeof(label),
                             "Frame %d (slot %d, delta %+d)", item.frame,
                             item.slot, delta);
                } else {
                    snprintf(label, sizeof(label), "Frame %d (slot %d)",
                             item.frame, item.slot);
                }
                if (ImGui::Selectable(label, selected_item == i)) {
                    uploadStimulusFrameToTexture(stimulus_player, item.slot);
                    stimulus_player.last_displayed_frame = item.frame;
                    if (crimson_stimulus_debug_logs_enabled()) {
                        std::cout << "[Stimulus] debug upload frame "
                                  << item.frame << " (slot " << item.slot
                                  << ")" << std::endl;
                    }
                }
            }
        }
    }
    ImGui::End();
    result.stimulus_buffer_window_ui_ms = durationMs(
        std::chrono::steady_clock::now() - stimulus_buffer_window_ui_start);

    return result;
}
