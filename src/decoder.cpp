#include "decoder.h"
#include "AppDecUtils.h"
#include "global.h"
#include "debug_flags.h"
#include "frame_slot.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

inline double decoder_duration_ms(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
               duration)
        .count();
}

void decoder_get_image_from_gpu(CUdeviceptr dpSrc, uint8_t *pDst, int nWidth,
                                int nHeight) {
    CUDA_MEMCPY2D m = {0};
    m.WidthInBytes = nWidth;
    m.Height = nHeight;
    m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    m.srcDevice = (CUdeviceptr)dpSrc;
    m.srcPitch = m.WidthInBytes;
    m.dstMemoryType = CU_MEMORYTYPE_HOST;
    m.dstDevice = (CUdeviceptr)(m.dstHost = pDst);
    m.dstPitch = m.WidthInBytes;
    cuMemcpy2D(&m);
}

void decoder_clear_buffer_with_constant_image(unsigned char *image_pt,
                                              int width, int height) {
    int counter = 0;
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            *(image_pt + counter) = 45;
            *(image_pt + counter + 1) = 85;
            *(image_pt + counter + 2) = 255;
            *(image_pt + counter + 3) = 255;
            counter += 4;
        }
    }
}

void decoder_print_one_display_buffer(unsigned char *image_pt, int width,
                                      int height, int channels) {
    int counter = 0;
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            for (int k = 0; k < channels; k++) {
                printf("%x ", *(image_pt + counter));
                counter++;
            }
            printf("  ");
        }
        printf("\n");
    }
}

inline void decoder_check_input_files(const char *sz_in_file_path) {
    std::ifstream fpIn(sz_in_file_path, std::ios::in | std::ios::binary);
    if (fpIn.fail()) {
        std::ostringstream err;
        err << "Unable to open input file: " << sz_in_file_path << std::endl;
        throw std::invalid_argument(err.str());
    }
}

void decoder_process(DecoderContext *dc_context, FFmpegDemuxer *demuxer,
                     std::string cam_name, PictureBuffer *display_buffer,
                     int size_of_buffer, SeekInfo *seek_info,
                     bool use_cpu_buffer) {
    CUdeviceptr pTmpImage = 0;
    ck(cuInit(0));
    CUcontext cuContext = NULL;
    createCudaContext(&cuContext, dc_context->gpu_index, 0);
    size_t nVideoBytes = 0;
    PacketData pktinfo;

    const cudaVideoCodec codec_id = FFmpeg2NvCodecId(demuxer->GetVideoCodec());
    auto make_decoder = [&]() {
        return std::make_unique<NvDecoder>(cuContext, true, codec_id);
    };
    std::unique_ptr<NvDecoder> dec = make_decoder();
    auto decoder_perf = [&]() -> std::shared_ptr<DecoderPerfSample> {
        std::lock_guard<std::mutex> lock(g_decoder_perf_mutex);
        auto &sample = decoder_perf_samples[cam_name];
        if (!sample) {
            sample = std::make_shared<DecoderPerfSample>();
        }
        return sample;
    }();
    const bool recreate_decoder_on_seek = []() {
        const char *env = std::getenv("CRIMSON_RECREATE_DECODER_ON_SEEK");
        if (!env) {
            return false;
        }
        return std::strcmp(env, "0") != 0;
    }();
    const bool allow_boundary_fallback = false;
    std::cout << "[Decoder] " << cam_name
              << " recreate_decoder_on_seek="
              << (recreate_decoder_on_seek ? "true" : "false")
              << " boundary_fallback="
              << (allow_boundary_fallback ? "true" : "false") << std::endl;
    int nWidth = 0, nHeight = 0;

    int nFrameReturned = 0, nFrame = 0, iMatrix = 0;
    uint8_t *pVideo = nullptr;
    uint8_t *pFrame;

    int buffer_head = 0;
    bool pending_seek_done = false;
    bool pending_seek_was_accurate = false;
    uint64_t pending_seek_id = 0;

    bool seek_success_flag;
    bool demux_success;

    double video_length = demuxer->GetDuration();
    double frame_rate = demuxer->GetFramerate();
    std::cout << "Video framerate: " << frame_rate << std::endl;
    std::cout << "Video length: " << video_length << std::endl;

    if (demuxer->GetNumFrames() == 0) {
        dc_context->estimated_num_frames = int(video_length * frame_rate);
    } else {
        dc_context->estimated_num_frames = demuxer->GetNumFrames() - 1;
    }

    std::cout << "estimated_num_frames:" << dc_context->estimated_num_frames
              << std::endl;
    int size_in_bytes;
    bool skip_first_decode_after_seek = false;
    int seek_debug_frames_to_log = 0;
    uint64_t seek_discard_count = 0;
    const bool buffer_requires_rgba =
        use_cpu_buffer || (size_of_buffer > 0 &&
                           display_buffer[0].format ==
                               PictureBufferFormat::RGBA32);
    auto seek_requested = [&]() -> bool {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        return seek_info->use_seek;
    };
    uint64_t active_seek_id = 0;
    auto publishFrameNumber = [&](uint64_t local_frame) -> int64_t {
        std::shared_ptr<const std::vector<int64_t>> frame_map;
        {
            std::lock_guard<std::mutex> lock(g_seek_info_mutex);
            frame_map = seek_info->frame_number_map;
        }
        if (frame_map != nullptr &&
            local_frame < static_cast<uint64_t>(frame_map->size())) {
            const int64_t parent_frame = (*frame_map)[static_cast<size_t>(local_frame)];
            if (parent_frame >= 0) {
                return parent_frame;
            }
        }
        return static_cast<int64_t>(local_frame);
    };
    auto publishFrameNumberInt = [&](uint64_t local_frame) -> int {
        const int64_t frame = publishFrameNumber(local_frame);
        return static_cast<int>(
            std::clamp<int64_t>(frame, 0, std::numeric_limits<int>::max()));
    };
    auto usesExternalFrameNumberMap = [&]() -> bool {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        return seek_info->frame_number_map != nullptr;
    };
    auto mark_seek_done = [&](uint64_t settled_local_frame) -> bool {
        const int64_t settled_frame = publishFrameNumber(settled_local_frame);
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        // If a newer request arrived while we processed this one, do not
        // overwrite it with stale completion state.
        if (seek_info->use_seek) {
            return false;
        }
        seek_info->seek_frame = static_cast<uint64_t>(
            std::max<int64_t>(0, settled_frame));
        seek_info->settled_seek_id = active_seek_id;
        seek_info->seek_done = true;
        return true;
    };
    double last_demux_ms = 0.0;
    double last_decode_ms = 0.0;
    int last_decode_returned = 0;
    bool last_demux_success = false;
    auto timedDemux = [&]() -> bool {
        const auto demux_start = std::chrono::steady_clock::now();
        const bool ok = demuxer->Demux(pVideo, nVideoBytes, pktinfo);
        last_demux_ms =
            decoder_duration_ms(std::chrono::steady_clock::now() -
                                demux_start);
        last_demux_success = ok;
        decoder_perf->demux_ms.store(last_demux_ms);
        decoder_perf->demux_success.store(ok ? 1 : 0);
        return ok;
    };
    auto timedDecode = [&](uint8_t* data,
                           size_t byte_count,
                           int flags = 0,
                           int64_t timestamp = 0) -> int {
        const auto decode_start = std::chrono::steady_clock::now();
        const int returned = dec->Decode(data, byte_count, flags, timestamp);
        last_decode_ms =
            decoder_duration_ms(std::chrono::steady_clock::now() -
                                decode_start);
        last_decode_returned = returned;
        decoder_perf->decode_ms.store(last_decode_ms);
        decoder_perf->decode_returned.store(returned);
        return returned;
    };
    auto mapTimestampToFrameNumber = [&](int64_t timestamp,
                                         int64_t fallback_frame) -> int64_t {
        if (usesExternalFrameNumberMap()) {
            return fallback_frame;
        }
        if (timestamp >= 0) {
            const int64_t frame_from_ts = demuxer->FrameNumberFromTs(timestamp);
            if (frame_from_ts >= 0) {
                return frame_from_ts;
            }
        }
        return fallback_frame;
    };
    auto discard_decoded_frames_until =
        [&](uint64_t &decode_frame_cursor, uint64_t target_frame) -> bool {
        while (nFrameReturned > 0) {
            if (decode_frame_cursor >= target_frame) {
                // Keep target frame (or nearest frame past it) in decoder
                // output queue for the normal write path.  Using >= instead
                // of == guards against the cursor overshooting the target by
                // one due to timestamp-to-frame rounding in FrameNumberFromTs
                // (AV_ROUND_NEAR_INF).  Without this, the loop would never
                // match the target and decode through the rest of the file.
                skip_first_decode_after_seek = true;
                return true;
            }
            int64_t discarded_timestamp = 0;
            dec->GetFrame(&discarded_timestamp);
            nFrameReturned--;
            ++seek_discard_count;
            const int64_t fallback_frame =
                static_cast<int64_t>(decode_frame_cursor);
            const int64_t mapped_frame =
                mapTimestampToFrameNumber(discarded_timestamp, fallback_frame);
            const int64_t next_frame = std::max(mapped_frame + 1, fallback_frame + 1);
            decode_frame_cursor = static_cast<uint64_t>(std::max<int64_t>(0, next_frame));
        }
        return false;
    };
    do {
        bool has_seek_request = false;
        uint64_t requested_frame = 0;
        bool seek_accurate = false;
        {
            std::lock_guard<std::mutex> lock(g_seek_info_mutex);
            if (seek_info->use_seek) {
                has_seek_request = true;
                requested_frame = seek_info->seek_frame;
                seek_accurate = seek_info->seek_accurate;
                active_seek_id = seek_info->seek_id;
                seek_info->use_seek = false;   // claim request
                seek_info->seek_done = false;  // new request in flight
            }
        }

        if (has_seek_request) {
            if (seek_accurate) {
                if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                          << " request id=" << active_seek_id
                          << " target=" << requested_frame
                          << " accurate=true" << std::endl;
            }
            pending_seek_was_accurate = seek_accurate;
            pending_seek_id = active_seek_id;
            seek_discard_count = 0;
            if (recreate_decoder_on_seek) {
                if (pTmpImage) {
                    ck(cuMemFree(pTmpImage));
                    pTmpImage = 0;
                }
                nWidth = 0;
                nHeight = 0;
                size_in_bytes = 0;
                dec = make_decoder();
            }

            demuxer->Flush();
            skip_first_decode_after_seek = false;

            SeekContext s = SeekContext(requested_frame);
            seek_success_flag = demuxer->Seek(s, pVideo, nVideoBytes, pktinfo);

            // For accurate seeks, add decode runway so the forward scan can
            // reliably produce the requested frame.
            if (seek_success_flag && seek_accurate) {
                int64_t rewind_frame = -1;
                if (cam_name == "Stimulus") {
                    // Some stimulus encodes expose unreliable packet keyframe
                    // flags. Seek with a fixed pre-roll and decode forward.
                    const int64_t preroll =
                        std::max<int64_t>(1, dc_context->seek_interval);
                    rewind_frame = std::max<int64_t>(
                        0, static_cast<int64_t>(requested_frame) - preroll);
                } else {
                    int64_t nearest_kf = -1;
                    if (pktinfo.pts >= 0)
                        nearest_kf = demuxer->FrameNumberFromTs(pktinfo.pts);
                    if (nearest_kf < 0 && pktinfo.dts >= 0)
                        nearest_kf = demuxer->FrameNumberFromTs(pktinfo.dts);
                    if (nearest_kf > 0) {
                        rewind_frame = nearest_kf - 1;
                    }
                }
                if (rewind_frame >= 0 &&
                    rewind_frame != static_cast<int64_t>(requested_frame)) {
                    if (cam_name == "Stimulus") {
                        if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                                  << " request id=" << active_seek_id
                                  << " preroll_seek_frame=" << rewind_frame
                                  << " target=" << requested_frame
                                  << std::endl;
                    }
                    SeekContext earlier(static_cast<uint64_t>(rewind_frame));
                    demuxer->Seek(earlier, pVideo, nVideoBytes, pktinfo);
                }
            }
            if (!seek_success_flag) {
                if (seek_accurate) {
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                              << " request id=" << active_seek_id
                              << " seek() failed before decode" << std::endl;
                }
                (void)mark_seek_done(requested_frame);
                pending_seek_done = false;
                pending_seek_was_accurate = false;
                continue;
            }

            // reset the display buffer after seeking
            for (int i = 0; i < size_of_buffer; i++) {
                // if (use_cpu_buffer) {
                //     decoder_clear_buffer_with_constant_image(display_buffer[i].frame,
                //     3208, 2200);
                // }
                frameSlotResetForWrite(display_buffer[i]);
                if (display_buffer[i].frame_bytes > 0 &&
                    display_buffer[i].frame) {
                    if (use_cpu_buffer) {
                        std::memset(display_buffer[i].frame, 0,
                                    display_buffer[i].frame_bytes);
                    } else {
                        ck(cudaMemset(display_buffer[i].frame, 0,
                                      display_buffer[i].frame_bytes));
                    }
                }
            }
            // Flush parser/display-queue state before seek discontinuity so
            // stale pre-seek frames cannot leak into post-seek output.
            nFrameReturned = timedDecode(nullptr, 0, 0);
            while (nFrameReturned > 0) {
                dec->GetFrame();
                nFrameReturned--;
            }
            // Mark the first post-seek packet as discontinuous so parser state
            // transitions to the new timeline.
            //
            // Note: Some H.264 streams used for Stimulus fail to emit any
            // post-seek frames when discontinuity is asserted here. Keep this
            // disabled for Stimulus while we validate seek behavior.
            const int seek_decode_flags =
                (cam_name == "Stimulus") ? 0 : CUVID_PKT_DISCONTINUITY;
            if (seek_accurate) {
                if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                          << " request id=" << active_seek_id
                          << " first_decode_flags=" << seek_decode_flags
                          << std::endl;
            }
            nFrameReturned = timedDecode(
                pVideo, nVideoBytes, seek_decode_flags, pktinfo.pts);

            uint64_t decode_frame_cursor = requested_frame;
            int64_t demux_frame = -1;
            if (pktinfo.pts >= 0) {
                demux_frame = demuxer->FrameNumberFromTs(pktinfo.pts);
            }
            if (demux_frame < 0 && pktinfo.dts >= 0) {
                demux_frame = demuxer->FrameNumberFromTs(pktinfo.dts);
            }
            if (demux_frame >= 0) {
                decode_frame_cursor = static_cast<uint64_t>(demux_frame);
            }
            if (seek_accurate) {
                if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                          << " request id=" << active_seek_id
                          << " post-seek decode returned=" << nFrameReturned
                          << " initial_cursor=" << decode_frame_cursor
                          << " pts=" << pktinfo.pts
                          << " dts=" << pktinfo.dts << std::endl;
            }

            uint64_t settled_seek_frame = decode_frame_cursor;
            bool accurate_reached_target = false;
            uint64_t accurate_demux_attempts = 0;
            uint64_t accurate_decode_calls = 0;
            if (seek_accurate) {
                // seek accurate implementation
                // keep decoding till the target frame
                constexpr uint64_t kAccurateSeekFallbackSlackFrames = 1;
                bool reached_target = (decode_frame_cursor >= requested_frame);
                if (!reached_target) {
                    reached_target =
                        discard_decoded_frames_until(decode_frame_cursor, requested_frame);
                    while (!reached_target) {
                        ++accurate_demux_attempts;
                        demux_success = timedDemux();
                        if (!demux_success) {
                            // Some streams intermittently fail to demux the exact
                            // terminal packet for a seek target. If we are already
                            // within one frame, keep the nearest frame instead of
                            // stalling the UI waiting for an exact frame forever.
                            const uint64_t cursor_plus_slack =
                                decode_frame_cursor + kAccurateSeekFallbackSlackFrames;
                            const bool near_target =
                                (cursor_plus_slack >= requested_frame);
                            if (allow_boundary_fallback && near_target) {
                                if (nFrameReturned == 0) {
                                    // Try draining any frame that may already be queued
                                    // in the decoder before we accept boundary fallback.
                                    nFrameReturned =
                                        timedDecode(nullptr, 0);
                                }
                                skip_first_decode_after_seek = (nFrameReturned > 0);
                                std::cout
                                    << "[Decoder] Demux boundary fallback: cam="
                                    << cam_name << " target_frame="
                                    << requested_frame << " cursor="
                                    << decode_frame_cursor << std::endl;
                                reached_target = true;
                                break;
                            }
                            // end of stream or demux discontinuity before target
                            std::cout << "[Decoder] Demux error: cam=" << cam_name
                                      << " target_frame=" << requested_frame
                                      << " cursor=" << decode_frame_cursor
                                      << std::endl;
                            nFrameReturned = timedDecode(nullptr, 0);
                            ++accurate_decode_calls;
                            dc_context->total_num_frame = nFrame + nFrameReturned;
                        } else {
                            nFrameReturned = timedDecode(
                                pVideo, nVideoBytes, 0, pktinfo.pts);
                            ++accurate_decode_calls;
                        }
                        if (!demux_success && nFrameReturned == 0) {
                            break;
                        }
                        reached_target =
                            discard_decoded_frames_until(decode_frame_cursor, requested_frame);
                    }
                } else {
                    skip_first_decode_after_seek = (nFrameReturned > 0);
                    if (!skip_first_decode_after_seek) {
                        while (nFrameReturned == 0) {
                            ++accurate_demux_attempts;
                            demux_success = timedDemux();
                            if (!demux_success) {
                                // End of stream/discontinuity while already at
                                // target. Drain delayed frames from decoder
                                // before giving up so seek can still settle
                                // with a queued frame when available.
                                nFrameReturned = timedDecode(nullptr, 0);
                                ++accurate_decode_calls;
                                if (seek_accurate) {
                                    if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                                              << " request id=" << active_seek_id
                                              << " demux_end_drain returned="
                                              << nFrameReturned << std::endl;
                                }
                                break;
                            }
                            nFrameReturned = timedDecode(
                                pVideo, nVideoBytes, 0, pktinfo.pts);
                            ++accurate_decode_calls;
                        }
                        skip_first_decode_after_seek = (nFrameReturned > 0);
                    }
                }
                if (!reached_target) {
                    settled_seek_frame = decode_frame_cursor;
                    skip_first_decode_after_seek = (nFrameReturned > 0);
                }
                accurate_reached_target = reached_target;
                if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                          << " request id=" << active_seek_id
                          << " result reached_target="
                          << (accurate_reached_target ? "true" : "false")
                          << " settled=" << settled_seek_frame
                          << " cursor=" << decode_frame_cursor
                          << " nFrameReturned=" << nFrameReturned
                          << " demux_attempts=" << accurate_demux_attempts
                          << " decode_calls=" << accurate_decode_calls
                          << " discarded=" << seek_discard_count
                          << std::endl;
            } else {
                settled_seek_frame = decode_frame_cursor;
                skip_first_decode_after_seek = (nFrameReturned > 0);
            }

            // dec.setReconfigParams(NULL, NULL);
            buffer_head = 0;
            nFrame = static_cast<int>(settled_seek_frame);
            if (nFrameReturned > 0) {
                latest_decoded_frame[cam_name].store(
                    publishFrameNumberInt(settled_seek_frame));
            } else {
                // No decoded frame is currently queued for display after this
                // seek operation, so keep latest_decoded_frame invalid until a
                // real frame lands in the ring buffer.
                latest_decoded_frame[cam_name].store(-1);
            }
            frameSlotResetForWrite(display_buffer[0]);
            // If no frame is currently queued (or this window is not actively
            // decoding), acknowledge seek completion now.  When nFrameReturned
            // is zero there is nothing to write, so deferring would leave
            // pending_seek_done stuck forever (especially after boundary
            // fallback where demux has already failed).
            const bool window_decoding_enabled =
                window_need_decoding[cam_name].load();
            if (!window_decoding_enabled || nFrameReturned == 0) {
                (void)mark_seek_done(settled_seek_frame);
                pending_seek_done = false;
                if (seek_accurate) {
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                              << " request id=" << active_seek_id
                              << " mark_done_immediate reason="
                              << (!window_decoding_enabled ? "window_not_decoding"
                                                           : "no_decoded_frame_queued")
                              << " settled=" << settled_seek_frame
                              << " nFrameReturned=" << nFrameReturned
                              << std::endl;
                }
                pending_seek_was_accurate = false;
            } else {
                pending_seek_done = true;
                if (seek_accurate) {
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                              << " request id=" << active_seek_id
                              << " defer_done_until_frame_write queued_frames="
                              << nFrameReturned
                              << " settled=" << settled_seek_frame
                              << std::endl;
                }
            }
            seek_debug_frames_to_log = crimson_seek_debug_logs_enabled() ? 10 : 0;
        } else {
            static thread_local bool logged_idle = false;
            if (window_need_decoding[cam_name].load()) {
                logged_idle = false;
                if (!skip_first_decode_after_seek) {
                    demux_success = timedDemux();
                    if (!demux_success) {
                        // end of stream
                        // std::cout << "Demux error..." << std::endl;
                        nFrameReturned = timedDecode(
                            nullptr, 0, CUVID_PKT_DISCONTINUITY);
                        dc_context->total_num_frame = nFrame + nFrameReturned;
                    } else {
                        nFrameReturned = timedDecode(
                            pVideo, nVideoBytes, 0, pktinfo.pts);
                    }
                } else {
                    skip_first_decode_after_seek = false;
                }

                if (!pTmpImage && nFrameReturned && buffer_requires_rgba) {
                    LOG(INFO) << dec->GetVideoInfo();
                    // Get output frame size from decoder
                    nWidth = dec->GetWidth();
                    nHeight = dec->GetHeight();
                    size_in_bytes = nWidth * nHeight * 4;
                    cuMemAlloc(&pTmpImage, size_in_bytes);
                } else if ((nWidth == 0 || nHeight == 0) && nFrameReturned) {
                    nWidth = dec->GetWidth();
                    nHeight = dec->GetHeight();
                }

                for (int i = 0; i < nFrameReturned; i++) {
                    // decode frame and conversion
                    const auto decode_pipeline_start =
                        std::chrono::steady_clock::now();
                    double decode_wait_ms = 0.0;
                    double decode_convert_ms = 0.0;
                    double decode_write_ms = 0.0;
                    int64_t frame_timestamp = 0;
                    pFrame = dec->GetFrame(&frame_timestamp);
                    iMatrix = dec->GetVideoFormatInfo()
                                  .video_signal_description.matrix_coefficients;
                    int64_t mapped_frame_num =
                        mapTimestampToFrameNumber(frame_timestamp, nFrame);
                    if (mapped_frame_num < 0) {
                        mapped_frame_num = nFrame;
                    }
                    const int local_frame_num = static_cast<int>(
                        std::clamp<int64_t>(
                            mapped_frame_num, 0,
                            std::numeric_limits<int>::max()));
                    const int assigned_frame_num =
                        publishFrameNumberInt(static_cast<uint64_t>(
                            local_frame_num));
                    const auto wait_start = std::chrono::steady_clock::now();
                    std::optional<FrameSlotWriteLease> write_lease;
                    while (!(dc_context->stop_flag) && !seek_requested()) {
                        write_lease =
                            frameSlotAcquireWritable(display_buffer[buffer_head]);
                        if (write_lease.has_value()) {
                            break;
                        }
                        // The queue is full until the GUI releases this slot.
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(1));
                    }
                    decode_wait_ms += decoder_duration_ms(
                        std::chrono::steady_clock::now() - wait_start);
                    if (!write_lease.has_value()) {
                        continue;
                    }

                    PictureBuffer& writable_slot = write_lease->slot();
                    const PictureBufferFormat slot_format = writable_slot.format;
                    const int slot_pitch =
                        writable_slot.pitch_bytes > 0 ? writable_slot.pitch_bytes
                                                       : dec->GetWidth();
                    const size_t slot_frame_bytes =
                        writable_slot.frame_bytes > 0
                            ? writable_slot.frame_bytes
                            : static_cast<size_t>(dec->GetFrameSize());
                    auto convert_to_rgba = [&]() {
                        const auto convert_start = std::chrono::steady_clock::now();
                        Nv12ToColor32<RGBA32>(
                            pFrame, dec->GetWidth(), (uint8_t *)pTmpImage,
                            4 * dec->GetWidth(), dec->GetWidth(),
                            dec->GetHeight(), iMatrix);
                        decode_convert_ms += decoder_duration_ms(
                            std::chrono::steady_clock::now() - convert_start);
                    };
                    auto write_buffered_frame = [&]() {
                        const auto write_start = std::chrono::steady_clock::now();
                        if (use_cpu_buffer) {
                            decoder_get_image_from_gpu(
                                pTmpImage, writable_slot.frame,
                                4 * dec->GetWidth(), dec->GetHeight());
                        } else if (slot_format ==
                                   PictureBufferFormat::RGBA32) {
                            cudaMemcpy(writable_slot.frame,
                                       (uint8_t *)pTmpImage, size_in_bytes,
                                       cudaMemcpyDeviceToDevice);
                        } else {
                            cudaMemcpy(writable_slot.frame,
                                       pFrame, slot_frame_bytes,
                                       cudaMemcpyDeviceToDevice);
                        }
                        decode_write_ms += decoder_duration_ms(
                            std::chrono::steady_clock::now() - write_start);
                    };
                    if (slot_format == PictureBufferFormat::RGBA32) {
                        convert_to_rgba();
                    }
                    write_buffered_frame();

                    FrameSlotMetadata published_metadata;
                    published_metadata.frame_number = assigned_frame_num;
                    published_metadata.local_frame_number = local_frame_num;
                    published_metadata.frame_pts = frame_timestamp;
                    published_metadata.frame_source_code =
                        pending_seek_done ? 1 : 2;
                    published_metadata.pitch_bytes = slot_pitch;
                    published_metadata.frame_bytes = slot_frame_bytes;
                    published_metadata.color_matrix = iMatrix;
                    published_metadata.format = slot_format;
                    write_lease->publish(published_metadata);
                    dc_context->decoding_flag = true;
                    latest_decoded_frame[cam_name].store(assigned_frame_num);
                    if (pending_seek_done) {
                        const bool marked =
                            mark_seek_done(static_cast<uint64_t>(local_frame_num));
                        if (pending_seek_was_accurate) {
                            if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekAccurate] cam=" << cam_name
                                      << " request id=" << pending_seek_id
                                      << " done_on_frame_write frame="
                                      << assigned_frame_num
                                      << " marked=" << (marked ? "true" : "false")
                                      << std::endl;
                        }
                        pending_seek_done = false;
                        pending_seek_was_accurate = false;
                    }
                    decoder_perf->nv12_to_rgba_ms.store(decode_convert_ms);
                    decoder_perf->buffer_wait_ms.store(decode_wait_ms);
                    decoder_perf->frame_write_ms.store(decode_write_ms);
                    decoder_perf->frame_total_ms.store(decoder_duration_ms(
                        std::chrono::steady_clock::now() -
                        decode_pipeline_start));
                    decoder_perf->demux_ms.store(last_demux_ms);
                    decoder_perf->demux_success.store(last_demux_success ? 1 : 0);
                    decoder_perf->decode_ms.store(last_decode_ms);
                    decoder_perf->decode_returned.store(last_decode_returned);
                    decoder_perf->packet_total_ms.store(last_demux_ms +
                                                       last_decode_ms);
                    decoder_perf->published_frame.store(assigned_frame_num);
                    (void)decoder_perf->sample_sequence.fetch_add(1);
                    if (seek_debug_frames_to_log > 0) {
                        const int64_t pts_frame =
                            (frame_timestamp >= 0)
                                ? demuxer->FrameNumberFromTs(frame_timestamp)
                                : -1;
                        if (crimson_seek_debug_logs_enabled()) std::cout << "[SeekDebug] cam=" << cam_name
                                  << " assigned=" << assigned_frame_num
                                  << " local=" << local_frame_num
                                  << " pts_frame=" << pts_frame
                                  << " fallback_counter=" << nFrame
                                  << " pts=" << frame_timestamp
                                  << " buffer_head=" << buffer_head << std::endl;
                        --seek_debug_frames_to_log;
                    }
                    nFrame = local_frame_num + 1;
                    buffer_head = (buffer_head + 1) % size_of_buffer;
                    // for debugging purpose
                    if (!demux_success) {
                        std::cout << "total_num_frame: "
                                  << dc_context->total_num_frame << std::endl;
                    }
                    if (cam_name == "Stimulus" &&
                        ((nFrame % 30) == 0 || buffer_head == 0)) {
                        if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus Decoder] produced frame " << nFrame
                                  << " buffer_head=" << buffer_head
                                  << " available_to_write=" << display_buffer[buffer_head].available_to_write
                                  << std::endl;
                    }
                }
            } else {
                if (!logged_idle) {
                    std::cout << "[Decoder] " << cam_name
                              << " idle (window_need_decoding=false)" << std::endl;
                    logged_idle = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    } while (!(dc_context->stop_flag));
    if (pTmpImage) {
        ck(cuMemFree(pTmpImage));
    }
}

void image_loader(DecoderContext *dc_context,
                  const std::vector<std::string> &img_list_vector,
                  PictureBuffer *display_buffer, int size_of_buffer,
                  SeekInfo *seek_info, bool use_cpu_buffer,
                  std::string cam_name, std::string root_dir) {
    int buffer_head = 0;
    int frame_number = 0;
    dc_context->total_num_frame = img_list_vector.size();
    dc_context->estimated_num_frames = img_list_vector.size();
    auto seek_requested = [&]() -> bool {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        return seek_info->use_seek;
    };
    uint64_t active_seek_id = 0;
    auto mark_seek_done = [&](uint64_t settled_frame) {
        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
        seek_info->seek_frame = settled_frame;
        seek_info->settled_seek_id = active_seek_id;
        seek_info->seek_done = true;
    };
    while (!(dc_context->stop_flag)) {
        bool has_seek_request = false;
        uint64_t requested_frame = 0;
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
            // reset the display buffer after seeking
            for (int i = 0; i < size_of_buffer; i++) {
                // if (use_cpu_buffer) {
                //     decoder_clear_buffer_with_constant_image(display_buffer[i].frame,
                //     3208, 2200);
                // }
                display_buffer[i].available_to_write = true;
                display_buffer[i].frame_number = -1;
                display_buffer[i].local_frame_number = -1;
                display_buffer[i].frame_pts = -1;
                display_buffer[i].frame_source_code = 0;
                display_buffer[i].color_matrix = ColorSpaceStandard_BT709;
            }
            buffer_head = 0;
            frame_number = static_cast<int>(requested_frame);
            display_buffer[0].frame_number = -1;
            display_buffer[0].local_frame_number = -1;
            display_buffer[0].frame_pts = -1;
            display_buffer[0].frame_source_code = 0;
            mark_seek_done(requested_frame);
        } else {
            if (frame_number < img_list_vector.size()) {
                if (frame_number == 0) {
                    std::string file_name = root_dir + "/" + cam_name + "_" +
                                            img_list_vector[frame_number];
                    cv::Mat image = cv::imread(file_name, cv::IMREAD_COLOR);
                    cv::Mat image_rgba;
                    cv::cvtColor(image, image_rgba, cv::COLOR_BGR2RGBA);
                    size_t buffer_size =
                        image_rgba.total() *
                        image_rgba.elemSize(); // Rows * Cols * Channels
                    memcpy(display_buffer[buffer_head].frame, image_rgba.data,
                           buffer_size);

                    display_buffer[buffer_head].available_to_write = false;
                    dc_context->decoding_flag = true;
                    display_buffer[buffer_head].frame_number = frame_number;
                    display_buffer[buffer_head].local_frame_number = frame_number;
                    display_buffer[buffer_head].frame_pts = -1;
                    display_buffer[buffer_head].frame_source_code = 2;
                    display_buffer[buffer_head].pitch_bytes =
                        image_rgba.cols * static_cast<int>(image_rgba.elemSize());
                    display_buffer[buffer_head].frame_bytes = buffer_size;
                    display_buffer[buffer_head].color_matrix =
                        ColorSpaceStandard_BT709;
                    display_buffer[buffer_head].format =
                        PictureBufferFormat::RGBA32;
                } else {
                    while (!display_buffer[buffer_head].available_to_write &&
                           !(dc_context->stop_flag) && !seek_requested()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(1));
                    }
                    std::string file_name = root_dir + "/" + cam_name + "_" +
                                            img_list_vector[frame_number];
                    cv::Mat image = cv::imread(file_name, cv::IMREAD_COLOR);
                    cv::Mat image_rgba;
                    cv::cvtColor(image, image_rgba, cv::COLOR_BGR2RGBA);
                    size_t buffer_size =
                        image_rgba.total() *
                        image_rgba.elemSize(); // Rows * Cols * Channels
                    memcpy(display_buffer[buffer_head].frame, image_rgba.data,
                           buffer_size);
                    display_buffer[buffer_head].available_to_write = false;
                    dc_context->decoding_flag = true;
                    display_buffer[buffer_head].frame_number = frame_number;
                    display_buffer[buffer_head].local_frame_number = frame_number;
                    display_buffer[buffer_head].frame_pts = -1;
                    display_buffer[buffer_head].frame_source_code = 2;
                    display_buffer[buffer_head].pitch_bytes =
                        image_rgba.cols * static_cast<int>(image_rgba.elemSize());
                    display_buffer[buffer_head].frame_bytes = buffer_size;
                    display_buffer[buffer_head].color_matrix =
                        ColorSpaceStandard_BT709;
                    display_buffer[buffer_head].format =
                        PictureBufferFormat::RGBA32;
                }
                frame_number = frame_number + 1;
                buffer_head = (buffer_head + 1) % size_of_buffer;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}
