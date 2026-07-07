#include "stimulus_playback.h"
#include "debug_flags.h"
#include "frame_slot.h"
#include <atomic>
#include <algorithm>
#include <optional>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace {
uint64_t nextSeekGeneration() {
    static std::atomic<uint64_t> g_seek_generation{1};
    return g_seek_generation.fetch_add(1, std::memory_order_relaxed);
}

bool markStimulusSeekDone(SeekInfo *seek_info, uint64_t seek_id,
                          uint64_t settled_frame) {
    std::lock_guard<std::mutex> lock(g_seek_info_mutex);
    if (seek_info->use_seek) {
        return false;
    }
    seek_info->seek_frame = settled_frame;
    seek_info->settled_seek_id = seek_id;
    seek_info->seek_done = true;
    return true;
}

void stimulus_software_decode_process(DecoderContext *dc_context,
                                      const std::string &video_path,
                                      std::string window_name,
                                      PictureBuffer *display_buffer,
                                      int size_of_buffer, SeekInfo *seek_info,
                                      int width, int height,
                                      bool use_cpu_buffer) {
    cv::VideoCapture capture(video_path, cv::CAP_FFMPEG);
    if (!capture.isOpened()) {
        std::cerr << "[Stimulus] Failed to open software decoder for "
                  << video_path << std::endl;
        return;
    }

    const size_t frame_bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    const int reported_frames =
        static_cast<int>(capture.get(cv::CAP_PROP_FRAME_COUNT));
    if (reported_frames > 0) {
        dc_context->total_num_frame = reported_frames;
        dc_context->estimated_num_frames = reported_frames;
    }

    int buffer_head = 0;
    int frame_number = 0;
    bool pending_seek_done = false;
    uint64_t pending_seek_id = 0;

    auto seek_requested = [&]() -> bool {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        return seek_info->use_seek;
    };

    while (!(dc_context->stop_flag)) {
        bool has_seek_request = false;
        uint64_t requested_frame = 0;
        uint64_t active_seek_id = 0;
        {
            std::lock_guard<std::mutex> lock(g_seek_info_mutex);
            if (seek_info->use_seek) {
                has_seek_request = true;
                requested_frame = seek_info->seek_frame;
                active_seek_id = seek_info->seek_id;
                seek_info->use_seek = false;
                seek_info->seek_done = false;
            }
        }

        if (has_seek_request) {
            for (int i = 0; i < size_of_buffer; ++i) {
                frameSlotReleaseForReuse(display_buffer[i]);
            }
            buffer_head = 0;
            frame_number = static_cast<int>(requested_frame);
            latest_decoded_frame[window_name].store(-1);

            const bool seek_ok =
                capture.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(requested_frame));
            if (!seek_ok) {
                (void)markStimulusSeekDone(seek_info, active_seek_id, requested_frame);
                pending_seek_done = false;
                continue;
            }

            pending_seek_done = true;
            pending_seek_id = active_seek_id;
            continue;
        }

        if (!window_need_decoding[window_name].load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        std::optional<FrameSlotWriteLease> write_lease;
        while (!(dc_context->stop_flag) && !seek_requested()) {
            write_lease = frameSlotAcquireWritable(display_buffer[buffer_head]);
            if (write_lease.has_value()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!write_lease.has_value() || dc_context->stop_flag ||
            seek_requested()) {
            continue;
        }

        cv::Mat frame_bgr;
        if (!capture.read(frame_bgr) || frame_bgr.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        cv::Mat frame_rgba;
        switch (frame_bgr.channels()) {
        case 4:
            cv::cvtColor(frame_bgr, frame_rgba, cv::COLOR_BGRA2RGBA);
            break;
        case 3:
            cv::cvtColor(frame_bgr, frame_rgba, cv::COLOR_BGR2RGBA);
            break;
        case 1:
            cv::cvtColor(frame_bgr, frame_rgba, cv::COLOR_GRAY2RGBA);
            break;
        default:
            std::cerr << "[Stimulus] Unsupported channel count in software decode: "
                      << frame_bgr.channels() << std::endl;
            continue;
        }

        if (frame_rgba.cols != width || frame_rgba.rows != height) {
            cv::resize(frame_rgba, frame_rgba, cv::Size(width, height),
                       0.0, 0.0, cv::INTER_LINEAR);
        }

        if (use_cpu_buffer) {
            std::memcpy(write_lease->frame(), frame_rgba.data, frame_bytes);
        } else {
            checkCudaStatus(
                cudaMemcpy(write_lease->frame(), frame_rgba.data,
                           frame_bytes, cudaMemcpyHostToDevice),
                "Stimulus software decode cudaMemcpy failed");
        }

        FrameSlotMetadata metadata;
        metadata.frame_number = frame_number;
        metadata.local_frame_number = frame_number;
        metadata.frame_pts = -1;
        metadata.frame_source_code = pending_seek_done ? 1 : 2;
        metadata.pitch_bytes = width * 4;
        metadata.frame_bytes = frame_bytes;
        metadata.color_matrix = ColorSpaceStandard_BT709;
        metadata.color_range = ColorRange_Unspecified;
        metadata.format = PictureBufferFormat::RGBA32;
        write_lease->publish(metadata);
        latest_decoded_frame[window_name].store(frame_number);
        dc_context->decoding_flag = true;

        if (pending_seek_done) {
            (void)markStimulusSeekDone(seek_info, pending_seek_id,
                                       static_cast<uint64_t>(frame_number));
            pending_seek_done = false;
        }

        ++frame_number;
        buffer_head = (buffer_head + 1) % size_of_buffer;
    }
}
}  // namespace

void destroyStimulusPlayback(StimulusPlayback &stim) {
    if (stim.decoder_context) {
        stim.decoder_context->stop_flag = true;
    }
    if (stim.decoder_thread.joinable()) {
        stim.decoder_thread.join();
    }
    // unique_ptr automatically deletes when reset() is called
    stim.decoder_context.reset();
    stim.demuxer.reset();
    if (stim.display_buffer) {
        for (int i = 0; i < stim.buffer_size; ++i) {
            frameSlotDestroy(stim.display_buffer[i]);
            if (stim.display_buffer[i].frame) {
                if (stim.use_cpu_buffer) {
                    free(stim.display_buffer[i].frame);
                } else {
                    cudaFree(stim.display_buffer[i].frame);
                }
                stim.display_buffer[i].frame = nullptr;
            }
        }
        free(stim.display_buffer);
        stim.display_buffer = nullptr;
    }
    if (stim.resources_initialized && stim.pbo.cuda_resource) {
        unmap_cuda_resource(&stim.pbo.cuda_resource);
        cudaGraphicsUnregisterResource(stim.pbo.cuda_resource);
        glDeleteBuffers(1, &stim.pbo.pbo);
    }
    if (stim.texture != 0) {
        glDeleteTextures(1, &stim.texture);
    }
    stim.pbo = {};
    stim.texture = 0;
    stim.resources_initialized = false;
    stim.loaded = false;
    stim.width = stim.height = 0;
    stim.fps = 0.0;
    stim.video_path.clear();
    stim.last_displayed_frame = -1;
    stim.throttled = false;
    stim.throttle_resume_frame = -1;
    stim.playback_catchup_seek_in_flight = false;
    stim.playback_catchup_seek_id = 0;
    stim.playback_catchup_target_frame = -1;
    stim.playback_catchup_last_request = std::chrono::steady_clock::time_point{};
    auto need_it = window_need_decoding.find(stim.window_name);
    if (need_it != window_need_decoding.end()) {
        need_it->second.store(false);
    }
    auto latest_it = latest_decoded_frame.find(stim.window_name);
    if (latest_it != latest_decoded_frame.end()) {
        latest_it->second.store(-1);
    }
}

bool allocateStimulusBuffers(StimulusPlayback &stim) {
    size_t frame_bytes = static_cast<size_t>(stim.width) * static_cast<size_t>(stim.height) * 4;
    stim.display_buffer =
        static_cast<PictureBuffer *>(malloc(sizeof(PictureBuffer) * stim.buffer_size));
    if (!stim.display_buffer) {
        return false;
    }
    for (int i = 0; i < stim.buffer_size; ++i) {
        stim.display_buffer[i].frame = nullptr;
        stim.display_buffer[i].frame_number = -1;
        stim.display_buffer[i].local_frame_number = -1;
        stim.display_buffer[i].frame_pts = -1;
        stim.display_buffer[i].frame_source_code = 0;
        stim.display_buffer[i].available_to_write = true;
        stim.display_buffer[i].pitch_bytes = stim.width * 4;
        stim.display_buffer[i].frame_bytes = frame_bytes;
        stim.display_buffer[i].color_matrix = ColorSpaceStandard_BT709;
        stim.display_buffer[i].color_range = ColorRange_Unspecified;
        stim.display_buffer[i].format = PictureBufferFormat::RGBA32;
        stim.display_buffer[i].frame_slot_state = nullptr;
        frameSlotInitialize(stim.display_buffer[i]);
        if (stim.use_cpu_buffer) {
            stim.display_buffer[i].frame =
                static_cast<unsigned char *>(malloc(frame_bytes));
            if (!stim.display_buffer[i].frame) {
                for (int j = 0; j <= i; ++j) {
                    frameSlotDestroy(stim.display_buffer[j]);
                    if (stim.display_buffer[j].frame) {
                        free(stim.display_buffer[j].frame);
                        stim.display_buffer[j].frame = nullptr;
                    }
                }
                free(stim.display_buffer);
                stim.display_buffer = nullptr;
                return false;
            }
            decoder_clear_buffer_with_constant_image(stim.display_buffer[i].frame,
                                                     stim.width, stim.height);
        } else {
            cudaError_t err =
                cudaMalloc(reinterpret_cast<void **>(&stim.display_buffer[i].frame),
                           frame_bytes);
            if (err != cudaSuccess) {
                for (int j = 0; j <= i; ++j) {
                    frameSlotDestroy(stim.display_buffer[j]);
                    if (stim.display_buffer[j].frame) {
                        cudaFree(stim.display_buffer[j].frame);
                        stim.display_buffer[j].frame = nullptr;
                    }
                }
                free(stim.display_buffer);
                stim.display_buffer = nullptr;
                return false;
            }
        }
    }
    return true;
}

bool initializeStimulusPlayback(StimulusPlayback &stim,
                                const std::string &video_path,
                                int buffer_size,
                                bool use_cpu_buffer,
                                bool use_software_decode,
                                int cuda_device_index) {
    destroyStimulusPlayback(stim);

    stim.video_path = video_path;
    stim.buffer_size = std::max(1, buffer_size);
    stim.use_cpu_buffer = use_cpu_buffer;
    stim.use_software_decode = use_software_decode;

    if (stim.use_software_decode) {
        cv::VideoCapture probe(video_path, cv::CAP_FFMPEG);
        if (!probe.isOpened()) {
            std::cout << "Failed to open stimulus video (software): "
                      << video_path << std::endl;
            return false;
        }
        stim.width = static_cast<uint32_t>(probe.get(cv::CAP_PROP_FRAME_WIDTH));
        stim.height = static_cast<uint32_t>(probe.get(cv::CAP_PROP_FRAME_HEIGHT));
        stim.fps = probe.get(cv::CAP_PROP_FPS);
        if (stim.width == 0 || stim.height == 0) {
            std::cout << "Stimulus video reports zero dimension; aborting load."
                      << std::endl;
            destroyStimulusPlayback(stim);
            return false;
        }
        if (stim.fps <= 0.0) {
            stim.fps = 30.0;
        }
    } else {
        std::map<std::string, std::string> ffmpeg_options;
        try {
            stim.demuxer =
                std::make_unique<FFmpegDemuxer>(video_path.c_str(), ffmpeg_options);
        } catch (const std::exception &e) {
            std::cout << "Failed to open stimulus video: " << e.what() << std::endl;
            stim.demuxer.reset();
            return false;
        }

        stim.width = stim.demuxer->GetWidth();
        stim.height = stim.demuxer->GetHeight();
        if (stim.width == 0 || stim.height == 0) {
            std::cout << "Stimulus video reports zero dimension; aborting load."
                      << std::endl;
            destroyStimulusPlayback(stim);
            return false;
        }
        stim.fps = stim.demuxer->GetFramerate();
        if (stim.fps <= 0.0) {
            stim.fps = stim.demuxer->GetAvgFramerate();
        }
    }

    if (!allocateStimulusBuffers(stim)) {
        std::cout << "Failed to allocate stimulus buffers." << std::endl;
        destroyStimulusPlayback(stim);
        return false;
    }

    create_pbo(&stim.pbo.pbo, stim.width, stim.height);
    register_pbo_to_cuda(&stim.pbo.pbo, &stim.pbo.cuda_resource);
    map_cuda_resource(&stim.pbo.cuda_resource);
    cuda_pointer_from_resource(&stim.pbo.cuda_buffer, &stim.pbo.cuda_pbo_storage_buffer_size,
                               &stim.pbo.cuda_resource);

    glGenTextures(1, &stim.texture);
    glBindTexture(GL_TEXTURE_2D, stim.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, stim.width, stim.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    stim.resources_initialized = true;

    auto decoder_context = DecoderContext{};
    decoder_context.decoding_flag = false;
    decoder_context.stop_flag = false;
    decoder_context.total_num_frame = int(INT_MAX);
    decoder_context.estimated_num_frames = 0;
    decoder_context.gpu_index = cuda_device_index;
    decoder_context.seek_interval = 250;
    stim.decoder_context = std::make_unique<DecoderContext>(decoder_context);

    stim.seek.use_seek = false;
    stim.seek.seek_done = false;
    stim.seek.seek_frame = 0;
    stim.seek.seek_accurate = false;
    stim.seek.seek_id = 0;
    stim.seek.settled_seek_id = 0;

    window_need_decoding[stim.window_name].store(false);
    latest_decoded_frame[stim.window_name].store(-1);

    if (stim.use_software_decode) {
        stim.decoder_thread = std::thread(
            &stimulus_software_decode_process, stim.decoder_context.get(),
            stim.video_path, stim.window_name, stim.display_buffer,
            stim.buffer_size, &stim.seek, static_cast<int>(stim.width),
            static_cast<int>(stim.height), stim.use_cpu_buffer);
    } else {
        stim.decoder_thread = std::thread(&decoder_process, stim.decoder_context.get(),
                                          stim.demuxer.get(), stim.window_name,
                                          stim.display_buffer, stim.buffer_size,
                                          &stim.seek, stim.use_cpu_buffer);
    }
    stim.loaded = true;
    stim.last_displayed_frame = -1;
    stim.throttled = false;
    stim.throttle_resume_frame = -1;
    stim.playback_catchup_seek_in_flight = false;
    stim.playback_catchup_seek_id = 0;
    stim.playback_catchup_target_frame = -1;
    stim.playback_catchup_last_request = std::chrono::steady_clock::time_point{};
    std::cout << "[Stimulus] decoder initialized: " << video_path
              << " size=" << stim.width << "x" << stim.height
              << " fps=" << stim.fps << " buffer=" << stim.buffer_size
              << " backend=" << (stim.use_software_decode ? "software" : "gpu")
              << " mode=" << (stim.use_cpu_buffer ? "cpu" : "gpu")
              << std::endl;
    return true;
}

int findStimulusBuffer(const StimulusPlayback &stim,
                       int target_frame) {
    if (!stim.display_buffer) {
        return -1;
    }
    int best_index = -1;
    int best_distance = std::numeric_limits<int>::max();
    int best_frame = std::numeric_limits<int>::min();
    for (int i = 0; i < stim.buffer_size; ++i) {
        auto metadata = frameSlotSnapshotReadable(stim.display_buffer[i]);
        if (!metadata) {
            continue;
        }
        if (metadata->frame_number == target_frame) {
            return i;
        }

        const int frame_num = metadata->frame_number;
        const int distance = (frame_num > target_frame)
                                 ? (frame_num - target_frame)
                                 : (target_frame - frame_num);
        bool choose_candidate = (best_index < 0) || (distance < best_distance);
        if (!choose_candidate && distance == best_distance) {
            const bool candidate_is_past_target = frame_num > target_frame;
            const bool best_is_past_target = best_frame > target_frame;
            if (candidate_is_past_target != best_is_past_target) {
                choose_candidate = !candidate_is_past_target;
            } else if (frame_num > best_frame) {
                choose_candidate = true;
            }
        }

        if (choose_candidate) {
            best_index = i;
            best_distance = distance;
            best_frame = frame_num;
        }
    }
    return best_index;
}

void releaseStimulusBufferSlot(StimulusPlayback &stim, int index) {
    if (!stim.display_buffer || index < 0 || index >= stim.buffer_size) {
        return;
    }
    frameSlotReleaseForReuse(stim.display_buffer[index]);
}

void uploadStimulusFrameToTexture(StimulusPlayback &stim, int buffer_index) {
    if (!stim.display_buffer || buffer_index < 0 ||
        buffer_index >= stim.buffer_size || !stim.resources_initialized) {
        return;
    }

    auto read_lease = frameSlotAcquireReadable(stim.display_buffer[buffer_index]);
    if (!read_lease.has_value() || !read_lease->frame()) {
        return;
    }

    size_t frame_bytes =
        static_cast<size_t>(stim.width) * static_cast<size_t>(stim.height) * 4;
    const size_t copy_bytes =
        read_lease->metadata().frame_bytes > 0
            ? std::min(read_lease->metadata().frame_bytes, frame_bytes)
            : frame_bytes;
    cudaMemcpyKind kind =
        stim.use_cpu_buffer ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToDevice;
    checkCudaStatus(cudaMemcpy(stim.pbo.cuda_buffer, read_lease->frame(),
                               copy_bytes, kind),
                    "Stimulus cudaMemcpy failed");

    bind_pbo(&stim.pbo.pbo);
    bind_texture(&stim.texture);
    upload_image_pbo_to_texture(stim.width, stim.height);
    unbind_pbo();
    unbind_texture();

    read_lease->release();
    releaseStimulusBufferSlot(stim, buffer_index);
}

void discardStimulusFramesOlderThan(StimulusPlayback &stim, int keep_threshold) {
    if (!stim.display_buffer) {
        return;
    }
    int released = 0;
    for (int i = 0; i < stim.buffer_size; ++i) {
        auto metadata = frameSlotSnapshotReadable(stim.display_buffer[i]);
        if (metadata && metadata->frame_number < keep_threshold) {
            frameSlotReleaseForReuse(stim.display_buffer[i]);
            ++released;
        }
    }
    if (released > 0) {
        if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus] discarded " << released
                  << " frames older than " << keep_threshold << std::endl;
    }
}

int getOldestStimulusFrame(const StimulusPlayback &stim) {
    if (!stim.display_buffer) {
        return std::numeric_limits<int>::max();
    }
    int oldest = std::numeric_limits<int>::max();
    for (int i = 0; i < stim.buffer_size; ++i) {
        auto metadata = frameSlotSnapshotReadable(stim.display_buffer[i]);
        if (metadata) {
            oldest = std::min(oldest, metadata->frame_number);
        }
    }
    return oldest;
}

int getNewestStimulusFrame(const StimulusPlayback &stim) {
    if (!stim.display_buffer) {
        return -1;
    }
    int newest = -1;
    for (int i = 0; i < stim.buffer_size; ++i) {
        auto metadata = frameSlotSnapshotReadable(stim.display_buffer[i]);
        if (metadata) {
            newest = std::max(newest, metadata->frame_number);
        }
    }
    return newest;
}

void scheduleStimulusSeek(StimulusPlayback &stim,
                          ZarrDetectionLoader *loader,
                          int camera_frame,
                          bool seek_accurate,
                          uint64_t seek_id) {
    if (!stim.loaded || !loader || !loader->hasStimulusAlignment()) {
        return;
    }
    auto stim_frame = loader->getStimulusFrameForCameraFrame(camera_frame);
    if (!stim_frame || *stim_frame < 0) {
        return;
    }

    if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus] schedule seek: camera_frame=" << camera_frame
              << " -> stimulus_frame=" << *stim_frame
              << " wait=false"
              << std::endl;

    const uint64_t request_seek_id =
        (seek_id != 0) ? seek_id : nextSeekGeneration();
    {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        stim.seek.seek_frame = static_cast<uint64_t>(*stim_frame);
        stim.seek.seek_id = request_seek_id;
        stim.seek.use_seek = true;
        stim.seek.seek_done = false;
        stim.seek.seek_accurate = seek_accurate;
    }
    stim.last_displayed_frame = -1;
    window_need_decoding[stim.window_name].store(true);
}

void seek_all_cameras(render_scene *scene, int frame_number, double video_fps,
                      PlaybackState &state, bool seek_accurate,
                      ZarrDetectionLoader *zarr_loader,
                      StimulusPlayback *stimulus) {
    const uint64_t seek_id = nextSeekGeneration();
    initiate_camera_seeks(scene, frame_number, seek_id, seek_accurate);

    // Update playback state
    state.to_display_frame_number = frame_number;
    state.read_head = 0;
    state.just_seeked = true;
    state.slider_frame_number = state.to_display_frame_number;

    state.accumulated_play_time = frame_number / video_fps;
    state.last_play_time_start = std::chrono::steady_clock::now();
    state.last_frame_num_playspeed = frame_number;
    state.last_wall_time_playspeed = std::chrono::steady_clock::now();

    if (stimulus && stimulus->loaded) {
        if (zarr_loader && zarr_loader->hasStimulusAlignment()) {
            auto stim_frame = zarr_loader->getStimulusFrameForCameraFrame(frame_number);
            if (stim_frame && *stim_frame >= 0) {
                state.current_stimulus_frame = *stim_frame;
            }
        }
        scheduleStimulusSeek(*stimulus, zarr_loader, frame_number, seek_accurate,
                             seek_id);
    }
}

void initiate_camera_seeks(render_scene *scene, int frame_number,
                           uint64_t seek_id, bool seek_accurate) {
    for (int i = 0; i < scene->num_cams; i++) {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        scene->cameras[i].seek_context.seek_frame = static_cast<uint64_t>(frame_number);
        scene->cameras[i].seek_context.seek_id = seek_id;
        scene->cameras[i].seek_context.use_seek = true;
        scene->cameras[i].seek_context.seek_done = false;
        scene->cameras[i].seek_context.seek_accurate = seek_accurate;
    }
}

int poll_camera_seeks(render_scene *scene, uint64_t seek_id) {
    int settled = 0;
    std::lock_guard<std::mutex> lock(g_seek_info_mutex);
    for (int i = 0; i < scene->num_cams; i++) {
        const auto &ctx = scene->cameras[i].seek_context;
        if (ctx.seek_done && ctx.settled_seek_id == seek_id) {
            ++settled;
        }
    }
    return settled;
}
