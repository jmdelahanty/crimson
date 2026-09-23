#include "decoder.h"
#include "frame_slot.h"
#include "global.h"
#include "Logger.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();
std::unordered_map<std::string, std::atomic<bool>> window_need_decoding;
std::unordered_map<std::string, std::atomic<int>> latest_decoded_frame;
std::unordered_map<std::string, std::shared_ptr<DecoderPerfSample>> decoder_perf_samples;
std::unordered_map<std::string, std::string> g_decoder_error_messages;
std::mutex g_seek_info_mutex, g_decoder_perf_mutex, g_decoder_error_mutex;

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}

int main() {
    try {
        constexpr auto camera = "worker-failure-fixture";
        const auto missing_root = std::filesystem::temp_directory_path() /
            ("crimson-missing-image-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        require(!std::filesystem::exists(missing_root), "fixture must not exist");
        std::vector<unsigned char> storage(16);
        PictureBuffer slot{};
        slot.frame = storage.data();
        slot.frame_bytes = storage.size();
        slot.available_to_write = true;
        slot.frame_number = -1;
        slot.format = FramePixelFormat::RGBA8;
        frameSlotInitialize(slot);
        SeekInfo seek{};
        g_decoder_error_messages["unrelated"] = "keep other worker errors";

        // Repeat failure -> clear/reopen. No worker thread may leak an exception,
        // publish an invalid image, retain its write lease, or leave stale progress.
        for (int attempt = 0; attempt != 2; ++attempt) {
            DecoderContext context{};
            window_need_decoding[camera].store(true);
            latest_decoded_frame[camera].store(100);
            std::thread failed([&] {
                image_loader(&context, {"missing.png"}, &slot, 1, &seek, true,
                             camera, missing_root.string());
            });
            failed.join();
            require(!g_decoder_error_messages.at(camera).empty(), "failure is reported");
            require(!window_need_decoding.at(camera).load(), "failed worker demand disabled");
            require(latest_decoded_frame.at(camera).load() == -1, "progress invalidated");
            require(frameSlotIsWritable(slot), "exception releases the write lease");
            require(!frameSlotSnapshotReadable(slot), "invalid frame was not published");
            context.stop_flag = true;
            std::thread reopened([&] {
                image_loader(&context, {}, &slot, 1, &seek, true, camera,
                             missing_root.string());
            });
            reopened.join();
            require(g_decoder_error_messages.count(camera) == 0, "reopen clears own error");
            require(g_decoder_error_messages.count("unrelated") == 1,
                    "reopen preserves other workers' errors");
        }
        frameSlotDestroy(slot);
        std::cout << "Worker failure, lease unwind, and reopen tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
