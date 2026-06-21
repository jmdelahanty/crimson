#include "playback_session_controller.h"
#include "frame_slot.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>

extern std::mutex g_seek_info_mutex;

double playbackPreviewScaleFactor(int playback_preview_scale_mode) {
    switch (playback_preview_scale_mode) {
    case 1:
        return 0.5;
    case 2:
        return 0.25;
    default:
        return 1.0;
    }
}

const char* playbackPreviewScaleLabel(int playback_preview_scale_mode) {
    switch (playback_preview_scale_mode) {
    case 1:
        return "1/2";
    case 2:
        return "1/4";
    default:
        return "1x";
    }
}

bool playbackPreviewIsActive(bool play_video,
                             bool yolo_detection,
                             int playback_preview_scale_mode) {
    return play_video && !yolo_detection &&
           playbackPreviewScaleFactor(playback_preview_scale_mode) < 1.0;
}

const char* playbackRendererModeLabel(int playback_renderer_mode) {
    switch (playback_renderer_mode) {
    case 1:
        return "lightweight";
    default:
        return "standard";
    }
}

bool playbackLightweightRendererIsActive(bool play_video,
                                         int playback_renderer_mode) {
    return play_video && playback_renderer_mode == 1;
}

PlaybackSessionController::PlaybackSessionController(
    const PlaybackSessionControllerContext& context)
    : context_(context) {}

int PlaybackSessionController::getVisibleCameraIndex() const {
    if (context_.scene == nullptr || context_.camera_names == nullptr ||
        context_.window_was_decoding == nullptr ||
        context_.window_need_decoding == nullptr ||
        context_.scene->num_cams <= 0 || context_.scene->size_of_buffer <= 0) {
        return -1;
    }
    for (int i = 0;
         i < context_.scene->num_cams &&
         i < static_cast<int>(context_.camera_names->size());
         ++i) {
        const auto& camera_name = (*context_.camera_names)[i];
        auto it = context_.window_was_decoding->find(camera_name);
        if (it != context_.window_was_decoding->end() && it->second) {
            return i;
        }
    }
    return 0;
}

void PlaybackSessionController::setCameraDecodeRequests(bool enabled) const {
    if (context_.camera_names == nullptr ||
        context_.window_need_decoding == nullptr) {
        return;
    }
    for (const auto& camera_name : *context_.camera_names) {
        auto it = context_.window_need_decoding->find(camera_name);
        if (it != context_.window_need_decoding->end()) {
            it->second.store(enabled);
        }
    }
}

int PlaybackSessionController::countBufferedStimulusFrames() const {
    if (context_.stimulus_player == nullptr ||
        !context_.stimulus_player->display_buffer ||
        context_.stimulus_player->buffer_size <= 0) {
        return 0;
    }
    int valid = 0;
    for (int i = 0; i < context_.stimulus_player->buffer_size; ++i) {
        auto metadata =
            frameSlotSnapshotReadable(context_.stimulus_player->display_buffer[i]);
        if (metadata.has_value() && metadata->frame_number >= 0) {
            ++valid;
        }
    }
    return valid;
}

int PlaybackSessionController::findNearestPausedBufferSlot(
    int visible_idx, int target_frame) const {
    if (context_.playback_state == nullptr || context_.scene == nullptr ||
        context_.playback_state->play_video || context_.scene->num_cams <= 0 ||
        context_.scene->size_of_buffer <= 0 || visible_idx < 0) {
        return -1;
    }

    int best_slot = -1;
    int best_distance = std::numeric_limits<int>::max();
    int best_frame = -1;
    for (int i = 0; i < context_.scene->size_of_buffer; ++i) {
        const auto& slot =
            context_.scene->cameras[visible_idx].display_buffer[i];
        if (slot.available_to_write || slot.frame_number < 0) {
            continue;
        }
        const int distance = std::abs(slot.frame_number - target_frame);
        if (distance < best_distance ||
            (distance == best_distance && slot.frame_number > best_frame)) {
            best_distance = distance;
            best_frame = slot.frame_number;
            best_slot = i;
        }
    }
    return best_slot;
}

bool PlaybackSessionController::stepPausedFrameFromBuffer(int target_frame) const {
    if (context_.playback_state == nullptr || context_.scene == nullptr ||
        context_.current_frame_num == nullptr ||
        context_.playback_state->play_video || context_.scene->num_cams <= 0 ||
        context_.scene->size_of_buffer <= 0) {
        return false;
    }
    const int visible_idx = getVisibleCameraIndex();
    if (visible_idx < 0) {
        return false;
    }

    int matched_slot = -1;
    for (int i = 0; i < context_.scene->size_of_buffer; ++i) {
        const auto& slot =
            context_.scene->cameras[visible_idx].display_buffer[i];
        if (!slot.available_to_write && slot.frame_number == target_frame) {
            matched_slot = i;
            break;
        }
    }

    if (matched_slot < 0) {
        return false;
    }

    context_.playback_state->read_head = matched_slot;
    context_.playback_state->pause_selected = 0;
    context_.playback_state->to_display_frame_number = target_frame;
    context_.playback_state->slider_frame_number = target_frame;
    *context_.current_frame_num = target_frame;
    context_.playback_state->pause_seeked = true;
    return true;
}

int PlaybackSessionController::findDisplaySlotForFrame(int cam_idx,
                                                       int target_frame,
                                                       int preferred_slot) const {
    if (context_.scene == nullptr) {
        return -1;
    }
    return findCameraDisplaySlotForFrame(*context_.scene, cam_idx, target_frame,
                                         preferred_slot);
}

void PlaybackSessionController::seekToFrame(int target_frame,
                                            bool prefer_buffer_when_paused,
                                            bool force_inaccurate,
                                            bool skip_stimulus_hard_seek) const {
    if (context_.scene == nullptr || context_.decoder_context == nullptr ||
        context_.playback_state == nullptr || context_.seek_progress == nullptr ||
        context_.stimulus_player == nullptr || context_.zarr_loader == nullptr ||
        context_.video_fps == nullptr) {
        return;
    }
    const bool clipped_collection =
        context_.zarr_loader->hasClippedCollection();
    const int max_frame = clipped_collection
                              ? std::max(0, static_cast<int>(
                                                context_.zarr_loader
                                                    ->getTotalFrames()) -
                                                1)
                              : std::max(0,
                                         context_.decoder_context
                                             ->total_num_frame -
                                             1);
    const int clamped_frame = std::clamp(target_frame, 0, max_frame);
    int decoder_seek_frame = clamped_frame;

    if (clipped_collection) {
        if (context_.ensure_clipped_media_for_parent_frame &&
            !context_.ensure_clipped_media_for_parent_frame(clamped_frame)) {
            return;
        }
        const auto* row =
            context_.zarr_loader->resolveClippedFrame(clamped_frame);
        if (row == nullptr) {
            std::cout << "[Seek] no clipped mapping for parent frame "
                      << clamped_frame << std::endl;
            return;
        }
        decoder_seek_frame = row->clip_local_frame_index;
    }

    if (context_.scene->num_cams <= 0) {
        return;
    }

    const bool seek_accurate =
        !force_inaccurate && !context_.playback_state->play_video;

    const bool seek_in_flight =
        (context_.seek_progress->state == SeekState::WaitingCameras ||
         context_.seek_progress->state == SeekState::WaitingStimulus);
    if (seek_in_flight &&
        context_.seek_progress->requested_camera_frame == clamped_frame &&
        context_.seek_progress->accurate == seek_accurate &&
        context_.seek_progress->skip_stimulus_hard_seek ==
            skip_stimulus_hard_seek) {
        if (crimson_seek_debug_logs_enabled()) {
            std::cout << "[Seek] dedupe: dropping duplicate request frame="
                      << clamped_frame
                      << " accurate=" << (seek_accurate ? "true" : "false")
                      << " state="
                      << seekStateName(context_.seek_progress->state)
                      << std::endl;
        }
        return;
    }

    if (prefer_buffer_when_paused && stepPausedFrameFromBuffer(clamped_frame)) {
        if (context_.stimulus_player->loaded &&
            context_.zarr_loader->hasStimulusAlignment()) {
            auto stim_frame =
                context_.zarr_loader->getStimulusFrameForCameraFrame(
                    clamped_frame);
            if (stim_frame && *stim_frame >= 0) {
                context_.playback_state->current_stimulus_frame = *stim_frame;
                context_.seek_progress->seek_id++;
                context_.seek_progress->state = SeekState::WaitingStimulus;
                context_.seek_progress->requested_camera_frame = clamped_frame;
                context_.seek_progress->target_camera_frame = clamped_frame;
                context_.seek_progress->target_stimulus_frame = *stim_frame;
                context_.seek_progress->accurate = seek_accurate;
                context_.seek_progress->skip_stimulus_hard_seek = false;
                context_.seek_progress->cameras_settled =
                    context_.scene->num_cams;
                context_.seek_progress->cameras_total =
                    context_.scene->num_cams;
                context_.seek_progress->deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
                {
                    std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                    context_.stimulus_player->seek.seek_frame =
                        static_cast<uint64_t>(*stim_frame);
                    context_.stimulus_player->seek.seek_id =
                        context_.seek_progress->seek_id;
                    context_.stimulus_player->seek.use_seek = true;
                    context_.stimulus_player->seek.seek_done = false;
                    context_.stimulus_player->seek.seek_accurate =
                        context_.seek_progress->accurate;
                }
                context_.stimulus_player->last_displayed_frame = -1;
                (*context_.window_need_decoding)
                    [context_.stimulus_player->window_name]
                        .store(true);
                if (crimson_seek_debug_logs_enabled()) {
                    std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                              << " buffer-hit camera=" << clamped_frame
                              << " -> stimulus=" << *stim_frame << std::endl;
                }
            }
        }
        return;
    }

    if (!context_.playback_state->play_video) {
        context_.playback_state->pause_seeked = false;
        setCameraDecodeRequests(true);
    }

    context_.seek_progress->seek_id++;
    context_.seek_progress->state = SeekState::WaitingCameras;
    context_.seek_progress->requested_camera_frame = clamped_frame;
    context_.seek_progress->target_camera_frame = clamped_frame;
    context_.seek_progress->target_stimulus_frame = -1;
    context_.seek_progress->accurate = seek_accurate;
    context_.seek_progress->skip_stimulus_hard_seek = skip_stimulus_hard_seek;
    context_.seek_progress->cameras_settled = 0;
    context_.seek_progress->cameras_total = context_.scene->num_cams;
    context_.seek_progress->deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);

    context_.playback_state->to_display_frame_number = clamped_frame;
    context_.playback_state->read_head = 0;
    context_.playback_state->just_seeked = true;
    context_.playback_state->slider_frame_number = clamped_frame;
    context_.playback_state->accumulated_play_time =
        clamped_frame / *context_.video_fps;
    context_.playback_state->last_play_time_start =
        std::chrono::steady_clock::now();
    context_.playback_state->last_frame_num_playspeed = clamped_frame;
    context_.playback_state->last_wall_time_playspeed =
        std::chrono::steady_clock::now();

    if (!skip_stimulus_hard_seek && context_.stimulus_player->loaded &&
        context_.zarr_loader->hasStimulusAlignment()) {
        auto stim_frame =
            context_.zarr_loader->getStimulusFrameForCameraFrame(clamped_frame);
        if (stim_frame && *stim_frame >= 0) {
            context_.playback_state->current_stimulus_frame = *stim_frame;
            context_.seek_progress->target_stimulus_frame = *stim_frame;
        }
    }

    initiate_camera_seeks(context_.scene, decoder_seek_frame,
                          context_.seek_progress->seek_id, seek_accurate);

    if (crimson_seek_debug_logs_enabled()) {
        std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                  << " initiated cameras=" << context_.scene->num_cams
                  << " frame=" << clamped_frame
                  << " decoder_frame=" << decoder_seek_frame
                  << " skip_stimulus_hard_seek="
                  << (skip_stimulus_hard_seek ? "true" : "false")
                  << " accurate=" << (seek_accurate ? "true" : "false")
                  << std::endl;
    }
}

void PlaybackSessionController::syncPlaybackStartToCurrentFrame() const {
    if (context_.playback_state == nullptr || context_.scene == nullptr ||
        context_.current_frame_num == nullptr || context_.video_fps == nullptr ||
        context_.stimulus_player == nullptr) {
        return;
    }
    const double fps_for_clock =
        (*context_.video_fps > 0.0) ? *context_.video_fps : 30.0;
    const int clamped_frame =
        std::max(0, context_.playback_state->to_display_frame_number);
    context_.playback_state->accumulated_play_time =
        static_cast<double>(clamped_frame) / fps_for_clock;
    const auto now_tp = std::chrono::steady_clock::now();
    context_.playback_state->last_play_time_start = now_tp;
    context_.playback_state->last_frame_num_playspeed = clamped_frame;
    context_.playback_state->last_wall_time_playspeed = now_tp;

    const int visible_idx = getVisibleCameraIndex();
    if (visible_idx >= 0 && context_.scene->size_of_buffer > 0) {
        const int preferred_slot =
            context_.playback_state->read_head % context_.scene->size_of_buffer;
        const int target_slot = findDisplaySlotForFrame(
            visible_idx, clamped_frame, preferred_slot);
        if (target_slot >= 0) {
            context_.playback_state->read_head = target_slot;
        }
    }

    if (context_.stimulus_player->loaded) {
        context_.stimulus_player->throttled = false;
        context_.stimulus_player->throttle_resume_frame = -1;
        (*context_.window_need_decoding)[context_.stimulus_player->window_name]
            .store(true);
    }
}

bool PlaybackSessionController::resumeFromBufferedFrame(int resume_frame) const {
    if (context_.playback_state == nullptr || context_.scene == nullptr ||
        context_.current_frame_num == nullptr || context_.video_fps == nullptr ||
        context_.stimulus_player == nullptr) {
        return false;
    }
    if (context_.scene->num_cams <= 0 || context_.scene->size_of_buffer <= 0) {
        return false;
    }

    const int clamped_frame = std::max(0, resume_frame);
    const int visible_idx = getVisibleCameraIndex();
    if (visible_idx < 0) {
        return false;
    }

    const int preferred_slot =
        context_.playback_state->read_head % context_.scene->size_of_buffer;
    const int target_slot = findDisplaySlotForFrame(
        visible_idx, clamped_frame, preferred_slot);
    if (target_slot < 0) {
        return false;
    }

    releaseBufferedHistoryBeforeFrame(visible_idx, clamped_frame);

    const double fps_for_clock =
        (*context_.video_fps > 0.0) ? *context_.video_fps : 30.0;
    const auto now_tp = std::chrono::steady_clock::now();
    context_.playback_state->to_display_frame_number = clamped_frame;
    context_.playback_state->slider_frame_number = clamped_frame;
    context_.playback_state->read_head = target_slot;
    context_.playback_state->accumulated_play_time =
        static_cast<double>(clamped_frame) / fps_for_clock;
    context_.playback_state->last_play_time_start = now_tp;
    context_.playback_state->last_frame_num_playspeed = clamped_frame;
    context_.playback_state->last_wall_time_playspeed = now_tp;
    *context_.current_frame_num = clamped_frame;

    if (context_.stimulus_player->loaded) {
        context_.stimulus_player->throttled = false;
        context_.stimulus_player->throttle_resume_frame = -1;
        (*context_.window_need_decoding)[context_.stimulus_player->window_name]
            .store(true);
        if (context_.zarr_loader != nullptr &&
            context_.zarr_loader->hasStimulusAlignment()) {
            auto stim_frame =
                context_.zarr_loader->getStimulusFrameForCameraFrame(
                    clamped_frame);
            context_.playback_state->current_stimulus_frame =
                (stim_frame && *stim_frame >= 0) ? *stim_frame : -1;
        }
    }

    return true;
}

void PlaybackSessionController::releaseBufferedHistoryBeforeFrame(
    int cam_idx, int frame) const {
    if (context_.scene == nullptr || cam_idx < 0 ||
        cam_idx >= context_.scene->num_cams) {
        return;
    }

    for (int slot_idx = 0; slot_idx < context_.scene->size_of_buffer;
         ++slot_idx) {
        auto visible_metadata = frameSlotSnapshotReadable(
            context_.scene->cameras[cam_idx].display_buffer[slot_idx]);
        if (!visible_metadata || visible_metadata->frame_number >= frame) {
            continue;
        }
        for (int camera_idx = 0; camera_idx < context_.scene->num_cams;
             ++camera_idx) {
            frameSlotReleaseForReuse(
                context_.scene->cameras[camera_idx].display_buffer[slot_idx]);
        }
    }
}

bool PlaybackSessionController::isWithinNewestContiguousBufferedSpan(
    int frame) const {
    if (context_.scene == nullptr || context_.playback_state == nullptr ||
        context_.scene->num_cams <= 0 || context_.scene->size_of_buffer <= 0) {
        return false;
    }

    const int visible_idx = getVisibleCameraIndex();
    if (visible_idx < 0) {
        return false;
    }

    std::vector<int> buffered_frames;
    buffered_frames.reserve(context_.scene->size_of_buffer);
    for (int i = 0; i < context_.scene->size_of_buffer; ++i) {
        const auto& slot =
            context_.scene->cameras[visible_idx].display_buffer[i];
        if (slot.available_to_write || slot.frame_number < 0) {
            continue;
        }
        buffered_frames.push_back(slot.frame_number);
    }
    if (buffered_frames.empty()) {
        return false;
    }

    std::sort(buffered_frames.begin(), buffered_frames.end());
    const int newest_frame = buffered_frames.back();
    int span_start = newest_frame;
    for (int i = static_cast<int>(buffered_frames.size()) - 2; i >= 0; --i) {
        if (buffered_frames[i] + 1 == span_start) {
            span_start = buffered_frames[i];
            continue;
        }
        break;
    }
    return frame >= span_start && frame <= newest_frame;
}

void PlaybackSessionController::stepFrames(int delta_frames) const {
    if (context_.current_frame_num == nullptr) {
        return;
    }
    int base_frame = *context_.current_frame_num;
    if (context_.playback_state != nullptr &&
        !context_.playback_state->play_video) {
        base_frame = context_.playback_state->to_display_frame_number;
    }
    seekToFrame(base_frame + delta_frames, true);
}

void PlaybackSessionController::applyPlaybackToggle() const {
    if (context_.playback_state == nullptr || context_.stimulus_player == nullptr) {
        return;
    }
    context_.playback_state->play_video = !context_.playback_state->play_video;
    if (context_.playback_state->play_video) {
        const int resume_frame =
            std::max(0, context_.playback_state->to_display_frame_number);
        const bool browsed_since_pause =
            context_.playback_state->buffer_browsed_since_pause &&
            context_.playback_state->paused_frame_on_toggle >= 0 &&
            resume_frame != context_.playback_state->paused_frame_on_toggle;
        context_.playback_state->last_resume_target_frame = resume_frame;
        context_.playback_state->pause_seeked = false;
        setCameraDecodeRequests(true);
        if (context_.stimulus_player->loaded) {
            (*context_.window_need_decoding)[context_.stimulus_player->window_name]
                .store(true);
        }
        if (browsed_since_pause) {
            if (isWithinNewestContiguousBufferedSpan(resume_frame)) {
                if (resumeFromBufferedFrame(resume_frame)) {
                    context_.playback_state->last_resume_path =
                        ResumePath::BufferedSoft;
                } else {
                    context_.playback_state->last_resume_path =
                        ResumePath::HardSeekFallback;
                    seekToFrame(resume_frame, false, true);
                }
            } else {
                context_.playback_state->last_resume_path =
                    ResumePath::CameraReanchor;
                seekToFrame(resume_frame, false, true, true);
            }
        } else {
            context_.playback_state->last_resume_path = ResumePath::SmoothPause;
            syncPlaybackStartToCurrentFrame();
        }
        context_.playback_state->buffer_browsed_since_pause = false;
    } else {
        context_.playback_state->pause_selected = 0;
        const int paused_frame =
            (context_.current_frame_num != nullptr)
                ? std::max(0, *context_.current_frame_num)
                : std::max(0, context_.playback_state->to_display_frame_number);
        context_.playback_state->paused_frame_on_toggle = paused_frame;
        context_.playback_state->buffer_browsed_since_pause = false;
        context_.playback_state->last_resume_path = ResumePath::None;
        context_.playback_state->last_resume_target_frame = -1;
        context_.playback_state->to_display_frame_number = paused_frame;
        context_.playback_state->slider_frame_number = paused_frame;
        stepPausedFrameFromBuffer(paused_frame);
    }
}

void PlaybackSessionController::pollSeekState() const {
    if (context_.seek_progress == nullptr || context_.playback_state == nullptr ||
        context_.scene == nullptr || context_.stimulus_player == nullptr ||
        context_.zarr_loader == nullptr) {
        return;
    }

    if (context_.seek_progress->state == SeekState::WaitingCameras) {
        const int settled =
            poll_camera_seeks(context_.scene, context_.seek_progress->seek_id);
        context_.seek_progress->cameras_settled = settled;
        if (settled >= context_.seek_progress->cameras_total) {
            int settled_camera_frame =
                context_.seek_progress->target_camera_frame;
            int settled_camera_index = getVisibleCameraIndex();
            if (settled_camera_index < 0 ||
                settled_camera_index >= context_.scene->num_cams) {
                settled_camera_index = 0;
            }
            {
                std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                if (context_.scene->num_cams > 0) {
                    const auto& settled_ctx =
                        context_.scene->cameras[settled_camera_index]
                            .seek_context;
                    if (settled_ctx.seek_done &&
                        settled_ctx.settled_seek_id ==
                            context_.seek_progress->seek_id) {
                        settled_camera_frame =
                            static_cast<int>(settled_ctx.seek_frame);
                    }
                }
            }

            if (settled_camera_frame !=
                context_.seek_progress->target_camera_frame) {
                if (crimson_seek_debug_logs_enabled()) {
                    std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                              << " camera settled to " << settled_camera_frame
                              << " (requested "
                              << context_.seek_progress->target_camera_frame
                              << ")" << std::endl;
                }
            }
            context_.seek_progress->target_camera_frame = settled_camera_frame;
            context_.playback_state->to_display_frame_number =
                settled_camera_frame;
            context_.playback_state->slider_frame_number =
                settled_camera_frame;

            if (!context_.playback_state->play_video) {
                stepPausedFrameFromBuffer(settled_camera_frame);
            }

            int remapped_stimulus_frame = -1;
            if (context_.stimulus_player->loaded &&
                context_.zarr_loader->hasStimulusAlignment()) {
                auto stim_frame =
                    context_.zarr_loader->getStimulusFrameForCameraFrame(
                        settled_camera_frame);
                if (stim_frame && *stim_frame >= 0) {
                    remapped_stimulus_frame = *stim_frame;
                }
            }
            context_.seek_progress->target_stimulus_frame =
                remapped_stimulus_frame;
            context_.playback_state->current_stimulus_frame =
                remapped_stimulus_frame;

            if (context_.stimulus_player->loaded &&
                remapped_stimulus_frame >= 0 &&
                !context_.seek_progress->skip_stimulus_hard_seek) {
                context_.seek_progress->state = SeekState::WaitingStimulus;
                context_.seek_progress->deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds(5);
                {
                    std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                    context_.stimulus_player->seek.seek_frame =
                        static_cast<uint64_t>(remapped_stimulus_frame);
                    context_.stimulus_player->seek.seek_id =
                        context_.seek_progress->seek_id;
                    context_.stimulus_player->seek.use_seek = true;
                    context_.stimulus_player->seek.seek_done = false;
                    context_.stimulus_player->seek.seek_accurate =
                        context_.seek_progress->accurate;
                }
                context_.stimulus_player->last_displayed_frame = -1;
                (*context_.window_need_decoding)
                    [context_.stimulus_player->window_name]
                        .store(true);
                if (crimson_seek_debug_logs_enabled()) {
                    std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                              << " issuing stimulus seek camera="
                              << settled_camera_frame << " -> stimulus="
                              << remapped_stimulus_frame
                              << " buffered_before="
                              << countBufferedStimulusFrames()
                              << " newest_before="
                              << getNewestStimulusFrame(
                                     *context_.stimulus_player)
                              << std::endl;
                }
            } else {
                context_.seek_progress->state = SeekState::Ready;
                if (crimson_seek_debug_logs_enabled()) {
                    std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                              << (context_.seek_progress->skip_stimulus_hard_seek
                                      ? " complete (camera-only re-anchor)"
                                      : " complete (no stimulus mapping)")
                              << std::endl;
                }
            }
        } else if (std::chrono::steady_clock::now() >=
                   context_.seek_progress->deadline) {
            std::cerr << "[Seek] id=" << context_.seek_progress->seek_id
                      << " TIMEOUT in WaitingCameras (" << settled << "/"
                      << context_.seek_progress->cameras_total << " settled)"
                      << std::endl;
            context_.seek_progress->state = SeekState::TimedOut;
        }
    }

    if (context_.seek_progress->state == SeekState::WaitingStimulus) {
        bool stim_done = false;
        int settled_stimulus_frame = -1;
        {
            std::lock_guard<std::mutex> lock(g_seek_info_mutex);
            stim_done = context_.stimulus_player->seek.seek_done &&
                        context_.stimulus_player->seek.settled_seek_id ==
                            context_.seek_progress->seek_id;
            if (stim_done) {
                settled_stimulus_frame =
                    static_cast<int>(context_.stimulus_player->seek.seek_frame);
            }
        }
        if (stim_done) {
            if (settled_stimulus_frame >= 0 &&
                settled_stimulus_frame !=
                    context_.seek_progress->target_stimulus_frame) {
                if (crimson_seek_debug_logs_enabled()) {
                    std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                              << " stimulus settled to "
                              << settled_stimulus_frame << " (requested "
                              << context_.seek_progress->target_stimulus_frame
                              << ")" << std::endl;
                }
                context_.seek_progress->target_stimulus_frame =
                    settled_stimulus_frame;
                context_.playback_state->current_stimulus_frame =
                    settled_stimulus_frame;
            }
            context_.seek_progress->state = SeekState::Ready;
            if (crimson_seek_debug_logs_enabled()) {
                std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                          << " stimulus settled buffered_after="
                          << countBufferedStimulusFrames()
                          << " newest_after="
                          << getNewestStimulusFrame(*context_.stimulus_player)
                          << std::endl;
            }
        } else if (std::chrono::steady_clock::now() >=
                   context_.seek_progress->deadline) {
            std::cerr << "[Seek] id=" << context_.seek_progress->seek_id
                      << " TIMEOUT in WaitingStimulus"
                      << " target_stimulus_frame="
                      << context_.seek_progress->target_stimulus_frame
                      << " buffered_now=" << countBufferedStimulusFrames()
                      << " newest_now="
                      << getNewestStimulusFrame(*context_.stimulus_player)
                      << std::endl;
            context_.seek_progress->state = SeekState::TimedOut;
        }
        (*context_.window_need_decoding)[context_.stimulus_player->window_name]
            .store(true);
    }

    if (context_.seek_progress->state == SeekState::Ready ||
        context_.seek_progress->state == SeekState::TimedOut) {
        if (!context_.playback_state->play_video &&
            !context_.playback_state->pause_seeked) {
            stepPausedFrameFromBuffer(
                context_.seek_progress->target_camera_frame);
        }
        context_.seek_progress->state = SeekState::Idle;
    }
}
