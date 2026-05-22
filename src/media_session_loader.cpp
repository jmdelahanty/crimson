#include "media_session_loader.h"
#include "global.h"
#include "render.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>

extern std::mutex g_seek_info_mutex;

namespace {

bool applyZarrCalibrationToCameraParams(const ZarrCalibrationData& calibration,
                                        CameraParams& camera_params,
                                        std::string& error_message) {
    cv::Mat projector_to_camera(3, 3, CV_64F);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            projector_to_camera.at<double>(row, col) =
                calibration.homography_projector_to_camera[static_cast<size_t>(row * 3 + col)];
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

void resetClippedMediaState(PaletteClippedMediaState& state) {
    const int64_t last_presented = state.last_presented_parent_frame;
    const int64_t pending_switch = state.pending_switch_parent_frame;
    const bool switch_in_progress = state.switch_in_progress;
    state = PaletteClippedMediaState();
    state.last_presented_parent_frame = last_presented;
    state.pending_switch_parent_frame = pending_switch;
    state.switch_in_progress = switch_in_progress;
}

std::pair<int64_t, int64_t> parentFrameRangeForClip(
    const std::vector<int64_t>& parent_frame_by_clip_local) {
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

}  // namespace

MediaSessionLoader::MediaSessionLoader(const MediaSessionLoaderContext& context)
    : context_(context) {}

void MediaSessionLoader::stopCameraDecodersForReload() const {
    if (context_.decoder_context == nullptr ||
        context_.decoder_threads == nullptr ||
        context_.demuxers == nullptr ||
        context_.camera_names == nullptr ||
        context_.is_view_focused == nullptr ||
        context_.window_need_decoding == nullptr ||
        context_.window_was_decoding == nullptr ||
        context_.video_loaded == nullptr ||
        context_.input_is_imgs == nullptr) {
        return;
    }

    context_.decoder_context->stop_flag = true;
    for (auto& thread : *context_.decoder_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    context_.decoder_threads->clear();

    for (const auto& camera_name : *context_.camera_names) {
        (*context_.window_need_decoding)[camera_name].store(false);
        (*context_.window_was_decoding)[camera_name] = false;
        latest_decoded_frame[camera_name].store(-1);
    }

    context_.demuxers->clear();
    context_.camera_names->clear();
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

bool MediaSessionLoader::loadClippedVideoForParentFrame(int parent_frame) const {
    if (context_.zarr_loaded == nullptr || !*context_.zarr_loaded ||
        context_.zarr_loader == nullptr ||
        !context_.zarr_loader->hasClippedCollection()) {
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

    const auto* row = context_.zarr_loader->resolveClippedFrame(parent_frame);
    if (row == nullptr) {
        std::cout << "[Zarr] No clipped frame-run mapping for parent frame "
                  << parent_frame << std::endl;
        return false;
    }

    const auto* selected =
        context_.zarr_loader->getClippedResolver().selectedRun(
            row->selected_run_index);
    if (selected == nullptr || selected->video_path.empty()) {
        std::cout << "[Zarr] Clipped mapping has no source video for parent frame "
                  << parent_frame << std::endl;
        return false;
    }

    auto resolved_video_opt = ResolveAffiliatedVideoPath(
        selected->video_path,
        context_.zarr_loader->getArchivePath());
    if (!resolved_video_opt.has_value()) {
        std::cout << "[Zarr] Could not resolve clipped source video path: "
                  << selected->video_path << std::endl;
        return false;
    }

    const std::string resolved_video = resolved_video_opt->string();
    PaletteClippedMediaState* clipped_state = context_.clipped_media_state;
    const bool requested_clip_loaded =
        *context_.video_loaded && clipped_state != nullptr &&
        clipped_state->selected_run_index == row->selected_run_index &&
        parent_frame >= clipped_state->first_parent_frame &&
        parent_frame <= clipped_state->last_parent_frame &&
        !context_.decoder_threads->empty();
    if (requested_clip_loaded) {
        if (context_.scene->num_cams > 0 &&
            !context_.scene->cameras.empty()) {
            std::lock_guard<std::mutex> lock(g_seek_info_mutex);
            context_.scene->cameras[0].seek_context.frame_number_map =
                clipped_state->parent_frame_by_clip_local;
        }
        return true;
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

        std::string camera_name = "Cam" + selected->camera_serial + "_" +
                                  selected->clip_id;
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
            render_allocate_scene_memory(context_.scene,
                                         *context_.label_buffer_size);
        } else {
            for (u32 i = 0; i < context_.scene->size_of_buffer; ++i) {
                context_.scene->cameras[0].display_buffer[i].available_to_write =
                    true;
                context_.scene->cameras[0].display_buffer[i].frame_number = -1;
                context_.scene->cameras[0].display_buffer[i].local_frame_number =
                    -1;
                context_.scene->cameras[0].display_buffer[i].frame_pts = -1;
                context_.scene->cameras[0].display_buffer[i].frame_source_code =
                    0;
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

        auto frame_map = std::make_shared<const std::vector<int64_t>>(
            selected->parent_frame_by_clip_local);
        const auto [first_parent_frame, last_parent_frame] =
            parentFrameRangeForClip(*frame_map);
        {
            std::lock_guard<std::mutex> lock(g_seek_info_mutex);
            context_.scene->cameras[0].seek_context.frame_number_map = frame_map;
        }
        if (clipped_state != nullptr) {
            clipped_state->current_video_path = resolved_video;
            clipped_state->clip_id = selected->clip_id;
            clipped_state->camera_serial = selected->camera_serial;
            clipped_state->parent_frame_by_clip_local = frame_map;
            clipped_state->selected_run_index = row->selected_run_index;
            clipped_state->first_parent_frame = first_parent_frame;
            clipped_state->last_parent_frame = last_parent_frame;
            if (clipped_state->pending_switch_parent_frame != parent_frame) {
                clipped_state->pending_switch_parent_frame = -1;
                clipped_state->switch_in_progress = false;
            }
        }

        const size_t total_parent_frames =
            context_.zarr_loader->getTotalFrames();
        context_.decoder_context->total_num_frame =
            static_cast<int>(std::min<size_t>(
                total_parent_frames,
                static_cast<size_t>(std::numeric_limits<int>::max())));
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

        std::filesystem::path inferred_root = InferRecordingRootPath(
            *resolved_video_opt, context_.zarr_loader->getArchivePath());
        if (!inferred_root.empty()) {
            *context_.root_dir = inferred_root.string();
            *context_.skeleton_dir = *context_.root_dir;
        }

        std::cout << "[Zarr] Loaded clipped video for parent frame "
                  << parent_frame << " -> " << selected->clip_id
                  << " local frame " << row->clip_local_frame_index
                  << ": " << resolved_video << std::endl;
        if (clipped_state != nullptr && clipped_state->switch_in_progress) {
            std::cout << "[ClippedHandoff] switch_loaded parent_frame="
                      << parent_frame << " clip=" << selected->clip_id
                      << " selected_run_index=" << row->selected_run_index
                      << " local_frame=" << row->clip_local_frame_index
                      << " range=" << first_parent_frame << "-"
                      << last_parent_frame << std::endl;
        }
        loadCameraCalibrationsForCurrentMedia();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Zarr] Failed to load clipped source video: "
                  << e.what() << std::endl;
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
        const std::string& camera_name = (*context_.camera_names)[i];
        std::cout << "\nProcessing camera " << i << ": "
                  << camera_name << std::endl;

        bool loaded_from_zarr = false;
        if (context_.zarr_loader != nullptr && context_.zarr_loaded != nullptr &&
            *context_.zarr_loaded &&
            !context_.zarr_loader->getArchivePath().empty()) {
            std::string zarr_status;
            auto calibration =
                context_.zarr_loader->loadCalibrationForCamera(camera_name,
                                                               zarr_status);
            if (calibration.has_value()) {
                std::string apply_error;
                if (applyZarrCalibrationToCameraParams(
                        *calibration, (*context_.camera_params)[i],
                        apply_error)) {
                    std::cout << "Loading homography from Zarr for camera: "
                              << camera_name << std::endl;
                    std::cout << "  " << zarr_status << std::endl;
                    std::cout << "  Zarr homography semantics: projector/texture px -> camera px"
                              << std::endl;
                    std::cout << "  Applied to legacy CameraParams: inverse_homography_matrix"
                              << " holds projector/texture px -> camera px" << std::endl;
                    std::cout << "  Stimulus texture offset: ("
                              << (*context_.camera_params)[i].stimulus_offset_x
                              << ", "
                              << (*context_.camera_params)[i].stimulus_offset_y
                              << ")" << std::endl;
                    camera_print_calibration_details(
                        (*context_.camera_params)[i], camera_name);
                    loaded_from_zarr = true;
                } else {
                    std::cerr
                        << "Warning: Failed to apply Zarr calibration for camera "
                        << camera_name << ": " << apply_error << std::endl;
                }
            } else if (!zarr_status.empty()) {
                std::cout << "  " << zarr_status << std::endl;
            }
        }

        if (loaded_from_zarr) {
            continue;
        }

        std::string yaml_file = *context_.root_dir + "/calibration/" +
                                camera_name + ".yaml";
        if (std::filesystem::exists(yaml_file)) {
            std::cout << "Loading homography from YAML for camera: "
                      << camera_name << std::endl;
            if (!camera_load_params_from_yaml(
                    yaml_file, (*context_.camera_params)[i],
                    *context_.error_message)) {
                std::cerr << "Error: Failed to load calibration from YAML: "
                          << *context_.error_message << std::endl;
                *context_.show_error = true;
                break;
            }
            camera_print_calibration_details((*context_.camera_params)[i],
                                             camera_name);
        } else {
            std::cerr << "Warning: No calibration YAML file found at: "
                      << yaml_file << std::endl;
        }
    }
}

bool MediaSessionLoader::loadSingleVideoMedia(
    const std::filesystem::path& video_path,
    bool infer_recording_root,
    const char* success_label) const {
    if (context_.scene == nullptr || context_.decoder_context == nullptr ||
        context_.zarr_loader == nullptr || context_.stimulus_player == nullptr ||
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

    try {
        *context_.input_is_imgs = false;
        context_.camera_names->clear();
        context_.demuxers->clear();
        context_.is_view_focused->clear();

        std::string camera_name = video_path.stem().string();
        if (camera_name.empty()) {
            camera_name = video_path.filename().string();
        }

        context_.camera_names->push_back(camera_name);
        (*context_.window_need_decoding)[camera_name].store(true);
        (*context_.window_was_decoding)[camera_name] = true;

        std::map<std::string, std::string> ffmpeg_options;
        context_.demuxers->push_back(std::make_unique<FFmpegDemuxer>(
            video_path.string().c_str(), ffmpeg_options));

        context_.decoder_context->seek_interval =
            static_cast<int>(context_.demuxers->at(0)->FindKeyFrameInterval());
        *context_.video_fps = context_.demuxers->at(0)->GetFramerate();
        context_.scene->num_cams = 1;
        context_.scene->cameras.resize(context_.scene->num_cams);
        context_.scene->cameras[0].image_width =
            context_.demuxers->at(0)->GetWidth();
        context_.scene->cameras[0].image_height =
            context_.demuxers->at(0)->GetHeight();
        render_allocate_scene_memory(context_.scene, *context_.label_buffer_size);

        context_.decoder_threads->push_back(std::thread(
            &decoder_process, context_.decoder_context,
            context_.demuxers->at(0).get(), (*context_.camera_names)[0],
            context_.scene->cameras[0].display_buffer,
            context_.scene->size_of_buffer,
            &context_.scene->cameras[0].seek_context,
            context_.scene->use_cpu_buffer));
        context_.is_view_focused->push_back(false);
        *context_.video_loaded = true;

        const int initial_frame =
            std::max(0, context_.playback_state->to_display_frame_number);
        const double seek_fps = (*context_.video_fps > 0.0)
                                    ? *context_.video_fps
                                    : 30.0;
        seek_all_cameras(context_.scene, initial_frame, seek_fps,
                         *context_.playback_state, true, context_.zarr_loader,
                         context_.stimulus_player);

        if (infer_recording_root) {
            std::filesystem::path inferred_root = InferRecordingRootPath(
                video_path, context_.zarr_loader->getArchivePath());
            if (!inferred_root.empty()) {
                *context_.root_dir = inferred_root.string();
                *context_.skeleton_dir = *context_.root_dir;
            }
        }

        std::cout << success_label << video_path.string() << std::endl;
        if (context_.clipped_media_state != nullptr) {
            context_.clipped_media_state->current_video_path.clear();
            context_.clipped_media_state->parent_frame_by_clip_local.reset();
        }
        loadCameraCalibrationsForCurrentMedia();
        return true;
    } catch (const std::exception& e) {
        std::cerr << success_label << " failed: " << e.what() << std::endl;
        return false;
    }
}

void MediaSessionLoader::tryAutoLoadAffiliatedVideoFromZarr(
    const char* trigger_label) const {
    if (context_.zarr_loaded == nullptr || !*context_.zarr_loaded ||
        context_.video_loaded == nullptr || context_.decoder_threads == nullptr ||
        context_.zarr_loader == nullptr) {
        return;
    }

    if (context_.zarr_loader->hasClippedCollection()) {
        const int parent_frame =
            context_.playback_state != nullptr
                ? std::max(0, context_.playback_state->to_display_frame_number)
                : 0;
        if (loadClippedVideoForParentFrame(parent_frame)) {
            std::cout << "[Zarr] Auto-loaded clipped collection media ("
                      << trigger_label << ")" << std::endl;
        }
        return;
    }

    if (*context_.video_loaded || !context_.decoder_threads->empty()) {
        std::cout << "[Zarr] Skipping affiliated video auto-load ("
                  << trigger_label << "): media already loaded" << std::endl;
        return;
    }

    const std::string source_hint = context_.zarr_loader->getSourceVideoPath();
    if (source_hint.empty()) {
        std::cout << "[Zarr] Archive did not provide source video metadata; "
                     "skipping affiliated video auto-load"
                  << std::endl;
        return;
    }

    auto resolved_video_opt = ResolveAffiliatedVideoPath(
        source_hint, context_.zarr_loader->getArchivePath());
    if (!resolved_video_opt.has_value()) {
        std::cout
            << "[Zarr] Could not resolve affiliated source video path from metadata: "
            << source_hint << std::endl;
        return;
    }

    if (!loadSingleVideoMedia(*resolved_video_opt, true,
                              (std::string("[Zarr] Auto-loaded affiliated video (") +
                               trigger_label + "): ")
                                  .c_str())) {
        std::cerr << "[Zarr] Failed to auto-load affiliated video ("
                  << trigger_label << ")" << std::endl;
    }
}

void MediaSessionLoader::tryAutoLoadStimulusVideo(
    const char* trigger_label) const {
    if (context_.zarr_loaded == nullptr || !*context_.zarr_loaded ||
        context_.stimulus_player == nullptr || context_.zarr_loader == nullptr ||
        context_.root_dir == nullptr || context_.stimulus_buffer_size == nullptr ||
        context_.stimulus_use_cpu_buffer == nullptr ||
        context_.stimulus_use_software_decode == nullptr ||
        context_.window_was_decoding == nullptr ||
        context_.window_need_decoding == nullptr || context_.video_loaded == nullptr ||
        context_.playback_state == nullptr) {
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
            *context_.stimulus_use_software_decode,
            context_.cuda_device_index)) {
        std::cerr << "[Stimulus] Failed to auto-load stimulus video: "
                  << resolved->string() << std::endl;
        return;
    }

    (*context_.window_was_decoding)[context_.stimulus_player->window_name] =
        false;
    (*context_.window_need_decoding)[context_.stimulus_player->window_name]
        .store(false);

    if (*context_.video_loaded) {
        scheduleStimulusSeek(*context_.stimulus_player, context_.zarr_loader,
                             context_.playback_state->to_display_frame_number,
                             !context_.playback_state->play_video);
    }

    std::cout << "[Stimulus] Auto-loaded stimulus video (" << trigger_label
              << "): " << resolved->string() << std::endl;
}

void MediaSessionLoader::bootstrapFromCli(
    const std::string& cli_zarr_override_path,
    const std::string& cli_recording_path,
    const std::function<void()>& refresh_detection_dataset_options,
    const std::function<void()>& clear_bbox_edits) const {
    if (context_.zarr_loaded == nullptr || context_.root_dir == nullptr ||
        context_.skeleton_dir == nullptr || context_.zarr_loader == nullptr) {
        return;
    }

    if (!cli_zarr_override_path.empty()) {
        std::string zarr_error;
        if (loadZarrDetectionFromPath(cli_zarr_override_path, *context_.zarr_loader,
                                      zarr_error)) {
            *context_.zarr_loaded = true;
            refresh_detection_dataset_options();
            clear_bbox_edits();
            std::cout << "Loaded Zarr archive from --zarr: "
                      << context_.zarr_loader->getArchivePath() << std::endl;
            tryAutoLoadAffiliatedVideoFromZarr("--zarr");
            tryAutoLoadStimulusVideo("--zarr");
        } else {
            clear_bbox_edits();
            std::cerr << "Failed to load --zarr archive: " << zarr_error
                      << std::endl;
        }
    }

    if (cli_recording_path.empty()) {
        return;
    }

    *context_.root_dir = cli_recording_path;
    *context_.skeleton_dir = *context_.root_dir;

    std::string zarr_error;
    if (loadZarrDetectionFromDirectory(*context_.root_dir, *context_.zarr_loader,
                                       zarr_error)) {
        *context_.zarr_loaded = true;
        refresh_detection_dataset_options();
        clear_bbox_edits();
        std::cout << "Loaded Zarr archive from --recording: "
                  << context_.zarr_loader->getArchivePath() << std::endl;
        tryAutoLoadAffiliatedVideoFromZarr("--recording");
        tryAutoLoadStimulusVideo("--recording");
        return;
    }

    clear_bbox_edits();
    std::cout << "[--recording] No zarr archive found (optional): "
              << zarr_error << std::endl;

    namespace fs = std::filesystem;
    std::string found_video;
    std::vector<fs::path> search_dirs;
    const fs::path cams_dir = fs::path(*context_.root_dir) / "cams";
    if (IsDirectoryNoThrow(cams_dir)) {
        search_dirs.push_back(cams_dir);
    }
    search_dirs.push_back(fs::path(*context_.root_dir));

    for (const auto& search_dir : search_dirs) {
        if (!found_video.empty()) {
            break;
        }
        std::error_code ec;
        for (auto it = fs::directory_iterator(search_dir, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && !ec &&
                IsSupportedVideoPath(it->path())) {
                found_video = it->path().string();
                break;
            }
        }
    }

    if (found_video.empty()) {
        std::cout << "[--recording] No video files found in "
                  << *context_.root_dir << std::endl;
        return;
    }

    if (loadSingleVideoMedia(found_video, false,
                             "[--recording] Auto-loaded video: ")) {
        tryAutoLoadStimulusVideo("--recording-fallback");
    } else {
        std::cerr << "[--recording] Failed to load video" << std::endl;
    }
}
