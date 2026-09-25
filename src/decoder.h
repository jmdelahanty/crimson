#ifndef RED_DECODER
#define RED_DECODER
#include "FFmpegDemuxer.h"
#include "NvCodecUtils.h"
#include "NvDecoder.h"
#include "frame_types.h"
#include <cuda.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <vector>
struct SeekInfo {
    bool use_seek = false;
    bool seek_done = false;
    uint64_t seek_frame = 0;
    bool seek_accurate = false;
    uint64_t seek_id = 0;           // generation set by requester
    uint64_t settled_seek_id = 0;   // generation echoed back on completion
    std::shared_ptr<const std::vector<int64_t>> frame_number_map;
};

struct DecoderContext {
    bool decoding_flag;
    std::atomic<bool> stop_flag{false};
    int total_num_frame;
    int estimated_num_frames;
    int gpu_index;
    int seek_interval;
};

// A speculative decoder publishes into a small private queue. Once that queue
// has filled, the owner can bind the same worker to the normal history ring.
// The worker reads the binding only between frame writes.
struct DecoderOutputHandoff {
    std::mutex mutex;
    PictureBuffer* buffer = nullptr;
    int buffer_size = 0;
    int next_slot = 0;
    SeekInfo* seek_info = nullptr;
    uint64_t generation = 0;
    std::atomic<bool> adopted{false};
    std::atomic<bool> need_decoding{true};
    std::atomic<int> latest_frame{-1};
    std::atomic<bool> failed{false};
};

void decoder_get_image_from_gpu(CUdeviceptr dpSrc, uint8_t *pDst, int nWidth,
                                int nHeight);
void decoder_clear_buffer_with_constant_image(unsigned char *image_pt,
                                              int width, int height);
void decoder_print_one_display_buffer(unsigned char *image_pt, int width,
                                      int height, int channels);
void decoder_process(DecoderContext *dc_context, FFmpegDemuxer *demuxer,
                     std::string cam_name, PictureBuffer *display_buffer,
                     int size_of_buffer, SeekInfo *seek_info,
                     bool use_cpu_buffer);
void decoder_process_with_handoff(DecoderContext *dc_context,
                     FFmpegDemuxer *demuxer, std::string cam_name,
                     PictureBuffer *display_buffer, int size_of_buffer,
                     SeekInfo *seek_info, bool use_cpu_buffer,
                     DecoderOutputHandoff *output_handoff);
void image_loader(DecoderContext *dc_context,
                  const std::vector<std::string> &img_list_vector,
                  PictureBuffer *display_buffer, int size_of_buffer,
                  SeekInfo *seek_info, bool use_cpu_buffer,
                  std::string cam_name, std::string root_dir);
#endif
