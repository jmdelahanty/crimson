#include <cuda.h>

#include "Logger.h"
#include "NvCodecUtils.h"
#include "FFmpegDemuxer.h"
#include "NvDecoder.h"
#include "AppDecUtils.h"
#include "json.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif

simplelogger::Logger *logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

struct Args {
    std::filesystem::path video_path;
    std::filesystem::path output_json;
    int frames = 160;
    int gpu = 0;
    bool probe_keyframe_interval = false;
};

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void usage(const char *argv0) {
    std::cerr
        << "Usage: " << argv0 << " VIDEO.mp4 --output-json PATH [--frames N] [--gpu N]"
        << " [--probe-keyframe-interval]\n"
        << "\n"
        << "Headless Crimson FFmpeg/NVDEC sequential decode timing smoke.\n";
}

int parse_int(const std::string &value, const char *name) {
    std::size_t consumed = 0;
    int parsed = 0;
    try {
        parsed = std::stoi(value, &consumed);
    } catch (const std::exception &) {
        throw std::invalid_argument(std::string(name) + " must be an integer");
    }
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string(name) + " must be an integer");
    }
    return parsed;
}

Args parse_args(int argc, char **argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string token(argv[i]);
        auto require_value = [&](const char *name) -> std::string {
            if (++i >= argc) {
                throw std::invalid_argument(std::string(name) + " requires a value");
            }
            return std::string(argv[i]);
        };
        if (token == "-h" || token == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (token == "--output-json") {
            args.output_json = require_value("--output-json");
        } else if (token == "--frames") {
            args.frames = parse_int(require_value("--frames"), "--frames");
        } else if (token == "--gpu") {
            args.gpu = parse_int(require_value("--gpu"), "--gpu");
        } else if (token == "--probe-keyframe-interval") {
            args.probe_keyframe_interval = true;
        } else if (token.rfind("-", 0) == 0) {
            throw std::invalid_argument("unknown option: " + token);
        } else if (args.video_path.empty()) {
            args.video_path = token;
        } else {
            throw std::invalid_argument("unexpected positional argument: " + token);
        }
    }
    if (args.video_path.empty()) {
        throw std::invalid_argument("missing VIDEO.mp4 path");
    }
    if (args.output_json.empty()) {
        throw std::invalid_argument("missing --output-json PATH");
    }
    if (args.frames <= 0) {
        throw std::invalid_argument("--frames must be positive");
    }
    if (args.gpu < 0) {
        throw std::invalid_argument("--gpu must be non-negative");
    }
    return args;
}

std::string cuda_device_name(int gpu) {
    CUdevice device = 0;
    ck(cuDeviceGet(&device, gpu));
    char name[256] = {};
    ck(cuDeviceGetName(name, sizeof(name), device));
    return std::string(name);
}

std::string codec_name(cudaVideoCodec codec) {
    switch (codec) {
    case cudaVideoCodec_H264:
        return "h264";
    case cudaVideoCodec_HEVC:
        return "hevc";
    case cudaVideoCodec_VP9:
        return "vp9";
    case cudaVideoCodec_AV1:
        return "av1";
    case cudaVideoCodec_MPEG1:
        return "mpeg1";
    case cudaVideoCodec_MPEG2:
        return "mpeg2";
    case cudaVideoCodec_MPEG4:
        return "mpeg4";
    default:
        return "unknown";
    }
}

json run_smoke(const Args &args) {
    const auto total_start = Clock::now();
    json payload;
    payload["status"] = "error";
    payload["decoder_backend"] = "crimson_ffmpeg_nvdec";
    payload["crimson_git_commit"] = CRIMSON_GIT_COMMIT;
    payload["video_path"] = args.video_path.string();
    payload["frames_requested"] = args.frames;
    payload["gpu_index"] = args.gpu;

    ck(cuInit(0));
    payload["gpu_name"] = cuda_device_name(args.gpu);

    const auto open_start = Clock::now();
    std::map<std::string, std::string> ffmpeg_options;
    FFmpegDemuxer demuxer(args.video_path.string().c_str(), ffmpeg_options);
    const double open_seconds = seconds_since(open_start);

    payload["open_seconds"] = open_seconds;
    payload["width"] = demuxer.GetWidth();
    payload["height"] = demuxer.GetHeight();
    payload["fps"] = demuxer.GetFramerate();
    payload["duration_seconds"] = demuxer.GetDuration();
    payload["container_reported_frames"] = demuxer.GetNumFrames();
    payload["keyframe_interval_probe_enabled"] =
        args.probe_keyframe_interval;
    if (args.probe_keyframe_interval) {
        payload["keyframe_interval"] = demuxer.FindKeyFrameInterval();
    }

    CUcontext cu_context = nullptr;
    const auto init_start = Clock::now();
    createCudaContext(&cu_context, args.gpu, 0);
    const cudaVideoCodec codec = FFmpeg2NvCodecId(demuxer.GetVideoCodec());
    NvDecoder decoder(cu_context, true, codec);
    const double decoder_init_seconds = seconds_since(init_start);

    payload["codec"] = codec_name(codec);
    payload["decoder_init_seconds"] = decoder_init_seconds;

    int frames_decoded = 0;
    int packets_demuxed = 0;
    int64_t first_packet_frame = -1;
    PacketData packet_data;
    uint8_t *video = nullptr;
    size_t video_bytes = 0;

    const auto decode_start = Clock::now();
    while (frames_decoded < args.frames &&
           demuxer.Demux(video, video_bytes, packet_data)) {
        ++packets_demuxed;
        if (packets_demuxed == 1) {
            const int64_t timestamp =
                packet_data.pts != AV_NOPTS_VALUE ? packet_data.pts
                                                  : packet_data.dts;
            if (timestamp != AV_NOPTS_VALUE) {
                first_packet_frame = demuxer.FrameNumberFromTs(timestamp);
            }
        }
        decoder.Decode(video, static_cast<int>(video_bytes), 0, packet_data.pts);
        int64_t timestamp = 0;
        while (frames_decoded < args.frames && decoder.GetFrame(&timestamp)) {
            ++frames_decoded;
        }
    }
    if (frames_decoded < args.frames) {
        decoder.Decode(nullptr, 0, 0, 0);
        int64_t timestamp = 0;
        while (frames_decoded < args.frames && decoder.GetFrame(&timestamp)) {
            ++frames_decoded;
        }
    }
    ck(cuCtxSynchronize());
    const double decode_seconds = seconds_since(decode_start);
    const double total_seconds = seconds_since(total_start);

    payload["packets_demuxed"] = packets_demuxed;
    payload["first_packet_frame"] = first_packet_frame;
    payload["frames_decoded"] = frames_decoded;
    payload["decode_seconds"] = decode_seconds;
    payload["total_seconds"] = total_seconds;
    payload["end_to_end_fps"] =
        total_seconds > 0.0 ? static_cast<double>(frames_decoded) / total_seconds : 0.0;
    payload["decode_fps"] =
        decode_seconds > 0.0 ? static_cast<double>(frames_decoded) / decode_seconds : 0.0;
    if (args.probe_keyframe_interval && first_packet_frame != 0) {
        throw std::runtime_error(
            "keyframe interval probe did not restore demuxer to frame 0; "
            "first packet frame was " + std::to_string(first_packet_frame));
    }
    payload["status"] = "ok";
    return payload;
}

void write_json(const std::filesystem::path &path, const json &payload) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to open output JSON: " + path.string());
    }
    out << payload.dump(2) << "\n";
}

} // namespace

int main(int argc, char **argv) {
    std::optional<std::filesystem::path> output_json;
    try {
        Args args = parse_args(argc, argv);
        output_json = args.output_json;
        json payload = run_smoke(args);
        write_json(args.output_json, payload);
        std::cout << payload.dump(2) << std::endl;
        return 0;
    } catch (const std::exception &exc) {
        json payload;
        payload["status"] = "error";
        payload["decoder_backend"] = "crimson_ffmpeg_nvdec";
        payload["crimson_git_commit"] = CRIMSON_GIT_COMMIT;
        payload["error"] = exc.what();
        if (output_json) {
            try {
                write_json(*output_json, payload);
            } catch (const std::exception &write_exc) {
                std::cerr << "failed to write error JSON: " << write_exc.what() << "\n";
            }
        }
        std::cerr << exc.what() << "\n";
        return 1;
    }
}
