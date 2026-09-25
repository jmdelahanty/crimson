#ifndef RED_GLOBAL
#define RED_GLOBAL
#include "opencv2/core/types.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#define MAX_VIEWS 17

struct DecoderPerfSample {
    std::atomic<double> demux_ms{0.0};
    std::atomic<double> decode_ms{0.0};
    std::atomic<double> nv12_to_rgba_ms{0.0};
    std::atomic<double> buffer_wait_ms{0.0};
    std::atomic<double> frame_write_ms{0.0};
    std::atomic<double> frame_total_ms{0.0};
    std::atomic<double> packet_total_ms{0.0};
    std::atomic<int> decode_returned{0};
    std::atomic<int> demux_success{0};
    std::atomic<int> published_frame{-1};
    std::atomic<uint64_t> sample_sequence{0};
};

extern std::vector<std::mutex> g_mutexes;
extern std::vector<std::condition_variable> g_cvs;
extern std::vector<bool> g_ready;
extern std::vector<std::vector<cv::Rect>> yolo_boxes;
extern std::vector<std::vector<std::string>> yolo_labels;
extern std::vector<std::vector<int>> yolo_classid;
extern std::vector<unsigned char *> yolo_input_frames_rgba;
extern std::unordered_map<std::string, std::atomic<bool>> window_need_decoding;
extern std::unordered_map<std::string, std::atomic<int>> latest_decoded_frame;
extern std::unordered_map<std::string, std::shared_ptr<DecoderPerfSample>> decoder_perf_samples;
extern std::unordered_map<std::string, std::string> g_decoder_error_messages;
extern std::mutex g_seek_info_mutex;
extern std::mutex g_decoder_perf_mutex;
extern std::mutex g_decoder_error_mutex;
#endif
