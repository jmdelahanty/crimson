#include "media_session_loader.h"
#include "clipped_media_prewarm_policy.h"
#include "frame_slot.h"
#include "global.h"
#include "platform/nvidia/nvidia_stimulus_media_loader.h"
#include "render.h"
#include "zarr/affiliated_video_repository.h"
#include "zarr/archive_context.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
#include <unordered_set>
#include <cuda_runtime_api.h>

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
    // A same-rate clip handoff does not change the parent timeline. Updating
    // it here floors the fractional playback position and delays the next
    // frame even when it is already decoded in the ring.
    if (context.playback_transport->framesPerSecond() != *context.video_fps ||
        context.playback_transport->frameCount() != frame_count) {
      context.playback_transport->updateTimeline(*context.video_fps,
                                                 frame_count);
    }
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

bool prepareCameraMedia(const crimson::media::CameraMediaOpenPlan &plan,
                        const std::string &image_root, int buffer_size,
                        double video_fps, PreparedCameraMedia &prepared,
                        std::string &error) {
  try {
    PreparedCameraMedia result;
    result.plan = plan;
    result.buffer_size = buffer_size;
    result.video_fps = video_fps;
    if (plan.camera_names.empty()) {
      error = "Camera media selection is empty";
      return false;
    }
    if (plan.kind == crimson::media::CameraMediaKind::VideoFiles) {
      if (plan.video_paths.size() != plan.camera_names.size()) {
        error = "Camera video paths do not match selected cameras";
        return false;
      }
      result.demuxers.reserve(plan.video_paths.size());
      result.camera_dimensions.reserve(plan.video_paths.size());
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
        result.camera_dimensions.push_back({width, height});
        result.demuxers.push_back(std::move(demuxer));
      }
      result.seek_interval = static_cast<int>(
          result.demuxers.front()->FindKeyFrameInterval());
      result.video_fps = result.demuxers.front()->GetFramerate();
    } else {
      if (plan.image_frame_names.empty()) {
        error = "Image sequence has no frames";
        return false;
      }
      result.camera_dimensions.reserve(plan.camera_names.size());
      for (const auto &camera_name : plan.camera_names) {
        const auto sample_path = std::filesystem::path(image_root) /
                                 (camera_name + "_" +
                                  plan.image_frame_names.front());
        const cv::Mat image = cv::imread(sample_path.string(), cv::IMREAD_COLOR);
        if (image.empty()) {
          throw std::runtime_error("Could not read image sequence sample: " +
                                   sample_path.string());
        }
        result.camera_dimensions.push_back({image.cols, image.rows});
      }
      result.buffer_size = static_cast<int>(std::min<size_t>(
          static_cast<size_t>(std::max(1, buffer_size)),
          plan.image_frame_names.size()));
    }
    prepared = std::move(result);
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = exception.what();
    return false;
  }
}

std::string discoverRecordingFallbackVideo(const std::string &recording_root) {
  namespace fs = std::filesystem;
  std::vector<fs::path> search_dirs;
  const fs::path cams_dir = fs::path(recording_root) / "cams";
  if (IsDirectoryNoThrow(cams_dir)) {
    search_dirs.push_back(cams_dir);
  }
  search_dirs.push_back(fs::path(recording_root));
  for (const auto &search_dir : search_dirs) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(search_dir, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it->is_regular_file(ec) && !ec && IsSupportedVideoPath(it->path())) {
        return it->path().string();
      }
    }
  }
  return {};
}

MediaSessionLoader::MediaSessionLoader(const MediaSessionLoaderContext &context)
    : context_(context) {}

struct MediaSessionLoader::PrewarmCandidate {
  crimson::playback::ClippedMediaTransitionWorker prepare_stage;
  PreparedCameraMedia prepared;
  std::string error;
  bool prepared_ok = false;
  bool decoder_started = false;
  bool canceled = false;
  std::shared_ptr<const crimson::media::RecordingClipMediaProvider> provider;
  size_t selected_run = crimson::playback::kNoClippedSelectedRun;
  std::string clip_id;
  std::string video_path;
  std::string camera_name;
  int boundary_parent = -1;
  int boundary_local = -1;
  DecoderContext decoder_context{};
  SeekInfo seek_info;
  DecoderOutputHandoff output;
  PictureBuffer staged{};
  std::thread decoder_thread;

  ~PrewarmCandidate() {
    decoder_context.stop_flag = true;
    prepare_stage.drain();
    if (decoder_thread.joinable()) decoder_thread.join();
    if (staged.frame) {
      cudaSetDevice(decoder_context.gpu_index);
      cudaFree(staged.frame);
      staged.frame = nullptr;
    }
    if (staged.frame_slot_state) frameSlotDestroy(staged);
  }
};

void MediaSessionLoader::discardPrewarmCandidate() {
  if (!prewarm_candidate_) return;
  prewarm_candidate_->canceled = true;
  prewarm_candidate_->decoder_context.stop_flag = true;
  if (prewarm_cleanup_stage_.active()) {
    if (!prewarm_cleanup_stage_.finished()) return;
    prewarm_cleanup_stage_.drain();
  }
  auto old = prewarm_candidate_;
  try {
    prewarm_cleanup_stage_.start([old] {
      old->prepare_stage.drain();
      if (old->decoder_thread.joinable()) old->decoder_thread.join();
    });
    prewarm_cleanup_launch_failures_ = 0;
    prewarm_candidate_.reset();
  } catch (const std::exception &e) {
    ++prewarm_cleanup_launch_failures_;
    std::cout << "[ClipPrewarm] stage=fallback reason=cleanup_launch "
              << e.what() << std::endl;
    if (prewarm_cleanup_launch_failures_ >= 3) {
      // Thread creation failure can otherwise leave the canceled candidate
      // resident forever. A one-time pause is the bounded recovery path.
      prewarm_candidate_.reset();
      prewarm_cleanup_launch_failures_ = 0;
    }
  }
}

MediaSessionLoader::~MediaSessionLoader() {
  cancelPendingClipSwitch(false);
  if (adopted_candidate_ && context_.decoder_threads) {
    adopted_candidate_->decoder_context.stop_flag = true;
    crimson::playback::joinLiveThreads(*context_.decoder_threads);
    adopted_candidate_.reset();
  }
}

void MediaSessionLoader::pollSequentialClipPrewarm(int presented_parent_frame,
                                                   bool playback_active) {
  if (prewarm_cleanup_stage_.finished()) prewarm_cleanup_stage_.drain();
  if (prewarm_old_join_stage_.finished()) {
    prewarm_old_join_stage_.drain();
    prewarm_old_retiring_ = false;
    prewarm_old_retired_ = true;
    std::cout << "[ClipPrewarm] stage=old_decoder_retired parent_frame="
              << presented_parent_frame << std::endl;
  }
  const auto *state = context_.clipped_media_state;
  const auto provider = state ? state->recording_clip_provider : nullptr;
  if (!playback_active || !provider || !context_.video_loaded ||
      !*context_.video_loaded || !context_.scene ||
      context_.scene->use_cpu_buffer || !context_.decoder_threads ||
      clip_switch_work_ || clip_join_started_ ||
      (std::getenv("CRIMSON_DISABLE_CLIP_PREWARM") &&
       std::strcmp(std::getenv("CRIMSON_DISABLE_CLIP_PREWARM"), "0") != 0)) {
    if (prewarm_candidate_) discardPrewarmCandidate();
    return;
  }
  const auto &active = state->handoff;
  const int remaining = static_cast<int>(active.last_parent_frame -
                                          presented_parent_frame);
  const double rate = context_.playback_transport
      ? context_.playback_transport->playbackRate() : 1.0;
  const int lead_frames = crimson::playback::leadFrames(*context_.video_fps,
                                                        rate);
  if (remaining <= 0 || remaining > lead_frames ||
      presented_parent_frame < active.first_parent_frame ||
      active.switch_in_progress) return;
  const int boundary = static_cast<int>(active.last_parent_frame + 1);
  const auto next = provider->resolveParentFrame(boundary);
  if (!next || next->clip_index == active.selected_run_index ||
      next->clip_local_frame < 0 || !next->parent_frame_by_clip_local ||
      next->camera_serial != state->camera_serial) return;
  if (prewarm_failed_boundary_ == boundary) return;
  if (prewarm_candidate_ &&
      (prewarm_candidate_->provider != provider ||
       prewarm_candidate_->selected_run != next->clip_index ||
       prewarm_candidate_->boundary_parent != boundary ||
       prewarm_candidate_->clip_id != next->clip_id ||
       prewarm_candidate_->video_path != next->video_path.string())) {
    discardPrewarmCandidate();
  }
  if (!prewarm_candidate_) {
    if (prewarm_cleanup_stage_.active()) return;
    auto candidate = std::make_shared<PrewarmCandidate>();
    candidate->provider = provider;
    candidate->selected_run = next->clip_index;
    candidate->clip_id = next->clip_id;
    candidate->video_path = next->video_path.string();
    candidate->camera_name = "Cam" + next->camera_serial + "_" + next->clip_id;
    candidate->boundary_parent = boundary;
    candidate->boundary_local = static_cast<int>(next->clip_local_frame);
    candidate->decoder_context.gpu_index = context_.decoder_context->gpu_index;
    candidate->decoder_context.seek_interval = context_.decoder_context->seek_interval;
    candidate->seek_info.frame_number_map = next->parent_frame_by_clip_local;
    crimson::media::CameraMediaOpenPlan plan;
    plan.kind = crimson::media::CameraMediaKind::VideoFiles;
    plan.camera_names = {candidate->camera_name};
    plan.video_paths = {candidate->video_path};
    try {
      candidate->prepare_stage.start([candidate, plan = std::move(plan),
                                      image_root = *context_.root_dir,
                                      buffer_size = *context_.label_buffer_size,
                                      fps = *context_.video_fps] {
        candidate->prepared_ok = prepareCameraMedia(
            plan, image_root, buffer_size, fps, candidate->prepared,
            candidate->error);
      });
    } catch (const std::exception &e) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=prepare_launch "
                << e.what() << std::endl;
      return;
    }
    prewarm_candidate_ = std::move(candidate);
    std::cout << "[ClipPrewarm] stage=prepare_started parent_frame="
              << boundary << std::endl;
    return;
  }
  auto &candidate = *prewarm_candidate_;
  if (candidate.canceled) { discardPrewarmCandidate(); return; }
  if (!candidate.decoder_started) {
    if (!candidate.prepare_stage.finished()) return;
    candidate.prepare_stage.drain();
    if (!candidate.prepared_ok || candidate.prepared.demuxers.size() != 1 ||
        candidate.prepared.camera_dimensions.size() != 1) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=prepare_failed "
                << candidate.error << std::endl;
      discardPrewarmCandidate();
      return;
    }
    const int width = candidate.prepared.camera_dimensions[0].first;
    const int height = candidate.prepared.camera_dimensions[0].second;
    const auto *active_demuxer = context_.demuxers &&
        !context_.demuxers->empty() ? context_.demuxers->front().get()
                                     : nullptr;
    const auto *next_demuxer = candidate.prepared.demuxers[0].get();
    const bool compatible_format = active_demuxer && next_demuxer &&
        active_demuxer->GetVideoCodec() == next_demuxer->GetVideoCodec() &&
        active_demuxer->GetPixelFormat() == next_demuxer->GetPixelFormat() &&
        active_demuxer->GetColorSpace() == next_demuxer->GetColorSpace() &&
        active_demuxer->GetColorRange() == next_demuxer->GetColorRange() &&
        std::fabs(active_demuxer->GetFramerate() -
                  next_demuxer->GetFramerate()) < 1e-6 &&
        context_.scene->size_of_buffer > 0 &&
        context_.scene->cameras[0].display_buffer &&
        context_.scene->cameras[0].display_buffer[0].format ==
            FramePixelFormat::NV12;
    if (!compatible_format) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=format_or_rate"
                << std::endl;
      discardPrewarmCandidate();
      return;
    }
    if (width <= 0 || height <= 0 ||
        width != context_.scene->cameras[0].image_width ||
        height != context_.scene->cameras[0].image_height) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=dimensions" << std::endl;
      discardPrewarmCandidate();
      return;
    }
    const int pitch = (width + 1) & ~1;
    const size_t bytes = static_cast<size_t>(pitch) *
                         (height + (height + 1) / 2);
    constexpr size_t kDefaultStagingBudget = 64ULL * 1024 * 1024;
    size_t staging_budget = kDefaultStagingBudget;
    if (const char *raw = std::getenv("CRIMSON_PREWARM_MAX_BYTES")) {
      char *end = nullptr;
      const auto parsed = std::strtoull(raw, &end, 10);
      if (end && *end == '\0') staging_budget = parsed;
    }
    if (!crimson::playback::allowedStageBytes(
            bytes, staging_budget, std::numeric_limits<size_t>::max(), 0)) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=capacity bytes="
                << bytes << " budget=" << staging_budget << std::endl;
      discardPrewarmCandidate();
      return;
    }
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    constexpr size_t kDecoderHeadroom = 1024ULL * 1024 * 1024;
    if (cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess ||
        !crimson::playback::allowedStageBytes(
            bytes, staging_budget, free_bytes, kDecoderHeadroom)) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=gpu_headroom"
                << std::endl;
      discardPrewarmCandidate();
      return;
    }
    if (cudaMalloc(reinterpret_cast<void **>(&candidate.staged.frame), bytes)
        != cudaSuccess) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=cuda_allocation"
                << std::endl;
      discardPrewarmCandidate();
      return;
    }
    candidate.staged.frame_number = -1;
    candidate.staged.local_frame_number = -1;
    candidate.staged.frame_pts = -1;
    candidate.staged.available_to_write = true;
    candidate.staged.pitch_bytes = pitch;
    candidate.staged.frame_bytes = bytes;
    candidate.staged.format = FramePixelFormat::NV12;
    frameSlotInitialize(candidate.staged);
    candidate.seek_info.use_seek = true;
    candidate.seek_info.seek_frame = candidate.boundary_local;
    candidate.seek_info.seek_accurate = true;
    candidate.seek_info.seek_id = 1;
    candidate.output.buffer = &candidate.staged;
    candidate.output.buffer_size = 1;
    try {
      candidate.decoder_thread = std::thread(
          &decoder_process_with_handoff, &candidate.decoder_context,
          candidate.prepared.demuxers[0].get(), candidate.camera_name,
          &candidate.staged, 1, &candidate.seek_info, false,
          &candidate.output);
      candidate.decoder_started = true;
    } catch (const std::exception &e) {
      prewarm_failed_boundary_ = boundary;
      std::cout << "[ClipPrewarm] stage=fallback reason=decoder_launch "
                << e.what() << std::endl;
      discardPrewarmCandidate();
    }
    return;
  }
  const auto staged = frameSlotSnapshotReadable(candidate.staged);
  if (candidate.output.failed.load() ||
      (staged && (staged->frame_number != boundary ||
                  staged->local_frame_number != candidate.boundary_local))) {
    prewarm_failed_boundary_ = boundary;
    std::cout << "[ClipPrewarm] stage=fallback reason=wrong_or_failed_frame"
              << std::endl;
    discardPrewarmCandidate();
    return;
  }
  if (!staged || prewarm_old_retiring_ || prewarm_old_retired_) return;
  std::cout << "[ClipPrewarm] stage=candidate_ready parent_frame="
            << boundary << " local_frame=" << staged->local_frame_number
            << " staged_frames=1 bytes=" << staged->frame_bytes
            << " normal_ring_slots=" << context_.scene->size_of_buffer
            << std::endl;
  // Hold leases while the old producer drains: a snapshot alone would allow
  // the ring writer to reuse a proved tail slot before the join finishes.
  auto leases = std::make_shared<std::vector<FrameSlotReadLease>>();
  std::vector<int64_t> resident;
  for (u32 i = 0; i < context_.scene->size_of_buffer; ++i) {
    auto lease = frameSlotAcquireReadable(
        context_.scene->cameras[0].display_buffer[i]);
    if (lease && lease->metadata().frame_number >= presented_parent_frame &&
        lease->metadata().frame_number <= active.last_parent_frame) {
      resident.push_back(lease->metadata().frame_number);
      leases->push_back(std::move(*lease));
    }
  }
  if (!crimson::playback::hasContiguousTail(
          presented_parent_frame, active.last_parent_frame, resident)) return;
  auto threads = std::make_shared<std::vector<std::thread>>(
      std::move(*context_.decoder_threads));
  context_.decoder_threads->clear();
  auto adopted = std::move(adopted_candidate_);
  if (adopted) adopted->decoder_context.stop_flag = true;
  else context_.decoder_context->stop_flag = true;
  try {
    prewarm_old_join_stage_.start([threads, leases, adopted] {
      crimson::playback::joinLiveThreads(*threads);
    });
  } catch (const std::exception &e) {
    prewarm_failed_boundary_ = boundary;
    crimson::playback::joinLiveThreads(*threads);
    restartCurrentClipDecoder();
    std::cout << "[ClipPrewarm] stage=fallback reason=retire_launch "
              << e.what() << std::endl;
    discardPrewarmCandidate();
    return;
  }
  prewarm_old_retiring_ = true;
  std::cout << "[ClipPrewarm] stage=old_decoder_retire_started parent_frame="
            << presented_parent_frame << " tail_frames=" << resident.size()
            << std::endl;
}

void MediaSessionLoader::finishClipSwitchWorkers() {
  if (clip_switch_work_) {
    clip_switch_work_->stage.drain();
  }
  clip_join_stage_.drain();
}

void MediaSessionLoader::restartCurrentClipDecoder() {
  if (adopted_candidate_) {
    adopted_candidate_->decoder_context.stop_flag = true;
    crimson::playback::joinLiveThreads(*context_.decoder_threads);
    adopted_candidate_.reset();
  }
  context_.decoder_context->stop_flag = false;
  context_.decoder_context->decoding_flag = false;
  if (context_.video_loaded && *context_.video_loaded &&
      context_.scene->num_cams > 0 && !context_.demuxers->empty() &&
      !context_.camera_names->empty()) {
    const auto &handoff = context_.clipped_media_state->handoff;
    const int64_t visible = context_.scene->cameras[0].last_uploaded_frame;
    const int64_t restore_parent = std::clamp<int64_t>(
        visible >= 0 ? visible : handoff.last_presented_parent_frame,
        std::max<int64_t>(0, handoff.first_parent_frame),
        std::max<int64_t>(0, handoff.last_parent_frame));
    const auto binding = resolveMappedMediaFrame(restore_parent);
    if (crimson::playback::isValidClippedFrameBinding(binding) &&
        binding.selected_run_index == handoff.selected_run_index) {
      std::lock_guard<std::mutex> lock(g_seek_info_mutex);
      auto &seek = context_.scene->cameras[0].seek_context;
      seek.seek_frame = static_cast<uint64_t>(binding.clip_local_frame_index);
      seek.seek_accurate = true;
      ++seek.seek_id;
      seek.use_seek = true;
      seek.seek_done = false;
    }
    context_.decoder_threads->clear();
    context_.decoder_threads->emplace_back(
        &decoder_process, context_.decoder_context,
        context_.demuxers->front().get(), context_.camera_names->front(),
        context_.scene->cameras[0].display_buffer,
        context_.scene->size_of_buffer,
        &context_.scene->cameras[0].seek_context,
        context_.scene->use_cpu_buffer);
  }
}

bool MediaSessionLoader::cancelPendingClipSwitch(bool restore_current) {
  const bool decoder_drained = clip_join_started_ || prewarm_old_retired_ ||
                               prewarm_old_retiring_;
  if (prewarm_candidate_) discardPrewarmCandidate();
  prewarm_cleanup_stage_.drain();
  if (prewarm_candidate_) {
    discardPrewarmCandidate();
    prewarm_cleanup_stage_.drain();
  }
  prewarm_old_join_stage_.drain();
  prewarm_old_retiring_ = false;
  if (decoder_drained && restore_current && !clip_join_started_) {
    restartCurrentClipDecoder();
  }
  prewarm_old_retired_ = false;
  prewarm_adoption_fallback_ = false;
  prewarm_failed_boundary_ = -1;
  finishClipSwitchWorkers();
  if (clip_join_started_ && restore_current) {
    restartCurrentClipDecoder();
  }
  clip_switch_work_.reset();
  pending_clip_parent_frame_ = -1;
  pending_clip_video_path_.clear();
  clip_join_started_ = false;
  return decoder_drained;
}

void MediaSessionLoader::restoreCurrentClipDecoderAfterCancellation() {
  restartCurrentClipDecoder();
}

bool MediaSessionLoader::hasPendingClipSwitch() const {
  return clip_switch_work_ != nullptr || clip_join_started_;
}

bool MediaSessionLoader::hasPendingPrewarmActivity() const {
  return prewarm_candidate_ != nullptr || prewarm_cleanup_stage_.active() ||
         prewarm_old_join_stage_.active() || prewarm_old_retired_;
}

void MediaSessionLoader::stopAdoptedDecoderForShutdown() {
  if (adopted_candidate_) adopted_candidate_->decoder_context.stop_flag = true;
}

bool MediaSessionLoader::adoptPrewarmCandidate(int parent_frame,
                                               int local_frame) {
  auto candidate = prewarm_candidate_;
  if (!candidate) return false;
  auto staged = frameSlotAcquireReadable(candidate->staged);
  if (!staged || candidate->prepared.demuxers.size() != 1 ||
      !crimson::playback::canAdopt(
          parent_frame, local_frame, staged->metadata().frame_number,
          staged->metadata().local_frame_number, prewarm_old_retired_,
          candidate->output.failed.load() ||
              candidate->boundary_parent != parent_frame ||
              candidate->boundary_local != local_frame))
    return false;
  const bool loaded = loadClippedVideoForParentFrame(
      parent_frame, &candidate->prepared, false, candidate.get());
  if (!loaded) {
    if (context_.clipped_media_state &&
        !context_.clipped_media_state->recording_clip_provider) {
      context_.clipped_media_state->recording_clip_provider =
          candidate->provider;
      context_.clipped_media_state->source =
          ClippedMediaSource::RecordingClipIndex;
    }
    prewarm_failed_boundary_ = parent_frame;
    return false;
  }
  auto &normal = context_.scene->cameras[0].display_buffer[0];
  auto writable = frameSlotAcquireWritable(normal);
  if (!writable || normal.format != candidate->staged.format ||
      normal.frame_bytes != candidate->staged.frame_bytes ||
      cudaMemcpy(normal.frame, candidate->staged.frame, normal.frame_bytes,
                 cudaMemcpyDeviceToDevice) != cudaSuccess) {
    std::cout << "[ClipPrewarm] stage=fallback reason=adoption_copy"
              << std::endl;
    prewarm_adoption_fallback_ = true;
    prewarm_failed_boundary_ = parent_frame;
    discardPrewarmCandidate();
    return false;
  }
  writable->publish(staged->metadata());
  latest_decoded_frame[candidate->camera_name].store(parent_frame);
  context_.decoder_context->decoding_flag = true;
  {
    std::lock_guard<std::mutex> lock(candidate->output.mutex);
    candidate->output.buffer = context_.scene->cameras[0].display_buffer;
    candidate->output.buffer_size = context_.scene->size_of_buffer;
    candidate->output.next_slot = 1;
    candidate->output.seek_info =
        &context_.scene->cameras[0].seek_context;
    candidate->output.adopted.store(true);
    ++candidate->output.generation;
  }
  context_.decoder_threads->push_back(std::move(candidate->decoder_thread));
  adopted_candidate_ = std::move(candidate);
  prewarm_candidate_.reset();
  prewarm_old_retired_ = false;
  std::cout << "[ClipPrewarm] stage=candidate_adopted parent_frame="
            << parent_frame << " local_frame=" << local_frame
            << " staged_frames=1" << std::endl;
  return true;
}

DecoderFrameResolution
MediaSessionLoader::requestDecoderFrameForParentFrame(int parent_frame) {
  const auto binding = resolveMappedMediaFrame(parent_frame);
  if (!hasMappedMedia()) {
    return {DecoderFrameResolutionStatus::Ready, parent_frame, {}};
  }
  if (!crimson::playback::isValidClippedFrameBinding(binding)) {
    return {DecoderFrameResolutionStatus::Failed, -1,
            "media mapping is unavailable for the requested frame"};
  }
  const int local_frame = static_cast<int>(std::clamp<int64_t>(
      binding.clip_local_frame_index, 0, std::numeric_limits<int>::max()));
  if (prewarm_cleanup_stage_.active()) {
    if (!prewarm_cleanup_stage_.finished()) {
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
    prewarm_cleanup_stage_.drain();
  }
  if (prewarm_adoption_fallback_) {
    if (prewarm_cleanup_stage_.active() &&
        !prewarm_cleanup_stage_.finished()) {
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
    prewarm_cleanup_stage_.drain();
    if (prewarm_candidate_) {
      discardPrewarmCandidate();
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
    restartCurrentClipDecoder();
    prewarm_adoption_fallback_ = false;
    return {DecoderFrameResolutionStatus::Ready, local_frame, {}};
  }
  const auto &active = *context_.clipped_media_state;
  if (prewarm_old_join_stage_.finished()) {
    prewarm_old_join_stage_.drain();
    prewarm_old_retiring_ = false;
    prewarm_old_retired_ = true;
    std::cout << "[ClipPrewarm] stage=old_decoder_retired parent_frame="
              << parent_frame << std::endl;
  }
  const bool requesting_active_clip =
      active.handoff.selected_run_index == binding.selected_run_index &&
      parent_frame >= active.handoff.first_parent_frame &&
      parent_frame <= active.handoff.last_parent_frame;
  if (requesting_active_clip &&
      (prewarm_old_retiring_ || prewarm_old_retired_)) {
    discardPrewarmCandidate();
    if (prewarm_candidate_ || prewarm_cleanup_stage_.active())
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    if (prewarm_old_retiring_) {
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
    restartCurrentClipDecoder();
    prewarm_old_retired_ = false;
    return {DecoderFrameResolutionStatus::Ready, local_frame, {}};
  }
  const bool automatic_boundary = context_.playback_state &&
      context_.playback_state->play_video &&
      active.handoff.switch_in_progress &&
      active.handoff.pending_switch_parent_frame == parent_frame &&
      parent_frame == active.handoff.last_parent_frame + 1;
  if (automatic_boundary && prewarm_candidate_ &&
      prewarm_candidate_->boundary_parent == parent_frame) {
    if (prewarm_old_retiring_) {
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
    if (context_.playback_transport &&
        context_.playback_transport->requestedFrame() < parent_frame) {
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
    if (adoptPrewarmCandidate(parent_frame, local_frame)) {
      return {DecoderFrameResolutionStatus::ReadyBuffered, local_frame, {}};
    }
    if (prewarm_adoption_fallback_) {
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
  }
  if (prewarm_candidate_) {
    discardPrewarmCandidate();
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  if (prewarm_cleanup_stage_.active()) {
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  if (prewarm_old_retiring_) {
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  const bool same_clip = context_.video_loaded && *context_.video_loaded &&
      active.handoff.selected_run_index == binding.selected_run_index &&
      parent_frame >= active.handoff.first_parent_frame &&
      parent_frame <= active.handoff.last_parent_frame &&
      context_.decoder_threads && !context_.decoder_threads->empty() &&
      !clip_join_started_;
  if (same_clip) {
    return {DecoderFrameResolutionStatus::Ready, local_frame, {}};
  }

  std::string video_path;
  std::string camera_serial;
  if (active.recording_clip_provider) {
    const auto resolved = active.recording_clip_provider->resolveParentFrame(
        parent_frame);
    if (!resolved) {
      return {DecoderFrameResolutionStatus::Failed, -1,
              "recording clip mapping disappeared"};
    }
    video_path = resolved->video_path.string();
    camera_serial = resolved->camera_serial;
  } else {
    const auto *row = context_.zarr_loader->resolveClippedFrame(parent_frame);
    const auto *selected = row ? context_.zarr_loader->getClippedResolver()
                                      .selectedRun(row->selected_run_index)
                               : nullptr;
    if (!selected) {
      return {DecoderFrameResolutionStatus::Failed, -1,
              "clipped source mapping disappeared"};
    }
    const auto resolved = ResolveAffiliatedVideoPath(
        selected->video_path, context_.zarr_loader->getArchivePath());
    if (!resolved) {
      return {DecoderFrameResolutionStatus::Failed, -1,
              "clipped source video path is unavailable"};
    }
    video_path = resolved->string();
    camera_serial = selected->camera_serial;
  }

  if (clip_switch_work_ && clip_switch_work_->stage.finished() &&
      !clip_join_started_ && pending_clip_video_path_ != video_path) {
    finishClipSwitchWorkers();
    clip_switch_work_.reset();
  }
  if (!clip_switch_work_) {
    if (clip_join_started_) {
      return {DecoderFrameResolutionStatus::Pending, -1, {}};
    }
    pending_clip_parent_frame_ = parent_frame;
    pending_clip_video_path_ = video_path;
    auto work = std::make_unique<ClipSwitchWork>();
    work->started = std::chrono::steady_clock::now();
    auto *work_ptr = work.get();
    crimson::media::CameraMediaOpenPlan plan;
    plan.kind = crimson::media::CameraMediaKind::VideoFiles;
    plan.camera_names = {"Cam" + camera_serial + "_" + binding.clip_id};
    plan.video_paths = {video_path};
    const std::string image_root = *context_.root_dir;
    const int buffer_size = *context_.label_buffer_size;
    const double fps = *context_.video_fps;
    const auto prepare = context_.prepare_clip_media_async;
    try {
      work->stage.start([work_ptr, plan = std::move(plan), image_root,
                         buffer_size, fps, prepare] {
        work_ptr->ready = prepare
            ? prepare(plan, image_root, buffer_size, fps,
                      work_ptr->prepared, work_ptr->error)
            : prepareCameraMedia(plan, image_root, buffer_size, fps,
                                 work_ptr->prepared, work_ptr->error);
      });
    } catch (const std::exception &e) {
      return {DecoderFrameResolutionStatus::Failed, -1, e.what()};
    }
    clip_switch_work_ = std::move(work);
    std::cout << "[ClipSwitchStage] stage=prepare_started parent_frame="
              << parent_frame << " clip=" << binding.clip_id << std::endl;
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  const auto transition_poll = crimson::playback::pollClippedMediaTransition(
      clip_join_started_, clip_switch_work_->stage, clip_join_stage_);
  if (transition_poll == crimson::playback::ClippedMediaTransitionPoll::Preparing) {
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  if (pending_clip_video_path_ != video_path && !clip_join_started_) {
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  if (!clip_switch_work_->ready) {
    const std::string error = !clip_switch_work_->stage.error().empty()
        ? clip_switch_work_->stage.error() : clip_switch_work_->error;
    finishClipSwitchWorkers();
    clip_switch_work_.reset();
    return {DecoderFrameResolutionStatus::Failed, -1, error};
  }
  if (transition_poll == crimson::playback::ClippedMediaTransitionPoll::Prepared) {
    clip_switch_work_->stage.drain();
    const auto prepare_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - clip_switch_work_->started).count();
    std::cout << "[ClipSwitchStage] stage=prepare_ready parent_frame="
              << parent_frame << " elapsed_ms=" << prepare_ms << std::endl;
    const auto join = context_.join_clip_decoders_async;
    auto *threads = context_.decoder_threads;
    clip_join_error_.clear();
    try {
      clip_join_stage_.start([this, join, threads] {
        try {
          if (join) {
            join(*threads);
          } else {
            for (auto &thread : *threads) {
              if (thread.joinable()) thread.join();
            }
          }
        } catch (const std::exception &e) {
          clip_join_error_ = e.what();
        } catch (...) {
          clip_join_error_ = "unknown decoder join error";
        }
        for (auto &thread : *threads) {
          if (thread.joinable()) thread.join();
        }
      });
    } catch (const std::exception &e) {
      clip_switch_work_.reset();
      pending_clip_parent_frame_ = -1;
      pending_clip_video_path_.clear();
      return {DecoderFrameResolutionStatus::Failed, -1, e.what()};
    }
    clip_join_started_ = true;
    clip_join_started_at_ = std::chrono::steady_clock::now();
    context_.decoder_context->stop_flag = true;
    if (adopted_candidate_) adopted_candidate_->decoder_context.stop_flag = true;
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  if (transition_poll == crimson::playback::ClippedMediaTransitionPoll::Joining) {
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  clip_join_stage_.drain();
  if (!clip_join_error_.empty()) {
    const std::string error = clip_join_error_;
    restartCurrentClipDecoder();
    clip_switch_work_.reset();
    clip_join_started_ = false;
    pending_clip_parent_frame_ = -1;
    pending_clip_video_path_.clear();
    return {DecoderFrameResolutionStatus::Failed, -1, error};
  }
  if (pending_clip_video_path_ != video_path) {
    // A newer seek won while the old decoder was draining. Restore its
    // decoder over the still-owned buffers before servicing that seek.
    restartCurrentClipDecoder();
    clip_switch_work_.reset();
    clip_join_started_ = false;
    pending_clip_parent_frame_ = -1;
    pending_clip_video_path_.clear();
    return {DecoderFrameResolutionStatus::Pending, -1, {}};
  }
  const auto join_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - clip_join_started_at_).count();
  std::cout << "[ClipSwitchStage] stage=decoder_join_ready parent_frame="
            << parent_frame << " elapsed_ms=" << join_ms << std::endl;
  const auto commit_started = std::chrono::steady_clock::now();
  const bool ready = loadClippedVideoForParentFrame(
      parent_frame, &clip_switch_work_->prepared);
  const auto commit_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - commit_started).count();
  std::cout << "[ClipSwitchStage] stage=owner_commit parent_frame="
            << parent_frame << " elapsed_ms=" << commit_ms
            << " ready=" << ready << std::endl;
  clip_switch_work_.reset();
  clip_join_started_ = false;
  pending_clip_parent_frame_ = -1;
  pending_clip_video_path_.clear();
  return ready ? DecoderFrameResolution{DecoderFrameResolutionStatus::Ready,
                                         local_frame, {}}
               : DecoderFrameResolution{DecoderFrameResolutionStatus::Failed,
                                         -1, "clipped media commit failed"};
}

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
  std::string local_error;
  std::string &error = error_message != nullptr ? *error_message : local_error;
  std::shared_ptr<const crimson::media::RecordingClipMediaProvider> provider;
  if (context_.prepare_recording_clip_index) {
    provider = context_.prepare_recording_clip_index(index_path, error);
  } else if (auto opened = crimson::media::RecordingClipMediaProvider::Open(
                 index_path, &error)) {
    provider =
        std::make_shared<const crimson::media::RecordingClipMediaProvider>(
            std::move(*opened));
  }
  if (!provider) {
    return false;
  }
  context_.clipped_media_state->source = ClippedMediaSource::RecordingClipIndex;
  context_.clipped_media_state->recording_clip_provider = std::move(provider);
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
  if (adopted_candidate_) adopted_candidate_->decoder_context.stop_flag = true;
  if (context_.join_camera_decoders) {
    context_.join_camera_decoders(*context_.decoder_threads);
  } else {
    for (auto &thread : *context_.decoder_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }
  context_.decoder_threads->clear();
  adopted_candidate_.reset();

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
  return loadClippedVideoForParentFrame(parent_frame, nullptr);
}

bool MediaSessionLoader::loadClippedVideoForParentFrame(
    int parent_frame, PreparedCameraMedia *preprepared,
    bool start_decoder, PrewarmCandidate *pending_adoption) const {
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

  const std::string camera_name = "Cam" + camera_serial + "_" + clip_id;
  crimson::media::CameraMediaOpenPlan clip_plan;
  clip_plan.kind = crimson::media::CameraMediaKind::VideoFiles;
  clip_plan.camera_names = {camera_name};
  clip_plan.video_paths = {resolved_video};
  PreparedCameraMedia prepared;
  auto stop_pending_adoption = [&] {
    if (!pending_adoption) return;
    pending_adoption->decoder_context.stop_flag = true;
    if (pending_adoption->decoder_thread.joinable())
      pending_adoption->decoder_thread.join();
  };
  std::string prepare_error;
  const bool prepared_ok = preprepared != nullptr
      ? (prepared = std::move(*preprepared), true)
      : (context_.prepare_camera_media
             ? context_.prepare_camera_media(
                   clip_plan, *context_.root_dir, *context_.label_buffer_size,
                   *context_.video_fps, prepared, prepare_error)
             : prepareCameraMedia(clip_plan, *context_.root_dir,
                                  *context_.label_buffer_size,
                                  *context_.video_fps, prepared, prepare_error));
  if (!prepared_ok) {
    std::cerr << "[Zarr] Failed to prepare clipped source video: "
              << prepare_error << std::endl;
    return false;
  }
  if (context_.opening_cancelled && context_.opening_cancelled()) {
    stop_pending_adoption();
    return false;
  }

  if (*context_.video_loaded || !context_.decoder_threads->empty() ||
      !context_.demuxers->empty()) {
    stopCameraDecodersForReload();
  }
  if (context_.opening_cancelled && context_.opening_cancelled()) {
    stop_pending_adoption();
    return false;
  }

  try {
    *context_.input_is_imgs = false;
    context_.camera_names->clear();
    context_.demuxers->clear();
    context_.is_view_focused->clear();

    context_.camera_names->push_back(camera_name);
    (*context_.window_need_decoding)[camera_name].store(true);
    (*context_.window_was_decoding)[camera_name] = true;
    latest_decoded_frame[camera_name].store(-1);

    *context_.demuxers = std::move(prepared.demuxers);
    context_.decoder_context->seek_interval = prepared.seek_interval;
    *context_.video_fps = prepared.video_fps;
    context_.scene->num_cams = 1;
    context_.scene->cameras.resize(context_.scene->num_cams);
    context_.scene->cameras[0].image_width =
        prepared.camera_dimensions[0].first;
    context_.scene->cameras[0].image_height =
        prepared.camera_dimensions[0].second;

    const bool needs_allocation =
        context_.scene->cameras[0].display_buffer == nullptr ||
        context_.scene->size_of_buffer == 0;
    if (needs_allocation) {
      render_allocate_scene_memory(context_.scene, *context_.label_buffer_size,
                                   context_.poll_owner_events);
    } else {
      for (u32 i = 0; i < context_.scene->size_of_buffer; ++i) {
        frameSlotReleaseForReuse(context_.scene->cameras[0].display_buffer[i]);
      }
      // The old GL texture is still owned by this camera and remains a
      // truthful displayed parent frame until the new decoder uploads one.
      context_.scene->cameras[0].playback_staging_frame = -1;
      context_.scene->cameras[0].playback_staging_local_frame = -1;
      context_.scene->cameras[0].playback_staging_pts = -1;
      context_.scene->cameras[0].playback_staging_valid = false;
    }
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      stop_pending_adoption();
      stopCameraDecodersForReload();
      return false;
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

    if (start_decoder) {
      context_.decoder_threads->push_back(std::thread(
          &decoder_process, context_.decoder_context,
          context_.demuxers->at(0).get(), context_.camera_names->at(0),
          context_.scene->cameras[0].display_buffer,
          context_.scene->size_of_buffer,
          &context_.scene->cameras[0].seek_context,
          context_.scene->use_cpu_buffer));
    }
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
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      stop_pending_adoption();
      stopCameraDecodersForReload();
      return false;
    }
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[Zarr] Failed to load clipped source video: " << e.what()
              << std::endl;
    stop_pending_adoption();
    stopCameraDecodersForReload();
    return false;
  }
}

bool prepareCameraCalibrations(const std::vector<std::string> &camera_names,
                               const std::string &recording_root,
                               ZarrDetectionLoader *zarr_loader,
                               bool zarr_loaded,
                               std::vector<CameraParams> &camera_params,
                               std::string &error) {
  camera_params.resize(camera_names.size());
  std::cout << "\n=== Loading Camera Calibrations from Zarr/YAML ==="
            << std::endl;
  for (size_t i = 0; i < camera_names.size(); ++i) {
    const std::string &camera_name = camera_names[i];
    std::cout << "\nProcessing camera " << i << ": " << camera_name
              << std::endl;

    bool loaded_from_zarr = false;
    if (zarr_loader != nullptr && zarr_loaded &&
        !zarr_loader->getArchivePath().empty()) {
      std::string zarr_status;
      auto calibration = zarr_loader->loadCalibrationForCamera(
          camera_name, zarr_status);
      if (calibration.has_value()) {
        std::string apply_error;
        if (applyZarrCalibrationToCameraParams(
                *calibration, camera_params[i], apply_error)) {
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
                    << camera_params[i].stimulus_offset_x << ", "
                    << camera_params[i].stimulus_offset_y << ")"
                    << std::endl;
          camera_print_calibration_details(camera_params[i], camera_name);
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
        recording_root + "/calibration/" + camera_name + ".yaml";
    if (std::filesystem::exists(yaml_file)) {
      std::cout << "Loading homography from YAML for camera: " << camera_name
                << std::endl;
      if (!camera_load_params_from_yaml(yaml_file, camera_params[i], error)) {
        std::cerr << "Error: Failed to load calibration from YAML: "
                  << error << std::endl;
        return false;
      }
      camera_print_calibration_details(camera_params[i], camera_name);
    } else {
      std::cerr << "Warning: No calibration YAML file found at: " << yaml_file
                << std::endl;
    }
  }
  error.clear();
  return true;
}

void MediaSessionLoader::loadCameraCalibrationsForCurrentMedia() const {
  if (context_.video_loaded == nullptr || !*context_.video_loaded ||
      context_.scene == nullptr || context_.camera_names == nullptr ||
      context_.camera_params == nullptr || context_.root_dir == nullptr ||
      context_.error_message == nullptr || context_.show_error == nullptr) {
    return;
  }
  auto prepared = *context_.camera_params;
  prepared.resize(context_.scene->num_cams);
  std::string error;
  const bool ready = context_.prepare_camera_calibrations
                         ? context_.prepare_camera_calibrations(
                               *context_.camera_names, *context_.root_dir,
                               context_.zarr_loader,
                               context_.zarr_loaded && *context_.zarr_loaded,
                               prepared, error)
                         : prepareCameraCalibrations(
                               *context_.camera_names, *context_.root_dir,
                               context_.zarr_loader,
                               context_.zarr_loaded && *context_.zarr_loaded,
                               prepared, error);
  if (context_.opening_cancelled && context_.opening_cancelled()) {
    return;
  }
  *context_.camera_params = std::move(prepared);
  if (!ready) {
    *context_.show_error = true;
    *context_.error_message = error;
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
  const bool ready = executeCameraMediaPlan(
      plan, infer_recording_root, success_label, &error_message);
  if (!ready && !error_message.empty()) {
    std::cerr << success_label << " failed: " << error_message << std::endl;
  }
  return ready;
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
    PreparedCameraMedia prepared;
    std::string prepare_error;
    const bool ready = context_.prepare_camera_media
                           ? context_.prepare_camera_media(
                                 plan, *context_.root_dir,
                                 *context_.label_buffer_size,
                                 *context_.video_fps, prepared, prepare_error)
                           : prepareCameraMedia(plan, *context_.root_dir,
                                                *context_.label_buffer_size,
                                                *context_.video_fps, prepared,
                                                prepare_error);
    if (!ready) {
      if (error_message != nullptr) {
        *error_message = prepare_error;
      }
      return false;
    }
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      if (error_message != nullptr) {
        *error_message = "Session opening cancelled";
      }
      return false;
    }
    stopCameraDecodersForReload();
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      if (error_message != nullptr) {
        *error_message = "Session opening cancelled";
      }
      return false;
    }
    replacement_started = true;
    *context_.input_is_imgs =
        plan.kind == crimson::media::CameraMediaKind::ImageSequence;
    *context_.camera_names = plan.camera_names;
    *context_.image_names = plan.image_frame_names;
    *context_.demuxers = std::move(prepared.demuxers);
    *context_.label_buffer_size = prepared.buffer_size;
    context_.decoder_context->seek_interval = prepared.seek_interval;
    if (plan.kind == crimson::media::CameraMediaKind::VideoFiles) {
      *context_.video_fps = prepared.video_fps;
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
    for (size_t index = 0; index < prepared.camera_dimensions.size(); ++index) {
      context_.scene->cameras[index].image_width =
          prepared.camera_dimensions[index].first;
      context_.scene->cameras[index].image_height =
          prepared.camera_dimensions[index].second;
    }
    render_allocate_scene_memory(context_.scene, *context_.label_buffer_size,
                                 context_.poll_owner_events);
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      if (error_message != nullptr) {
        *error_message = "Session opening cancelled";
      }
      return false;
    }

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
                       *context_.playback_state, true,
                       context_.stimulus_repository,
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
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      stopCameraDecodersForReload();
      if (error_message != nullptr) {
        *error_message = "Session opening cancelled";
      }
      return false;
    }
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

AffiliatedMediaDiscovery discoverAffiliatedMedia(
    const std::string &archive_path, const std::string &legacy_source_hint) {
  AffiliatedMediaDiscovery result;
  auto archive = crimson::zarr::ArchiveContext::Open(archive_path,
                                                     &result.error);
  if (!archive) {
    return result;
  }
  auto affiliated_video = crimson::zarr::DiscoverAffiliatedVideo(
      archive, &result.error);
  if (affiliated_video) {
    result.video_path = affiliated_video->resolved_path;
    result.error.clear();
    return result;
  }
  const std::string video_error = std::move(result.error);
  result.error.clear();
  auto clip_index = crimson::zarr::DiscoverAffiliatedRecordingClipIndex(
      archive, &result.error);
  if (clip_index) {
    result.clip_index_path = clip_index->index_path;
    result.error.clear();
    return result;
  }
  if (!result.error.empty()) {
    return result;
  }
  if (!video_error.empty()) {
    result.error = video_error;
    return result;
  }
  if (!legacy_source_hint.empty()) {
    result.video_path = ResolveAffiliatedVideoPath(legacy_source_hint,
                                                   archive_path);
    if (!result.video_path) {
      result.error = "Legacy affiliated-video hint could not be resolved: " +
                     legacy_source_hint;
    }
  }
  return result;
}

void MediaSessionLoader::tryAutoLoadAffiliatedVideoFromZarr(
    const char *trigger_label) const {
  if (context_.zarr_loaded == nullptr || !*context_.zarr_loaded ||
      context_.video_loaded == nullptr || context_.decoder_threads == nullptr ||
      context_.zarr_loader == nullptr) {
    return;
  }

  const std::string archive_path = context_.zarr_loader->getArchivePath();
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

  const std::string source_hint = context_.zarr_loader->getSourceVideoPath();
  auto discovered = context_.discover_affiliated_media
      ? context_.discover_affiliated_media(archive_path, source_hint)
      : discoverAffiliatedMedia(archive_path, source_hint);
  if (context_.opening_cancelled && context_.opening_cancelled()) {
    return;
  }
  if (discovered.clip_index_path) {
      std::string provider_error;
      if (!activateRecordingClipIndex(*discovered.clip_index_path,
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
                  << ") index=" << *discovered.clip_index_path
                  << std::endl;
      }
      return;
  }
  if (!discovered.video_path) {
    if (!discovered.error.empty()) {
      std::cout << "[Zarr] Affiliated media discovery failed: "
                << discovered.error << std::endl;
    }
    return;
  }

  if (!loadSingleVideoMedia(
          *discovered.video_path, true,
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
      context_.stimulus_repository == nullptr ||
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
      !context_.stimulus_repository->hasMapping()) {
    return;
  }

  std::optional<std::filesystem::path> resolved;
  if (!context_.stimulus_repository->resolvedSourceVideoPath().empty()) {
    resolved = std::filesystem::path(
        context_.stimulus_repository->resolvedSourceVideoPath());
  } else {
    const auto source_hint = context_.stimulus_repository->sourceVideoPath();
    const auto h5_hint = context_.zarr_loader->getStimulusSourceH5();
    const auto archive_path = context_.zarr_loader->getArchivePath();
    resolved = context_.resolve_stimulus_video
        ? context_.resolve_stimulus_video(source_hint, h5_hint, archive_path,
                                          *context_.root_dir)
        : ResolveStimulusVideoPath(source_hint, h5_hint, archive_path,
                                   *context_.root_dir);
  }
  if (context_.opening_cancelled && context_.opening_cancelled()) {
    return;
  }
  if (!resolved.has_value()) {
    std::cout << "[Stimulus] Could not auto-discover stimulus video ("
              << trigger_label << ")" << std::endl;
    return;
  }

  const auto result = openStimulusMedia(
      {resolved->string(), *context_.stimulus_buffer_size,
       *context_.stimulus_use_cpu_buffer,
       *context_.stimulus_use_software_decode, *context_.video_loaded,
       context_.playback_state->to_display_frame_number,
       !context_.playback_state->play_video});
  if (!result.ready) {
    std::cerr << "[Stimulus] " << result.error << std::endl;
    return;
  }

  std::cout << "[Stimulus] Auto-loaded stimulus video (" << trigger_label
            << "): " << resolved->string() << std::endl;
}

crimson::media::StimulusMediaOpenResult MediaSessionLoader::openStimulusMedia(
    const crimson::media::StimulusMediaOpenRequest &request) const {
  crimson::media::StimulusMediaOpenResult result;
  result.path = request.path;
  if (context_.stimulus_player == nullptr ||
      context_.stimulus_repository == nullptr ||
      context_.window_was_decoding == nullptr ||
      context_.window_need_decoding == nullptr) {
    result.error = "Stimulus media loader is not configured";
    return result;
  }

  PreparedStimulusPlayback prepared;
  PreparedStimulusPlayback *prepared_ptr = nullptr;
  if (context_.prepare_stimulus) {
    if (!context_.prepare_stimulus(request, prepared, result.error)) {
      return result;
    }
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      result.error = "Session opening cancelled";
      return result;
    }
    prepared_ptr = &prepared;
  }
  return crimson::platform::nvidia::openStimulusMedia(
      request, {context_.stimulus_player, context_.stimulus_repository,
                context_.window_need_decoding, context_.window_was_decoding,
                context_.cuda_device_index, prepared_ptr,
                context_.join_stimulus_decoder, context_.opening_cancelled,
                context_.poll_owner_events});
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
  auto stopIfCancelled = [&]() {
    if (context_.opening_cancelled && context_.opening_cancelled()) {
      session_open.cancel("Session opening cancelled");
      return true;
    }
    return false;
  };
  if (stopIfCancelled()) {
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
    const bool archive_ready =
        context_.open_archive
            ? context_.open_archive(cli_zarr_override_path, false, zarr_error)
            : loadZarrDetectionFromPath(cli_zarr_override_path,
                                        *context_.zarr_loader, zarr_error);
    if (stopIfCancelled()) {
      return;
    }
    if (archive_ready) {
      *context_.zarr_loaded = true;
      refresh_detection_dataset_options();
      clear_bbox_edits();
      std::cout << "Loaded Zarr archive from --zarr: "
                << context_.zarr_loader->getArchivePath() << std::endl;
      session_open.completeProduct("archive", "Archive ready", true);
      session_open.startProduct("affiliated_media",
                                "Resolving affiliated media");
      std::string media_error;
      const bool explicit_media_ready =
          loadExplicitRecordingClipIndex(&media_error);
      if (stopIfCancelled()) {
        return;
      }
      if (explicit_media_ready && cli_recording_clip_index_path.empty()) {
        tryAutoLoadAffiliatedVideoFromZarr("--zarr");
      }
      if (stopIfCancelled()) {
        return;
      }
      tryAutoLoadStimulusVideo("--zarr");
      if (stopIfCancelled()) {
        return;
      }
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

  if (cli_recording_path.empty() || stopIfCancelled()) {
    return;
  }

  *context_.root_dir = cli_recording_path;
  *context_.skeleton_dir = *context_.root_dir;

  session_open.startProduct("recording_archive", "Resolving recording archive");
  std::string zarr_error;
  const bool recording_archive_ready =
      context_.open_archive
          ? context_.open_archive(*context_.root_dir, true, zarr_error)
          : loadZarrDetectionFromDirectory(*context_.root_dir,
                                           *context_.zarr_loader, zarr_error);
  if (stopIfCancelled()) {
    return;
  }
  if (recording_archive_ready) {
    *context_.zarr_loaded = true;
    refresh_detection_dataset_options();
    clear_bbox_edits();
    std::cout << "Loaded Zarr archive from --recording: "
              << context_.zarr_loader->getArchivePath() << std::endl;
    session_open.completeProduct("recording_archive", "Recording archive ready",
                                 true);
    session_open.startProduct("recording_media", "Resolving recording media");
    std::string media_error;
    const bool explicit_media_ready =
        loadExplicitRecordingClipIndex(&media_error);
    if (stopIfCancelled()) {
      return;
    }
    if (explicit_media_ready && cli_recording_clip_index_path.empty()) {
      tryAutoLoadAffiliatedVideoFromZarr("--recording");
    }
    if (stopIfCancelled()) {
      return;
    }
    tryAutoLoadStimulusVideo("--recording");
    if (stopIfCancelled()) {
      return;
    }
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

  const std::string found_video = context_.discover_recording_video
                                      ? context_.discover_recording_video(
                                            *context_.root_dir)
                                      : discoverRecordingFallbackVideo(
                                            *context_.root_dir);
  if (stopIfCancelled()) {
    return;
  }

  if (found_video.empty()) {
    std::cout << "[--recording] No video files found in " << *context_.root_dir
              << std::endl;
    finishSession(false, {}, "No supported recording media was found");
    return;
  }

  session_open.startProduct("recording_media", "Opening recording media");
  const bool fallback_media_ready =
      loadSingleVideoMedia(found_video, false,
                           "[--recording] Auto-loaded video: ");
  if (stopIfCancelled()) {
    return;
  }
  if (fallback_media_ready) {
    tryAutoLoadStimulusVideo("--recording-fallback");
    if (stopIfCancelled()) {
      return;
    }
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
