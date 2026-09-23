#include "playback_session_controller.h"
#include "decoder_seek_bookkeeping.h"
#include "frame_selection.h"
#include "frame_slot.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>

extern std::mutex g_seek_info_mutex;

namespace {

int legacyPlaybackFrame(int64_t frame) {
  return static_cast<int>(std::clamp<int64_t>(
      frame, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

std::optional<int32_t> mappedStimulusFrame(
    const PlaybackSessionControllerContext &context, int camera_frame) {
  return crimson::zarr::StimulusFrameForCamera(context.stimulus_repository,
                                                camera_frame);
}

} // namespace

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

const char *playbackPreviewScaleLabel(int playback_preview_scale_mode) {
  switch (playback_preview_scale_mode) {
  case 1:
    return "1/2";
  case 2:
    return "1/4";
  default:
    return "1x";
  }
}

bool playbackPreviewIsActive(bool play_video, bool yolo_detection,
                             int playback_preview_scale_mode) {
  return play_video && !yolo_detection &&
         playbackPreviewScaleFactor(playback_preview_scale_mode) < 1.0;
}

const char *playbackRendererModeLabel(int playback_renderer_mode) {
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
    const PlaybackSessionControllerContext &context)
    : context_(context) {}

int PlaybackSessionController::getVisibleCameraIndex() const {
  if (context_.scene == nullptr || context_.camera_names == nullptr ||
      context_.window_was_decoding == nullptr ||
      context_.window_need_decoding == nullptr ||
      context_.scene->num_cams <= 0 || context_.scene->size_of_buffer <= 0) {
    return -1;
  }
  for (int i = 0; i < context_.scene->num_cams &&
                  i < static_cast<int>(context_.camera_names->size());
       ++i) {
    const auto &camera_name = (*context_.camera_names)[i];
    auto it = context_.window_was_decoding->find(camera_name);
    if (it != context_.window_was_decoding->end() && it->second) {
      return i;
    }
  }
  return 0;
}

crimson::playback::PlaybackPresentationTarget
PlaybackSessionController::planPresentationTarget(
    int requested_frame, std::optional<int> minimum_decoded_frame) const {
  crimson::playback::PlaybackPresentationAdapterPlanInput input;
  input.previous_committed_frame = 0;
  input.requested_frame = requested_frame;
  input.minimum_decoded_frame = minimum_decoded_frame;
  const auto makeTarget = [&](const auto &adapter) {
    crimson::playback::PlaybackPresentationTarget result{
        adapter.active,
        requested_frame,
        legacyPlaybackFrame(adapter.bounded_target_frame),
        legacyPlaybackFrame(adapter.minimum_decoded_frame),
        legacyPlaybackFrame(adapter.frame),
        adapter.slot.value_or(-1),
        adapter.clamped_to_buffer ||
            adapter.bounded_target_frame != adapter.frame};
    return result;
  };
  if (context_.playback_state == nullptr || context_.scene == nullptr ||
      context_.decoder_context == nullptr) {
    const auto adapter =
        crimson::playback::planPlaybackPresentationAdapter(input);
    return makeTarget(adapter);
  }

  input.just_seeked = context_.playback_state->just_seeked;
  input.decoding_active = context_.decoder_context->decoding_flag;
  input.playing = context_.playback_state->play_video;
  input.buffer_size = static_cast<int>(context_.scene->size_of_buffer);
  input.previous_committed_frame =
      context_.playback_state->to_display_frame_number;
  input.preferred_slot =
      input.buffer_size > 0
          ? context_.playback_state->read_head % input.buffer_size
          : -1;
  if (input.just_seeked || !input.decoding_active || !input.playing ||
      input.buffer_size <= 0) {
    const auto adapter =
        crimson::playback::planPlaybackPresentationAdapter(input);
    return makeTarget(adapter);
  }
  int visible_idx = getVisibleCameraIndex();
  if (visible_idx < 0 || visible_idx >= context_.scene->num_cams) {
    visible_idx = context_.scene->num_cams > 0 ? 0 : -1;
  }
  if (visible_idx >= 0) {
    input.buffered_frames.reserve(context_.scene->size_of_buffer);
    for (int slot_idx = 0;
         slot_idx < static_cast<int>(context_.scene->size_of_buffer);
         ++slot_idx) {
      if (auto metadata = frameSlotSnapshotReadable(
              context_.scene->cameras[visible_idx].display_buffer[slot_idx])) {
        input.buffered_frames.push_back({metadata->frame_number, slot_idx});
      }
    }
  }
  const auto adapter =
      crimson::playback::planPlaybackPresentationAdapter(input);
  return makeTarget(adapter);
}

crimson::playback::PlaybackPresentationCommit
PlaybackSessionController::commitPresentedFrame(int presenter_target_frame,
                                                int presented_frame,
                                                int presented_slot) const {
  crimson::playback::PlaybackPresentationAdapterCommitInput input;
  input.presenter_target_frame = presenter_target_frame;
  input.release_history_explicitly = true;
  if (context_.playback_state == nullptr || context_.scene == nullptr ||
      context_.decoder_context == nullptr) {
    const auto adapter =
        crimson::playback::planPlaybackPresentationAdapterCommit(input);
    return {adapter.eligible,
            adapter.observed_slot,
            adapter.committed && adapter.observed_slot,
            legacyPlaybackFrame(adapter.previous_committed_frame),
            legacyPlaybackFrame(adapter.frame),
            adapter.slot,
            adapter.release_policy ==
                crimson::playback::PlaybackPresentationReleasePolicy::
                    DeferUntilPresentation};
  }

  input.just_seeked = context_.playback_state->just_seeked;
  input.decoding_active = context_.decoder_context->decoding_flag;
  input.playing = context_.playback_state->play_video;
  input.buffer_size = static_cast<int>(context_.scene->size_of_buffer);
  input.previous_committed_frame =
      context_.playback_state->to_display_frame_number;
  if (presented_slot >= 0 && presented_frame >= 0) {
    input.observation.presented_frame = presented_frame;
    input.observation.slot = presented_slot;
  }
  const auto adapter =
      crimson::playback::planPlaybackPresentationAdapterCommit(input);
  crimson::playback::PlaybackPresentationCommit result{
      adapter.eligible,
      adapter.observed_slot,
      adapter.committed && adapter.observed_slot,
      legacyPlaybackFrame(adapter.previous_committed_frame),
      legacyPlaybackFrame(adapter.frame),
      adapter.slot,
      adapter.release_policy ==
          crimson::playback::PlaybackPresentationReleasePolicy::
              DeferUntilPresentation};
  if (!result.committed) {
    return result;
  }
  context_.playback_state->to_display_frame_number = result.frame;
  context_.playback_state->slider_frame_number = result.frame;
  if (result.slot >= 0) {
    context_.playback_state->read_head = result.slot;
  }

  std::vector<crimson::playback::PlaybackHistoryReleaseCandidate> candidates;
  candidates.reserve(context_.scene->size_of_buffer * context_.scene->num_cams);
  for (int slot_idx = 0;
       slot_idx < static_cast<int>(context_.scene->size_of_buffer);
       ++slot_idx) {
    for (int cam_idx = 0; cam_idx < static_cast<int>(context_.scene->num_cams);
         ++cam_idx) {
      const auto &slot =
          context_.scene->cameras[cam_idx].display_buffer[slot_idx];
      auto metadata = frameSlotSnapshotReadable(slot);
      if (metadata) {
        candidates.push_back({cam_idx, slot_idx, metadata->frame_number});
      }
    }
  }
  for (const auto &candidate :
       crimson::playback::selectPlaybackHistoryReleaseCandidates(
           candidates, result.frame)) {
    auto &slot = context_.scene->cameras[candidate.camera_index]
                     .display_buffer[candidate.slot_index];
    ++result.release_attempts;
    if (frameSlotTryReleaseForReuse(slot, candidate.frame_number)) {
      ++result.release_count;
    } else {
      ++result.release_skip_count;
    }
  }
  return result;
}

void PlaybackSessionController::setCameraDecodeRequests(bool enabled) const {
  if (context_.camera_names == nullptr ||
      context_.window_need_decoding == nullptr) {
    return;
  }
  for (const auto &camera_name : *context_.camera_names) {
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

  std::vector<BufferedFrameCandidate> candidates;
  candidates.reserve(context_.scene->size_of_buffer);
  for (int i = 0; i < context_.scene->size_of_buffer; ++i) {
    if (auto metadata = frameSlotSnapshotReadable(
            context_.scene->cameras[visible_idx].display_buffer[i])) {
      candidates.push_back({i, std::move(*metadata)});
    }
  }
  FrameSelectionRequest request;
  request.target_frame = target_frame;
  request.fallback = FrameSelectionFallback::Nearest;
  return selectBufferedFrame(candidates, request).slot_index;
}

bool PlaybackSessionController::stepPausedFrameFromBuffer(
    int target_frame) const {
  if (context_.playback_state == nullptr || context_.scene == nullptr ||
      context_.current_frame_num == nullptr ||
      context_.playback_state->play_video || context_.scene->num_cams <= 0 ||
      context_.scene->size_of_buffer <= 0 ||
      context_.playback_state->rejected_seek_ring_quarantined) {
    return false;
  }
  const int visible_idx = getVisibleCameraIndex();
  if (visible_idx < 0) {
    return false;
  }

  std::vector<BufferedFrameCandidate> candidates;
  candidates.reserve(context_.scene->size_of_buffer);
  for (int i = 0; i < context_.scene->size_of_buffer; ++i) {
    if (auto metadata = frameSlotSnapshotReadable(
            context_.scene->cameras[visible_idx].display_buffer[i])) {
      candidates.push_back({i, std::move(*metadata)});
    }
  }
  FrameSelectionRequest request;
  request.target_frame = target_frame;
  request.fallback = FrameSelectionFallback::ExactOnly;
  const int matched_slot = selectBufferedFrame(candidates, request).slot_index;

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

int PlaybackSessionController::findDisplaySlotForFrame(
    int cam_idx, int target_frame, int preferred_slot) const {
  if (context_.scene == nullptr) {
    return -1;
  }
  return findCameraDisplaySlotForFrame(*context_.scene, cam_idx, target_frame,
                                       preferred_slot);
}

crimson::playback::PlaybackSeekExecutionResult
PlaybackSessionController::seekToFrame(int target_frame,
                                       bool prefer_buffer_when_paused,
                                       bool force_inaccurate,
                                       bool skip_stimulus_hard_seek) const {
  const auto service_start = std::chrono::steady_clock::now();
  crimson::playback::PlaybackSeekExecutionResult result;
  result.status = crimson::playback::PlaybackSeekExecutionStatus::Failed;
  if (context_.scene == nullptr || context_.decoder_context == nullptr ||
      context_.playback_state == nullptr || context_.seek_progress == nullptr ||
      context_.stimulus_player == nullptr ||
      context_.video_fps == nullptr || context_.playback_transport == nullptr) {
    result.error = "playback session is incomplete";
    return result;
  }
  const int64_t max_frame_value =
      context_.playback_transport->configured()
          ? context_.playback_transport->frameCount() - 1
          : static_cast<int64_t>(context_.decoder_context->total_num_frame) - 1;
  const int max_frame = static_cast<int>(
      std::clamp<int64_t>(max_frame_value, 0, std::numeric_limits<int>::max()));
  const int clamped_frame = std::clamp(target_frame, 0, max_frame);
  int decoder_seek_frame = clamped_frame;

  if (context_.resolve_decoder_frame_for_parent_frame) {
    const auto resolved_decoder_frame =
        context_.resolve_decoder_frame_for_parent_frame(clamped_frame);
    if (resolved_decoder_frame.status == DecoderFrameResolutionStatus::Pending) {
      pending_media_frame_ = clamped_frame;
      pending_media_prefer_buffer_ = prefer_buffer_when_paused;
      pending_media_force_inaccurate_ = force_inaccurate;
      pending_media_skip_stimulus_ = skip_stimulus_hard_seek;
      context_.seek_progress->state = SeekState::WaitingMedia;
      context_.seek_progress->requested_camera_frame = clamped_frame;
      context_.playback_state->slider_frame_number = clamped_frame;
      result.status = crimson::playback::PlaybackSeekExecutionStatus::Submitted;
      result.path = crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
      result.resolved_frame = clamped_frame;
      result.service_ms = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - service_start).count();
      return result;
    }
    if (resolved_decoder_frame.status == DecoderFrameResolutionStatus::Failed) {
      pending_media_frame_ = -1;
      if (context_.seek_progress->state == SeekState::WaitingMedia) {
        context_.seek_progress->state = SeekState::Idle;
      }
      result.error = resolved_decoder_frame.error;
      return result;
    }
    if (resolved_decoder_frame.status ==
        DecoderFrameResolutionStatus::ReadyBuffered) {
      pending_media_frame_ = -1;
      context_.seek_progress->state = SeekState::Idle;
      result.status = crimson::playback::PlaybackSeekExecutionStatus::Completed;
      result.path = crimson::playback::PlaybackSeekExecutionPath::ResidentBuffer;
      result.resolved_frame = clamped_frame;
      result.service_ms = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - service_start).count();
      return result;
    }
    pending_media_frame_ = -1;
    if (context_.seek_progress->state == SeekState::WaitingMedia) {
      context_.seek_progress->state = SeekState::Idle;
    }
    decoder_seek_frame = resolved_decoder_frame.decoder_frame;
  }

  if (context_.scene->num_cams <= 0) {
    result.error = "no camera decoder is available";
    return result;
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
                << " state=" << seekStateName(context_.seek_progress->state)
                << std::endl;
    }
    result.status =
        crimson::playback::PlaybackSeekExecutionStatus::Deduplicated;
    result.path = crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
    result.resolved_frame = clamped_frame;
    result.service_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - service_start)
                            .count();
    return result;
  }

  context_.playback_transport->seek(clamped_frame);

  if (prefer_buffer_when_paused && stepPausedFrameFromBuffer(clamped_frame)) {
    if (context_.stimulus_player->loaded) {
      auto stim_frame = mappedStimulusFrame(context_, clamped_frame);
      if (stim_frame && *stim_frame >= 0) {
        context_.playback_state->current_stimulus_frame = *stim_frame;
        context_.seek_progress->seek_id++;
        context_.seek_progress->state = SeekState::WaitingStimulus;
        context_.seek_progress->requested_camera_frame = clamped_frame;
        context_.seek_progress->target_camera_frame = clamped_frame;
        context_.seek_progress->target_stimulus_frame = *stim_frame;
        context_.seek_progress->accurate = seek_accurate;
        context_.seek_progress->skip_stimulus_hard_seek = false;
        context_.seek_progress->cameras_settled = context_.scene->num_cams;
        context_.seek_progress->cameras_total = context_.scene->num_cams;
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
        (*context_.window_need_decoding)[context_.stimulus_player->window_name]
            .store(true);
        if (crimson_seek_debug_logs_enabled()) {
          std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                    << " buffer-hit camera=" << clamped_frame
                    << " -> stimulus=" << *stim_frame << std::endl;
        }
      }
    }
    result.status = crimson::playback::PlaybackSeekExecutionStatus::Completed;
    result.path = crimson::playback::PlaybackSeekExecutionPath::ResidentBuffer;
    result.resolved_frame = clamped_frame;
    result.service_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - service_start)
                            .count();
    return result;
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

  if (!skip_stimulus_hard_seek && context_.stimulus_player->loaded) {
    auto stim_frame = mappedStimulusFrame(context_, clamped_frame);
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
  result.status = crimson::playback::PlaybackSeekExecutionStatus::Submitted;
  result.path = crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
  result.resolved_frame = clamped_frame;
  result.service_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - service_start)
                          .count();
  return result;
}

void PlaybackSessionController::syncPlaybackStartToCurrentFrame() const {
  if (context_.playback_state == nullptr || context_.scene == nullptr ||
      context_.current_frame_num == nullptr || context_.video_fps == nullptr ||
      context_.stimulus_player == nullptr ||
      context_.playback_transport == nullptr) {
    return;
  }
  const double fps_for_clock =
      (*context_.video_fps > 0.0) ? *context_.video_fps : 30.0;
  const int clamped_frame =
      std::max(0, context_.playback_state->to_display_frame_number);
  context_.playback_state->accumulated_play_time =
      static_cast<double>(clamped_frame) / fps_for_clock;
  const auto now_tp = std::chrono::steady_clock::now();
  context_.playback_transport->seek(clamped_frame, now_tp);
  context_.playback_state->last_play_time_start = now_tp;
  context_.playback_state->last_frame_num_playspeed = clamped_frame;
  context_.playback_state->last_wall_time_playspeed = now_tp;

  const int visible_idx = getVisibleCameraIndex();
  if (visible_idx >= 0 && context_.scene->size_of_buffer > 0) {
    const int preferred_slot =
        context_.playback_state->read_head % context_.scene->size_of_buffer;
    const int target_slot =
        findDisplaySlotForFrame(visible_idx, clamped_frame, preferred_slot);
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

bool PlaybackSessionController::resumeFromBufferedFrame(
    int resume_frame) const {
  if (context_.playback_state == nullptr || context_.scene == nullptr ||
      context_.current_frame_num == nullptr || context_.video_fps == nullptr ||
      context_.stimulus_player == nullptr ||
      context_.playback_transport == nullptr ||
      context_.playback_state->rejected_seek_ring_quarantined) {
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
  const int target_slot =
      findDisplaySlotForFrame(visible_idx, clamped_frame, preferred_slot);
  if (target_slot < 0) {
    return false;
  }

  releaseBufferedHistoryBeforeFrame(visible_idx, clamped_frame);

  const double fps_for_clock =
      (*context_.video_fps > 0.0) ? *context_.video_fps : 30.0;
  const auto now_tp = std::chrono::steady_clock::now();
  context_.playback_transport->seek(clamped_frame, now_tp);
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
    auto stim_frame = mappedStimulusFrame(context_, clamped_frame);
    context_.playback_state->current_stimulus_frame =
        (stim_frame && *stim_frame >= 0) ? *stim_frame : -1;
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
    const auto &slot = context_.scene->cameras[visible_idx].display_buffer[i];
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
  if (context_.playback_state == nullptr ||
      context_.stimulus_player == nullptr ||
      context_.playback_transport == nullptr) {
    return;
  }
  const bool was_playing = context_.playback_state->play_video;
  const int transport_anchor =
      was_playing && context_.current_frame_num != nullptr
          ? std::max(0, *context_.current_frame_num)
          : std::max(0, context_.playback_state->to_display_frame_number);
  const auto now = std::chrono::steady_clock::now();
  context_.playback_transport->seek(transport_anchor, now);
  const auto command =
      was_playing ? crimson::playback::PlaybackTransportCommand::pause()
                  : crimson::playback::PlaybackTransportCommand::play();
  const auto transition = context_.playback_transport->apply(command, now);
  if (!transition.accepted) {
    return;
  }
  context_.playback_state->play_video = transition.playing;
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
    if (context_.playback_state->rejected_seek_ring_quarantined) {
      context_.playback_state->last_resume_path = ResumePath::HardSeekFallback;
      seekToFrame(resume_frame, false, true);
      context_.playback_state->buffer_browsed_since_pause = false;
      return;
    }
    if (browsed_since_pause) {
      if (isWithinNewestContiguousBufferedSpan(resume_frame)) {
        if (resumeFromBufferedFrame(resume_frame)) {
          context_.playback_state->last_resume_path = ResumePath::BufferedSoft;
        } else {
          context_.playback_state->last_resume_path =
              ResumePath::HardSeekFallback;
          seekToFrame(resume_frame, false, true);
        }
      } else {
        context_.playback_state->last_resume_path = ResumePath::CameraReanchor;
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

std::optional<crimson::playback::PlaybackSeekExecutionResult>
PlaybackSessionController::pollSeekState() const {
  if (context_.seek_progress == nullptr || context_.playback_state == nullptr ||
      context_.scene == nullptr || context_.stimulus_player == nullptr ||
      context_.window_need_decoding == nullptr) {
    return std::nullopt;
  }

  std::optional<crimson::playback::PlaybackSeekExecutionResult> completion;

  if (context_.seek_progress->state == SeekState::WaitingMedia &&
      pending_media_frame_ >= 0 &&
      context_.resolve_decoder_frame_for_parent_frame) {
    const auto resolved = context_.resolve_decoder_frame_for_parent_frame(
        pending_media_frame_);
    if (resolved.status == DecoderFrameResolutionStatus::Pending) {
      return std::nullopt;
    }
    if (resolved.status == DecoderFrameResolutionStatus::Failed) {
      crimson::playback::PlaybackSeekExecutionResult failed;
      failed.status = crimson::playback::PlaybackSeekExecutionStatus::Failed;
      failed.path = crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
      failed.resolved_frame = pending_media_frame_;
      failed.error = resolved.error;
      context_.seek_progress->state = SeekState::Idle;
      pending_media_frame_ = -1;
      return failed;
    }
    const int frame = pending_media_frame_;
    pending_media_frame_ = -1;
    context_.seek_progress->state = SeekState::Idle;
    if (resolved.status == DecoderFrameResolutionStatus::ReadyBuffered) {
      crimson::playback::PlaybackSeekExecutionResult ready;
      ready.status = crimson::playback::PlaybackSeekExecutionStatus::Completed;
      ready.path = crimson::playback::PlaybackSeekExecutionPath::ResidentBuffer;
      ready.resolved_frame = frame;
      return ready;
    }
    const auto resumed = seekToFrame(frame, pending_media_prefer_buffer_,
                                     pending_media_force_inaccurate_,
                                     pending_media_skip_stimulus_);
    if (resumed.status !=
        crimson::playback::PlaybackSeekExecutionStatus::Submitted) {
      return resumed;
    }
  }

  if (context_.seek_progress->state == SeekState::WaitingCameras) {
    const int settled =
        poll_camera_seeks(context_.scene, context_.seek_progress->seek_id);
    context_.seek_progress->cameras_settled = settled;
    if (settled >= context_.seek_progress->cameras_total) {
      int settled_camera_frame = context_.seek_progress->target_camera_frame;
      int settled_camera_index = getVisibleCameraIndex();
      std::optional<int> exact_mismatch_frame;
      if (settled_camera_index < 0 ||
          settled_camera_index >= context_.scene->num_cams) {
        settled_camera_index = 0;
      }
      {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        if (context_.scene->num_cams > 0) {
          const auto &settled_ctx =
              context_.scene->cameras[settled_camera_index].seek_context;
          if (settled_ctx.seek_done &&
              settled_ctx.settled_seek_id == context_.seek_progress->seek_id) {
            settled_camera_frame = static_cast<int>(settled_ctx.seek_frame);
          }
        }
        if (context_.seek_progress->accurate) {
          for (int cam_idx = 0; cam_idx < context_.scene->num_cams;
               ++cam_idx) {
            const auto &camera_ctx =
                context_.scene->cameras[cam_idx].seek_context;
            if (camera_ctx.seek_done &&
                camera_ctx.settled_seek_id ==
                    context_.seek_progress->seek_id) {
              const int camera_frame =
                  static_cast<int>(camera_ctx.seek_frame);
              if (!crimson::playback::cameraSeekSettlementMatches(
                      true, context_.seek_progress->target_camera_frame,
                      camera_frame)) {
                exact_mismatch_frame = camera_frame;
                break;
              }
            }
          }
        }
      }

      if (settled_camera_frame != context_.seek_progress->target_camera_frame) {
        if (crimson_seek_debug_logs_enabled()) {
          std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                    << " camera settled to " << settled_camera_frame
                    << " (requested "
                    << context_.seek_progress->target_camera_frame << ")"
                    << std::endl;
        }
      }
      if (exact_mismatch_frame.has_value()) {
        std::cerr << "[Seek] id=" << context_.seek_progress->seek_id
                  << " exact camera seek mismatch: settled="
                  << *exact_mismatch_frame << " requested="
                  << context_.seek_progress->target_camera_frame << std::endl;
        crimson::playback::PlaybackSeekExecutionResult result;
        result.status = crimson::playback::PlaybackSeekExecutionStatus::Failed;
        result.path =
            crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
        result.resolved_frame = *exact_mismatch_frame;
        result.error = "exact decoder seek settled to an unexpected frame";
        context_.playback_state->rejected_seek_ring_quarantined = true;
        context_.seek_progress->state = SeekState::Idle;
        return result;
      }
      context_.playback_state->rejected_seek_ring_quarantined = false;
      context_.seek_progress->target_camera_frame = settled_camera_frame;
      context_.playback_state->to_display_frame_number = settled_camera_frame;
      context_.playback_state->slider_frame_number = settled_camera_frame;

      if (!context_.playback_state->play_video) {
        stepPausedFrameFromBuffer(settled_camera_frame);
      }

      int remapped_stimulus_frame = -1;
      if (context_.stimulus_player->loaded) {
        auto stim_frame = mappedStimulusFrame(context_, settled_camera_frame);
        if (stim_frame && *stim_frame >= 0) {
          remapped_stimulus_frame = *stim_frame;
        }
      }
      context_.seek_progress->target_stimulus_frame = remapped_stimulus_frame;
      context_.playback_state->current_stimulus_frame = remapped_stimulus_frame;

      if (context_.stimulus_player->loaded && remapped_stimulus_frame >= 0 &&
          !context_.seek_progress->skip_stimulus_hard_seek) {
        context_.seek_progress->state = SeekState::WaitingStimulus;
        context_.seek_progress->deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
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
        (*context_.window_need_decoding)[context_.stimulus_player->window_name]
            .store(true);
        if (crimson_seek_debug_logs_enabled()) {
          std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                    << " issuing stimulus seek camera=" << settled_camera_frame
                    << " -> stimulus=" << remapped_stimulus_frame
                    << " buffered_before=" << countBufferedStimulusFrames()
                    << " newest_before="
                    << getNewestStimulusFrame(*context_.stimulus_player)
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
                    << " stimulus settled to " << settled_stimulus_frame
                    << " (requested "
                    << context_.seek_progress->target_stimulus_frame << ")"
                    << std::endl;
        }
        context_.seek_progress->target_stimulus_frame = settled_stimulus_frame;
        context_.playback_state->current_stimulus_frame =
            settled_stimulus_frame;
      }
      context_.seek_progress->state = SeekState::Ready;
      if (crimson_seek_debug_logs_enabled()) {
        std::cout << "[Seek] id=" << context_.seek_progress->seek_id
                  << " stimulus settled buffered_after="
                  << countBufferedStimulusFrames() << " newest_after="
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
    crimson::playback::PlaybackSeekExecutionResult result;
    result.resolved_frame = context_.seek_progress->target_camera_frame;
    result.path = crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
    if (context_.seek_progress->state == SeekState::Ready) {
      result.status = crimson::playback::PlaybackSeekExecutionStatus::Completed;
    } else {
      result.status = crimson::playback::PlaybackSeekExecutionStatus::Failed;
      result.error = "decoder seek timed out";
    }
    completion = std::move(result);
    if (!context_.playback_state->play_video &&
        !context_.playback_state->pause_seeked) {
      stepPausedFrameFromBuffer(context_.seek_progress->target_camera_frame);
    }
    context_.seek_progress->state = SeekState::Idle;
  }
  return completion;
}

void PlaybackSessionController::cancelActiveSeekForSessionOpen() const {
  pending_media_frame_ = -1;
  if (context_.seek_progress) {
    context_.seek_progress->state = SeekState::Idle;
  }
}
