#include "decoder.h"
#include "AppDecUtils.h"
#include "global.h"
#include <cstdlib>
#include <cstring>
#include <memory>

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
    const bool recreate_decoder_on_seek = []() {
        const char *env = std::getenv("CRIMSON_RECREATE_DECODER_ON_SEEK");
        if (!env) {
            return true;
        }
        return std::strcmp(env, "0") != 0;
    }();
    std::cout << "[Decoder] " << cam_name
              << " recreate_decoder_on_seek="
              << (recreate_decoder_on_seek ? "true" : "false") << std::endl;
    int nWidth = 0, nHeight = 0;

    int nFrameReturned = 0, nFrame = 0, iMatrix = 0;
    uint8_t *pVideo = nullptr;
    uint8_t *pFrame;

    int buffer_head = 0;

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
    auto mapTimestampToFrameNumber = [&](int64_t timestamp,
                                         int64_t fallback_frame) -> int64_t {
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
            if (decode_frame_cursor == target_frame) {
                // Keep target frame in decoder output queue for normal write path.
                skip_first_decode_after_seek = true;
                return true;
            }
            int64_t discarded_timestamp = 0;
            dec->GetFrame(&discarded_timestamp);
            nFrameReturned--;
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
        if (seek_info->use_seek) {
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
            // std::cout << "target_frame_number:" << seek_info->seek_frame
            //           << std::endl;
            skip_first_decode_after_seek = false;
            const uint64_t requested_frame = seek_info->seek_frame;

            SeekContext s = SeekContext(requested_frame);

            seek_success_flag = demuxer->Seek(s, pVideo, nVideoBytes, pktinfo);
            // std::cout << "seek_success_flag: " << seek_success_flag <<
            // std::endl;
            if (!seek_success_flag) {
                seek_info->use_seek = false;
                seek_info->seek_done = true;
                continue;
            }

            // reset the display buffer after seeking
            size_t clear_bytes = 0;
            int clear_w = static_cast<int>(demuxer->GetWidth());
            int clear_h = static_cast<int>(demuxer->GetHeight());
            if (clear_w > 0 && clear_h > 0) {
                clear_bytes = static_cast<size_t>(clear_w) *
                              static_cast<size_t>(clear_h) * 4;
            }
            for (int i = 0; i < size_of_buffer; i++) {
                // if (use_cpu_buffer) {
                //     decoder_clear_buffer_with_constant_image(display_buffer[i].frame,
                //     3208, 2200);
                // }
                display_buffer[i].available_to_write = true;
                display_buffer[i].frame_number = -1;
                if (clear_bytes > 0 && display_buffer[i].frame) {
                    if (use_cpu_buffer) {
                        std::memset(display_buffer[i].frame, 0, clear_bytes);
                    } else {
                        ck(cudaMemset(display_buffer[i].frame, 0, clear_bytes));
                    }
                }
            }
            // Flush parser/display-queue state before seek discontinuity so
            // stale pre-seek frames cannot leak into post-seek output.
            nFrameReturned = dec->Decode(NULL, 0, 0);
            while (nFrameReturned > 0) {
                dec->GetFrame();
                nFrameReturned--;
            }
            // Mark the first post-seek packet as discontinuous so parser state
            // transitions to the new timeline.
            nFrameReturned = dec->Decode(
                pVideo, nVideoBytes, CUVID_PKT_DISCONTINUITY, pktinfo.pts);

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

            if (seek_info->seek_accurate) {
                // seek accurate implementation
                // keep decoding till the target frame
                bool reached_target = (decode_frame_cursor >= requested_frame);
                if (!reached_target) {
                    reached_target =
                        discard_decoded_frames_until(decode_frame_cursor, requested_frame);
                    while (!reached_target) {
                        demux_success =
                            demuxer->Demux(pVideo, nVideoBytes, pktinfo);
                        if (!demux_success) {
                            // end of stream
                            std::cout << "Demux error..." << std::endl;
                            nFrameReturned = dec->Decode(NULL, 0);
                            dc_context->total_num_frame = nFrame + nFrameReturned;
                        } else {
                            nFrameReturned =
                                dec->Decode(pVideo, nVideoBytes, 0, pktinfo.pts);
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
                            demux_success =
                                demuxer->Demux(pVideo, nVideoBytes, pktinfo);
                            if (!demux_success) {
                                break;
                            }
                            nFrameReturned =
                                dec->Decode(pVideo, nVideoBytes, 0, pktinfo.pts);
                        }
                        skip_first_decode_after_seek = (nFrameReturned > 0);
                    }
                }
                if (!reached_target) {
                    seek_info->seek_frame = decode_frame_cursor;
                    skip_first_decode_after_seek = (nFrameReturned > 0);
                }
            } else {
                seek_info->seek_frame = decode_frame_cursor;
                skip_first_decode_after_seek = (nFrameReturned > 0);
            }

            // dec.setReconfigParams(NULL, NULL);
            buffer_head = 0;
            nFrame = seek_info->seek_frame;
            latest_decoded_frame[cam_name].store(static_cast<int>(seek_info->seek_frame));
            display_buffer[0].frame_number = -1;
            seek_info->use_seek = false;
            seek_info->seek_done = true;
            seek_debug_frames_to_log = 10;
        } else {
            static thread_local bool logged_idle = false;
            if (window_need_decoding[cam_name].load()) {
                logged_idle = false;
                if (!skip_first_decode_after_seek) {
                    demux_success =
                        demuxer->Demux(pVideo, nVideoBytes, pktinfo);
                    if (!demux_success) {
                        // end of stream
                        // std::cout << "Demux error..." << std::endl;
                        nFrameReturned =
                            dec->Decode(NULL, 0, CUVID_PKT_DISCONTINUITY);
                        dc_context->total_num_frame = nFrame + nFrameReturned;
                    } else {
                        nFrameReturned =
                            dec->Decode(pVideo, nVideoBytes, 0, pktinfo.pts);
                    }
                } else {
                    skip_first_decode_after_seek = false;
                }

                if (!pTmpImage && nFrameReturned) {
                    LOG(INFO) << dec->GetVideoInfo();
                    // Get output frame size from decoder
                    nWidth = dec->GetWidth();
                    nHeight = dec->GetHeight();
                    size_in_bytes = nWidth * nHeight * 4;
                    cuMemAlloc(&pTmpImage, size_in_bytes);
                }

                for (int i = 0; i < nFrameReturned; i++) {
                    // decode frame and conversion
                    int64_t frame_timestamp = 0;
                    pFrame = dec->GetFrame(&frame_timestamp);
                    iMatrix = dec->GetVideoFormatInfo()
                                  .video_signal_description.matrix_coefficients;
                    int64_t mapped_frame_num =
                        mapTimestampToFrameNumber(frame_timestamp, nFrame);
                    if (mapped_frame_num < 0) {
                        mapped_frame_num = nFrame;
                    }
                    const int assigned_frame_num = static_cast<int>(mapped_frame_num);
                    if (nFrame == 0) {
                        if (use_cpu_buffer) {
                            Nv12ToColor32<RGBA32>(
                                pFrame, dec->GetWidth(), (uint8_t *)pTmpImage,
                                4 * dec->GetWidth(), dec->GetWidth(),
                                dec->GetHeight(), iMatrix);
                            decoder_get_image_from_gpu(
                                pTmpImage, display_buffer[buffer_head].frame,
                                4 * dec->GetWidth(), dec->GetHeight());
                        } else {
                            Nv12ToColor32<RGBA32>(
                                pFrame, dec->GetWidth(), (uint8_t *)pTmpImage,
                                4 * dec->GetWidth(), dec->GetWidth(),
                                dec->GetHeight(), iMatrix);
                            cudaMemcpy(display_buffer[buffer_head].frame,
                                       (uint8_t *)pTmpImage, size_in_bytes,
                                       cudaMemcpyDeviceToDevice);
                        }
                        display_buffer[buffer_head].available_to_write = false;
                        dc_context->decoding_flag = true;
                        display_buffer[buffer_head].frame_number = assigned_frame_num;
                        latest_decoded_frame[cam_name].store(assigned_frame_num);
                    } else {
                        while (
                            !display_buffer[buffer_head].available_to_write &&
                            !(dc_context->stop_flag) &&
                            !(seek_info->use_seek)) {
                            // if the next frame hasn't been displayed, the
                            // queue is full, sleep std::cout << "thread wait, "
                            // << display_buffer[buffer_head].available_to_write
                            // << ", " << buffer_head << ", " <<
                            // display_buffer[buffer_head].frame_number <<
                            // std::endl;
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(1));
                        }
                        if (use_cpu_buffer) {
                            Nv12ToColor32<RGBA32>(
                                pFrame, dec->GetWidth(), (uint8_t *)pTmpImage,
                                4 * dec->GetWidth(), dec->GetWidth(),
                                dec->GetHeight(), iMatrix);
                            decoder_get_image_from_gpu(
                                pTmpImage, display_buffer[buffer_head].frame,
                                4 * dec->GetWidth(), dec->GetHeight());
                        } else {
                            Nv12ToColor32<RGBA32>(
                                pFrame, dec->GetWidth(), (uint8_t *)pTmpImage,
                                4 * dec->GetWidth(), dec->GetWidth(),
                                dec->GetHeight(), iMatrix);
                            cudaMemcpy(display_buffer[buffer_head].frame,
                                       (uint8_t *)pTmpImage, size_in_bytes,
                                       cudaMemcpyDeviceToDevice);
                        }

                        display_buffer[buffer_head].available_to_write = false;
                        display_buffer[buffer_head].frame_number = assigned_frame_num;
                        latest_decoded_frame[cam_name].store(assigned_frame_num);
                    }
                    if (seek_debug_frames_to_log > 0) {
                        const int64_t pts_frame =
                            (frame_timestamp >= 0)
                                ? demuxer->FrameNumberFromTs(frame_timestamp)
                                : -1;
                        std::cout << "[SeekDebug] cam=" << cam_name
                                  << " assigned=" << assigned_frame_num
                                  << " pts_frame=" << pts_frame
                                  << " fallback_counter=" << nFrame
                                  << " pts=" << frame_timestamp
                                  << " buffer_head=" << buffer_head << std::endl;
                        --seek_debug_frames_to_log;
                    }
                    nFrame = assigned_frame_num + 1;
                    buffer_head = (buffer_head + 1) % size_of_buffer;
                    // for debugging purpose
                    if (!demux_success) {
                        std::cout << "total_num_frame: "
                                  << dc_context->total_num_frame << std::endl;
                    }
                    if (cam_name == "Stimulus" &&
                        ((nFrame % 30) == 0 || buffer_head == 0)) {
                        std::cout << "[Stimulus Decoder] produced frame " << nFrame
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
    while (!(dc_context->stop_flag)) {
        if (seek_info->use_seek) {
            // reset the display buffer after seeking
            for (int i = 0; i < size_of_buffer; i++) {
                // if (use_cpu_buffer) {
                //     decoder_clear_buffer_with_constant_image(display_buffer[i].frame,
                //     3208, 2200);
                // }
                display_buffer[i].available_to_write = true;
                display_buffer[i].frame_number = -1;
            }
            buffer_head = 0;
            frame_number = seek_info->seek_frame;
            display_buffer[0].frame_number = -1;
            seek_info->use_seek = false;
            seek_info->seek_done = true;
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
                } else {
                    while (!display_buffer[buffer_head].available_to_write &&
                           !(dc_context->stop_flag) && !(seek_info->use_seek)) {
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
                    display_buffer[buffer_head].frame_number = frame_number;
                }
                frame_number = frame_number + 1;
                buffer_head = (buffer_head + 1) % size_of_buffer;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}
