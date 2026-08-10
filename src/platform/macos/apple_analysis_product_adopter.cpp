#include "platform/macos/apple_analysis_product_adopter.h"

#include "platform/macos/apple_analysis_product_adoption_policy.h"

#include "debug_flags.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <utility>

AppleAnalysisProductAdopter::AppleAnalysisProductAdopter(
    AppleAnalysisProductAdoptionOptions options,
    AppleAnalysisProductAdoptionContext context)
    : options_(std::move(options)), context_(std::move(context)) {}

bool AppleAnalysisProductAdopter::hasDeferredSwimBoutResult() const {
  return deferred_swim_bout_result_.has_value();
}

void AppleAnalysisProductAdopter::adopt(AppleAnalysisRepositoryBundle result,
                                        bool analysis_loader_loading) {
  auto &analysis_archive = context_.archive.repository;
  auto &analysis_loading_start_error = context_.archive.loading_start_error;
  auto &video_playback = context_.presentation.video;
  auto &workspace_state = context_.presentation.workspace;
  auto &analysis_timeline_controls = context_.presentation.timelines;

  auto &chaser_distance_polar_buffer = context_.chaser_polar.buffer;
  auto &chaser_distance_polar_descriptor = context_.chaser_polar.descriptor;
  auto &chaser_distance_polar_available = context_.chaser_polar.available;
  auto &chaser_distance_polar_failed = context_.chaser_polar.failed;
  auto &chaser_distance_polar_error = context_.chaser_polar.error;
  auto &last_chaser_distance_polar_camera_request =
      context_.chaser_polar.last_camera_request;

  auto &stimulus_playback = context_.stimulus.playback;
  auto &stimulus_enabled = context_.stimulus.enabled;
  auto &stimulus_error = context_.stimulus.error;

  auto &canonical_detection_buffer = context_.detection.buffer;
  auto &canonical_detection_descriptor = context_.detection.descriptor;
  auto &canonical_detection_open_metrics = context_.detection.open_metrics;
  auto &canonical_detection_available = context_.detection.available;
  auto &canonical_detection_failed = context_.detection.failed;
  auto &canonical_detection_error = context_.detection.error;
  auto &last_canonical_detection_camera_request =
      context_.detection.last_camera_request;

  auto &keypoint_overlay_buffer = context_.keypoints.buffer;
  auto &keypoint_descriptor = context_.keypoints.descriptor;
  auto &keypoint_overlay_available = context_.keypoints.available;
  auto &keypoint_overlay_failed = context_.keypoints.failed;
  auto &keypoint_error = context_.keypoints.error;
  auto &last_keypoint_camera_request = context_.keypoints.last_camera_request;

  auto &subject_mask_overlay_buffer = context_.subject_masks.buffer;
  auto &subject_mask_frame_presentation = context_.subject_masks.presentation;
  auto &subject_mask_descriptor = context_.subject_masks.descriptor;
  auto &subject_mask_overlay_available = context_.subject_masks.available;
  auto &subject_mask_overlay_failed = context_.subject_masks.failed;
  auto &subject_mask_smoke_start_pending =
      context_.subject_masks.smoke_start_pending;
  auto &subject_mask_initial_request_started =
      context_.subject_masks.initial_request_started;
  auto &subject_mask_initial_request_frame =
      context_.subject_masks.initial_request_frame;
  auto &subject_mask_error = context_.subject_masks.error;
  auto &logSubjectMaskFirstReady = context_.subject_masks.log_first_ready;

  auto &subject_shape_overlay_buffer = context_.subject_shape.buffer;
  auto &subject_shape_descriptor = context_.subject_shape.descriptor;
  auto &subject_shape_overlay_available = context_.subject_shape.available;
  auto &subject_shape_overlay_failed = context_.subject_shape.failed;
  auto &subject_shape_error = context_.subject_shape.error;

  auto &eye_geometry_overlay_buffer = context_.eye_geometry.buffer;
  auto &eye_geometry_descriptor = context_.eye_geometry.descriptor;
  auto &eye_geometry_overlay_available = context_.eye_geometry.available;
  auto &eye_geometry_overlay_failed = context_.eye_geometry.failed;
  auto &eye_geometry_error = context_.eye_geometry.error;

  auto &motion_timeline_buffer = context_.motion.buffer;
  auto &motion_timeline_descriptor = context_.motion.descriptor;
  auto &motion_timeline_available = context_.motion.available;
  auto &motion_timeline_failed = context_.motion.failed;
  auto &motion_timeline_error = context_.motion.error;

  auto &swim_bout_timeline_buffer = context_.swim_bouts.buffer;
  auto &swim_bout_timeline_descriptor = context_.swim_bouts.descriptor;
  auto &swim_bout_timeline_available = context_.swim_bouts.available;
  auto &swim_bout_timeline_failed = context_.swim_bouts.failed;
  auto &swim_bout_timeline_error = context_.swim_bouts.error;

  auto &eye_angle_timeline_buffer = context_.eye_angles.buffer;
  auto &eye_angle_timeline_descriptor = context_.eye_angles.descriptor;
  auto &eye_angle_timeline_available = context_.eye_angles.available;
  auto &eye_angle_timeline_failed = context_.eye_angles.failed;
  auto &eye_angle_timeline_error = context_.eye_angles.error;

  auto &tail_kinematics_timeline_buffer = context_.tail_kinematics.buffer;
  auto &tail_kinematics_timeline_descriptor =
      context_.tail_kinematics.descriptor;
  auto &tail_kinematics_timeline_available = context_.tail_kinematics.available;
  auto &tail_kinematics_timeline_failed = context_.tail_kinematics.failed;
  auto &tail_kinematics_timeline_error = context_.tail_kinematics.error;

  auto &stimulus_context_timeline_descriptor =
      context_.stimulus_context.descriptor;
  auto &stimulus_context_timeline_snapshot = context_.stimulus_context.snapshot;
  auto &stimulus_context_timeline_available =
      context_.stimulus_context.available;
  auto &stimulus_context_timeline_failed = context_.stimulus_context.failed;
  auto &stimulus_context_timeline_error = context_.stimulus_context.error;

  auto &analysis_crop_geometry = context_.crop.geometry;
  auto &crop_playback = context_.crop.playback;
  auto &analysis_crop_view_info = context_.crop.analysis_view_info;
  auto &crop_view_info = context_.crop.view_info;
  auto &crop_controls = context_.crop.controls;
  auto &active_crop_preference = context_.crop.active_preference;
  auto &crop_enabled = context_.crop.enabled;
  auto &composite_enabled = context_.crop.composite_enabled;
  auto &crop_error = context_.crop.error;

  auto loaded_analysis =
      std::optional<AppleAnalysisRepositoryBundle>(std::move(result));
  if (DecideAppleAnalysisProductAdoption(
          loaded_analysis->hasResultFor("swim_bouts"),
          motion_timeline_available, motion_timeline_failed) ==
      AppleAnalysisProductAdoptionDecision::DeferSwimBoutsUntilMotionSettles) {
    deferred_swim_bout_result_ = std::move(*loaded_analysis);
    return;
  }
  const bool request_initial_analysis_frame =
      ShouldRequestInitialAppleAnalysisFrame(options_.video_smoke,
                                             options_.ui_reference_enabled,
                                             analysis_loader_loading);
  std::printf(
      "[AppleAnalysisLoad] state=ready total_ms=%.1f products=%zu "
      "preloaded_trace_bytes=%llu preload_budget_bytes=%llu\n",
      loaded_analysis->total_elapsed_ms, loaded_analysis->timings.size(),
      static_cast<unsigned long long>(loaded_analysis->preloaded_trace_bytes),
      static_cast<unsigned long long>(128ULL * 1024ULL * 1024ULL));
  for (const auto &timing : loaded_analysis->timings) {
    std::printf("[AppleAnalysisLoad] product=%s state=%s elapsed_ms=%.1f "
                "error=%s\n",
                timing.product.c_str(),
                timing.available ? "ready" : "unavailable", timing.elapsed_ms,
                timing.error.c_str());
  }
  if (loaded_analysis->hasResultFor("archive") && loaded_analysis->archive) {
    analysis_archive = loaded_analysis->archive;
    std::printf("[AppleTensorStore] cache_pool_bytes=%zu "
                "recheck_cached_metadata=open recheck_cached_data=open\n",
                loaded_analysis->archive->cachePoolBytes());
  }
  if (loaded_analysis->hasResultFor("chaser_polar")) {
    auto chaser_distance_polar_repository =
        std::move(loaded_analysis->chaser_distance_polar);
    chaser_distance_polar_error = loaded_analysis->errorFor("chaser_polar");
    if (chaser_distance_polar_repository) {
      chaser_distance_polar_descriptor =
          chaser_distance_polar_repository->descriptor();
      if (!chaser_distance_polar_descriptor.ready() &&
          chaser_distance_polar_error.empty()) {
        chaser_distance_polar_error = chaser_distance_polar_descriptor.error;
      }
    }
    bool chaser_distance_polar_initialization_ready =
        chaser_distance_polar_repository != nullptr &&
        chaser_distance_polar_descriptor.ready() &&
        chaser_distance_polar_buffer.open(
            std::move(chaser_distance_polar_repository), 8, 16,
            &chaser_distance_polar_error);
    if (chaser_distance_polar_initialization_ready &&
        request_initial_analysis_frame) {
      chaser_distance_polar_initialization_ready =
          chaser_distance_polar_buffer.requestFrame(
              options_.initial_frame, true, &chaser_distance_polar_error) &&
          (!options_.video_smoke ||
           chaser_distance_polar_buffer.waitForFrame(options_.initial_frame,
                                                     std::chrono::seconds(10)));
    }
    if (chaser_distance_polar_initialization_ready) {
      chaser_distance_polar_available = true;
      if (request_initial_analysis_frame) {
        last_chaser_distance_polar_camera_request = options_.initial_frame;
      }
      std::printf(
          "[AppleChaserPolar] run=%s component=%s rows=%zu "
          "chasers=%zu radial_max_mm=%.3f buffer=16 lookahead=8\n",
          chaser_distance_polar_descriptor.provenance.run_name.c_str(),
          chaser_distance_polar_descriptor.provenance.component_name.c_str(),
          chaser_distance_polar_descriptor.row_count,
          chaser_distance_polar_descriptor.chaser_count,
          chaser_distance_polar_descriptor.radial_scale
              .display_max_distance_mm);
    } else {
      chaser_distance_polar_buffer.close();
      chaser_distance_polar_failed = chaser_distance_polar_descriptor.ready();
      if (chaser_distance_polar_error.empty()) {
        chaser_distance_polar_error =
            "chaser-distance polar dataset is unavailable";
      }
      std::fprintf(stderr, "[AppleChaserPolar] Unavailable: %s\n",
                   chaser_distance_polar_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("stimulus")) {
    stimulus_error = loaded_analysis->errorFor("stimulus");
    auto stimulus_repository = std::move(loaded_analysis->stimulus);
    const std::string stimulus_run =
        stimulus_repository ? stimulus_repository->runName() : std::string{};
    bool stimulus_initialization_ready =
        stimulus_repository &&
        stimulus_playback.open(std::move(stimulus_repository),
                               options_.stimulus_buffer_capacity,
                               &stimulus_error) &&
        options_.initial_frame <= std::numeric_limits<int32_t>::max() &&
        stimulus_playback.requestCameraFrame(
            static_cast<int32_t>(options_.initial_frame), true,
            &stimulus_error);
    if (stimulus_initialization_ready) {
      const auto initial_resolution = stimulus_playback.resolveCameraFrame(
          static_cast<int32_t>(options_.initial_frame));
      if (options_.video_smoke &&
          initial_resolution.status ==
              crimson::zarr::StimulusMappingStatus::Mapped) {
        stimulus_initialization_ready = stimulus_playback.waitForCameraFrame(
            static_cast<int32_t>(options_.initial_frame),
            std::chrono::seconds(10), &stimulus_error);
      }
    }
    if (stimulus_initialization_ready) {
      stimulus_enabled = true;
      const auto &stimulus_info = stimulus_playback.info();
      std::printf(
          "[AppleStimulus] run=%s asset=%dx%d frames=%lld fps=%.6f "
          "buffer_capacity=%zu startup_ms=%.1f path=%s\n",
          stimulus_run.c_str(), stimulus_info.width, stimulus_info.height,
          static_cast<long long>(stimulus_info.frame_count),
          stimulus_info.nominal_frame_rate, options_.stimulus_buffer_capacity,
          stimulus_playback.metrics().decoder.startup_ms,
          stimulus_info.path.c_str());
    } else {
      stimulus_playback.close();
      std::fprintf(stderr, "[AppleStimulus] Unavailable: %s\n",
                   stimulus_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("canonical_detection")) {
    canonical_detection_error =
        loaded_analysis->errorFor("canonical_detection");
    canonical_detection_open_metrics =
        loaded_analysis->canonical_detection_open_metrics;
    auto repository = std::move(loaded_analysis->canonical_detection);
    bool canonical_ready = repository != nullptr;
    if (canonical_ready) {
      canonical_detection_descriptor = repository->descriptor();
      canonical_ready = canonical_detection_buffer.open(
          std::move(repository), 70, 32, &canonical_detection_error);
    }
    if (canonical_ready) {
      canonical_ready = canonical_detection_buffer.requestFrame(
          options_.initial_frame, true, &canonical_detection_error);
    }
    if (canonical_ready && options_.video_smoke) {
      canonical_ready = canonical_detection_buffer.waitForFrame(
          options_.initial_frame, std::chrono::seconds(20));
    }
    if (!canonical_ready) {
      canonical_detection_failed = true;
      canonical_detection_buffer.close();
      if (canonical_detection_error.empty()) {
        canonical_detection_error = "Canonical detection initialization failed";
      }
      if (options_.explicit_detection_requested) {
        analysis_loading_start_error = canonical_detection_error;
      }
      std::fprintf(stderr, "[AppleCanonicalDetection] Unavailable: %s\n",
                   canonical_detection_error.c_str());
    } else {
      canonical_detection_available = true;
      last_canonical_detection_camera_request = options_.initial_frame;
      std::printf(
          "[AppleCanonicalDetection] surface=%s group=%s run=%s "
          "rows=%zu "
          "camera_frames=%zu page_frames=70 cache_pages=32 "
          "stable_identity=%d source_audit_lazy=%d consolidated=%d "
          "root_reads=%zu declarations=%zu "
          "exact_opens=%zu fallback_metadata=%zu fallback_dtype=%zu "
          "offset_reads=%zu offset_bytes=%zu open_ms=%.1f "
          "offset_ms=%.1f residency_budget_bytes=%llu "
          "residency_chunk_bytes=%llu\n",
          canonical_detection_descriptor.surface_kind ==
                  crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1
              ? "refined_v1"
              : "canonical_raw_v1",
          canonical_detection_descriptor.source_group.c_str(),
          canonical_detection_descriptor.run_name.c_str(),
          canonical_detection_descriptor.row_count,
          canonical_detection_descriptor.camera_frame_count,
          canonical_detection_descriptor.stable_identity ? 1 : 0,
          canonical_detection_descriptor.source_audit_lazy ? 1 : 0,
          canonical_detection_descriptor.consolidated_metadata ? 1 : 0,
          canonical_detection_open_metrics.root_metadata_reads,
          canonical_detection_open_metrics.consolidated_array_declarations,
          canonical_detection_open_metrics.exact_handle_opens,
          canonical_detection_open_metrics.fallback_metadata_reads,
          canonical_detection_open_metrics.fallback_dtype_opens,
          canonical_detection_open_metrics.offset_read_calls,
          canonical_detection_open_metrics.retained_offset_bytes,
          canonical_detection_open_metrics.total_ms,
          canonical_detection_open_metrics.offset_read_ms,
          static_cast<unsigned long long>(
              options_.detection_residency_budget_bytes),
          static_cast<unsigned long long>(
              options_.detection_residency_chunk_bytes));
    }
  }

  if (loaded_analysis->hasResultFor("keypoints")) {
    keypoint_error = loaded_analysis->errorFor("keypoints");
    if (loaded_analysis->keypoint_v2_selected ||
        options_.keypoint_v2_requested) {
      const auto &metrics = loaded_analysis->keypoint_v2_open_metrics;
      std::printf("[AppleKeypointV2Open] state=%s total_ms=%.1f "
                  "metadata_ms=%.1f handles_ms=%.1f identity_ms=%.1f "
                  "root_reads=%zu direct_reads=%zu declarations=%zu "
                  "exact_opens=%zu fallback_metadata=%zu fallback_dtype=%zu "
                  "raw_offset_reads=%zu selected_offset_reads=%zu "
                  "quality_offset_reads=%zu body_offset_reads=%zu "
                  "quality_payload_reads=%zu retained_offset_bytes=%zu\n",
                  loaded_analysis->keypoint_v2_selected ? "ready" : "failed",
                  metrics.total_ms, metrics.metadata_ms,
                  metrics.exact_handle_open_ms, metrics.identity_validation_ms,
                  metrics.root_metadata_reads, metrics.direct_metadata_reads,
                  metrics.consolidated_array_declarations,
                  metrics.exact_handle_opens, metrics.fallback_metadata_reads,
                  metrics.fallback_dtype_opens, metrics.raw_offset_read_calls,
                  metrics.selected_offset_read_calls,
                  metrics.quality_offset_read_calls,
                  metrics.body_frame_offset_read_calls,
                  metrics.quality_payload_reads, metrics.retained_offset_bytes);
    } else {
      const auto &open_metrics = loaded_analysis->keypoint_open_metrics;
      const char *open_mode = open_metrics.lazy_path       ? "lazy"
                              : open_metrics.fallback_path ? "fallback"
                                                           : "failed";
      std::printf(
          "[AppleKeypointOpen] mode=%s total_ms=%.1f "
          "selection_ms=%.1f run_attributes_ms=%.1f "
          "required_handles_ms=%.1f frame_counts_read_ms=%.1f "
          "prefix_sum_ms=%.1f crop_lineage_ms=%.1f "
          "optional_handles_ms=%.1f fallback_ms=%.1f "
          "attributes=%zu array_open_attempts=%zu "
          "array_open_successes=%zu array_open_failures=%zu "
          "array_reads=%zu events=%zu\n",
          open_mode, open_metrics.total_ms, open_metrics.selection_ms,
          open_metrics.run_attributes_ms, open_metrics.required_handles_ms,
          open_metrics.frame_counts_read_ms, open_metrics.prefix_sum_ms,
          open_metrics.crop_lineage_ms, open_metrics.optional_handles_ms,
          open_metrics.fallback_materialization_ms,
          open_metrics.attribute_reads, open_metrics.array_open_attempts,
          open_metrics.array_open_successes, open_metrics.array_open_failures,
          open_metrics.array_reads, open_metrics.events.size());
      if (crimson_env_flag_enabled("CRIMSON_STARTUP_TRACE")) {
        for (const auto &event : open_metrics.events) {
          std::printf("[AppleKeypointOpenTrace] phase=%s operation=%s "
                      "state=%s elapsed_ms=%.1f candidate=%s path=%s\n",
                      event.phase.c_str(), event.operation.c_str(),
                      event.success ? "ready" : "unavailable", event.elapsed_ms,
                      event.candidate.c_str(), event.path.c_str());
        }
      }
    }
    auto keypoint_repository = std::move(loaded_analysis->keypoints);
    if (keypoint_repository) {
      keypoint_descriptor = keypoint_repository->descriptor();
      bool keypoint_ready = keypoint_overlay_buffer.open(
          std::move(keypoint_repository), 12, 24, &keypoint_error);
      if (keypoint_ready && request_initial_analysis_frame) {
        keypoint_ready = keypoint_overlay_buffer.requestFrame(
            options_.initial_frame, video_playback.info().width,
            video_playback.info().height, true, &keypoint_error);
      }
      if (keypoint_ready && request_initial_analysis_frame &&
          options_.video_smoke) {
        keypoint_ready = keypoint_overlay_buffer.waitForFrame(
            options_.initial_frame, std::chrono::seconds(20));
      }
      if (!keypoint_ready) {
        keypoint_overlay_failed = true;
        keypoint_overlay_buffer.close();
        if (keypoint_error.empty()) {
          keypoint_error = "Initial keypoint frame did not settle";
        }
        if (options_.keypoint_v2_requested) {
          analysis_loading_start_error = keypoint_error;
        }
        std::fprintf(stderr, "[AppleKeypoints] Initialization failed: %s\n",
                     keypoint_error.c_str());
      } else {
        keypoint_overlay_available = true;
        if (request_initial_analysis_frame) {
          last_keypoint_camera_request = options_.initial_frame;
        }
        const auto &descriptor = keypoint_descriptor;
        const char *coordinate_space =
            descriptor.coordinate_space ==
                    crimson::zarr::KeypointCoordinateSpace::Image
                ? "image"
            : descriptor.coordinate_space ==
                    crimson::zarr::KeypointCoordinateSpace::Roi
                ? "roi"
                : "normalized-roi";
        std::printf("[AppleKeypoints] group=%s run=%s refined=%d crop_run=%s "
                    "rows=%zu camera_frames=%zu labels=%zu edges=%zu "
                    "space=%s\n",
                    descriptor.source_group.c_str(),
                    descriptor.run_name.c_str(), descriptor.refined,
                    descriptor.source_crop_run.c_str(), descriptor.row_count,
                    descriptor.camera_frame_count,
                    descriptor.keypoint_labels.size(),
                    descriptor.skeleton_edges.size(), coordinate_space);
      }
    } else {
      std::fprintf(stderr, "[AppleKeypoints] Unavailable: %s\n",
                   keypoint_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("subject_masks") &&
      options_.subject_masks_enabled) {
    std::printf("[AppleDataAccess] source=subject_masks phase=open "
                "state=begin\n");
    std::fflush(stdout);
    const auto subject_mask_open_started = std::chrono::steady_clock::now();
    subject_mask_error = loaded_analysis->errorFor("subject_masks");
    auto subject_mask_repository = std::move(loaded_analysis->subject_masks);
    const double subject_mask_open_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - subject_mask_open_started)
            .count();
    if (subject_mask_repository) {
      const auto open_metrics = subject_mask_repository->metrics();
      std::printf(
          "[AppleDataAccess] source=subject_masks phase=open "
          "state=ready elapsed_ms=%.1f repository_ms=%.1f "
          "lazy_mapping=%d catalog_ms=%.1f mapping_ms=%.1f "
          "storage_ms=%.1f "
          "contour_ms=%.1f index_ms=%.1f metadata_decoded_bytes=%llu "
          "metadata_retained_bytes=%llu\n",
          subject_mask_open_ms, open_metrics.open_total_ms,
          open_metrics.lazy_mapping ? 1 : 0, open_metrics.catalog_ms,
          open_metrics.mapping_read_ms, open_metrics.storage_open_ms,
          open_metrics.contour_open_ms, open_metrics.metadata_index_ms,
          static_cast<unsigned long long>(open_metrics.metadata_decoded_bytes),
          static_cast<unsigned long long>(
              open_metrics.metadata_retained_bytes));
      std::printf(
          "[AppleDataAccess] source=subject_masks phase=mapping "
          "frame_indices_ms=%.1f detection_indices_ms=%.1f "
          "source_crop_row_ids_ms=%.1f crop_frame_indices_ms=%.1f "
          "crop_coordinates_ms=%.1f crop_detection_indices_ms=%.1f "
          "subject_bytes=%llu crop_bytes=%llu\n",
          open_metrics.frame_indices_ms, open_metrics.detection_indices_ms,
          open_metrics.source_crop_row_ids_ms,
          open_metrics.crop_frame_indices_ms, open_metrics.crop_coordinates_ms,
          open_metrics.crop_detection_indices_ms,
          static_cast<unsigned long long>(open_metrics.subject_mapping_bytes),
          static_cast<unsigned long long>(open_metrics.crop_mapping_bytes));
      std::fflush(stdout);
      subject_mask_descriptor = subject_mask_repository->descriptor();
      subject_mask_frame_presentation.reset();
      bool subject_mask_initialization_ready = subject_mask_overlay_buffer.open(
          std::move(subject_mask_repository), 12, 24, &subject_mask_error);
      if (subject_mask_initialization_ready && request_initial_analysis_frame) {
        subject_mask_initial_request_started = std::chrono::steady_clock::now();
        subject_mask_initial_request_frame = options_.initial_frame;
        subject_mask_initialization_ready =
            subject_mask_overlay_buffer.requestFrame(
                options_.initial_frame, video_playback.info().width,
                video_playback.info().height, true, &subject_mask_error);
      }
      if (subject_mask_initialization_ready && request_initial_analysis_frame &&
          options_.video_smoke) {
        subject_mask_initialization_ready =
            subject_mask_overlay_buffer.waitForFrame(options_.initial_frame,
                                                     std::chrono::seconds(15));
      }
      if (subject_mask_initialization_ready) {
        subject_mask_overlay_available = true;
        subject_mask_smoke_start_pending = options_.video_smoke;
        if (options_.video_smoke) {
          logSubjectMaskFirstReady();
        }
        const char *storage =
            subject_mask_descriptor.storage ==
                    crimson::zarr::SubjectMaskStorage::Dense
                ? "dense"
            : subject_mask_descriptor.storage ==
                    crimson::zarr::SubjectMaskStorage::Bitpacked
                ? "bitpacked"
                : "rle";
        const auto subject_mask_repository_metrics =
            subject_mask_overlay_buffer.repositoryMetrics();
        std::printf("[AppleSubjectMasks] group=%s run=%s crop_run=%s "
                    "storage=%s rows=%zu camera_frames=%zu components=%zu "
                    "mask=%zux%zu chunk_rows=%zu strict_v1=%d "
                    "contour_only=%d contour_cache_run=%s offset_reads=%llu "
                    "lookahead=12 cache=24\n",
                    subject_mask_descriptor.source_group.c_str(),
                    subject_mask_descriptor.run_name.c_str(),
                    subject_mask_descriptor.source_crop_run.c_str(), storage,
                    subject_mask_descriptor.row_count,
                    subject_mask_descriptor.camera_frame_count,
                    subject_mask_descriptor.component_labels.size(),
                    subject_mask_descriptor.mask_width,
                    subject_mask_descriptor.mask_height,
                    subject_mask_descriptor.storage_chunk_rows,
                    subject_mask_descriptor.strict_v1 ? 1 : 0,
                    subject_mask_descriptor.contour_only ? 1 : 0,
                    subject_mask_descriptor.presentation_cache_run.c_str(),
                    static_cast<unsigned long long>(
                        subject_mask_repository_metrics.frame_offset_reads));
      } else {
        subject_mask_overlay_failed = true;
        subject_mask_overlay_buffer.close();
        if (subject_mask_error.empty()) {
          subject_mask_error =
              "Timed out settling the initial subject-mask smoke frame";
        }
        std::fprintf(stderr, "[AppleSubjectMasks] Initialization failed: %s\n",
                     subject_mask_error.c_str());
      }
    } else {
      std::printf("[AppleDataAccess] source=subject_masks phase=open "
                  "state=failed elapsed_ms=%.1f error=%s\n",
                  subject_mask_open_ms, subject_mask_error.c_str());
      std::fflush(stdout);
      std::fprintf(stderr, "[AppleSubjectMasks] Unavailable: %s\n",
                   subject_mask_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("subject_shape") &&
      options_.subject_shapes_enabled) {
    subject_shape_error = loaded_analysis->errorFor("subject_shape");
    auto subject_shape_repository = std::move(loaded_analysis->subject_shape);
    if (subject_shape_repository) {
      subject_shape_descriptor = subject_shape_repository->descriptor();
      bool subject_shape_ready = subject_shape_overlay_buffer.open(
          std::move(subject_shape_repository), 6, 16, &subject_shape_error);
      if (subject_shape_ready && request_initial_analysis_frame) {
        subject_shape_ready =
            subject_shape_overlay_buffer.requestFrame(
                options_.initial_frame, video_playback.info().width,
                video_playback.info().height, true, &subject_shape_error) &&
            (!options_.video_smoke ||
             subject_shape_overlay_buffer.waitForFrame(
                 options_.initial_frame, std::chrono::seconds(20)));
      }
      if (subject_shape_ready) {
        subject_shape_overlay_available = true;
        std::printf(
            "[AppleSubjectShape] group=%s run=%s refined_masks=%s "
            "crop_run=%s rows=%zu camera_frames=%zu "
            "coordinates=%zux%zu "
            "centerline=%zu bspline=%zu lookahead=6 cache=16\n",
            subject_shape_descriptor.source_group.c_str(),
            subject_shape_descriptor.run_name.c_str(),
            subject_shape_descriptor.source_refined_subject_masks_run.c_str(),
            subject_shape_descriptor.source_crop_run.c_str(),
            subject_shape_descriptor.row_count,
            subject_shape_descriptor.camera_frame_count,
            subject_shape_descriptor.coordinate_width,
            subject_shape_descriptor.coordinate_height,
            subject_shape_descriptor.centerline_point_count,
            subject_shape_descriptor.bspline_sample_point_count);
      } else {
        subject_shape_overlay_failed = true;
        subject_shape_overlay_buffer.close();
        if (subject_shape_error.empty()) {
          subject_shape_error = "Timed out settling the initial "
                                "subject-shape smoke frame";
        }
        std::fprintf(stderr, "[AppleSubjectShape] Initialization failed: %s\n",
                     subject_shape_error.c_str());
      }
    } else {
      std::fprintf(stderr, "[AppleSubjectShape] Unavailable: %s\n",
                   subject_shape_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("eye_geometry") &&
      options_.eye_geometry_enabled) {
    eye_geometry_error = loaded_analysis->errorFor("eye_geometry");
    auto eye_geometry_repository = std::move(loaded_analysis->eye_geometry);
    if (eye_geometry_repository) {
      eye_geometry_descriptor = eye_geometry_repository->descriptor();
      bool eye_geometry_ready = eye_geometry_overlay_buffer.open(
          std::move(eye_geometry_repository), 6, 16, &eye_geometry_error);
      if (eye_geometry_ready && request_initial_analysis_frame) {
        eye_geometry_ready =
            eye_geometry_overlay_buffer.requestFrame(
                options_.initial_frame, video_playback.info().width,
                video_playback.info().height, true, &eye_geometry_error) &&
            (!options_.video_smoke ||
             eye_geometry_overlay_buffer.waitForFrame(
                 options_.initial_frame, std::chrono::seconds(20)));
      }
      if (eye_geometry_ready) {
        eye_geometry_overlay_available = true;
        std::printf(
            "[AppleEyeGeometry] group=%s run=%s refined_masks=%s "
            "crop_run=%s schema=%s:%d method=%s rows=%zu "
            "camera_frames=%zu coordinates=%zux%zu lookahead=6 "
            "cache=16\n",
            eye_geometry_descriptor.source_group.c_str(),
            eye_geometry_descriptor.run_name.c_str(),
            eye_geometry_descriptor.source_refined_subject_masks_run.c_str(),
            eye_geometry_descriptor.source_crop_run.c_str(),
            eye_geometry_descriptor.schema_id.c_str(),
            eye_geometry_descriptor.schema_version,
            eye_geometry_descriptor.method.c_str(),
            eye_geometry_descriptor.row_count,
            eye_geometry_descriptor.camera_frame_count,
            eye_geometry_descriptor.coordinate_width,
            eye_geometry_descriptor.coordinate_height);
      } else {
        eye_geometry_overlay_failed = true;
        eye_geometry_overlay_buffer.close();
        if (eye_geometry_error.empty()) {
          eye_geometry_error =
              "Timed out settling the initial eye-geometry smoke frame";
        }
        std::fprintf(stderr, "[AppleEyeGeometry] Initialization failed: %s\n",
                     eye_geometry_error.c_str());
      }
    } else {
      std::fprintf(stderr, "[AppleEyeGeometry] Unavailable: %s\n",
                   eye_geometry_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("motion") &&
      options_.motion_timeline_enabled) {
    motion_timeline_error = loaded_analysis->errorFor("motion");
    auto motion_repository = std::move(loaded_analysis->motion);
    if (motion_repository) {
      const auto preload_metrics = motion_repository->metrics();
      motion_timeline_descriptor = motion_repository->descriptor();
      workspace_state.selections().motion_source_key =
          crimson::timeline::defaultAnalysisSeriesSource(
              motion_timeline_descriptor);
      const double fps = video_playback.info().nominal_frame_rate;
      bool timeline_initialization_ready =
          motion_timeline_buffer.open(std::move(motion_repository), 4096, 2048,
                                      1200, 3, &motion_timeline_error);
      const bool initial_page_required =
          request_initial_analysis_frame &&
          (options_.show_analysis_timeline || options_.video_smoke ||
           options_.require_motion_timeline);
      if (timeline_initialization_ready && initial_page_required) {
        timeline_initialization_ready =
            motion_timeline_buffer.requestFrame(
                options_.initial_frame,
                workspace_state.selections().motion_source_key, fps, true,
                &motion_timeline_error) &&
            (!options_.video_smoke ||
             motion_timeline_buffer.waitForFrame(
                 options_.initial_frame,
                 workspace_state.selections().motion_source_key, fps,
                 std::chrono::seconds(20)));
      }
      if (timeline_initialization_ready) {
        motion_timeline_available = true;
        std::printf("[AppleMotionTimeline] sources=%zu camera_frames=%zu "
                    "default=%s page=4096 step=2048 points=1200 cache=3 "
                    "preloaded=%d preload_bytes=%llu preload_ms=%.1f\n",
                    motion_timeline_descriptor.sources.size(),
                    motion_timeline_descriptor.frame_count,
                    motion_timeline_descriptor.default_source.c_str(),
                    preload_metrics.default_source_preloaded ? 1 : 0,
                    static_cast<unsigned long long>(
                        preload_metrics.preloaded_retained_bytes),
                    preload_metrics.preload_ms);
      } else {
        motion_timeline_failed = true;
        motion_timeline_buffer.close();
        if (motion_timeline_error.empty()) {
          motion_timeline_error =
              "Timed out settling the initial motion timeline page";
        }
        std::fprintf(stderr,
                     "[AppleMotionTimeline] Initialization failed: %s\n",
                     motion_timeline_error.c_str());
      }
    } else {
      if (options_.require_motion_timeline) {
        motion_timeline_failed = true;
      }
      std::fprintf(stderr, "[AppleMotionTimeline] Unavailable: %s\n",
                   motion_timeline_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("swim_bouts") &&
      options_.swim_bout_timeline_enabled && motion_timeline_available) {
    swim_bout_timeline_error = loaded_analysis->errorFor("swim_bouts");
    auto swim_bout_repository = std::move(loaded_analysis->swim_bouts);
    if (swim_bout_repository) {
      swim_bout_timeline_descriptor = swim_bout_repository->descriptor();
      const auto *motion_source = crimson::timeline::findAnalysisSeriesSource(
          motion_timeline_descriptor,
          workspace_state.selections().motion_source_key);
      if (motion_source != nullptr) {
        workspace_state.selections().swim_bout_candidate_key =
            crimson::timeline::defaultSwimBoutCandidate(
                swim_bout_timeline_descriptor, *motion_source);
      }
      bool timeline_initialization_ready =
          !workspace_state.selections().swim_bout_candidate_key.empty() &&
          swim_bout_timeline_buffer.open(std::move(swim_bout_repository), 4096,
                                         2048, 1200, 4,
                                         &swim_bout_timeline_error);
      const bool initial_page_required =
          request_initial_analysis_frame &&
          (options_.show_analysis_timeline || options_.video_smoke ||
           options_.require_swim_bout_timeline);
      const double fps = video_playback.info().nominal_frame_rate;
      if (timeline_initialization_ready && initial_page_required) {
        timeline_initialization_ready =
            swim_bout_timeline_buffer.requestFrame(
                options_.initial_frame,
                workspace_state.selections().swim_bout_candidate_key, fps,
                analysis_timeline_controls.swim_bouts.show_detector_response,
                true, &swim_bout_timeline_error) &&
            (!options_.video_smoke ||
             swim_bout_timeline_buffer.waitForFrame(
                 options_.initial_frame,
                 workspace_state.selections().swim_bout_candidate_key, fps,
                 analysis_timeline_controls.swim_bouts.show_detector_response,
                 std::chrono::seconds(20)));
      }
      if (timeline_initialization_ready) {
        swim_bout_timeline_available = true;
        std::printf(
            "[AppleSwimBoutTimeline] candidates=%zu camera_frames=%zu "
            "default=%s page=4096 step=2048 points=1200 cache=4\n",
            swim_bout_timeline_descriptor.candidates.size(),
            swim_bout_timeline_descriptor.frame_count,
            workspace_state.selections().swim_bout_candidate_key.c_str());
      } else {
        swim_bout_timeline_failed = true;
        swim_bout_timeline_buffer.close();
        if (swim_bout_timeline_error.empty()) {
          swim_bout_timeline_error =
              "No swim-bout candidate is compatible with the selected "
              "motion source";
        }
        std::fprintf(stderr,
                     "[AppleSwimBoutTimeline] Initialization failed: %s\n",
                     swim_bout_timeline_error.c_str());
      }
    } else {
      if (options_.require_swim_bout_timeline) {
        swim_bout_timeline_failed = true;
      }
      std::fprintf(stderr, "[AppleSwimBoutTimeline] Unavailable: %s\n",
                   swim_bout_timeline_error.c_str());
    }
  } else if (loaded_analysis->hasResultFor("swim_bouts") &&
             options_.require_swim_bout_timeline) {
    swim_bout_timeline_failed = true;
    swim_bout_timeline_error =
        "Swim-bout timelines require a compatible motion timeline";
    std::fprintf(stderr, "[AppleSwimBoutTimeline] Unavailable: %s\n",
                 swim_bout_timeline_error.c_str());
  }

  if (loaded_analysis->hasResultFor("eye_angles") &&
      options_.eye_angle_timeline_enabled) {
    eye_angle_timeline_error = loaded_analysis->errorFor("eye_angles");
    auto eye_angle_repository = std::move(loaded_analysis->eye_angles);
    if (eye_angle_repository) {
      const auto preload_metrics = eye_angle_repository->metrics();
      eye_angle_timeline_descriptor = eye_angle_repository->descriptor();
      workspace_state.selections().eye_angle_representation_key =
          crimson::timeline::defaultEyeAngleTimelineRepresentation(
              eye_angle_timeline_descriptor);
      if (options_.prefer_alternate_eye_angle_representation &&
          eye_angle_timeline_descriptor.representations.size() > 1) {
        const std::string default_representation =
            workspace_state.selections().eye_angle_representation_key;
        const auto alternate =
            std::find_if(eye_angle_timeline_descriptor.representations.begin(),
                         eye_angle_timeline_descriptor.representations.end(),
                         [&default_representation](const auto &candidate) {
                           return candidate.key != default_representation;
                         });
        if (alternate != eye_angle_timeline_descriptor.representations.end()) {
          workspace_state.selections().eye_angle_representation_key =
              alternate->key;
        }
      }
      const double fps = video_playback.info().nominal_frame_rate;
      bool timeline_initialization_ready = eye_angle_timeline_buffer.open(
          std::move(eye_angle_repository), 4096, 2048, 1200, 3,
          &eye_angle_timeline_error);
      const bool initial_page_required =
          request_initial_analysis_frame &&
          (options_.show_analysis_timeline || options_.video_smoke ||
           options_.require_eye_angle_timeline);
      if (timeline_initialization_ready && initial_page_required) {
        timeline_initialization_ready =
            eye_angle_timeline_buffer.requestFrame(
                options_.initial_frame,
                workspace_state.selections().eye_angle_representation_key, fps,
                true, &eye_angle_timeline_error) &&
            (!options_.video_smoke ||
             eye_angle_timeline_buffer.waitForFrame(
                 options_.initial_frame,
                 workspace_state.selections().eye_angle_representation_key, fps,
                 std::chrono::seconds(20)));
      }
      if (timeline_initialization_ready) {
        eye_angle_timeline_available = true;
        std::printf(
            "[AppleEyeAngleTimeline] group=%s run=%s schema=%s:%d "
            "layout=%s rows=%zu camera_frames=%zu default=%s "
            "representations=%zu page=4096 step=2048 points=1200 "
            "cache=3 preloaded=%d preload_bytes=%llu preload_ms=%.1f\n",
            eye_angle_timeline_descriptor.source_group.c_str(),
            eye_angle_timeline_descriptor.run_name.c_str(),
            eye_angle_timeline_descriptor.schema_id.c_str(),
            eye_angle_timeline_descriptor.schema_version,
            eye_angle_timeline_descriptor.layout.c_str(),
            eye_angle_timeline_descriptor.roi_row_count,
            eye_angle_timeline_descriptor.frame_count,
            eye_angle_timeline_descriptor.default_representation.c_str(),
            eye_angle_timeline_descriptor.representations.size(),
            preload_metrics.frame_series_preloaded ? 1 : 0,
            static_cast<unsigned long long>(
                preload_metrics.preloaded_retained_bytes),
            preload_metrics.preload_ms);
      } else {
        eye_angle_timeline_failed = true;
        eye_angle_timeline_buffer.close();
        if (eye_angle_timeline_error.empty()) {
          eye_angle_timeline_error =
              "Timed out settling the initial eye-angle timeline page";
        }
        std::fprintf(stderr,
                     "[AppleEyeAngleTimeline] Initialization failed: %s\n",
                     eye_angle_timeline_error.c_str());
      }
    } else {
      if (options_.require_eye_angle_timeline) {
        eye_angle_timeline_failed = true;
      }
      std::fprintf(stderr, "[AppleEyeAngleTimeline] Unavailable: %s\n",
                   eye_angle_timeline_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("tail_kinematics") &&
      options_.tail_kinematics_timeline_enabled) {
    tail_kinematics_timeline_error =
        loaded_analysis->errorFor("tail_kinematics");
    auto tail_repository = std::move(loaded_analysis->tail_kinematics);
    if (tail_repository) {
      const auto preload_metrics = tail_repository->metrics();
      tail_kinematics_timeline_descriptor = tail_repository->descriptor();
      workspace_state.selections().tail_kinematics_source_key =
          crimson::timeline::defaultAnalysisSeriesSource(
              tail_kinematics_timeline_descriptor);
      const double fps = video_playback.info().nominal_frame_rate;
      bool timeline_initialization_ready = tail_kinematics_timeline_buffer.open(
          std::move(tail_repository), 4096, 2048, 1200, 3,
          &tail_kinematics_timeline_error);
      const bool initial_page_required =
          request_initial_analysis_frame &&
          (options_.show_analysis_timeline || options_.video_smoke ||
           options_.require_tail_kinematics_timeline);
      if (timeline_initialization_ready && initial_page_required) {
        timeline_initialization_ready =
            tail_kinematics_timeline_buffer.requestFrame(
                options_.initial_frame,
                workspace_state.selections().tail_kinematics_source_key, fps,
                true, &tail_kinematics_timeline_error) &&
            (!options_.video_smoke ||
             tail_kinematics_timeline_buffer.waitForFrame(
                 options_.initial_frame,
                 workspace_state.selections().tail_kinematics_source_key, fps,
                 std::chrono::seconds(20)));
      }
      if (timeline_initialization_ready) {
        tail_kinematics_timeline_available = true;
        std::printf("[AppleTailKinematicsTimeline] sources=%zu "
                    "camera_frames=%zu default=%s page=4096 step=2048 "
                    "points=1200 cache=3 preloaded=%d preload_bytes=%llu "
                    "preload_ms=%.1f\n",
                    tail_kinematics_timeline_descriptor.sources.size(),
                    tail_kinematics_timeline_descriptor.frame_count,
                    tail_kinematics_timeline_descriptor.default_source.c_str(),
                    preload_metrics.default_source_preloaded ? 1 : 0,
                    static_cast<unsigned long long>(
                        preload_metrics.preloaded_retained_bytes),
                    preload_metrics.preload_ms);
      } else {
        tail_kinematics_timeline_failed = true;
        tail_kinematics_timeline_buffer.close();
        if (tail_kinematics_timeline_error.empty()) {
          tail_kinematics_timeline_error =
              "Timed out settling the initial tail-kinematics timeline "
              "page";
        }
        std::fprintf(
            stderr, "[AppleTailKinematicsTimeline] Initialization failed: %s\n",
            tail_kinematics_timeline_error.c_str());
      }
    } else {
      if (options_.require_tail_kinematics_timeline) {
        tail_kinematics_timeline_failed = true;
      }
      std::fprintf(stderr, "[AppleTailKinematicsTimeline] Unavailable: %s\n",
                   tail_kinematics_timeline_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("stimulus_context") &&
      options_.stimulus_context_timeline_enabled) {
    stimulus_context_timeline_error =
        loaded_analysis->errorFor("stimulus_context");
    auto stimulus_context_repository =
        std::move(loaded_analysis->stimulus_context);
    if (stimulus_context_repository) {
      stimulus_context_timeline_descriptor =
          stimulus_context_repository->descriptor();
      stimulus_context_timeline_snapshot =
          stimulus_context_repository->snapshot();
      stimulus_context_timeline_available =
          stimulus_context_timeline_snapshot != nullptr &&
          (!stimulus_context_timeline_snapshot->events.empty() ||
           !stimulus_context_timeline_snapshot->steps.empty());
      if (stimulus_context_timeline_available) {
        size_t unresolved_camera_frames = 0;
        for (const auto &event : stimulus_context_timeline_snapshot->events) {
          unresolved_camera_frames += event.camera_frame < 0 ? 1 : 0;
        }
        std::printf("[AppleStimulusContextTimeline] run=%s events=%zu "
                    "steps=%zu "
                    "event_types=%zu camera_frames=%zu unresolved=%zu\n",
                    stimulus_context_timeline_descriptor.run_name.c_str(),
                    stimulus_context_timeline_descriptor.event_count,
                    stimulus_context_timeline_descriptor.step_count,
                    stimulus_context_timeline_descriptor.event_types.size(),
                    stimulus_context_timeline_descriptor.frame_count,
                    unresolved_camera_frames);
      }
    }
    if (!stimulus_context_timeline_available) {
      if (options_.require_stimulus_context_timeline) {
        stimulus_context_timeline_failed = true;
      }
      std::fprintf(stderr, "[AppleStimulusContextTimeline] Unavailable: %s\n",
                   stimulus_context_timeline_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("crop_geometry")) {
    std::string geometry_error;
    geometry_error = loaded_analysis->errorFor("crop_geometry");
    analysis_crop_geometry = std::move(loaded_analysis->crop_geometry);
    if (analysis_crop_geometry) {
      crop_controls.live_geometry_available = true;
      const auto &descriptor = analysis_crop_geometry->descriptor();
      AppleVideoAssetInfo geometry_info;
      geometry_info.stream_id = "analysis-crop-geometry";
      geometry_info.width = descriptor.output_width;
      geometry_info.height = descriptor.output_height;
      geometry_info.frame_count =
          static_cast<int64_t>(descriptor.camera_frame_count);
      geometry_info.nominal_frame_rate =
          video_playback.info().nominal_frame_rate;
      analysis_crop_view_info = geometry_info;
      crop_view_info = std::move(geometry_info);
      const auto geometry_memory = analysis_crop_geometry->memoryMetrics();
      std::printf(
          "[AppleCropGeometry] run=%s rows=%zu camera_frames=%zu "
          "output=%dx%d pixel_source=full-camera index=%s payload=%s "
          "construction=%s retained_bytes=%llu\n",
          descriptor.run_name.c_str(), descriptor.row_count,
          descriptor.camera_frame_count, descriptor.output_width,
          descriptor.output_height,
          descriptor.retained_frame_offsets ? "frame-offsets" : "legacy",
          descriptor.pageable_payload ? "pageable" : "resident",
          descriptor.direct_compact_columns ? "direct-columns" : "row-adapter",
          static_cast<unsigned long long>(
              geometry_memory.reportedRetainedBytes()));
    } else {
      std::fprintf(stderr, "[AppleCropGeometry] Unavailable: %s\n",
                   geometry_error.c_str());
    }
  }

  if (loaded_analysis->hasResultFor("acquisition_crop")) {
    crop_error = loaded_analysis->errorFor("acquisition_crop");
    auto acquisition_repository = std::move(loaded_analysis->acquisition_crop);
    if (acquisition_repository &&
        crop_playback.open(
            std::move(acquisition_repository), video_playback.info().width,
            video_playback.info().height,
            options_.acquisition_crop_buffer_capacity, &crop_error)) {
      crop_controls.acquisition_available = true;
      crop_controls.live_geometry_available = true;
      crop_view_info = crop_playback.info();
      const auto &crop_info = crop_playback.info();
      std::printf("[AppleCrop] stream=%s asset=%dx%d frames=%lld fps=%.6f "
                  "buffer_capacity=%zu startup_ms=%.1f path=%s\n",
                  crop_playback.repository()->descriptor().stream_id.c_str(),
                  crop_info.width, crop_info.height,
                  static_cast<long long>(crop_info.frame_count),
                  crop_info.nominal_frame_rate,
                  options_.acquisition_crop_buffer_capacity,
                  crop_playback.metrics().decoder.startup_ms,
                  crop_info.path.c_str());
    } else {
      crop_playback.close();
      std::fprintf(stderr, "[AppleCrop] Acquisition unavailable: %s\n",
                   crop_error.c_str());
    }
  }

  if ((loaded_analysis->hasResultFor("crop_geometry") ||
       loaded_analysis->hasResultFor("acquisition_crop")) &&
      !crop_controls.acquisition_available &&
      crop_controls.live_geometry_available) {
    crop_controls.preference =
        crimson::crop::CropSourcePreference::PreferLiveGeometry;
  }
  active_crop_preference = crop_controls.preference;
  crop_enabled = crop_controls.acquisition_available ||
                 crop_controls.live_geometry_available;

  if (loaded_analysis->hasResultFor("acquisition_crop") &&
      crop_controls.acquisition_available) {
    bool acquisition_ready = true;
    if (crop_controls.preference ==
        crimson::crop::CropSourcePreference::PreferAcquisitionVideo) {
      acquisition_ready = crop_playback.requestCameraFrame(
          options_.initial_frame, true, &crop_error);
      const auto initial_crop_resolution =
          crop_playback.resolveCameraFrame(options_.initial_frame);
      if (options_.video_smoke && acquisition_ready &&
          initial_crop_resolution.status ==
              crimson::zarr::AcquisitionCropMappingStatus::Mapped) {
        acquisition_ready = crop_playback.waitForCameraFrame(
            options_.initial_frame, std::chrono::seconds(10), &crop_error);
      }
    } else {
      crop_playback.suspend();
    }
    if (!acquisition_ready) {
      crop_controls.acquisition_available = false;
      crop_playback.close();
      crop_controls.live_geometry_available = analysis_crop_geometry != nullptr;
      crop_view_info = analysis_crop_view_info;
      std::fprintf(stderr,
                   "[AppleCrop] Acquisition initialization failed: %s\n",
                   crop_error.c_str());
      if (crop_controls.live_geometry_available) {
        crop_controls.preference =
            crimson::crop::CropSourcePreference::PreferLiveGeometry;
        active_crop_preference = crop_controls.preference;
      } else {
        crop_enabled = false;
      }
    }
  }
  composite_enabled = stimulus_enabled || crop_enabled;
  if (loaded_analysis->hasResultFor("motion") && deferred_swim_bout_result_) {
    auto deferred = std::move(*deferred_swim_bout_result_);
    deferred_swim_bout_result_.reset();
    adopt(std::move(deferred), analysis_loader_loading);
  }
}
