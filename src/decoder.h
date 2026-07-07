#ifndef RED_DECODER
#define RED_DECODER
#include "ColorSpace.h"
#include "FFmpegDemuxer.h"
#include "NvCodecUtils.h"
#include "NvDecoder.h"
#include <cuda.h>
#include <cstddef>
#include <memory>
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

enum class PictureBufferFormat {
    RGBA32 = 0,
    NV12 = 1,
};

struct FrameSlotState;

struct PictureBuffer {
    unsigned char *frame;
    int frame_number;
    int local_frame_number;
    int64_t frame_pts;
    int frame_source_code;
    bool available_to_write;
    int pitch_bytes;
    size_t frame_bytes;
    int color_matrix;
    int color_range;
    PictureBufferFormat format;
    FrameSlotState *frame_slot_state;
};

struct DecoderContext {
    bool decoding_flag;
    bool stop_flag;
    int total_num_frame;
    int estimated_num_frames;
    int gpu_index;
    int seek_interval;
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
void image_loader(DecoderContext *dc_context,
                  const std::vector<std::string> &img_list_vector,
                  PictureBuffer *display_buffer, int size_of_buffer,
                  SeekInfo *seek_info, bool use_cpu_buffer,
                  std::string cam_name, std::string root_dir);
#endif
