#include "perf_log_writer.h"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

std::filesystem::path
makeAutoAppendedPerfPath(const std::filesystem::path &requested_path) {
  std::error_code error;
  if (!std::filesystem::exists(requested_path, error) || error) {
    return requested_path;
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#ifdef _WIN32
  localtime_s(&local_time, &now_time);
#else
  localtime_r(&now_time, &local_time);
#endif
  std::ostringstream timestamp;
  timestamp << std::put_time(&local_time, "%Y%m%d-%H%M%S");

  const auto parent = requested_path.parent_path();
  const auto stem = requested_path.stem().string();
  const auto extension = requested_path.extension().string();
  for (int attempt = 0; attempt < 1000; ++attempt) {
    std::ostringstream candidate_name;
    candidate_name << stem << '-' << timestamp.str();
    if (attempt > 0) {
      candidate_name << '-' << attempt;
    }
    candidate_name << extension;
    const auto candidate = parent / candidate_name.str();
    std::error_code candidate_error;
    if (!std::filesystem::exists(candidate, candidate_error) ||
        candidate_error) {
      return candidate;
    }
  }

  return requested_path;
}

} // namespace

bool PerfLogWriter::open(const std::filesystem::path &output_path) {
  close();
  if (output_path.empty()) {
    return false;
  }
  csv_path = makeAutoAppendedPerfPath(output_path);
  metadata_path = csv_path;
  metadata_path.replace_extension(".meta.json");
  std::error_code error;
  if (csv_path.has_parent_path()) {
    std::filesystem::create_directories(csv_path.parent_path(), error);
    if (error) {
      std::cerr << "[PerfLog] Failed to create parent directory for "
                << csv_path << ": " << error.message() << std::endl;
      return false;
    }
  }
  stream.open(csv_path, std::ios::out | std::ios::trunc);
  if (!stream.is_open()) {
    std::cerr << "[PerfLog] Failed to open " << csv_path << " for writing"
              << std::endl;
    return false;
  }
  start_steady = std::chrono::steady_clock::now();
  last_sample_steady = start_steady;
  stream << std::fixed << std::setprecision(3);
  stream
      << "elapsed_s,wall_epoch_ms,play_video,set_playback_speed,inst_speed,"
      << "video_fps,requested_camera_frame,displayed_camera_frame,current_"
         "frame_num,"
      << "min_decoded_camera_frame,camera_decode_gap_frames,"
      << "playback_start_warmup_active,frames_since_playback_start,"
      << "playback_start_frame,playback_resume_path,"
      << "playback_resume_target_frame,"
      << "camera_decode_demux_ms,camera_decode_decode_ms,"
      << "camera_decode_convert_ms,camera_decode_wait_ms,"
      << "camera_decode_write_ms,camera_decode_pipeline_ms,"
      << "camera_decode_packet_ms,camera_decode_returned,"
      << "camera_decode_demux_success,camera_decode_published_frame,"
      << "camera_decode_sample_sequence,"
      << "visible_camera_count,"
      << "bbox_query_frame,bbox_loaded_count,bbox_display_count,"
      << "bbox_get_boxes_ms,bbox_edit_resolve_ms,"
      << "bbox_get_raw_detections_ms,bbox_load_total_ms,"
      << "bbox_overlay_build_ms,bbox_overlay_draw_ms,"
      << "bbox_overlay_item_count,"
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
      << "camera_playback_prewarm_total_ms,"
      << "camera_playback_prewarm_upload_ms,"
      << "camera_playback_prewarm_count,"
      << "camera_playback_swap_ms,"
      << "camera_plot_image_ui_ms,camera_overlay_ui_ms,camera_scene_ui_ms,"
      << "file_browser_ui_ms,frame_debug_ui_ms,buffer_window_ui_ms,"
      << "crop_preview_ui_ms,stimulus_buffer_window_ui_ms,"
      << "keypoints_window_ui_ms,labeling_tool_ui_ms,"
      << "stimulus_window_ui_ms,stimulus_timeline_ui_ms,movement_timeline_ui_"
         "ms,"
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

void PerfLogWriter::close() {
  if (stream.is_open()) {
    stream.flush();
    stream.close();
  }
}

bool MaskPerfLogWriter::open(const std::filesystem::path &output_path) {
  close();
  if (output_path.empty()) {
    return false;
  }
  jsonl_path = output_path;
  std::error_code error;
  if (jsonl_path.has_parent_path()) {
    std::filesystem::create_directories(jsonl_path.parent_path(), error);
    if (error) {
      std::cerr << "[MaskPerfLog] Failed to create parent directory for "
                << jsonl_path << ": " << error.message() << std::endl;
      return false;
    }
  }
  stream.open(jsonl_path, std::ios::out | std::ios::trunc);
  if (!stream.is_open()) {
    std::cerr << "[MaskPerfLog] Failed to open " << jsonl_path << " for writing"
              << std::endl;
    return false;
  }
  samples_since_flush = 0;
  last_flush_steady = std::chrono::steady_clock::now();
  std::cout << "[MaskPerfLog] Writing JSONL samples to " << jsonl_path
            << std::endl;
  return true;
}

bool MaskPerfLogWriter::enabled() const { return stream.is_open(); }

void MaskPerfLogWriter::close() {
  if (stream.is_open()) {
    stream.flush();
    stream.close();
  }
}

std::filesystem::path
defaultMaskPerfLogPath(const std::filesystem::path &default_buffer_dump_root) {
  return default_buffer_dump_root / "mask_perf_latest.jsonl";
}
