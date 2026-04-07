#include "perf_logging.h"

#include "global.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>

namespace {

using json = nlohmann::json;

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::filesystem::path makeAutoAppendedPerfPath(
    const std::filesystem::path& requested_path) {
    std::error_code ec;
    if (!std::filesystem::exists(requested_path, ec) || ec) {
        return requested_path;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif
    std::ostringstream timestamp_stream;
    timestamp_stream << std::put_time(&local_tm, "%Y%m%d-%H%M%S");

    const std::filesystem::path parent = requested_path.parent_path();
    const std::string stem = requested_path.stem().string();
    const std::string extension = requested_path.extension().string();

    for (int attempt = 0; attempt < 1000; ++attempt) {
        std::ostringstream candidate_name;
        candidate_name << stem << "-" << timestamp_stream.str();
        if (attempt > 0) {
            candidate_name << "-" << attempt;
        }
        candidate_name << extension;
        const std::filesystem::path candidate = parent / candidate_name.str();
        std::error_code candidate_ec;
        if (!std::filesystem::exists(candidate, candidate_ec) || candidate_ec) {
            return candidate;
        }
    }

    return requested_path;
}

int countBufferedStimulusFrames(const StimulusPlayback& stim) {
    if (!stim.display_buffer || stim.buffer_size <= 0) {
        return 0;
    }
    int valid = 0;
    for (int i = 0; i < stim.buffer_size; ++i) {
        const auto& slot = stim.display_buffer[i];
        if (!slot.available_to_write && slot.frame_number >= 0) {
            ++valid;
        }
    }
    return valid;
}

void updateMaxFinite(double& dst, double value) {
    if (!std::isfinite(value)) {
        return;
    }
    if (!std::isfinite(dst) || value > dst) {
        dst = value;
    }
}

}  // namespace

bool PerfLogWriter::open(const std::filesystem::path& output_path) {
    if (output_path.empty()) {
        return false;
    }
    csv_path = makeAutoAppendedPerfPath(output_path);
    metadata_path = csv_path;
    metadata_path.replace_extension(".meta.json");
    std::error_code ec;
    if (csv_path.has_parent_path()) {
        std::filesystem::create_directories(csv_path.parent_path(), ec);
        if (ec) {
            std::cerr << "[PerfLog] Failed to create parent directory for "
                      << csv_path << ": " << ec.message() << std::endl;
            return false;
        }
    }
    stream.open(csv_path, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        std::cerr << "[PerfLog] Failed to open " << csv_path
                  << " for writing" << std::endl;
        return false;
    }
    start_steady = std::chrono::steady_clock::now();
    last_sample_steady = start_steady;
    stream << std::fixed << std::setprecision(3);
    stream
        << "elapsed_s,wall_epoch_ms,play_video,set_playback_speed,inst_speed,"
        << "video_fps,requested_camera_frame,displayed_camera_frame,current_frame_num,"
        << "min_decoded_camera_frame,camera_decode_gap_frames,"
        << "camera_decode_convert_ms,camera_decode_wait_ms,"
        << "camera_decode_write_ms,camera_decode_pipeline_ms,"
        << "visible_camera_count,"
        << "main_buffer_mode,playback_preview_scale,playback_preview_active,"
        << "playback_renderer_mode,"
        << "camera_viewport_width_px,camera_viewport_height_px,"
        << "camera_view_x_min,camera_view_x_max,"
        << "camera_view_y_min,camera_view_y_max,"
        << "camera_view_visible_fraction,camera_view_zoomed_in,"
        << "camera_upload_count,camera_upload_ms,camera_texture_resize_ms,"
        << "camera_preview_resize_ms,camera_display_convert_ms,"
        << "camera_pbo_copy_ms,camera_texture_upload_ms,"
        << "camera_playback_front_path_ms,"
        << "camera_playback_stage_total_ms,"
        << "camera_playback_stage_upload_ms,"
        << "camera_playback_swap_ms,"
        << "camera_plot_image_ui_ms,camera_overlay_ui_ms,camera_scene_ui_ms,"
        << "file_browser_ui_ms,frame_debug_ui_ms,buffer_window_ui_ms,"
        << "crop_preview_ui_ms,stimulus_buffer_window_ui_ms,"
        << "keypoints_window_ui_ms,labeling_tool_ui_ms,"
        << "stimulus_window_ui_ms,stimulus_timeline_ui_ms,movement_timeline_ui_ms,"
        << "help_menu_ui_ms,"
        << "gl_draw_ms,swap_ms,frame_loop_ms,ui_build_ms,imgui_render_ms,"
        << "imgui_draw_cmd_count,imgui_draw_list_count,imgui_total_vtx_count,"
        << "imgui_total_idx_count,"
        << "stimulus_loaded,stimulus_decode_backend,"
        << "stimulus_buffer_mode,stimulus_target_frame,stimulus_latest_decoded,"
        << "stimulus_last_displayed,stimulus_buffered_frames,"
        << "stimulus_progress_gap_frames\n";
    stream.flush();
    if (csv_path != output_path) {
        std::cout << "[PerfLog] Requested path exists; auto-appended to "
                  << csv_path << std::endl;
    }
    std::cout << "[PerfLog] Writing CSV samples to " << csv_path << std::endl;
    std::cout << "[PerfLog] Writing metadata sidecar to " << metadata_path
              << std::endl;
    return true;
}

bool PerfLogWriter::enabled() const { return stream.is_open(); }

void maybeWritePerfLogSample(PerfLogWriter& writer,
                             const PerfLogFrameContext& context,
                             std::chrono::milliseconds sample_period) {
    if (!writer.enabled()) {
        return;
    }

    const auto now_steady = std::chrono::steady_clock::now();
    if (now_steady - writer.last_sample_steady < sample_period) {
        return;
    }
    writer.last_sample_steady = now_steady;

    const auto now_system = std::chrono::system_clock::now();
    const auto wall_epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now_system.time_since_epoch())
            .count();

    int visible_camera_count = 0;
    for (const auto& cam_name : context.camera_names) {
        auto it = window_need_decoding.find(cam_name);
        if (it != window_need_decoding.end() && it->second.load()) {
            visible_camera_count++;
        }
    }

    const int camera_decode_gap_frames =
        (context.perf_requested_camera_frame >= 0 &&
         context.perf_min_decoded_camera_frame >= 0)
            ? (context.perf_requested_camera_frame -
               context.perf_min_decoded_camera_frame)
            : -1;

    double perf_camera_decode_convert_ms =
        std::numeric_limits<double>::quiet_NaN();
    double perf_camera_decode_wait_ms =
        std::numeric_limits<double>::quiet_NaN();
    double perf_camera_decode_write_ms =
        std::numeric_limits<double>::quiet_NaN();
    double perf_camera_decode_pipeline_ms =
        std::numeric_limits<double>::quiet_NaN();
    {
        std::lock_guard<std::mutex> lock(g_decoder_perf_mutex);
        for (const auto& cam_name : context.camera_names) {
            auto need_it = window_need_decoding.find(cam_name);
            if (need_it == window_need_decoding.end() || !need_it->second.load()) {
                continue;
            }
            auto perf_it = decoder_perf_samples.find(cam_name);
            if (perf_it == decoder_perf_samples.end() || !perf_it->second) {
                continue;
            }
            const auto& perf = perf_it->second;
            updateMaxFinite(perf_camera_decode_convert_ms,
                            perf->nv12_to_rgba_ms.load());
            updateMaxFinite(perf_camera_decode_wait_ms,
                            perf->buffer_wait_ms.load());
            updateMaxFinite(perf_camera_decode_write_ms,
                            perf->frame_write_ms.load());
            updateMaxFinite(perf_camera_decode_pipeline_ms,
                            perf->frame_total_ms.load());
        }
    }

    const bool stimulus_loaded =
        context.stimulus_player != nullptr && context.stimulus_player->loaded;
    const int stimulus_last_displayed =
        context.stimulus_player != nullptr
            ? context.stimulus_player->last_displayed_frame
            : -1;
    const int stimulus_progress_frame =
        std::max(context.stimulus_latest_decoded, stimulus_last_displayed);
    const int stimulus_progress_gap_frames =
        (context.stimulus_target_frame >= 0 && stimulus_progress_frame >= 0)
            ? (context.stimulus_target_frame - stimulus_progress_frame)
            : -1;
    const int stimulus_buffered_frames =
        context.stimulus_player != nullptr
            ? countBufferedStimulusFrames(*context.stimulus_player)
            : 0;
    const int stimulus_buffer_size =
        stimulus_loaded ? context.stimulus_player->buffer_size
                        : context.stimulus_buffer_size_fallback;
    const bool stimulus_use_software_decode =
        stimulus_loaded ? context.stimulus_player->use_software_decode
                        : context.stimulus_use_software_decode_fallback;
    const bool stimulus_use_cpu_buffer =
        stimulus_loaded ? context.stimulus_player->use_cpu_buffer
                        : context.stimulus_use_cpu_buffer_fallback;

    const double elapsed_s =
        std::chrono::duration<double>(now_steady - writer.start_steady).count();
    const double frame_loop_ms = durationMs(now_steady - context.frame_loop_start);

    writer.stream
        << elapsed_s << "," << wall_epoch_ms << ","
        << (context.play_video ? 1 : 0) << "," << context.set_playback_speed
        << "," << context.inst_speed << "," << context.video_fps << ","
        << context.perf_requested_camera_frame << ","
        << context.displayed_camera_frame << "," << context.current_frame_num
        << "," << context.perf_min_decoded_camera_frame << ","
        << camera_decode_gap_frames << ","
        << perf_camera_decode_convert_ms << ","
        << perf_camera_decode_wait_ms << ","
        << perf_camera_decode_write_ms << ","
        << perf_camera_decode_pipeline_ms << ","
        << visible_camera_count << ","
        << (context.scene_use_cpu_buffer ? "cpu" : "gpu") << ","
        << context.playback_preview_scale_label << ","
        << (context.playback_preview_active ? 1 : 0) << ","
        << context.playback_renderer_mode_label << ","
        << context.perf_camera_viewport_width_px << ","
        << context.perf_camera_viewport_height_px << ","
        << context.perf_camera_view_x_min << ","
        << context.perf_camera_view_x_max << ","
        << context.perf_camera_view_y_min << ","
        << context.perf_camera_view_y_max << ","
        << context.perf_camera_view_visible_fraction << ","
        << context.perf_camera_view_zoomed_in << ","
        << context.frame_camera_upload_count << ","
        << context.frame_camera_upload_ms << ","
        << context.frame_camera_texture_resize_ms << ","
        << context.frame_camera_preview_resize_ms << ","
        << context.frame_camera_display_convert_ms << ","
        << context.frame_camera_pbo_copy_ms << ","
        << context.frame_camera_texture_upload_ms << ","
        << context.frame_camera_playback_front_path_ms << ","
        << context.frame_camera_playback_stage_total_ms << ","
        << context.frame_camera_playback_stage_upload_ms << ","
        << context.frame_camera_playback_swap_ms << ","
        << context.frame_camera_plot_image_ui_ms << ","
        << context.frame_camera_overlay_ui_ms << ","
        << context.frame_camera_scene_ui_ms << ","
        << context.frame_file_browser_ui_ms << ","
        << context.frame_frame_debug_ui_ms << ","
        << context.frame_buffer_window_ui_ms << ","
        << context.frame_crop_preview_ui_ms << ","
        << context.frame_stimulus_buffer_window_ui_ms << ","
        << context.frame_keypoints_window_ui_ms << ","
        << context.frame_labeling_tool_ui_ms << ","
        << context.frame_stimulus_window_ui_ms << ","
        << context.frame_stimulus_timeline_ui_ms << ","
        << context.frame_movement_timeline_ui_ms << ","
        << context.frame_help_menu_ui_ms << ","
        << context.frame_gl_draw_ms << ","
        << context.frame_swap_ms << "," << frame_loop_ms << ","
        << context.frame_ui_build_ms << ","
        << context.frame_imgui_render_ms << ","
        << context.frame_imgui_draw_cmd_count << ","
        << context.frame_imgui_draw_list_count << ","
        << context.frame_imgui_total_vtx_count << ","
        << context.frame_imgui_total_idx_count << ","
        << (stimulus_loaded ? 1 : 0) << ","
        << (stimulus_use_software_decode ? "software" : "gpu") << ","
        << (stimulus_use_cpu_buffer ? "cpu" : "gpu") << ","
        << context.stimulus_target_frame << ","
        << context.stimulus_latest_decoded << ","
        << stimulus_last_displayed << ","
        << stimulus_buffered_frames << ","
        << stimulus_progress_gap_frames << "\n";
    writer.stream.flush();

    json metadata = {
        {"format", "crimson_perf_metadata_v1"},
        {"generated_wall_epoch_ms", wall_epoch_ms},
        {"perf_csv_path", writer.csv_path.string()},
        {"cwd", context.cwd.string()},
        {"argv0_path", context.argv0_path.string()},
        {"recording_path", context.cli_recording_path.empty()
                               ? json(nullptr)
                               : json(context.cli_recording_path)},
        {"zarr_override_path", context.cli_zarr_override_path.empty()
                                   ? json(nullptr)
                                   : json(context.cli_zarr_override_path)},
        {"window",
         {{"swap_interval", context.window_swap_interval},
          {"width", context.window_width},
          {"height", context.window_height}}},
        {"main_video",
         {{"loaded", context.video_loaded},
          {"fps", context.video_fps},
          {"buffer_mode", context.scene_use_cpu_buffer ? "cpu" : "gpu"},
          {"buffer_storage_format",
           context.scene_use_cpu_buffer ? "rgba32" : "nv12"},
          {"playback_preview_scale", context.playback_preview_scale_label},
          {"playback_preview_active", context.playback_preview_active},
          {"playback_renderer_mode", context.playback_renderer_mode_label},
          {"viewport_width_px", context.perf_camera_viewport_width_px},
          {"viewport_height_px", context.perf_camera_viewport_height_px},
          {"view_x_min", context.perf_camera_view_x_min},
          {"view_x_max", context.perf_camera_view_x_max},
          {"view_y_min", context.perf_camera_view_y_min},
          {"view_y_max", context.perf_camera_view_y_max},
          {"view_visible_fraction", context.perf_camera_view_visible_fraction},
          {"view_zoomed_in", context.perf_camera_view_zoomed_in},
          {"buffer_size", context.video_loaded ? context.scene_buffer_size
                                               : context.label_buffer_size},
          {"requested_playback_speed", context.set_playback_speed},
          {"measured_playback_speed", context.inst_speed},
          {"requested_camera_frame", context.perf_requested_camera_frame},
          {"displayed_camera_frame", context.displayed_camera_frame},
          {"current_frame_num", context.current_frame_num},
          {"min_decoded_camera_frame", context.perf_min_decoded_camera_frame},
          {"camera_decode_gap_frames", camera_decode_gap_frames},
          {"visible_camera_count", visible_camera_count},
          {"camera_names", context.camera_names}}},
        {"stimulus",
         {{"loaded", stimulus_loaded},
          {"decode_backend",
           stimulus_use_software_decode ? "software" : "gpu"},
          {"buffer_mode", stimulus_use_cpu_buffer ? "cpu" : "gpu"},
          {"buffer_size", stimulus_buffer_size},
          {"target_frame", context.stimulus_target_frame},
          {"latest_decoded_frame", context.stimulus_latest_decoded},
          {"last_displayed_frame", stimulus_last_displayed},
          {"buffered_frames", stimulus_buffered_frames},
          {"progress_gap_frames", stimulus_progress_gap_frames}}}
    };

    if (!writer.metadata_path.empty()) {
        std::ofstream meta_stream(writer.metadata_path,
                                  std::ios::out | std::ios::trunc);
        if (!meta_stream.is_open()) {
            std::cerr << "[PerfLog] Failed to write metadata sidecar "
                      << writer.metadata_path << std::endl;
            return;
        }
        meta_stream << metadata.dump(2) << "\n";
    }
}
