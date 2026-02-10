#include "stimulus_playback.h"

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
        stim.display_buffer[i].available_to_write = true;
        if (stim.use_cpu_buffer) {
            stim.display_buffer[i].frame =
                static_cast<unsigned char *>(malloc(frame_bytes));
            if (!stim.display_buffer[i].frame) {
                for (int j = 0; j < i; ++j) {
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
                                int cuda_device_index) {
    destroyStimulusPlayback(stim);

    stim.video_path = video_path;
    stim.buffer_size = buffer_size;
    stim.use_cpu_buffer = false;

    std::map<std::string, std::string> ffmpeg_options;
    try {
        stim.demuxer = std::make_unique<FFmpegDemuxer>(video_path.c_str(), ffmpeg_options);
    } catch (const std::exception &e) {
        std::cout << "Failed to open stimulus video: " << e.what() << std::endl;
        stim.demuxer.reset();  // Clear to nullptr (already null, but explicit)
        return false;
    }

    stim.width = stim.demuxer->GetWidth();
    stim.height = stim.demuxer->GetHeight();
    if (stim.width == 0 || stim.height == 0) {
        std::cout << "Stimulus video reports zero dimension; aborting load." << std::endl;
        destroyStimulusPlayback(stim);
        return false;
    }
    stim.fps = stim.demuxer->GetFramerate();
    if (stim.fps <= 0.0) {
        stim.fps = stim.demuxer->GetAvgFramerate();
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

    stim.decoder_context = std::make_unique<DecoderContext>(DecoderContext{
        .decoding_flag = false,
        .stop_flag = false,
        .total_num_frame = int(INT_MAX),
        .estimated_num_frames = 0,
        .gpu_index = cuda_device_index,
        .seek_interval = 250});

    stim.seek.use_seek = false;
    stim.seek.seek_done = false;
    stim.seek.seek_frame = 0;
    stim.seek.seek_accurate = false;

    window_need_decoding[stim.window_name].store(false);
    latest_decoded_frame[stim.window_name].store(-1);

    stim.decoder_thread = std::thread(&decoder_process, stim.decoder_context.get(),
                                      stim.demuxer.get(), stim.window_name,
                                      stim.display_buffer, stim.buffer_size,
                                      &stim.seek, stim.use_cpu_buffer);
    stim.loaded = true;
    stim.last_displayed_frame = -1;
    stim.throttled = false;
    stim.throttle_resume_frame = -1;
    std::cout << "[Stimulus] decoder initialized: " << video_path
              << " size=" << stim.width << "x" << stim.height
              << " fps=" << stim.fps << " buffer=" << stim.buffer_size
              << std::endl;
    return true;
}

int findStimulusBuffer(const StimulusPlayback &stim,
                       int target_frame) {
    if (!stim.display_buffer) {
        return -1;
    }
    int exact_index = -1;
    int best_lower_index = -1;
    int best_lower_value = std::numeric_limits<int>::min();
    for (int i = 0; i < stim.buffer_size; ++i) {
        const PictureBuffer &buf = stim.display_buffer[i];
        if (buf.available_to_write || buf.frame_number < 0) {
            continue;
        }
        if (buf.frame_number == target_frame) {
            exact_index = i;
            break;
        }
        if (buf.frame_number < target_frame && buf.frame_number > best_lower_value) {
            best_lower_value = buf.frame_number;
            best_lower_index = i;
        }
    }
    if (exact_index != -1) {
        return exact_index;
    }
    if (best_lower_index != -1) {
        return best_lower_index;
    }
    return -1;
}

void releaseStimulusBufferSlot(StimulusPlayback &stim, int index) {
    if (!stim.display_buffer || index < 0 || index >= stim.buffer_size) {
        return;
    }
    stim.display_buffer[index].available_to_write = true;
    stim.display_buffer[index].frame_number = -1;
}

void uploadStimulusFrameToTexture(StimulusPlayback &stim, int buffer_index) {
    if (!stim.display_buffer || buffer_index < 0 ||
        buffer_index >= stim.buffer_size || !stim.resources_initialized) {
        return;
    }

    PictureBuffer &buffer = stim.display_buffer[buffer_index];
    if (buffer.available_to_write || !buffer.frame) {
        return;
    }

    size_t frame_bytes =
        static_cast<size_t>(stim.width) * static_cast<size_t>(stim.height) * 4;
    cudaMemcpyKind kind =
        stim.use_cpu_buffer ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToDevice;
    checkCudaStatus(cudaMemcpy(stim.pbo.cuda_buffer, buffer.frame, frame_bytes, kind),
                    "Stimulus cudaMemcpy failed");

    bind_pbo(&stim.pbo.pbo);
    bind_texture(&stim.texture);
    upload_image_pbo_to_texture(stim.width, stim.height);
    unbind_pbo();
    unbind_texture();

    releaseStimulusBufferSlot(stim, buffer_index);
}

void discardStimulusFramesOlderThan(StimulusPlayback &stim, int keep_threshold) {
    if (!stim.display_buffer) {
        return;
    }
    int released = 0;
    for (int i = 0; i < stim.buffer_size; ++i) {
        auto &buf = stim.display_buffer[i];
        if (!buf.available_to_write && buf.frame_number >= 0 &&
            buf.frame_number < keep_threshold) {
            buf.available_to_write = true;
            buf.frame_number = -1;
            ++released;
        }
    }
    if (released > 0) {
        std::cout << "[Stimulus] discarded " << released
                  << " frames older than " << keep_threshold << std::endl;
    }
}

int getOldestStimulusFrame(const StimulusPlayback &stim) {
    if (!stim.display_buffer) {
        return std::numeric_limits<int>::max();
    }
    int oldest = std::numeric_limits<int>::max();
    for (int i = 0; i < stim.buffer_size; ++i) {
        const auto &buf = stim.display_buffer[i];
        if (!buf.available_to_write && buf.frame_number >= 0) {
            oldest = std::min(oldest, buf.frame_number);
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
        const auto &buf = stim.display_buffer[i];
        if (!buf.available_to_write && buf.frame_number >= 0) {
            newest = std::max(newest, buf.frame_number);
        }
    }
    return newest;
}

void scheduleStimulusSeek(StimulusPlayback &stim,
                          ZarrDetectionLoader *loader,
                          int camera_frame,
                          bool wait_for_completion) {
    if (!stim.loaded || !loader || !loader->hasStimulusAlignment()) {
        return;
    }
    auto stim_frame = loader->getStimulusFrameForCameraFrame(camera_frame);
    if (!stim_frame || *stim_frame < 0) {
        return;
    }

    std::cout << "[Stimulus] schedule seek: camera_frame=" << camera_frame
              << " -> stimulus_frame=" << *stim_frame
              << " wait=" << (wait_for_completion ? "true" : "false")
              << std::endl;

    stim.seek.seek_frame = static_cast<uint64_t>(*stim_frame);
    stim.seek.use_seek = true;
    stim.seek.seek_done = false;
    stim.seek.seek_accurate = wait_for_completion;
    stim.last_displayed_frame = -1;
    window_need_decoding[stim.window_name].store(true);

    if (wait_for_completion && stim.decoder_context) {
        while (!stim.seek.seek_done && !stim.decoder_context->stop_flag) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stim.seek.seek_done = false;
        std::cout << "[Stimulus] seek complete for camera_frame=" << camera_frame
                  << std::endl;
    }
}

void seek_all_cameras(render_scene *scene, int frame_number, double video_fps,
                      PlaybackState &state, bool seek_accurate,
                      ZarrDetectionLoader *zarr_loader,
                      StimulusPlayback *stimulus) {
    // Trigger seek request
    for (int i = 0; i < scene->num_cams; i++) {
        scene->cameras[i].seek_context.seek_frame = (uint64_t)frame_number;
        scene->cameras[i].seek_context.use_seek = true;
        scene->cameras[i].seek_context.seek_accurate = seek_accurate;
    }

    // Wait for seek to complete
    for (int i = 0; i < scene->num_cams; i++) {
        const auto seek_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!scene->cameras[i].seek_context.seek_done) {
            if (std::chrono::steady_clock::now() >= seek_deadline) {
                std::cerr << "[Seek] Timeout waiting for camera index " << i
                          << " to complete seek to frame " << frame_number
                          << std::endl;
                scene->cameras[i].seek_context.use_seek = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    // Reset seek_done flags
    for (int i = 0; i < scene->num_cams; i++) {
        scene->cameras[i].seek_context.seek_done = false;
    }

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
        scheduleStimulusSeek(*stimulus, zarr_loader, frame_number, seek_accurate);
    }
}
