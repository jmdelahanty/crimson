#include "media_session_loader.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>

MediaSessionLoader::MediaSessionLoader(const MediaSessionLoaderContext& context)
    : context_(context) {}

void MediaSessionLoader::loadCameraCalibrationsForCurrentMedia() const {
    if (context_.video_loaded == nullptr || !*context_.video_loaded ||
        context_.scene == nullptr || context_.camera_names == nullptr ||
        context_.camera_params == nullptr || context_.root_dir == nullptr ||
        context_.error_message == nullptr || context_.show_error == nullptr) {
        return;
    }

    context_.camera_params->resize(context_.scene->num_cams);
    std::cout << "\n=== Loading Camera Calibrations from YAML ==="
              << std::endl;
    for (size_t i = 0; i < context_.camera_names->size(); ++i) {
        std::cout << "\nProcessing camera " << i << ": "
                  << (*context_.camera_names)[i] << std::endl;
        std::string yaml_file = *context_.root_dir + "/calibration/" +
                                (*context_.camera_names)[i] + ".yaml";
        if (std::filesystem::exists(yaml_file)) {
            std::cout << "Loading homography from YAML for camera: "
                      << (*context_.camera_names)[i] << std::endl;
            if (!camera_load_params_from_yaml(
                    yaml_file, (*context_.camera_params)[i],
                    *context_.error_message)) {
                std::cerr << "Error: Failed to load calibration from YAML: "
                          << *context_.error_message << std::endl;
                *context_.show_error = true;
                break;
            }
            camera_print_calibration_details((*context_.camera_params)[i],
                                             (*context_.camera_names)[i]);
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
