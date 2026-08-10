#include "media_session_loader.h"
#include "global.h"
#include "render.h"
#include "zarr/affiliated_video_repository.h"
#include "zarr/archive_context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <stdexcept>
#include <tuple>

extern std::mutex g_seek_info_mutex;

namespace {

bool applyZarrCalibrationToCameraParams(const ZarrCalibrationData &calibration,
                                        CameraParams &camera_params,
                                        std::string &error_message) {
  cv::Mat projector_to_camera(3, 3, CV_64F);
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      projector_to_camera.at<double>(row, col) =
          calibration.homography_projector_to_camera[static_cast<size_t>(
              row * 3 + col)];
    }
  }

  cv::Mat camera_to_projector;
  if (!cv::invert(projector_to_camera, camera_to_projector)) {
    error_message = "Zarr calibration homography is not invertible";
    return false;
  }

  // CameraParams predates the Palette Zarr contract. Existing Crimson overlay
  // code uses inverse_homography_matrix as projector/texture -> camera, so
  // keep that internal convention while preserving Palette's source semantics.
  camera_params.homography_matrix = camera_to_projector;
  camera_params.inverse_homography_matrix = projector_to_camera;
  camera_params.has_valid_homography = true;

  if (std::isfinite(calibration.pixels_per_mm_projector)) {
    camera_params.pixels_per_mm_projector =
        static_cast<float>(calibration.pixels_per_mm_projector);
  }
  if (std::isfinite(calibration.pixels_per_mm_camera)) {
    camera_params.pixels_per_mm_camera =
        static_cast<float>(calibration.pixels_per_mm_camera);
  }
  if (std::isfinite(calibration.real_world_ref_mm)) {
    camera_params.real_world_ref_mm =
        static_cast<float>(calibration.real_world_ref_mm);
  }
  camera_params.stimulus_offset_x =
      static_cast<float>(calibration.sub_arena_x_px);
  camera_params.stimulus_offset_y =
      static_cast<float>(calibration.sub_arena_y_px);

  std::ostringstream provenance;
  provenance << "zarr:" << calibration.source_group;
  if (!calibration.source_stimulus_run.empty()) {
    provenance << " stimulus=" << calibration.source_stimulus_run;
  }
  if (!calibration.homography_source.empty()) {
    provenance << " homography_source=" << calibration.homography_source;
  }
  camera_params.calibration_timestamp = provenance.str();
  return true;
}

void resetClippedMediaState(PaletteClippedMediaState &state) {
  const int64_t last_presented = state.handoff.last_presented_parent_frame;
  const int64_t pending_switch = state.handoff.pending_switch_parent_frame;
  const bool switch_in_progress = state.handoff.switch_in_progress;
  state = PaletteClippedMediaState();
  state.handoff.last_presented_parent_frame = last_presented;
  state.handoff.pending_switch_parent_frame = pending_switch;
  state.handoff.switch_in_progress = switch_in_progress;
}

std::pair<int64_t, int64_t> parentFrameRangeForClip(
    const std::vector<int64_t> &parent_frame_by_clip_local) {
  int64_t first = std::numeric_limits<int64_t>::max();
  int64_t last = -1;
  for (const int64_t parent_frame : parent_frame_by_clip_local) {
    if (parent_frame < 0) {
      continue;
    }
    first = std::min(first, parent_frame);
    last = std::max(last, parent_frame);
  }
  if (last < 0) {
    return {-1, -1};
  }
  return {first, last};
}

int64_t playbackFrameCount(const MediaSessionLoaderContext &context,
                           const FFmpegDemuxer *demuxer) {
  if (context.zarr_loader != nullptr &&
      context.zarr_loader->getTotalFrames() > 0) {
    return static_cast<int64_t>(std::min<size_t>(
        context.zarr_loader->getTotalFrames(),
        static_cast<size_t>(std::numeric_limits<int64_t>::max())));
  }
  if (demuxer != nullptr && demuxer->GetNumFrames() > 0) {
    return static_cast<int64_t>(demuxer->GetNumFrames());
  }
  if (context.input_is_imgs != nullptr && *context.input_is_imgs &&
      context.image_names != nullptr && !context.image_names->empty()) {
    return static_cast<int64_t>(context.image_names->size());
  }
  if (context.decoder_context != nullptr &&
      context.decoder_context->total_num_frame > 0 &&
      context.decoder_context->total_num_frame !=
          std::numeric_limits<int>::max()) {
    return context.decoder_context->total_num_frame;
  }
  if (context.decoder_context != nullptr &&
      context.decoder_context->estimated_num_frames >= 0) {
    return static_cast<int64_t>(context.decoder_context->estimated_num_frames) +
           1;
  }
  return 0;
}

void configurePlaybackTransport(const MediaSessionLoaderContext &context,
                                const FFmpegDemuxer *demuxer,
                                int64_t initial_frame, bool preserve_state) {
  if (context.playback_transport == nullptr || context.video_fps == nullptr) {
    return;
  }
  const int64_t frame_count = playbackFrameCount(context, demuxer);
  if (preserve_state && context.playback_transport->configured()) {
    context.playback_transport->updateTimeline(*context.video_fps, frame_count);
    return;
  }
  context.playback_transport->configure(*context.video_fps, frame_count,
                                        std::chrono::steady_clock::now(),
                                        initial_frame);
  if (context.playback_state != nullptr) {
    context.playback_state->play_video = false;
  }
}

} // namespace

MediaSessionLoader::MediaSessionLoader(const MediaSessionLoaderContext &context)
    : context_(context) {}

bool MediaSessionLoader::hasMappedMedia() const {
  return context_.clipped_media_state != nullptr &&
         (context_.clipped_media_state->recording_clip_provider != nullptr ||
          (context_.zarr_loaded != nullptr && *context_.zarr_loaded &&
           context_.zarr_loader != nullptr &&
           context_.zarr_loader->hasClippedCollection()));
}

int64_t MediaSessionLoader::mappedMediaFrameCount() const {
  if (context_.clipped_media_state != nullptr &&
      context_.clipped_media_state->recording_clip_provider != nullptr) {
    return context_.clipped_media_state->recording_clip_provider->index()
        .totalFrameCount();
  }
  if (context_.zarr_loaded != nullptr && *context_.zarr_loaded &&
      context_.zarr_loader != nullptr &&
      context_.zarr_loader->hasClippedCollection()) {
    return static_cast<int64_t>(std::min<size_t>(
        context_.zarr_loader->getTotalFrames(),
        static_cast<size_t>(std::numeric_limits<int64_t>::max())));
  }
  return 0;
}

crimson::playback::ClippedFrameBinding
MediaSessionLoader::resolveMappedMediaFrame(int64_t parent_frame) const {
  crimson::playback::ClippedFrameBinding binding;
  if (parent_frame < 0 || context_.clipped_media_state == nullptr) {
    return binding;
  }
  const auto &state = *context_.clipped_media_state;
  if (state.recording_clip_provider != nullptr) {
    const auto resolved =
        state.recording_clip_provider->resolveParentFrame(parent_frame);
    if (!resolved) {
      return binding;
    }
    binding.mapped = true;
    binding.selected_run_index = resolved->clip_index;
    binding.clip_id = resolved->clip_id;
    binding.clip_local_frame_index = resolved->clip_local_frame;
    binding.first_parent_frame = resolved->first_parent_frame;
    binding.last_parent_frame = resolved->last_parent_frame;
    return binding;
  }
  if (context_.zarr_loaded == nullptr || !*context_.zarr_loaded ||
      context_.zarr_loader == nullptr ||
      !context_.zarr_loader->hasClippedCollection()) {
    return binding;
  }

  const auto *row = context_.zarr_loader->resolveClippedFrame(parent_frame);
  if (row == nullptr) {
    return binding;
  }
  const auto *selected = context_.zarr_loader->getClippedResolver().selectedRun(
      row->selected_run_index);
  if (selected == nullptr || selected->clip_id.empty()) {
    return binding;
  }

  binding.mapped = true;
  binding.selected_run_index = row->selected_run_index;
  binding.clip_id = selected->clip_id;
  binding.clip_local_frame_index = row->clip_local_frame_index;
  if (state.handoff.selected_run_index == row->selected_run_index &&
      state.handoff.first_parent_frame >= 0 &&
      state.handoff.last_parent_frame >= state.handoff.first_parent_frame) {
    binding.first_parent_frame = state.handoff.first_parent_frame;
    binding.last_parent_frame = state.handoff.last_parent_frame;
  } else {
    std::tie(binding.first_parent_frame, binding.last_parent_frame) =
        parentFrameRangeForClip(selected->parent_frame_by_clip_local);
  }
  return binding;
}

bool MediaSessionLoader::activateRecordingClipIndex(
    const std::filesystem::path &index_path, std::string *error_message) const {
  if (context_.clipped_media_state == nullptr) {
    if (error_message != nullptr) {
      *error_message = "recording clip media state is unavailable";
    }
    return false;
  }
  auto provider = crimson::media::RecordingClipMediaProvider::Open(
      index_path, error_message);
  if (!provider) {
    return false;
  }
  context_.clipped_media_state->source = ClippedMediaSource::RecordingClipIndex;
  context_.clipped_media_state->recording_clip_provider =
      std::make_shared<const crimson::media::RecordingClipMediaProvider>(
          std::move(*provider));
  return true;
}

std::optional<int>
MediaSessionLoader::resolveDecoderFrameForParentFrame(int parent_frame) const {
  if (parent_frame < 0) {
    return std::nullopt;
  }
  if (context_.clipped_media_state != nullptr &&
      context_.clipped_media_state->recording_clip_provider != nullptr) {
    const auto binding = context_.clipped_media_state->recording_clip_provider
                             ->resolveParentFrame(parent_frame);
    if (!binding || !loadClippedVideoForParentFrame(parent_frame)) {
      return std::nullopt;
    }
    return static_cast<int>(std::clamp<int64_t>(
        binding->clip_local_frame, 0, std::numeric_limits<int>::max()));
  }
  if (context_.zarr_loaded != nullptr && *context_.zarr_loaded &&
      context_.zarr_loader != nullptr &&
      context_.zarr_loader->hasClippedCollection()) {
    if (!loadClippedVideoForParentFrame(parent_frame)) {
      return std::nullopt;
    }
    const auto *row = context_.zarr_loader->resolveClippedFrame(parent_frame);
    if (row == nullptr) {
      return std::nullopt;
    }
    return static_cast<int>(std::clamp<int64_t>(
        row->clip_local_frame_index, 0, std::numeric_limits<int>::max()));
  }
  return parent_frame;
}

std::string MediaSessionLoader::activeRecordingClipIndexPath() const {
  if (context_.clipped_media_state == nullptr ||
      context_.clipped_media_state->recording_clip_provider == nullptr) {
    return {};
  }
  return context_.clipped_media_state->recording_clip_provider->index()
      .indexPath()
      .string();
}

void MediaSessionLoader::stopCameraDecodersForReload() const {
  if (context_.decoder_context == nullptr ||
      context_.decoder_threads == nullptr || context_.demuxers == nullptr ||
      context_.camera_names == nullptr || context_.image_names == nullptr ||
      context_.is_view_focused == nullptr ||
      context_.window_need_decoding == nullptr ||
      context_.window_was_decoding == nullptr ||
      context_.video_loaded == nullptr || context_.input_is_imgs == nullptr) {
    return;
  }

  context_.decoder_context->stop_flag = true;
  for (auto &thread : *context_.decoder_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  context_.decoder_threads->clear();

  for (const auto &camera_name : *context_.camera_names) {
    (*context_.window_need_decoding)[camera_name].store(false);
    (*context_.window_was_decoding)[camera_name] = false;
    latest_decoded_frame[camera_name].store(-1);
  }

  context_.demuxers->clear();
  context_.camera_names->clear();
  context_.image_names->clear();
  context_.is_view_focused->clear();
  *context_.video_loaded = false;
  *context_.input_is_imgs = false;
  context_.decoder_context->stop_flag = false;
  context_.decoder_context->decoding_flag = false;
  context_.decoder_context->estimated_num_frames = 0;
  if (context_.clipped_media_state != nullptr) {
    resetClippedMediaState(*context_.clipped_media_state);
  }
}

bool MediaSessionLoader::loadClippedVideoForParentFrame(
    int parent_frame) const {
  const auto recording_provider =
      context_.clipped_media_state != nullptr
          ? context_.clipped_media_state->recording_clip_provider
          : nullptr;
  const bool recording_clip_media = recording_provider != nullptr;
  const bool legacy_clipped_media =
      context_.zarr_loaded != nullptr && *context_.zarr_loaded &&
      context_.zarr_loader != nullptr &&
      context_.zarr_loader->hasClippedCollection();
  if (!recording_clip_media && !legacy_clipped_media) {
    return true;
  }
  if (context_.scene == nullptr || context_.decoder_context == nullptr ||
      context_.playback_state == nullptr || context_.root_dir == nullptr ||
      context_.skeleton_dir == nullptr || context_.camera_names == nullptr ||
      context_.decoder_threads == nullptr || context_.demuxers == nullptr ||
      context_.is_view_focused == nullptr ||
      context_.window_need_decoding == nullptr ||
      context_.window_was_decoding == nullptr ||
      context_.video_loaded == nullptr || context_.input_is_imgs == nullptr ||
      context_.label_buffer_size == nullptr || context_.video_fps == nullptr) {
    return false;
  }

  size_t selected_index = std::numeric_limits<size_t>::max();
  std::string clip_id;
  std::string camera_serial;
  std::string resolved_video;
  int64_t clip_local_frame = -1;
  int64_t first_parent_frame = -1;
  int64_t last_parent_frame = -1;
  std::shared_ptr<const std::vector<int64_t>> frame_map;
  const std::vector<int64_t> *legacy_frame_map_source = nullptr;
  ClippedMediaSource source = ClippedMediaSource::None;

  if (recording_clip_media) {
    const auto binding = recording_provider->resolveParentFrame(parent_frame);
    if (!binding) {
      std::cout << "[RecordingClipIndex] No media mapping for parent frame "
                << parent_frame << std::endl;
      return false;
    }
    selected_index = binding->clip_index;
    clip_id = binding->clip_id;
    camera_serial = binding->camera_serial;
    resolved_video = binding->video_path.string();
    clip_local_frame = binding->clip_local_frame;
    first_parent_frame = binding->first_parent_frame;
    last_parent_frame = binding->last_parent_frame;
    frame_map = binding->parent_frame_by_clip_local;
    source = ClippedMediaSource::RecordingClipIndex;
  } else {
    const auto *row = context_.zarr_loader->resolveClippedFrame(parent_frame);
    if (row == nullptr) {
      std::cout << "[Zarr] No clipped frame-run mapping for parent frame "
                << parent_frame << std::endl;
      return false;
    }
    const auto *selected =
        context_.zarr_loader->getClippedResolver().selectedRun(
            row->selected_run_index);
    if (selected == nullptr || selected->video_path.empty()) {
      std::cout
          << "[Zarr] Clipped mapping has no source video for parent frame "
          << parent_frame << std::endl;
      return false;
    }
    auto resolved_video_opt = ResolveAffiliatedVideoPath(
        selected->video_path, context_.zarr_loader->getArchivePath());
    if (!resolved_video_opt.has_value()) {
      std::cout << "[Zarr] Could not resolve clipped source video path: "
                << selected->video_path << std::endl;
      return false;
    }
    selected_index = row->selected_run_index;
    clip_id = selected->clip_id;
    camera_serial = selected->camera_serial;
    resolved_video = resolved_video_opt->string();
    clip_local_frame = row->clip_local_frame_index;
    legacy_frame_map_source = &selected->parent_frame_by_clip_local;
    std::tie(first_parent_frame, last_parent_frame) =
        parentFrameRangeForClip(*legacy_frame_map_source);
    source = ClippedMediaSource::LegacyZarrCollection;
  }

  PaletteClippedMediaState *clipped_state = context_.clipped_media_state;
  const bool requested_clip_loaded =
      *context_.video_loaded && clipped_state != nullptr &&
      clipped_state->source == source &&
      clipped_state->handoff.selected_run_index == selected_index &&
      parent_frame >= clipped_state->handoff.first_parent_frame &&
      parent_frame <= clipped_state->handoff.last_parent_frame &&
      !context_.decoder_threads->empty();
  if (requested_clip_loaded) {
    if (context_.scene->num_cams > 0 && !context_.scene->cameras.empty()) {
      std::lock_guard<std::mutex> lock(g_seek_info_mutex);
      context_.scene->cameras[0].seek_context.frame_number_map =
          clipped_state->parent_frame_by_clip_local;
    }
    return true;
  }
  if (frame_map == nullptr && legacy_frame_map_source != nullptr) {
    frame_map =
        std::make_shared<const std::vector<int64_t>>(*legacy_frame_map_source);
  }

  if (*context_.video_loaded || !context_.decoder_threads->empty() ||
      !context_.demuxers->empty()) {
    stopCameraDecodersForReload();
  }

  try {
    *context_.input_is_imgs = false;
    context_.camera_names->clear();
    context_.demuxers->clear();
    context_.is_view_focused->clear();

    std::string camera_name = "Cam" + camera_serial + "_" + clip_id;
    context_.camera_names->push_back(camera_name);
    (*context_.window_need_decoding)[camera_name].store(true);
    (*context_.window_was_decoding)[camera_name] = true;
    latest_decoded_frame[camera_name].store(-1);

    std::map<std::string, std::string> ffmpeg_options;
    context_.demuxers->push_back(std::make_unique<FFmpegDemuxer>(
        resolved_video.c_str(), ffmpeg_options));

    context_.decoder_context->seek_interval =
        static_cast<int>(context_.demuxers->at(0)->FindKeyFrameInterval());
    *context_.video_fps = context_.demuxers->at(0)->GetFramerate();
    context_.scene->num_cams = 1;
    context_.scene->cameras.resize(context_.scene->num_cams);
    context_.scene->cameras[0].image_width =
        context_.demuxers->at(0)->GetWidth();
    context_.scene->cameras[0].image_height =
        context_.demuxers->at(0)->GetHeight();

    const bool needs_allocation =
        context_.scene->cameras[0].display_buffer == nullptr ||
        context_.scene->size_of_buffer == 0;
    if (needs_allocation) {
      render_allocate_scene_memory(context_.scene, *context_.label_buffer_size);
    } else {
      for (u32 i = 0; i < context_.scene->size_of_buffer; ++i) {
        frameSlotReleaseForReuse(context_.scene->cameras[0].display_buffer[i]);
      }
      context_.scene->cameras[0].last_uploaded_frame = -1;
      context_.scene->cameras[0].last_uploaded_local_frame = -1;
      context_.scene->cameras[0].last_uploaded_pts = -1;
      context_.scene->cameras[0].texture_has_valid_frame = false;
      context_.scene->cameras[0].playback_staging_frame = -1;
      context_.scene->cameras[0].playback_staging_local_frame = -1;
      context_.scene->cameras[0].playback_staging_pts = -1;
      context_.scene->cameras[0].playback_staging_valid = false;
    }

    {
      std::lock_guard<std::mutex> lock(g_seek_info_mutex);
      context_.scene->cameras[0].seek_context.frame_number_map = frame_map;
    }
    if (clipped_state != nullptr) {
      clipped_state->source = source;
      clipped_state->recording_clip_provider = recording_provider;
      clipped_state->current_video_path = resolved_video;
      clipped_state->camera_serial = camera_serial;
      clipped_state->parent_frame_by_clip_local = frame_map;
      clipped_state->handoff.selected_run_index = selected_index;
      clipped_state->handoff.clip_id = clip_id;
      clipped_state->handoff.first_parent_frame = first_parent_frame;
      clipped_state->handoff.last_parent_frame = last_parent_frame;
      if (clipped_state->handoff.pending_switch_parent_frame != parent_frame) {
        clipped_state->handoff.pending_switch_parent_frame = -1;
        clipped_state->handoff.switch_in_progress = false;
      }
    }

    const int64_t total_parent_frames =
        recording_clip_media
            ? recording_provider->index().totalFrameCount()
            : static_cast<int64_t>(context_.zarr_loader->getTotalFrames());
    context_.decoder_context->total_num_frame =
        static_cast<int>(std::clamp<int64_t>(total_parent_frames, 0,
                                             std::numeric_limits<int>::max()));
    context_.decoder_context->estimated_num_frames =
        std::max(0, context_.decoder_context->total_num_frame - 1);

    context_.decoder_threads->push_back(std::thread(
        &decoder_process, context_.decoder_context,
        context_.demuxers->at(0).get(), context_.camera_names->at(0),
        context_.scene->cameras[0].display_buffer,
        context_.scene->size_of_buffer,
        &context_.scene->cameras[0].seek_context,
        context_.scene->use_cpu_buffer));
    context_.is_view_focused->push_back(false);
    *context_.video_loaded = true;
    configurePlaybackTransport(context_, context_.demuxers->at(0).get(),
                               parent_frame, true);

    std::filesystem::path inferred_root = InferRecordingRootPath(
        resolved_video, context_.zarr_loader->getArchivePath());
    if (!inferred_root.empty()) {
      *context_.root_dir = inferred_root.string();
      *context_.skeleton_dir = *context_.root_dir;
    }

    std::cout << (recording_clip_media ? "[RecordingClipIndex]" : "[Zarr]")
              << " Loaded clipped video for parent frame " << parent_frame
              << " -> " << clip_id << " local frame " << clip_local_frame
              << ": " << resolved_video << std::endl;
    loadCameraCalibrationsForCurrentMedia();
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[Zarr] Failed to load clipped source video: " << e.what()
              << std::endl;
    stopCameraDecodersForReload();
    return false;
  }
}

void MediaSessionLoader::loadCameraCalibrationsForCurrentMedia() const {
  if (context_.video_loaded == nullptr || !*context_.video_loaded ||
      context_.scene == nullptr || context_.camera_names == nullptr ||
      context_.camera_params == nullptr || context_.root_dir == nullptr ||
      context_.error_message == nullptr || context_.show_error == nullptr) {
    return;
  }

  context_.camera_params->resize(context_.scene->num_cams);
  std::cout << "\n=== Loading Camera Calibrations from Zarr/YAML ==="
            << std::endl;
  for (size_t i = 0; i < context_.camera_names->size(); ++i) {
    const std::string &camera_name = (*context_.camera_names)[i];
    std::cout << "\nProcessing camera " << i << ": " << camera_name
              << std::endl;

    bool loaded_from_zarr = false;
    if (context_.zarr_loader != nullptr && context_.zarr_loaded != nullptr &&
        *context_.zarr_loaded &&
        !context_.zarr_loader->getArchivePath().empty()) {
      std::string zarr_status;
      auto calibration = context_.zarr_loader->loadCalibrationForCamera(
          camera_name, zarr_status);
      if (calibration.has_value()) {
        std::string apply_error;
        if (applyZarrCalibrationToCameraParams(
                *calibration, (*context_.camera_params)[i], apply_error)) {
          std::cout << "Loading homography from Zarr for camera: "
                    << camera_name << std::endl;
          std::cout << "  " << zarr_status << std::endl;
          std::cout << "  Zarr homography semantics: projector/texture px -> "
                       "camera px"
                    << std::endl;
          std::cout
              << "  Applied to legacy CameraParams: inverse_homography_matrix"
              << " holds projector/texture px -> camera px" << std::endl;
          std::cout << "  Stimulus texture offset: ("
                    << (*context_.camera_params)[i].stimulus_offset_x << ", "
                    << (*context_.camera_params)[i].stimulus_offset_y << ")"
                    << std::endl;
          camera_print_calibration_details((*context_.camera_params)[i],
                                           camera_name);
          loaded_from_zarr = true;
        } else {
          std::cerr << "Warning: Failed to apply Zarr calibration for camera "
                    << camera_name << ": " << apply_error << std::endl;
        }
      } else if (!zarr_status.empty()) {
        std::cout << "  " << zarr_status << std::endl;
      }
    }

    if (loaded_from_zarr) {
      continue;
    }

    std::string yaml_file =
        *context_.root_dir + "/calibration/" + camera_name + ".yaml";
    if (std::filesystem::exists(yaml_file)) {
      std::cout << "Loading homography from YAML for camera: " << camera_name
                << std::endl;
      if (!camera_load_params_from_yaml(yaml_file, (*context_.camera_params)[i],
                                        *context_.error_message)) {
        std::cerr << "Error: Failed to load calibration from YAML: "
                  << *context_.error_message << std::endl;
        *context_.show_error = true;
        break;
      }
      camera_print_calibration_details((*context_.camera_params)[i],
                                       camera_name);
    } else {
      std::cerr << "Warning: No calibration YAML file found at: " << yaml_file
                << std::endl;
    }
  }
}

bool MediaSessionLoader::loadSingleVideoMedia(
    const std::filesystem::path &video_path, bool infer_recording_root,
    const char *success_label) const {
  crimson::media::CameraMediaOpenPlan plan;
  std::string error_message;
  if (!crimson::media::BuildCameraMediaOpenPlan(
          {{video_path.filename().string(), video_path}}, plan,
          error_message)) {
    std::cerr << success_label << " failed: " << error_message << std::endl;
    return false;
  }
  return executeCameraMediaPlan(plan, infer_recording_root, success_label,
                                nullptr);
}

bool MediaSessionLoader::loadSelectedCameraMedia(
    const std::vector<crimson::media::CameraMediaSelection> &selections,
    std::string &error_message) const {
  crimson::media::CameraMediaOpenPlan plan;
  if (!crimson::media::BuildCameraMediaOpenPlan(selections, plan,
                                                error_message)) {
    return false;
  }
  return executeCameraMediaPlan(
      plan, false, "[Media] Loaded camera media: ", &error_message);
}

bool MediaSessionLoader::executeCameraMediaPlan(
    const crimson::media::CameraMediaOpenPlan &plan, bool infer_recording_root,
    const char *success_label, std::string *error_message) const {
  if (context_.scene == nullptr || context_.decoder_context == nullptr ||
      context_.zarr_loader == nullptr || context_.stimulus_player == nullptr ||
      context_.playback_state == nullptr || context_.root_dir == nullptr ||
      context_.skeleton_dir == nullptr || context_.camera_names == nullptr ||
      context_.image_names == nullptr || context_.decoder_threads == nullptr ||
      context_.demuxers == nullptr || context_.is_view_focused == nullptr ||
      context_.window_need_decoding == nullptr ||
      context_.window_was_decoding == nullptr ||
      context_.video_loaded == nullptr || context_.input_is_imgs == nullptr ||
      context_.label_buffer_size == nullptr || context_.video_fps == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Camera media loader is not fully configured";
    }
    return false;
  }

  bool replacement_started = false;
  try {
    std::vector<std::unique_ptr<FFmpegDemuxer>> prepared_demuxers;
    std::vector<std::pair<int, int>> camera_dimensions;
    int prepared_seek_interval = 1;
    double prepared_video_fps = *context_.video_fps;
    int prepared_buffer_size = *context_.label_buffer_size;

    if (plan.kind == crimson::media::CameraMediaKind::VideoFiles) {
      prepared_demuxers.reserve(plan.video_paths.size());
      camera_dimensions.reserve(plan.video_paths.size());
      std::map<std::string, std::string> ffmpeg_options;
      for (const auto &video_path : plan.video_paths) {
        auto demuxer = std::make_unique<FFmpegDemuxer>(
            video_path.string().c_str(), ffmpeg_options);
        const int width = demuxer->GetWidth();
        const int height = demuxer->GetHeight();
        if (width <= 0 || height <= 0) {
          throw std::runtime_error("Camera video has invalid dimensions: " +
                                   video_path.string());
        }
        camera_dimensions.push_back({width, height});
        prepared_demuxers.push_back(std::move(demuxer));
      }
      prepared_seek_interval =
          static_cast<int>(prepared_demuxers.front()->FindKeyFrameInterval());
      prepared_video_fps = prepared_demuxers.front()->GetFramerate();
    } else {
      camera_dimensions.reserve(plan.camera_names.size());
      for (const auto &camera_name : plan.camera_names) {
        const std::filesystem::path sample_path =
            std::filesystem::path(*context_.root_dir) /
            (camera_name + "_" + plan.image_frame_names.front());
        const cv::Mat image =
            cv::imread(sample_path.string(), cv::IMREAD_COLOR);
        if (image.empty()) {
          throw std::runtime_error("Could not read image sequence sample: " +
                                   sample_path.string());
        }
        camera_dimensions.push_back({image.cols, image.rows});
      }
      prepared_buffer_size = static_cast<int>(std::min<size_t>(
          static_cast<size_t>(std::max(1, *context_.label_buffer_size)),
          plan.image_frame_names.size()));
    }

    stopCameraDecodersForReload();
    replacement_started = true;
    *context_.input_is_imgs =
        plan.kind == crimson::media::CameraMediaKind::ImageSequence;
    *context_.camera_names = plan.camera_names;
    *context_.image_names = plan.image_frame_names;
    *context_.demuxers = std::move(prepared_demuxers);
    *context_.label_buffer_size = prepared_buffer_size;
    context_.decoder_context->seek_interval = prepared_seek_interval;
    if (plan.kind == crimson::media::CameraMediaKind::VideoFiles) {
      *context_.video_fps = prepared_video_fps;
    } else {
      context_.decoder_context->total_num_frame =
          static_cast<int>(plan.image_frame_names.size());
      context_.decoder_context->estimated_num_frames =
          static_cast<int>(plan.image_frame_names.size());
    }

    for (const auto &camera_name : *context_.camera_names) {
      (*context_.window_need_decoding)[camera_name].store(true);
      (*context_.window_was_decoding)[camera_name] = true;
    }

    context_.scene->num_cams = static_cast<decltype(context_.scene->num_cams)>(
        context_.camera_names->size());
    context_.scene->cameras.resize(context_.scene->num_cams);
    for (size_t index = 0; index < camera_dimensions.size(); ++index) {
      context_.scene->cameras[index].image_width =
          camera_dimensions[index].first;
      context_.scene->cameras[index].image_height =
          camera_dimensions[index].second;
    }
    render_allocate_scene_memory(context_.scene, *context_.label_buffer_size);

    for (size_t index = 0; index < context_.camera_names->size(); ++index) {
      if (*context_.input_is_imgs) {
        context_.decoder_threads->push_back(std::thread(
            &image_loader, context_.decoder_context, *context_.image_names,
            context_.scene->cameras[index].display_buffer,
            context_.scene->size_of_buffer,
            &context_.scene->cameras[index].seek_context,
            context_.scene->use_cpu_buffer, context_.camera_names->at(index),
            *context_.root_dir));
      } else {
        context_.decoder_threads->push_back(
            std::thread(&decoder_process, context_.decoder_context,
                        context_.demuxers->at(index).get(),
                        context_.camera_names->at(index),
                        context_.scene->cameras[index].display_buffer,
                        context_.scene->size_of_buffer,
                        &context_.scene->cameras[index].seek_context,
                        context_.scene->use_cpu_buffer));
      }
      context_.is_view_focused->push_back(false);
    }
    *context_.video_loaded = true;

    const int initial_frame =
        std::max(0, context_.playback_state->to_display_frame_number);
    const FFmpegDemuxer *primary_demuxer =
        context_.demuxers->empty() ? nullptr : context_.demuxers->front().get();
    configurePlaybackTransport(context_, primary_demuxer, initial_frame, false);
    if (!*context_.input_is_imgs) {
      const double seek_fps =
          (*context_.video_fps > 0.0) ? *context_.video_fps : 30.0;
      seek_all_cameras(context_.scene, initial_frame, seek_fps,
                       *context_.playback_state, true, context_.zarr_loader,
                       context_.stimulus_player);
    }

    if (infer_recording_root && !plan.video_paths.empty()) {
      std::filesystem::path inferred_root = InferRecordingRootPath(
          plan.video_paths.front(), context_.zarr_loader->getArchivePath());
      if (!inferred_root.empty()) {
        *context_.root_dir = inferred_root.string();
        *context_.skeleton_dir = *context_.root_dir;
      }
    }

    std::cout << success_label;
    if (plan.kind == crimson::media::CameraMediaKind::VideoFiles) {
      std::cout << plan.video_paths.front().string();
    } else {
      std::cout << *context_.root_dir;
    }
    std::cout << " cameras=" << context_.camera_names->size() << std::endl;
    if (context_.clipped_media_state != nullptr) {
      *context_.clipped_media_state = PaletteClippedMediaState();
    }
    loadCameraCalibrationsForCurrentMedia();
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  } catch (const std::exception &e) {
    std::cerr << success_label << " failed: " << e.what() << std::endl;
    if (replacement_started) {
      stopCameraDecodersForReload();
    }
    if (error_message != nullptr) {
      *error_message = e.what();
    }
    return false;
  }
}

void MediaSessionLoader::tryAutoLoadAffiliatedVideoFromZarr(
    const char *trigger_label) const {
  if (context_.zarr_loaded == nullptr || !*context_.zarr_loaded ||
      context_.video_loaded == nullptr || context_.decoder_threads == nullptr ||
      context_.zarr_loader == nullptr) {
    return;
  }

  const std::string archive_path = context_.zarr_loader->getArchivePath();
  std::string discovery_error;
  const auto archive =
      crimson::zarr::ArchiveContext::Open(archive_path, &discovery_error);
  if (!archive) {
    std::cout << "[Zarr] Could not open shared archive context for affiliated "
                 "video discovery: "
              << discovery_error << std::endl;
    return;
  }

  if (context_.zarr_loader->hasClippedCollection()) {
    const int parent_frame =
        context_.playback_state != nullptr
            ? std::max(0, context_.playback_state->to_display_frame_number)
            : 0;
    if (loadClippedVideoForParentFrame(parent_frame)) {
      std::cout << "[Zarr] Auto-loaded legacy clipped collection media ("
                << trigger_label << ")" << std::endl;
    }
    return;
  }

  if (*context_.video_loaded || !context_.decoder_threads->empty()) {
    std::cout << "[Zarr] Skipping affiliated video auto-load (" << trigger_label
              << "): media already loaded" << std::endl;
    return;
  }

  const auto affiliated_video =
      crimson::zarr::DiscoverAffiliatedVideo(archive, &discovery_error);
  std::optional<std::filesystem::path> resolved_video_opt;
  if (affiliated_video) {
    resolved_video_opt = affiliated_video->resolved_path;
    std::cout << "[Zarr] Discovered affiliated video source="
              << crimson::zarr::AffiliatedVideoSourceName(
                     affiliated_video->source)
              << " resolution="
              << crimson::zarr::AffiliatedVideoResolutionName(
                     affiliated_video->resolution)
              << " stored=" << affiliated_video->stored_path
              << " resolved=" << affiliated_video->resolved_path << std::endl;
  } else {
    const std::string video_discovery_error = discovery_error;
    discovery_error.clear();
    const auto recording_clip_index =
        crimson::zarr::DiscoverAffiliatedRecordingClipIndex(archive,
                                                            &discovery_error);
    if (recording_clip_index) {
      std::string provider_error;
      if (!activateRecordingClipIndex(recording_clip_index->index_path,
                                      &provider_error)) {
        std::cout
            << "[RecordingClipIndex] Could not activate affiliated media: "
            << provider_error << std::endl;
        return;
      }
      const int parent_frame =
          context_.playback_state != nullptr
              ? std::max(0, context_.playback_state->to_display_frame_number)
              : 0;
      if (loadClippedVideoForParentFrame(parent_frame)) {
        std::cout << "[RecordingClipIndex] Auto-loaded indexed media ("
                  << trigger_label
                  << ") index=" << recording_clip_index->index_path
                  << " frames=" << recording_clip_index->frame_count
                  << " fps=" << recording_clip_index->frames_per_second
                  << std::endl;
      }
      return;
    }
    if (!discovery_error.empty()) {
      std::cout << "[RecordingClipIndex] Affiliated media discovery failed: "
                << discovery_error << std::endl;
      return;
    }
    if (!video_discovery_error.empty()) {
      std::cout << "[Zarr] Affiliated video discovery failed: "
                << video_discovery_error << std::endl;
      return;
    }

    const std::string source_hint = context_.zarr_loader->getSourceVideoPath();
    if (source_hint.empty()) {
      std::cout << "[Zarr] Archive did not provide source video metadata; "
                   "skipping affiliated video auto-load"
                << std::endl;
      return;
    }
    resolved_video_opt = ResolveAffiliatedVideoPath(source_hint, archive_path);
    if (!resolved_video_opt) {
      std::cout << "[Zarr] Legacy affiliated-video hint could not be resolved: "
                << source_hint << std::endl;
      return;
    }
    std::cout << "[Zarr] Shared affiliated-video metadata absent; using "
                 "legacy loader hint compatibility stored="
              << source_hint << " resolved=" << *resolved_video_opt
              << std::endl;
  }

  if (!loadSingleVideoMedia(
          *resolved_video_opt, true,
          (std::string("[Zarr] Auto-loaded affiliated video (") +
           trigger_label + "): ")
              .c_str())) {
    std::cerr << "[Zarr] Failed to auto-load affiliated video ("
              << trigger_label << ")" << std::endl;
  }
}

void MediaSessionLoader::tryAutoLoadStimulusVideo(
    const char *trigger_label) const {
  if (context_.zarr_loaded == nullptr || !*context_.zarr_loaded ||
      context_.stimulus_player == nullptr || context_.zarr_loader == nullptr ||
      context_.root_dir == nullptr ||
      context_.stimulus_buffer_size == nullptr ||
      context_.stimulus_use_cpu_buffer == nullptr ||
      context_.stimulus_use_software_decode == nullptr ||
      context_.window_was_decoding == nullptr ||
      context_.window_need_decoding == nullptr ||
      context_.video_loaded == nullptr || context_.playback_state == nullptr) {
    return;
  }
  if (context_.stimulus_player->loaded ||
      !context_.zarr_loader->hasStimulusAlignment()) {
    return;
  }

  auto resolved = ResolveStimulusVideoPath(
      context_.zarr_loader->getStimulusVideoPath(),
      context_.zarr_loader->getStimulusSourceH5(),
      context_.zarr_loader->getArchivePath(), *context_.root_dir);
  if (!resolved.has_value()) {
    std::cout << "[Stimulus] Could not auto-discover stimulus video ("
              << trigger_label << ")" << std::endl;
    return;
  }

  const int stim_buf_size = std::max(1, *context_.stimulus_buffer_size);
  if (!initializeStimulusPlayback(
          *context_.stimulus_player, resolved->string(), stim_buf_size,
          *context_.stimulus_use_cpu_buffer,
          *context_.stimulus_use_software_decode, context_.cuda_device_index)) {
    std::cerr << "[Stimulus] Failed to auto-load stimulus video: "
              << resolved->string() << std::endl;
    return;
  }

  (*context_.window_was_decoding)[context_.stimulus_player->window_name] =
      false;
  (*context_.window_need_decoding)[context_.stimulus_player->window_name].store(
      false);

  if (*context_.video_loaded) {
    scheduleStimulusSeek(*context_.stimulus_player, context_.zarr_loader,
                         context_.playback_state->to_display_frame_number,
                         !context_.playback_state->play_video);
  }

  std::cout << "[Stimulus] Auto-loaded stimulus video (" << trigger_label
            << "): " << resolved->string() << std::endl;
}

void MediaSessionLoader::bootstrapFromCli(
    const std::string &cli_zarr_override_path,
    const std::string &cli_recording_path,
    const std::string &cli_recording_clip_index_path,
    const std::function<void()> &refresh_detection_dataset_options,
    const std::function<void()> &clear_bbox_edits) const {
  if (context_.zarr_loaded == nullptr || context_.root_dir == nullptr ||
      context_.skeleton_dir == nullptr || context_.zarr_loader == nullptr ||
      context_.video_loaded == nullptr || context_.recording_opens == nullptr) {
    return;
  }
  if (cli_zarr_override_path.empty() && cli_recording_path.empty()) {
    return;
  }

  crimson::session::SessionDescriptor requested_session;
  requested_session.zarr_path = cli_zarr_override_path;
  requested_session.recording_clip_index_path = cli_recording_clip_index_path;
  std::vector<crimson::session::SessionReadinessProductRule> products;
  if (!cli_zarr_override_path.empty()) {
    products.push_back(
        {"archive",
         cli_recording_path.empty()
             ? crimson::session::ProductAvailabilityRequirement::Required
             : crimson::session::ProductAvailabilityRequirement::Optional});
    products.push_back(
        {"affiliated_media",
         crimson::session::ProductAvailabilityRequirement::Optional});
  }
  if (!cli_recording_path.empty()) {
    products.push_back(
        {"recording_archive",
         crimson::session::ProductAvailabilityRequirement::Optional});
    products.push_back(
        {"recording_media",
         crimson::session::ProductAvailabilityRequirement::Optional});
  }
  auto &session_open = *context_.recording_opens;
  if (!session_open.begin(
          {requested_session, "Opening session", std::move(products)})) {
    return;
  }
  auto finishSession = [&](bool ready,
                           crimson::session::SessionDescriptor resolved,
                           const std::string &error) {
    if (session_open.active()) {
      if (ready) {
        std::string transaction_error;
        session_open.commit(std::move(resolved), "Session ready",
                            "Session unavailable", &transaction_error);
      } else {
        session_open.fail(error);
      }
    }
  };
  auto loadExplicitRecordingClipIndex = [&](std::string *error) {
    if (cli_recording_clip_index_path.empty()) {
      return true;
    }
    if (!activateRecordingClipIndex(cli_recording_clip_index_path, error)) {
      return false;
    }
    const int parent_frame =
        context_.playback_state != nullptr
            ? std::max(0, context_.playback_state->to_display_frame_number)
            : 0;
    if (!loadClippedVideoForParentFrame(parent_frame)) {
      if (error != nullptr && error->empty()) {
        *error = "Failed to open explicit recording clip media";
      }
      return false;
    }
    std::cout << "[RecordingClipIndex] Loaded explicit index="
              << cli_recording_clip_index_path << std::endl;
    return true;
  };

  if (!cli_zarr_override_path.empty()) {
    session_open.startProduct("archive", "Resolving archive");
    std::string zarr_error;
    if (loadZarrDetectionFromPath(cli_zarr_override_path, *context_.zarr_loader,
                                  zarr_error)) {
      *context_.zarr_loaded = true;
      refresh_detection_dataset_options();
      clear_bbox_edits();
      std::cout << "Loaded Zarr archive from --zarr: "
                << context_.zarr_loader->getArchivePath() << std::endl;
      std::string media_error;
      const bool explicit_media_ready =
          loadExplicitRecordingClipIndex(&media_error);
      if (explicit_media_ready && cli_recording_clip_index_path.empty()) {
        tryAutoLoadAffiliatedVideoFromZarr("--zarr");
      }
      tryAutoLoadStimulusVideo("--zarr");
      session_open.completeProduct("archive", "Archive ready", true);
      session_open.startProduct("affiliated_media",
                                "Resolving affiliated media");
      session_open.completeProduct("affiliated_media",
                                   *context_.video_loaded
                                       ? "Affiliated media ready"
                                       : "No affiliated media",
                                   *context_.video_loaded, media_error);
      if (!explicit_media_ready) {
        finishSession(false, {}, media_error);
        return;
      }
      if (cli_recording_path.empty()) {
        crimson::session::SessionDescriptor resolved;
        resolved.zarr_path = context_.zarr_loader->getArchivePath();
        resolved.recording_clip_index_path = activeRecordingClipIndexPath();
        finishSession(true, std::move(resolved), {});
      }
    } else {
      clear_bbox_edits();
      std::cerr << "Failed to load --zarr archive: " << zarr_error << std::endl;
      session_open.completeProduct("archive", "Archive unavailable", false,
                                   zarr_error);
      session_open.startProduct("affiliated_media",
                                "Skipping affiliated media");
      session_open.completeProduct("affiliated_media", "No affiliated media",
                                   false);
      if (cli_recording_path.empty()) {
        finishSession(false, {}, zarr_error);
      }
    }
  }

  if (cli_recording_path.empty()) {
    return;
  }

  *context_.root_dir = cli_recording_path;
  *context_.skeleton_dir = *context_.root_dir;

  session_open.startProduct("recording_archive", "Resolving recording archive");
  std::string zarr_error;
  if (loadZarrDetectionFromDirectory(*context_.root_dir, *context_.zarr_loader,
                                     zarr_error)) {
    *context_.zarr_loaded = true;
    refresh_detection_dataset_options();
    clear_bbox_edits();
    std::cout << "Loaded Zarr archive from --recording: "
              << context_.zarr_loader->getArchivePath() << std::endl;
    std::string media_error;
    const bool explicit_media_ready =
        loadExplicitRecordingClipIndex(&media_error);
    if (explicit_media_ready && cli_recording_clip_index_path.empty()) {
      tryAutoLoadAffiliatedVideoFromZarr("--recording");
    }
    tryAutoLoadStimulusVideo("--recording");
    session_open.completeProduct("recording_archive", "Recording archive ready",
                                 true);
    session_open.startProduct("recording_media", "Resolving recording media");
    session_open.completeProduct("recording_media",
                                 *context_.video_loaded
                                     ? "Recording media ready"
                                     : "No affiliated recording media",
                                 *context_.video_loaded, media_error);
    if (!explicit_media_ready) {
      finishSession(false, {}, media_error);
      return;
    }
    crimson::session::SessionDescriptor resolved;
    resolved.zarr_path = context_.zarr_loader->getArchivePath();
    resolved.recording_clip_index_path = activeRecordingClipIndexPath();
    finishSession(true, std::move(resolved), {});
    return;
  }

  session_open.completeProduct("recording_archive", "No recording archive",
                               false);

  clear_bbox_edits();
  std::cout << "[--recording] No zarr archive found (optional): " << zarr_error
            << std::endl;

  namespace fs = std::filesystem;
  std::string found_video;
  std::vector<fs::path> search_dirs;
  const fs::path cams_dir = fs::path(*context_.root_dir) / "cams";
  if (IsDirectoryNoThrow(cams_dir)) {
    search_dirs.push_back(cams_dir);
  }
  search_dirs.push_back(fs::path(*context_.root_dir));

  for (const auto &search_dir : search_dirs) {
    if (!found_video.empty()) {
      break;
    }
    std::error_code ec;
    for (auto it = fs::directory_iterator(search_dir, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it->is_regular_file(ec) && !ec && IsSupportedVideoPath(it->path())) {
        found_video = it->path().string();
        break;
      }
    }
  }

  if (found_video.empty()) {
    std::cout << "[--recording] No video files found in " << *context_.root_dir
              << std::endl;
    finishSession(false, {}, "No supported recording media was found");
    return;
  }

  session_open.startProduct("recording_media", "Opening recording media");
  if (loadSingleVideoMedia(found_video, false,
                           "[--recording] Auto-loaded video: ")) {
    tryAutoLoadStimulusVideo("--recording-fallback");
    session_open.completeProduct("recording_media", "Recording media ready",
                                 true);
    crimson::session::SessionDescriptor resolved;
    resolved.video_path = found_video;
    finishSession(true, std::move(resolved), {});
  } else {
    std::cerr << "[--recording] Failed to load video" << std::endl;
    session_open.completeProduct("recording_media",
                                 "Recording media unavailable", false,
                                 "Failed to load recording video");
    finishSession(false, {}, "Failed to load recording video");
  }
}
