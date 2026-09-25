#include "decode_debug_workflow.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace {

std::string sanitizePathComponent(std::string value) {
    if (value.empty()) {
        return "unnamed";
    }
    for (char& ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                        ch == '.';
        if (!ok) {
            ch = '_';
        }
    }
    return value;
}

size_t countCandidateSlots(bool video_loaded, const render_scene& scene) {
    if (!video_loaded || scene.num_cams <= 0 || scene.size_of_buffer <= 0) {
        return 0;
    }
    size_t count = 0;
    for (int cam_idx = 0; cam_idx < static_cast<int>(scene.num_cams); ++cam_idx) {
        for (int slot_idx = 0; slot_idx < static_cast<int>(scene.size_of_buffer);
             ++slot_idx) {
            const auto& slot = scene.cameras[cam_idx].display_buffer[slot_idx];
            if (!slot.available_to_write && slot.frame_number >= 0 &&
                slot.frame != nullptr) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

std::optional<std::filesystem::path> dumpDecodeBuffersToVideos(
    const DecodeDebugDumpContext& context,
    const std::string& tag,
    std::string& status) {
    if (!context.video_loaded || context.scene == nullptr ||
        context.scene->num_cams <= 0 || context.scene->size_of_buffer <= 0 ||
        context.camera_names == nullptr || context.window_need_decoding == nullptr ||
        context.playback_state == nullptr) {
        status = "Decode dump skipped: no decoded video buffers are available.";
        return std::nullopt;
    }

    const bool was_playing = context.playback_state->play_video;
    if (was_playing) {
        context.playback_state->play_video = false;
        context.playback_state->pause_selected = 0;
    }

    std::filesystem::path dump_root = context.default_buffer_dump_root;
    if (const char* env_dump_root = std::getenv("CRIMSON_BUFFER_DUMP_DIR")) {
        if (*env_dump_root != '\0') {
            dump_root = env_dump_root;
        }
    }

    const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const std::filesystem::path dump_dir =
        dump_root / (sanitizePathComponent(tag) + "_" + std::to_string(epoch_ms));

    std::error_code mkdir_ec;
    std::filesystem::create_directories(dump_dir, mkdir_ec);
    if (mkdir_ec) {
        status = "Decode dump failed to create directory: " + dump_dir.string();
        return std::nullopt;
    }

    std::unordered_map<std::string, bool> prior_decode_requests;
    prior_decode_requests.reserve(context.camera_names->size());
    for (const auto& camera_name : *context.camera_names) {
        auto it = context.window_need_decoding->find(camera_name);
        if (it == context.window_need_decoding->end()) {
            continue;
        }
        prior_decode_requests[camera_name] = it->second.load();
        it->second.store(false);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    bool wrote_any_frames = false;
    size_t total_frames_written = 0;
    size_t total_candidate_frames = 0;

    for (int cam_idx = 0; cam_idx < static_cast<int>(context.scene->num_cams);
         ++cam_idx) {
        if (cam_idx >= static_cast<int>(context.camera_names->size())) {
            continue;
        }
        const int width = context.scene->cameras[cam_idx].image_width;
        const int height = context.scene->cameras[cam_idx].image_height;
        if (width <= 0 || height <= 0) {
            continue;
        }

        struct BufferSample {
            int slot = -1;
            int frame_number = -1;
            bool available = true;
            bool has_frame_ptr = false;
        };

        std::vector<BufferSample> all_slots;
        all_slots.reserve(context.scene->size_of_buffer);
        std::vector<BufferSample> samples;
        samples.reserve(context.scene->size_of_buffer);
        for (int slot_idx = 0; slot_idx < static_cast<int>(context.scene->size_of_buffer);
             ++slot_idx) {
            const auto& slot =
                context.scene->cameras[cam_idx].display_buffer[slot_idx];
            BufferSample sample;
            sample.slot = slot_idx;
            sample.frame_number = slot.frame_number;
            sample.available = slot.available_to_write;
            sample.has_frame_ptr = slot.frame != nullptr;
            all_slots.push_back(sample);

            if (sample.available || sample.frame_number < 0 ||
                !sample.has_frame_ptr) {
                continue;
            }
            samples.push_back(sample);
        }

        const std::string camera_stem =
            sanitizePathComponent((*context.camera_names)[cam_idx]);
        std::ofstream slot_file((dump_dir / (camera_stem + "_slots.txt")).string());
        slot_file << "# slot_index,frame_number,available_to_write,has_frame_ptr\n";
        for (const auto& sample : all_slots) {
            slot_file << sample.slot << "," << sample.frame_number << ","
                      << (sample.available ? 1 : 0) << ","
                      << (sample.has_frame_ptr ? 1 : 0) << "\n";
        }

        if (samples.empty()) {
            continue;
        }

        total_candidate_frames += samples.size();
        std::sort(samples.begin(), samples.end(),
                  [](const BufferSample& a, const BufferSample& b) {
                      if (a.frame_number == b.frame_number) {
                          return a.slot < b.slot;
                      }
                      return a.frame_number < b.frame_number;
                  });

        std::filesystem::path video_path = dump_dir / (camera_stem + ".avi");
        cv::VideoWriter writer;
        const double fps_for_dump = (context.video_fps > 0.0) ? context.video_fps
                                                              : 30.0;
        writer.open(video_path.string(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                    fps_for_dump, cv::Size(width, height), true);
        if (!writer.isOpened()) {
            video_path = dump_dir / (camera_stem + ".mp4");
            writer.open(video_path.string(),
                        cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                        fps_for_dump, cv::Size(width, height), true);
        }
        const bool video_writer_enabled = writer.isOpened();
        if (!video_writer_enabled) {
            std::cerr << "[DecodeDebug] VideoWriter unavailable for camera "
                      << (*context.camera_names)[cam_idx]
                      << "; dumping PNG frames only." << std::endl;
        }

        std::ofstream mapping_file(
            (dump_dir / (camera_stem + "_frames.txt")).string());
        mapping_file << "# dump_frame_index,source_frame_number,slot_index,png_path\n";

        const size_t bytes = static_cast<size_t>(width) *
                             static_cast<size_t>(height) * 4;
        std::vector<uint8_t> rgba_bytes(bytes);

        size_t local_written = 0;
        for (const auto& sample : samples) {
            if (sample.slot < 0 ||
                sample.slot >= static_cast<int>(context.scene->size_of_buffer)) {
                continue;
            }
            const auto& slot =
                context.scene->cameras[cam_idx].display_buffer[sample.slot];
            const int frame_before = slot.frame_number;
            const bool available_before = slot.available_to_write;
            if (available_before || frame_before < 0 || !slot.frame) {
                continue;
            }

            if (context.scene->use_cpu_buffer) {
                std::memcpy(rgba_bytes.data(), slot.frame, bytes);
            } else {
                cudaError_t copy_status =
                    cudaMemcpy(rgba_bytes.data(), slot.frame, bytes,
                               cudaMemcpyDeviceToHost);
                if (copy_status != cudaSuccess) {
                    std::cerr << "[DecodeDebug] cudaMemcpy failed while dumping slot "
                              << sample.slot << " for camera "
                              << (*context.camera_names)[cam_idx] << std::endl;
                    continue;
                }
            }

            const int frame_after = slot.frame_number;
            const bool available_after = slot.available_to_write;
            if (available_after || frame_after != frame_before) {
                continue;
            }

            cv::Mat rgba_view(height, width, CV_8UC4, rgba_bytes.data(),
                              static_cast<size_t>(width) * 4);
            cv::Mat bgr_view;
            cv::cvtColor(rgba_view, bgr_view, cv::COLOR_RGBA2BGR);

            std::ostringstream png_name;
            png_name << camera_stem << "_f" << frame_after << "_slot"
                     << sample.slot << ".png";
            const std::filesystem::path png_path = dump_dir / png_name.str();
            bool png_ok = false;
            try {
                png_ok = cv::imwrite(png_path.string(), bgr_view);
            } catch (const std::exception& e) {
                std::cerr << "[DecodeDebug] Failed to write " << png_path
                          << ": " << e.what() << std::endl;
                png_ok = false;
            }

            bool wrote_sample = png_ok;
            if (video_writer_enabled) {
                writer.write(bgr_view);
                wrote_sample = true;
            }

            if (!wrote_sample) {
                continue;
            }
            mapping_file << local_written << "," << frame_after << ","
                         << sample.slot << ","
                         << (png_ok ? png_name.str() : "") << "\n";
            ++local_written;
        }

        if (video_writer_enabled) {
            writer.release();
        }
        if (local_written > 0) {
            wrote_any_frames = true;
            total_frames_written += local_written;
        }
    }

    for (const auto& [camera_name, was_enabled] : prior_decode_requests) {
        auto it = context.window_need_decoding->find(camera_name);
        if (it != context.window_need_decoding->end()) {
            it->second.store(was_enabled);
        }
    }

    if (was_playing) {
        context.playback_state->play_video = true;
        context.playback_state->pause_seeked = false;
        context.playback_state->last_play_time_start =
            std::chrono::steady_clock::now();
    }

    if (!wrote_any_frames) {
        status = "Decode dump completed but no valid frames were captured: " +
                 dump_dir.string();
    } else {
        status = "Decode dump wrote " + std::to_string(total_frames_written) +
                 " frames (" + std::to_string(total_candidate_frames) +
                 " candidates) to " + dump_dir.string();
    }
    return dump_dir;
}

void randomSeekAndDumpBuffers(const RandomSeekDumpContext& context,
                              std::string& status) {
    if (context.decoder_context == nullptr || context.debug_rng == nullptr ||
        !context.seek_to_frame || !context.set_camera_decode_requests ||
        context.dump_context.scene == nullptr ||
        context.dump_context.window_need_decoding == nullptr ||
        context.dump_context.playback_state == nullptr) {
        status = "Random seek + dump skipped: missing workflow dependencies.";
        return;
    }

    const int max_frame =
        std::max(0, context.decoder_context->total_num_frame - 1);
    if (!context.dump_context.video_loaded || max_frame <= 0) {
        status = "Random seek + dump skipped: no loaded video timeline.";
        return;
    }

    std::uniform_int_distribution<int> frame_dist(0, max_frame);
    const int target_frame = frame_dist(*context.debug_rng);

    const bool was_playing = context.dump_context.playback_state->play_video;
    if (was_playing) {
        context.dump_context.playback_state->play_video = false;
        context.dump_context.playback_state->pause_selected = 0;
    }

    context.seek_to_frame(target_frame, false);

    std::unordered_map<std::string, bool> prior_decode_requests;
    prior_decode_requests.reserve(context.dump_context.window_need_decoding->size());
    for (auto& [camera_name, enabled] : *context.dump_context.window_need_decoding) {
        prior_decode_requests[camera_name] = enabled.load();
        enabled.store(true);
    }

    const size_t target_slots_per_camera = static_cast<size_t>(
        std::clamp(context.dump_context.scene->size_of_buffer, 4u, 8u));
    const size_t target_total_slots =
        target_slots_per_camera *
        static_cast<size_t>(std::max(1u, context.dump_context.scene->num_cams));
    const auto fill_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    while (std::chrono::steady_clock::now() < fill_deadline) {
        if (countCandidateSlots(context.dump_context.video_loaded,
                                *context.dump_context.scene) >=
            target_total_slots) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    dumpDecodeBuffersToVideos(
        context.dump_context, "random_seek_" + std::to_string(target_frame),
        status);

    if (was_playing) {
        context.set_camera_decode_requests(true);
        if (context.dump_context.stimulus_player != nullptr &&
            context.dump_context.stimulus_player->loaded) {
            (*context.dump_context.window_need_decoding)
                [context.dump_context.stimulus_player->window_name]
                    .store(true);
        }
    } else {
        for (const auto& [camera_name, was_enabled] : prior_decode_requests) {
            auto it =
                context.dump_context.window_need_decoding->find(camera_name);
            if (it != context.dump_context.window_need_decoding->end()) {
                it->second.store(was_enabled);
            }
        }
    }

    status = "Random seek target frame " + std::to_string(target_frame) +
             ". " + status;

    if (was_playing) {
        context.dump_context.playback_state->play_video = true;
        context.dump_context.playback_state->pause_seeked = false;
        context.dump_context.playback_state->last_play_time_start =
            std::chrono::steady_clock::now();
    }
}
