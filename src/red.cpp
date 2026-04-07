#include "IconsForkAwesome.h"
#include "Logger.h"
#include "camera.h"
#include "filesystem"
#include "global.h"
#include "gui.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "render.h"
#include "skeleton.h"
#include "utils.h"
#include "debug_flags.h"
#include "yolo_detection.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <cmath>
#include <chrono>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <numeric>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <ctime>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include "chained_crop_image_provider.h"
#include "live_crop_image_provider.h"
#include "refined_keypoint_repository.h"
#include "zarr_persisted_crop_provider.h"
#include "zarr_loader.h"
#include "gui/file_browser_window.h"
#include "gui/crop_preview_window.h"
#include "gui/frame_debug_window.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "gui/keypoints_window.h"
#include "gui/labeling_tool_window.h"
#include "gui/camera_view_overlay_renderer.h"
#include "gui/refined_keypoint_review_window.h"
#include "gui/refined_keypoint_write_workflow.h"
#include "gui/camera_view_manual_keypoint_input.h"
#include "gui/stimulus_playback_windows.h"
#include "gui/camera_view_transport_controls.h"
#include "gui_interpolation.h"
#include "gui/movement_timeline_window.h"
#include "gui/stimulus_event_timeline_window.h"
#include <opencv2/imgproc.hpp>

#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

simplelogger::Logger *logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

#include "ui_path_config.h"
#include "zarr_bbox_edit.h"

namespace {

struct Nv12PlaybackPresenter {
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLint luma_texture_location = -1;
    GLint chroma_texture_location = -1;
    GLint yuv_matrix_location = -1;
};

static GLuint compileGlShader(GLenum shader_type, const char *source) {
    GLuint shader = glCreateShader(shader_type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compile_status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
    if (compile_status != GL_TRUE) {
        GLint log_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        std::string log(static_cast<size_t>(std::max(log_length, 1)), '\0');
        glGetShaderInfoLog(shader, log_length, nullptr, log.data());
        std::cerr << "OpenGL shader compilation failed: " << log << std::endl;
    }
    return shader;
}

static void ensureNv12PlaybackPresenter(Nv12PlaybackPresenter *presenter) {
    if (presenter == nullptr || presenter->program != 0) {
        return;
    }

    static const char *kVertexShader = R"GLSL(
        #version 130
        attribute vec2 aPos;
        attribute vec2 aUV;
        varying vec2 vUV;

        void main() {
            vUV = aUV;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )GLSL";

    static const char *kFragmentShader = R"GLSL(
        #version 130
        uniform sampler2D uLumaTex;
        uniform sampler2D uChromaTex;
        uniform mat3 uYuvToRgb;
        varying vec2 vUV;

        void main() {
            float y = texture2D(uLumaTex, vUV).r * 255.0 - 16.0;
            vec2 uv = texture2D(uChromaTex, vUV).rg * 255.0 - vec2(128.0, 128.0);
            vec3 rgb = clamp(uYuvToRgb * vec3(y, uv), 0.0, 1.0);
            gl_FragColor = vec4(rgb, 1.0);
        }
    )GLSL";

    GLuint vertex_shader = compileGlShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment_shader =
        compileGlShader(GL_FRAGMENT_SHADER, kFragmentShader);

    presenter->program = glCreateProgram();
    glAttachShader(presenter->program, vertex_shader);
    glAttachShader(presenter->program, fragment_shader);
    glBindAttribLocation(presenter->program, 0, "aPos");
    glBindAttribLocation(presenter->program, 1, "aUV");
    glLinkProgram(presenter->program);

    GLint link_status = GL_FALSE;
    glGetProgramiv(presenter->program, GL_LINK_STATUS, &link_status);
    if (link_status != GL_TRUE) {
        GLint log_length = 0;
        glGetProgramiv(presenter->program, GL_INFO_LOG_LENGTH, &log_length);
        std::string log(static_cast<size_t>(std::max(log_length, 1)), '\0');
        glGetProgramInfoLog(presenter->program, log_length, nullptr, log.data());
        std::cerr << "OpenGL NV12 playback program link failed: " << log
                  << std::endl;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    presenter->luma_texture_location =
        glGetUniformLocation(presenter->program, "uLumaTex");
    presenter->chroma_texture_location =
        glGetUniformLocation(presenter->program, "uChromaTex");
    presenter->yuv_matrix_location =
        glGetUniformLocation(presenter->program, "uYuvToRgb");

    const float quad_vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };

    glGenVertexArrays(1, &presenter->vao);
    glBindVertexArray(presenter->vao);
    glGenBuffers(1, &presenter->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, presenter->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

static void fillNv12YuvToRgbMatrix(int color_matrix, float matrix_out[9]) {
    float wr = 0.2126f;
    float wb = 0.0722f;
    switch (color_matrix) {
    case ColorSpaceStandard_FCC:
        wr = 0.30f;
        wb = 0.11f;
        break;
    case ColorSpaceStandard_BT470:
    case ColorSpaceStandard_BT601:
        wr = 0.2990f;
        wb = 0.1140f;
        break;
    case ColorSpaceStandard_SMPTE240M:
        wr = 0.212f;
        wb = 0.087f;
        break;
    case ColorSpaceStandard_BT2020:
    case ColorSpaceStandard_BT2020C:
        wr = 0.2627f;
        wb = 0.0593f;
        break;
    case ColorSpaceStandard_BT709:
    default:
        break;
    }

    const float scale = 1.0f / 219.0f;
    const float one_minus_wb_wr = 1.0f - wb - wr;
    matrix_out[0] = scale;
    matrix_out[1] = 0.0f;
    matrix_out[2] = scale * ((1.0f - wr) / 0.5f);
    matrix_out[3] = scale;
    matrix_out[4] =
        scale * (-wb * (1.0f - wb) / 0.5f / one_minus_wb_wr);
    matrix_out[5] =
        scale * (-wr * (1.0f - wr) / 0.5f / one_minus_wb_wr);
    matrix_out[6] = scale;
    matrix_out[7] = scale * ((1.0f - wb) / 0.5f);
    matrix_out[8] = 0.0f;
}

static void presentNv12PboToTexture(const CameraResources &camera,
                                    const PBO_CUDA &nv12_pbo,
                                    GLuint destination_texture,
                                    Nv12PlaybackPresenter *presenter,
                                    int nv12_pitch_bytes,
                                    int color_matrix) {
    ensureNv12PlaybackPresenter(presenter);

    const int image_width = static_cast<int>(camera.image_width);
    const int image_height = static_cast<int>(camera.image_height);
    const int chroma_width = (image_width + 1) / 2;
    const int chroma_height = (image_height + 1) / 2;
    const intptr_t chroma_offset =
        static_cast<intptr_t>(nv12_pitch_bytes) * image_height;

    GLint previous_framebuffer = 0;
    GLint previous_program = 0;
    GLint previous_vertex_array = 0;
    GLint previous_array_buffer = 0;
    GLint previous_active_texture = 0;
    GLint previous_texture0 = 0;
    GLint previous_texture1 = 0;
    GLint previous_unpack_alignment = 0;
    GLint previous_unpack_row_length = 0;
    GLint previous_viewport[4] = {0, 0, 0, 0};

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_unpack_row_length);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture1);

    GLuint upload_pbo = nv12_pbo.pbo;
    bind_pbo(&upload_pbo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glBindTexture(GL_TEXTURE_2D, camera.nv12_luma_texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, nv12_pitch_bytes);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image_width, image_height, GL_RED,
                    GL_UNSIGNED_BYTE, reinterpret_cast<void *>(0));

    glBindTexture(GL_TEXTURE_2D, camera.nv12_chroma_texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, nv12_pitch_bytes / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chroma_width, chroma_height, GL_RG,
                    GL_UNSIGNED_BYTE,
                    reinterpret_cast<void *>(chroma_offset));

    glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_unpack_row_length);
    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
    glBindTexture(GL_TEXTURE_2D, 0);
    unbind_pbo();

    glBindFramebuffer(GL_FRAMEBUFFER, camera.nv12_stage_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           destination_texture, 0);
    glViewport(0, 0, image_width, image_height);
    glUseProgram(presenter->program);

    float yuv_to_rgb[9];
    fillNv12YuvToRgbMatrix(color_matrix, yuv_to_rgb);
    glUniformMatrix3fv(presenter->yuv_matrix_location, 1, GL_TRUE,
                       yuv_to_rgb);

    glUniform1i(presenter->luma_texture_location, 0);
    glUniform1i(presenter->chroma_texture_location, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, camera.nv12_luma_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, camera.nv12_chroma_texture);

    glBindVertexArray(presenter->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(previous_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, previous_array_buffer);
    glUseProgram(previous_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previous_texture0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, previous_texture1);
    glActiveTexture(previous_active_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2],
               previous_viewport[3]);
}

} // namespace

std::vector<std::mutex> g_mutexes(MAX_VIEWS);
std::vector<std::condition_variable> g_cvs(MAX_VIEWS);
std::vector<bool> g_ready(MAX_VIEWS);
std::vector<std::vector<cv::Rect>> yolo_boxes(MAX_VIEWS);
std::vector<std::vector<std::string>> yolo_labels(MAX_VIEWS);
std::vector<std::vector<int>> yolo_classid(MAX_VIEWS);
std::vector<unsigned char *> yolo_input_frames_rgba(MAX_VIEWS);
std::unordered_map<std::string, std::atomic<bool>> window_need_decoding;
std::unordered_map<std::string, std::atomic<int>> latest_decoded_frame;
std::unordered_map<std::string, std::shared_ptr<DecoderPerfSample>> decoder_perf_samples;
std::mutex g_seek_info_mutex;
std::mutex g_decoder_perf_mutex;

// Global variables
bool show_interpolation_debug = false;
std::vector<ZarrDetectionLoader::DetectionDataset> detection_dataset_ids;
std::vector<std::string> detection_dataset_labels;
int detection_dataset_choice = 0;
ZarrBBoxEditState g_zarr_bbox_edit_state;

#include "review_frame_state.h"

void refreshDetectionDatasetOptions(ZarrDetectionLoader& loader) {
    detection_dataset_ids.clear();
    detection_dataset_labels.clear();
    detection_dataset_choice = 0;
    auto options = loader.getAvailableDetectionDatasets();
    auto active = loader.getActiveDetectionDataset();
    for (size_t i = 0; i < options.size(); ++i) {
        detection_dataset_ids.push_back(options[i].first);
        detection_dataset_labels.push_back(options[i].second);
        if (options[i].first == active) {
            detection_dataset_choice = static_cast<int>(i);
        }
    }
}

#include "stimulus_playback.h"

static StimulusPlayback stimulus_player;

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

struct PerfLogWriter {
    std::ofstream stream;
    std::filesystem::path csv_path;
    std::filesystem::path metadata_path;
    std::chrono::steady_clock::time_point start_steady{};
    std::chrono::steady_clock::time_point last_sample_steady{};

    bool open(const std::filesystem::path& output_path) {
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
        std::cout << "[PerfLog] Writing CSV samples to " << csv_path
                  << std::endl;
        std::cout << "[PerfLog] Writing metadata sidecar to " << metadata_path
                  << std::endl;
        return true;
    }

    bool enabled() const { return stream.is_open(); }

    void writeMetadata(const json& payload) const {
        if (metadata_path.empty()) {
            return;
        }
        std::ofstream meta_stream(metadata_path, std::ios::out | std::ios::trunc);
        if (!meta_stream.is_open()) {
            std::cerr << "[PerfLog] Failed to write metadata sidecar "
                      << metadata_path << std::endl;
            return;
        }
        meta_stream << payload.dump(2) << "\n";
    }
};

}  // namespace

int main(int argc, char **argv) {
    std::string cli_zarr_override_path;
    std::string cli_recording_path;
    std::filesystem::path cli_perf_log_path;
    const std::filesystem::path argv0_path = (argc > 0) ? argv[0] : "";
    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--zarr") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --zarr" << std::endl;
                return 1;
            }
            cli_zarr_override_path = argv[++i];
            continue;
        }
        if (arg == "--recording") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --recording" << std::endl;
                return 1;
            }
            cli_recording_path = argv[++i];
            continue;
        }
        if (arg == "--perf-log") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --perf-log" << std::endl;
                return 1;
            }
            cli_perf_log_path = argv[++i];
            continue;
        }
        std::cerr << "Ignoring unknown argument: " << arg << std::endl;
    }

    // Mutual exclusion: --recording takes precedence over --zarr
    if (!cli_recording_path.empty() && !cli_zarr_override_path.empty()) {
        std::cerr << "Warning: both --recording and --zarr specified; "
                  << "using --recording, ignoring --zarr" << std::endl;
        cli_zarr_override_path.clear();
    }

    // Validate --recording path early
    if (!cli_recording_path.empty() && !IsDirectoryNoThrow(cli_recording_path)) {
        std::cerr << "Error: --recording path is not a directory: "
                  << cli_recording_path << std::endl;
        cli_recording_path.clear();
    }

    gx_context *window = new gx_context();
    *window = gx_context{};
    window->swap_interval = 1;  // use vsync
    window->width = 1920;
    window->height = 1080;
    window->render_target_title = (char *)malloc(100);  // window title
    window->glsl_version = (char *)malloc(100);

    constexpr int kCudaDeviceIndex = 0;
    render_initialize_target(window, kCudaDeviceIndex, argv0_path);
    Nv12PlaybackPresenter nv12_playback_presenter;

    render_scene *scene = new render_scene();

    std::string root_dir;
    std::string skeleton_dir;
    std::vector<std::string> camera_names;
    std::vector<CameraParams> camera_params;
    std::vector<std::thread> decoder_threads;
    std::vector<std::unique_ptr<FFmpegDemuxer>> demuxers;

    // Zarr loading
    ZarrDetectionLoader zarr_loader;
    bool zarr_loaded = false;

    DecoderContext *dc_context = new DecoderContext();
    *dc_context = DecoderContext{};
    dc_context->decoding_flag = false;
    dc_context->stop_flag = false;
    dc_context->total_num_frame = int(INT_MAX);
    dc_context->estimated_num_frames = 0;
    dc_context->gpu_index = kCudaDeviceIndex;
    dc_context->seek_interval = 250;

    // gui states, todo: bundle this later
    std::time_t last_saved = static_cast<std::time_t>(-1);
    bool video_loaded = false;
    bool cpu_buffer_toggle = true;
    bool plot_keypoints_flag = false;
    bool show_keypoint_markers = true;
    bool show_heading_arrows = true;
    bool show_eye_masks = false;
    int current_frame_num = 0;
    bool skeleton_chosen = false;
    std::vector<std::string> imgs_names;

    constexpr bool kHeadingDebugLoggingEnabled = false;
    constexpr int kHeadingDebugMaxMessages = 400;
    constexpr bool kPlaybackDebugLoggingEnabled = false;
    int heading_debug_message_count = 0;
    int heading_debug_draw_log_count = 0;
    int heading_debug_entry_log_count = 0;
    int heading_debug_last_frame_logged = -1;
    bool heading_debug_logged_toggle_disabled = false;
    bool heading_debug_logged_no_data = false;
    bool heading_debug_logged_interpolated = false;
    auto headingDebugLog = [&](const std::string &message) {
        if (!kHeadingDebugLoggingEnabled) {
            return;
        }
        if (heading_debug_message_count >= kHeadingDebugMaxMessages) {
            if (heading_debug_message_count == kHeadingDebugMaxMessages) {
                std::cout << "[HEADING_DEBUG] Log limit reached, suppressing further messages"
                          << std::endl;
            }
            heading_debug_message_count++;
            return;
        }
        std::cout << "[HEADING_DEBUG] " << message << std::endl;
        heading_debug_message_count++;
    };

    constexpr bool kEyeMaskDebugLoggingEnabled = false;
    constexpr int kEyeMaskDebugMaxMessages = 100;
    int eye_mask_debug_message_count = 0;
    int eye_mask_debug_entry_log_count = 0;
    int eye_mask_debug_draw_log_count = 0;
    int eye_mask_debug_last_frame_logged = -1;
    bool eye_mask_debug_logged_toggle_disabled = false;
    bool eye_mask_debug_logged_no_data = false;
    auto eyeMaskDebugLog = [&](const std::string &message) {
        if (!kEyeMaskDebugLoggingEnabled) {
            return;
        }
        if (eye_mask_debug_message_count >= kEyeMaskDebugMaxMessages) {
            if (eye_mask_debug_message_count == kEyeMaskDebugMaxMessages) {
                std::cout << "[EYE_MASK_DEBUG] Log limit reached, suppressing further messages"
                          << std::endl;
            }
            eye_mask_debug_message_count++;
            return;
        }
        std::cout << "[EYE_MASK_DEBUG] " << message << std::endl;
        eye_mask_debug_message_count++;
    };

    // for labeling
    std::unique_ptr<SkeletonContext> skeleton;
    std::map<u32, KeyPoints *> keypoints_map;
    bool keypoints_find = false;
    std::map<std::string, SkeletonPrimitive> skeleton_map;

    // others
    UiPathConfig ui_path_config = LoadUiPathConfig(cwd, argv0_path);
    std::string start_folder_name = ui_path_config.default_start_path;
    if (start_folder_name.empty() || !IsDirectoryNoThrow(start_folder_name)) {
        start_folder_name = cwd.string();
    }
    const std::filesystem::path default_buffer_dump_root =
        GetDefaultCrimsonBufferDumpRoot();
    if (!ui_path_config.loaded_from.empty()) {
        std::cout << "[UIPathConfig] Loaded: " << ui_path_config.loaded_from
                  << std::endl;
    } else {
        std::cout << "[UIPathConfig] Using default start path: "
                  << start_folder_name << std::endl;
    }
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    ImGuiIO &io = ImGui::GetIO();

    ImPlotStyle &style = ImPlot::GetStyle();
    ImVec4 *colors = style.Colors;
    colors[ImPlotCol_Crosshairs] = ImVec4(0.3f, 0.10f, 0.64f, 1.00f);

    bool yolo_detection = false;
    std::vector<std::thread> yolo_threads;
    yolo_param yolo_setting = yolo_param();
    std::string keypoints_root_folder;
    int label_buffer_size = 100;
    int playback_preview_scale_mode = 0;
    int playback_renderer_mode = 1;
    int stimulus_buffer_size = 12;
    bool stimulus_use_cpu_buffer = false;
#ifdef _WIN32
    bool stimulus_use_software_decode = true;
#else
    bool stimulus_use_software_decode = false;
#endif
    uint64_t stimulus_catchup_seek_generation = 1;
    bool show_help_window = false;
    std::vector<bool> is_view_focused;
    bool input_is_imgs = false;
    bool show_error = false;
    std::string error_message;
    std::unordered_map<std::string, bool> window_was_decoding;
    double inst_speed = 1.0;
    double video_fps = 60.0f;
    float set_playback_speed = 1.0f;
    PlaybackState ps;
    SeekProgress seek_progress;
    std::string frame_sync_debug_line;
    int frame_sync_valid_slots = -1;
    int frame_sync_empty_slots = -1;
    int frame_sync_latest_decoded = -1;
    int frame_sync_recording_remaining = -1;
    int frame_sync_recording_total = -1;
    PerfLogWriter perf_log_writer;
    constexpr auto kPerfLogSamplePeriod = std::chrono::milliseconds(250);
    if (!cli_perf_log_path.empty()) {
        (void)perf_log_writer.open(cli_perf_log_path);
    }

    window_need_decoding[stimulus_player.window_name].store(false);
    latest_decoded_frame[stimulus_player.window_name].store(-1);
    window_was_decoding[stimulus_player.window_name] = false;

    auto loadCameraCalibrationsForCurrentMedia = [&]() {
        if (!video_loaded) {
            return;
        }
        camera_params.resize(scene->num_cams);
        std::cout << "\n=== Loading Camera Calibrations from YAML ===" << std::endl;
        for (size_t i = 0; i < camera_names.size(); ++i) {
            std::cout << "\nProcessing camera " << i << ": " << camera_names[i] << std::endl;
            std::string yaml_file = root_dir + "/calibration/" + camera_names[i] + ".yaml";
            if (std::filesystem::exists(yaml_file)) {
                std::cout << "Loading homography from YAML for camera: " << camera_names[i]
                          << std::endl;
                if (!camera_load_params_from_yaml(yaml_file, camera_params[i], error_message)) {
                    std::cerr << "Error: Failed to load calibration from YAML: "
                              << error_message << std::endl;
                    show_error = true;
                    break;
                }
                camera_print_calibration_details(camera_params[i], camera_names[i]);
            } else {
                std::cerr << "Warning: No calibration YAML file found at: "
                          << yaml_file << std::endl;
            }
        }
    };

    auto tryAutoLoadAffiliatedVideoFromZarr = [&](const char* trigger_label) {
        if (!zarr_loaded) {
            return;
        }
        if (video_loaded || !decoder_threads.empty()) {
            std::cout << "[Zarr] Skipping affiliated video auto-load (" << trigger_label
                      << "): media already loaded" << std::endl;
            return;
        }

        const std::string source_hint = zarr_loader.getSourceVideoPath();
        if (source_hint.empty()) {
            std::cout << "[Zarr] Archive did not provide source video metadata; "
                      << "skipping affiliated video auto-load" << std::endl;
            return;
        }

        auto resolved_video_opt =
            ResolveAffiliatedVideoPath(source_hint, zarr_loader.getArchivePath());
        if (!resolved_video_opt.has_value()) {
            std::cout << "[Zarr] Could not resolve affiliated source video path from metadata: "
                      << source_hint << std::endl;
            return;
        }

        const std::filesystem::path resolved_video = *resolved_video_opt;
        std::string camera_name = resolved_video.stem().string();
        if (camera_name.empty()) {
            camera_name = resolved_video.filename().string();
        }

        try {
            input_is_imgs = false;
            camera_names.clear();
            demuxers.clear();
            is_view_focused.clear();

            camera_names.push_back(camera_name);
            window_need_decoding[camera_name].store(true);
            window_was_decoding[camera_name] = true;

            std::map<std::string, std::string> ffmpeg_options;
            demuxers.push_back(
                std::make_unique<FFmpegDemuxer>(resolved_video.string().c_str(), ffmpeg_options));

            dc_context->seek_interval =
                static_cast<int>(demuxers[0]->FindKeyFrameInterval());
            video_fps = demuxers[0]->GetFramerate();
            scene->num_cams = 1;
            scene->cameras.resize(scene->num_cams);
            scene->cameras[0].image_width = demuxers[0]->GetWidth();
            scene->cameras[0].image_height = demuxers[0]->GetHeight();
            render_allocate_scene_memory(scene, label_buffer_size);

            decoder_threads.push_back(std::thread(
                &decoder_process, dc_context, demuxers[0].get(), camera_names[0],
                scene->cameras[0].display_buffer, scene->size_of_buffer, &scene->cameras[0].seek_context,
                scene->use_cpu_buffer));
            is_view_focused.push_back(false);
            video_loaded = true;

            int initial_frame = std::max(0, ps.to_display_frame_number);
            double seek_fps = (video_fps > 0.0) ? video_fps : 30.0;
            seek_all_cameras(scene, initial_frame, seek_fps, ps, true,
                             &zarr_loader, &stimulus_player);

            std::filesystem::path inferred_root =
                InferRecordingRootPath(resolved_video, zarr_loader.getArchivePath());
            if (!inferred_root.empty()) {
                root_dir = inferred_root.string();
                skeleton_dir = root_dir;
            }

            std::cout << "[Zarr] Auto-loaded affiliated video (" << trigger_label
                      << "): " << resolved_video.string() << std::endl;
            loadCameraCalibrationsForCurrentMedia();
        } catch (const std::exception& e) {
            std::cerr << "[Zarr] Failed to auto-load affiliated video (" << trigger_label
                      << "): " << e.what() << std::endl;
        }
    };

    auto tryAutoLoadStimulusVideo = [&](const char* trigger_label) {
        if (!zarr_loaded) return;
        if (stimulus_player.loaded) return;
        if (!zarr_loader.hasStimulusAlignment()) return;

        auto resolved = ResolveStimulusVideoPath(
            zarr_loader.getStimulusVideoPath(),
            zarr_loader.getStimulusSourceH5(),
            zarr_loader.getArchivePath(),
            root_dir);
        if (!resolved.has_value()) {
            std::cout << "[Stimulus] Could not auto-discover stimulus video ("
                      << trigger_label << ")" << std::endl;
            return;
        }

        int stim_buf_size = std::max(1, stimulus_buffer_size);
        if (!initializeStimulusPlayback(stimulus_player, resolved->string(),
                                         stim_buf_size, stimulus_use_cpu_buffer,
                                         stimulus_use_software_decode,
                                         kCudaDeviceIndex)) {
            std::cerr << "[Stimulus] Failed to auto-load stimulus video: "
                      << resolved->string() << std::endl;
            return;
        }

        window_was_decoding[stimulus_player.window_name] = false;
        window_need_decoding[stimulus_player.window_name].store(false);

        if (video_loaded) {
            scheduleStimulusSeek(stimulus_player, &zarr_loader,
                                 ps.to_display_frame_number, !ps.play_video);
        }

        std::cout << "[Stimulus] Auto-loaded stimulus video (" << trigger_label
                  << "): " << resolved->string() << std::endl;
    };

    if (!cli_zarr_override_path.empty()) {
        std::string zarr_error;
        if (loadZarrDetectionFromPath(cli_zarr_override_path, zarr_loader, zarr_error)) {
            zarr_loaded = true;
            refreshDetectionDatasetOptions(zarr_loader);
            g_zarr_bbox_edit_state.clearAll();
            std::cout << "Loaded Zarr archive from --zarr: "
                      << zarr_loader.getArchivePath() << std::endl;
            tryAutoLoadAffiliatedVideoFromZarr("--zarr");
            tryAutoLoadStimulusVideo("--zarr");
        } else {
            g_zarr_bbox_edit_state.clearAll();
            std::cerr << "Failed to load --zarr archive: " << zarr_error << std::endl;
        }
    }

    if (!cli_recording_path.empty()) {
        root_dir = cli_recording_path;
        skeleton_dir = root_dir;

        std::string zarr_error;
        if (loadZarrDetectionFromDirectory(root_dir, zarr_loader, zarr_error)) {
            zarr_loaded = true;
            refreshDetectionDatasetOptions(zarr_loader);
            g_zarr_bbox_edit_state.clearAll();
            std::cout << "Loaded Zarr archive from --recording: "
                      << zarr_loader.getArchivePath() << std::endl;
            tryAutoLoadAffiliatedVideoFromZarr("--recording");
            tryAutoLoadStimulusVideo("--recording");
        } else {
            g_zarr_bbox_edit_state.clearAll();
            std::cout << "[--recording] No zarr archive found (optional): "
                      << zarr_error << std::endl;

            // Fallback: find first .mp4 in cams/ or root
            namespace fs = std::filesystem;
            std::string found_video;
            std::vector<fs::path> search_dirs;
            fs::path cams_dir = fs::path(root_dir) / "cams";
            if (IsDirectoryNoThrow(cams_dir)) {
                search_dirs.push_back(cams_dir);
            }
            search_dirs.push_back(fs::path(root_dir));

            for (const auto& search_dir : search_dirs) {
                if (!found_video.empty()) break;
                std::error_code ec;
                for (auto it = fs::directory_iterator(search_dir, ec);
                     it != fs::directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (it->is_regular_file(ec) && !ec &&
                        IsSupportedVideoPath(it->path())) {
                        found_video = it->path().string();
                        break;
                    }
                }
            }

            if (!found_video.empty()) {
                try {
                    fs::path video_path(found_video);
                    std::string camera_name = video_path.stem().string();
                    if (camera_name.empty()) {
                        camera_name = video_path.filename().string();
                    }

                    input_is_imgs = false;
                    camera_names.clear();
                    demuxers.clear();
                    is_view_focused.clear();

                    camera_names.push_back(camera_name);
                    window_need_decoding[camera_name].store(true);
                    window_was_decoding[camera_name] = true;

                    std::map<std::string, std::string> ffmpeg_options;
                    demuxers.push_back(
                        std::make_unique<FFmpegDemuxer>(found_video.c_str(), ffmpeg_options));

                    dc_context->seek_interval =
                        static_cast<int>(demuxers[0]->FindKeyFrameInterval());
                    video_fps = demuxers[0]->GetFramerate();
                    scene->num_cams = 1;
                    scene->cameras.resize(scene->num_cams);
                    scene->cameras[0].image_width = demuxers[0]->GetWidth();
                    scene->cameras[0].image_height = demuxers[0]->GetHeight();
                    render_allocate_scene_memory(scene, label_buffer_size);

                    decoder_threads.push_back(std::thread(
                        &decoder_process, dc_context, demuxers[0].get(), camera_names[0],
                        scene->cameras[0].display_buffer, scene->size_of_buffer, &scene->cameras[0].seek_context,
                        scene->use_cpu_buffer));
                    is_view_focused.push_back(false);
                    video_loaded = true;

                    int initial_frame = std::max(0, ps.to_display_frame_number);
                    double seek_fps = (video_fps > 0.0) ? video_fps : 30.0;
                    seek_all_cameras(scene, initial_frame, seek_fps, ps, true,
                                     &zarr_loader, &stimulus_player);

                    std::cout << "[--recording] Auto-loaded video: "
                              << found_video << std::endl;
                    loadCameraCalibrationsForCurrentMedia();
                    tryAutoLoadStimulusVideo("--recording-fallback");
                } catch (const std::exception& e) {
                    std::cerr << "[--recording] Failed to load video: "
                              << e.what() << std::endl;
                }
            } else {
                std::cout << "[--recording] No video files found in "
                          << root_dir << std::endl;
            }
        }
    }

    auto getVisibleCameraIndex = [&]() -> int {
        if (scene->num_cams <= 0 || scene->size_of_buffer <= 0) {
            return -1;
        }
        for (int i = 0; i < scene->num_cams && i < static_cast<int>(camera_names.size()); ++i) {
            auto it = window_was_decoding.find(camera_names[i]);
            if (it != window_was_decoding.end() && it->second) {
                return i;
            }
        }
        return 0;
    };

    auto setCameraDecodeRequests = [&](bool enabled) {
        for (const auto& camera_name : camera_names) {
            auto it = window_need_decoding.find(camera_name);
            if (it != window_need_decoding.end()) {
                it->second.store(enabled);
            }
        }
    };

    auto countBufferedStimulusFrames = [&](const StimulusPlayback& stim) -> int {
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
    };

    auto playbackPreviewScaleFactor = [&]() -> double {
        switch (playback_preview_scale_mode) {
        case 1:
            return 0.5;
        case 2:
            return 0.25;
        default:
            return 1.0;
        }
    };

    auto playbackPreviewScaleLabel = [&]() -> const char* {
        switch (playback_preview_scale_mode) {
        case 1:
            return "1/2";
        case 2:
            return "1/4";
        default:
            return "1x";
        }
    };

    auto playbackPreviewIsActive = [&]() -> bool {
        return ps.play_video && !yolo_detection &&
               playbackPreviewScaleFactor() < 1.0;
    };

    auto playbackRendererModeLabel = [&]() -> const char* {
        switch (playback_renderer_mode) {
        case 1:
            return "lightweight";
        default:
            return "standard";
        }
    };

    auto playbackLightweightRendererIsActive = [&]() -> bool {
        return ps.play_video && playback_renderer_mode == 1;
    };

    auto stepPausedFrameFromBuffer = [&](int target_frame) -> bool {
        if (ps.play_video || scene->num_cams <= 0 || scene->size_of_buffer <= 0) {
            return false;
        }
        const int visible_idx = getVisibleCameraIndex();
        if (visible_idx < 0) {
            return false;
        }

        int matched_slot = -1;
        for (int i = 0; i < scene->size_of_buffer; ++i) {
            const auto& slot = scene->cameras[visible_idx].display_buffer[i];
            if (!slot.available_to_write && slot.frame_number == target_frame) {
                matched_slot = i;
                break;
            }
        }

        if (matched_slot < 0) {
            return false;
        }

        ps.read_head = matched_slot;
        ps.pause_selected = 0;
        ps.to_display_frame_number = target_frame;
        ps.slider_frame_number = target_frame;
        current_frame_num = target_frame;
        ps.pause_seeked = true;
        return true;
    };

    auto findNearestPausedBufferSlot = [&](int visible_idx,
                                           int target_frame) -> int {
        if (ps.play_video || scene->num_cams <= 0 || scene->size_of_buffer <= 0 ||
            visible_idx < 0) {
            return -1;
        }

        int best_slot = -1;
        int best_distance = std::numeric_limits<int>::max();
        int best_frame = -1;
        for (int i = 0; i < scene->size_of_buffer; ++i) {
            const auto& slot = scene->cameras[visible_idx].display_buffer[i];
            if (slot.available_to_write || slot.frame_number < 0) {
                continue;
            }
            const int distance = std::abs(slot.frame_number - target_frame);
            if (distance < best_distance ||
                (distance == best_distance && slot.frame_number > best_frame)) {
                best_distance = distance;
                best_frame = slot.frame_number;
                best_slot = i;
            }
        }
        return best_slot;
    };

    auto findDisplaySlotForFrame = [&](int cam_idx,
                                       int target_frame,
                                       int preferred_slot) -> int {
        if (scene->num_cams <= 0 || scene->size_of_buffer <= 0 ||
            cam_idx < 0 || cam_idx >= scene->num_cams) {
            return -1;
        }

        auto slot_is_valid = [&](int slot_idx) -> bool {
            return slot_idx >= 0 &&
                   slot_idx < scene->size_of_buffer &&
                   !scene->cameras[cam_idx].display_buffer[slot_idx].available_to_write &&
                   scene->cameras[cam_idx].display_buffer[slot_idx].frame_number >= 0;
        };

        if (slot_is_valid(preferred_slot)) {
            const int preferred_frame =
                scene->cameras[cam_idx].display_buffer[preferred_slot].frame_number;
            if (target_frame < 0 || preferred_frame == target_frame) {
                return preferred_slot;
            }
        }

        int exact_slot = -1;
        int best_lower_slot = -1;
        int best_lower_frame = std::numeric_limits<int>::min();
        int best_abs_slot = -1;
        int best_abs_distance = std::numeric_limits<int>::max();

        for (int i = 0; i < scene->size_of_buffer; ++i) {
            if (!slot_is_valid(i)) {
                continue;
            }
            const int frame_num = scene->cameras[cam_idx].display_buffer[i].frame_number;
            if (frame_num == target_frame) {
                exact_slot = i;
                break;
            }
            if (frame_num <= target_frame && frame_num > best_lower_frame) {
                best_lower_frame = frame_num;
                best_lower_slot = i;
            }
            const int distance = std::abs(frame_num - target_frame);
            if (distance < best_abs_distance) {
                best_abs_distance = distance;
                best_abs_slot = i;
            }
        }

        if (exact_slot >= 0) {
            return exact_slot;
        }
        if (best_lower_slot >= 0) {
            return best_lower_slot;
        }
        return best_abs_slot;
    };

    auto seekToFrame = [&](int target_frame, bool prefer_buffer_when_paused,
                           bool force_inaccurate = false) {
        if (scene->num_cams <= 0) {
            return;
        }

        const int max_frame = std::max(0, dc_context->total_num_frame - 1);
        const int clamped_frame = std::clamp(target_frame, 0, max_frame);
        const bool seek_accurate = !force_inaccurate && !ps.play_video;

        // Drop duplicate requests while an equivalent seek is still in flight.
        const bool seek_in_flight =
            (seek_progress.state == SeekState::WaitingCameras ||
             seek_progress.state == SeekState::WaitingStimulus);
        if (seek_in_flight &&
            seek_progress.requested_camera_frame == clamped_frame &&
            seek_progress.accurate == seek_accurate) {
            if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] dedupe: dropping duplicate request frame="
                      << clamped_frame
                      << " accurate=" << (seek_accurate ? "true" : "false")
                      << " state=" << seekStateName(seek_progress.state)
                      << std::endl;
            return;
        }

        // Fast path: frame already in buffer (paused only)
        if (prefer_buffer_when_paused && stepPausedFrameFromBuffer(clamped_frame)) {
            // Still need stimulus seek for this frame
            if (stimulus_player.loaded && zarr_loader.hasStimulusAlignment()) {
                auto stim_frame = zarr_loader.getStimulusFrameForCameraFrame(clamped_frame);
                if (stim_frame && *stim_frame >= 0) {
                    ps.current_stimulus_frame = *stim_frame;
                    seek_progress.seek_id++;
                    seek_progress.state = SeekState::WaitingStimulus;
                    seek_progress.requested_camera_frame = clamped_frame;
                    seek_progress.target_camera_frame = clamped_frame;
                    seek_progress.target_stimulus_frame = *stim_frame;
                    seek_progress.accurate = !force_inaccurate && !ps.play_video;
                    seek_progress.cameras_settled = scene->num_cams;
                    seek_progress.cameras_total = scene->num_cams;
                    seek_progress.deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    {
                        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                        stimulus_player.seek.seek_frame = static_cast<uint64_t>(*stim_frame);
                        stimulus_player.seek.seek_id = seek_progress.seek_id;
                        stimulus_player.seek.use_seek = true;
                        stimulus_player.seek.seek_done = false;
                        stimulus_player.seek.seek_accurate = seek_progress.accurate;
                    }
                    stimulus_player.last_displayed_frame = -1;
                    window_need_decoding[stimulus_player.window_name].store(true);
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] id=" << seek_progress.seek_id
                              << " buffer-hit camera=" << clamped_frame
                              << " -> stimulus=" << *stim_frame << std::endl;
                }
            }
            return;
        }

        // Full decoder seek path (non-blocking)
        if (!ps.play_video) {
            ps.pause_seeked = false;
            setCameraDecodeRequests(true);
        }

        seek_progress.seek_id++;
        seek_progress.state = SeekState::WaitingCameras;
        seek_progress.requested_camera_frame = clamped_frame;
        seek_progress.target_camera_frame = clamped_frame;
        seek_progress.target_stimulus_frame = -1;
        seek_progress.accurate = seek_accurate;
        seek_progress.cameras_settled = 0;
        seek_progress.cameras_total = scene->num_cams;
        seek_progress.deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);

        // Update playback state immediately (same as old seek_all_cameras)
        ps.to_display_frame_number = clamped_frame;
        ps.read_head = 0;
        ps.just_seeked = true;
        ps.slider_frame_number = clamped_frame;
        ps.accumulated_play_time = clamped_frame / video_fps;
        ps.last_play_time_start = std::chrono::steady_clock::now();
        ps.last_frame_num_playspeed = clamped_frame;
        ps.last_wall_time_playspeed = std::chrono::steady_clock::now();

        // Resolve stimulus target frame now (for state tracking)
        if (stimulus_player.loaded && zarr_loader.hasStimulusAlignment()) {
            auto stim_frame = zarr_loader.getStimulusFrameForCameraFrame(clamped_frame);
            if (stim_frame && *stim_frame >= 0) {
                ps.current_stimulus_frame = *stim_frame;
                seek_progress.target_stimulus_frame = *stim_frame;
            }
        }

        // Fire camera seeks (returns immediately)
        initiate_camera_seeks(scene, clamped_frame, seek_progress.seek_id, seek_accurate);

        if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] id=" << seek_progress.seek_id
                  << " initiated cameras=" << scene->num_cams
                  << " frame=" << clamped_frame
                  << " accurate=" << (seek_accurate ? "true" : "false")
                  << std::endl;
    };

    auto syncPlaybackStartToCurrentFrame = [&]() {
        const double fps_for_clock = (video_fps > 0.0) ? video_fps : 30.0;
        const int clamped_frame = std::max(0, ps.to_display_frame_number);
        ps.accumulated_play_time =
            static_cast<double>(clamped_frame) / fps_for_clock;
        const auto now_tp = std::chrono::steady_clock::now();
        ps.last_play_time_start = now_tp;
        ps.last_frame_num_playspeed = clamped_frame;
        ps.last_wall_time_playspeed = now_tp;

        const int visible_idx = getVisibleCameraIndex();
        if (visible_idx >= 0 && scene->size_of_buffer > 0) {
            const int preferred_slot = ps.read_head % scene->size_of_buffer;
            const int target_slot = findDisplaySlotForFrame(
                visible_idx, clamped_frame, preferred_slot);
            if (target_slot >= 0) {
                ps.read_head = target_slot;
            }
        }

        if (stimulus_player.loaded) {
            // Clear throttle state when playback resumes so stimulus decode
            // can restart immediately if the queue had been throttled.
            stimulus_player.throttled = false;
            stimulus_player.throttle_resume_frame = -1;
            window_need_decoding[stimulus_player.window_name].store(true);
        }
    };

    auto stepFrames = [&](int delta_frames) {
        seekToFrame(current_frame_num + delta_frames, true);
    };

    auto applyPlaybackToggle = [&]() {
        ps.play_video = !ps.play_video;
        if (ps.play_video) {
            ps.pause_seeked = false;
            setCameraDecodeRequests(true);
            if (stimulus_player.loaded) {
                window_need_decoding[stimulus_player.window_name].store(true);
            }
            syncPlaybackStartToCurrentFrame();
        } else {
            ps.pause_selected = 0;
        }
    };

    ReviewFrameFilters review_frame_filters;
    ReviewFrameCache review_frame_cache;
    std::string review_frame_status;
    std::string decode_debug_status;
    std::string bbox_payload_status;
    std::mt19937 debug_rng(
        static_cast<uint32_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    struct ManualDetectPayloadPreview {
        size_t total_frames = 0;
        size_t total_detections = 0;
        size_t clean_rows = 0;
        size_t interpolated_rows = 0;
        size_t manual_rows = 0;
        bool valid = false;
        std::string error;
        std::vector<int32_t> frame_indices;
        std::vector<std::array<double, 4>> bbox_norm_coords;
        std::vector<float> scores;
        std::vector<int32_t> class_ids;
        std::vector<int32_t> frame_counts;
        std::vector<int32_t> n_detections;
        std::vector<int32_t> frame_mapping;
        std::vector<int8_t> detection_source;
        std::vector<std::string> reason;
    };
    std::optional<ManualDetectPayloadPreview> manual_payload_preview;
    static int manual_write_intended_use = 0;  // 0 = full_recording, 1 = training
    static int manual_write_review_state = 0;  // 0 = approved, 1 = needs_review, 2 = pending, 3 = rejected
    static RefinedKeypointReviewWindowState
        refined_keypoint_review_window_state;
    CropPreviewWindowState crop_preview_window_state;
    LabelingToolWindowState labeling_tool_window_state;
    FrameDebugWindowState frame_debug_window_state;

    auto sanitizePathComponent = [](std::string value) -> std::string {
        if (value.empty()) {
            return "unnamed";
        }
        for (char& ch : value) {
            const bool ok =
                (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '_' || ch == '-' || ch == '.';
            if (!ok) {
                ch = '_';
            }
        }
        return value;
    };

    auto buildManualDetectPayloadPreview = [&]() -> ManualDetectPayloadPreview {
        ManualDetectPayloadPreview preview;
        if (!zarr_loaded || !zarr_loader.hasDetectionData()) {
            preview.error = "No active Zarr detection dataset.";
            return preview;
        }
        preview.total_frames = zarr_loader.getTotalFrames();
        if (preview.total_frames == 0) {
            preview.error = "Active dataset has zero frames.";
            return preview;
        }

        int image_width = zarr_loader.getImageWidth();
        int image_height = zarr_loader.getImageHeight();
        if (image_width <= 0 || image_height <= 0) {
            if (scene->num_cams > 0) {
                image_width = static_cast<int>(scene->cameras[0].image_width);
                image_height = static_cast<int>(scene->cameras[0].image_height);
            }
        }
        if (image_width <= 0 || image_height <= 0) {
            preview.error = "Could not resolve image dimensions for bbox normalization.";
            return preview;
        }

        auto toNormalizedCxCyWh = [&](float x_min,
                                      float y_min,
                                      float width,
                                      float height) -> std::array<double, 4> {
            const double max_w = static_cast<double>(image_width);
            const double max_h = static_cast<double>(image_height);
            const double clamped_x_min =
                std::clamp(static_cast<double>(x_min), 0.0, max_w);
            const double clamped_y_min =
                std::clamp(static_cast<double>(y_min), 0.0, max_h);
            const double clamped_width = std::clamp(
                static_cast<double>(width), 0.0, std::max(0.0, max_w - clamped_x_min));
            const double clamped_height = std::clamp(
                static_cast<double>(height), 0.0, std::max(0.0, max_h - clamped_y_min));
            const double cx = (clamped_x_min + 0.5 * clamped_width) / max_w;
            const double cy = (clamped_y_min + 0.5 * clamped_height) / max_h;
            const double w = clamped_width / max_w;
            const double h = clamped_height / max_h;
            return {
                std::clamp(cx, 0.0, 1.0),
                std::clamp(cy, 0.0, 1.0),
                std::clamp(w, 0.0, 1.0),
                std::clamp(h, 0.0, 1.0)};
        };

        auto resolveReasonAndSource = [&](bool force_manual,
                                          uint8_t source_flag,
                                          const std::string& source_reason)
            -> std::pair<int8_t, std::string> {
            if (force_manual) {
                return {0, "manual"};
            }
            const std::string lowered = ToLowerCopy(source_reason);
            if (!lowered.empty()) {
                if (lowered == "manual" ||
                    lowered.find("manual") != std::string::npos) {
                    return {0, "manual"};
                }
                if (lowered == "interpolated" ||
                    lowered.find("interp") != std::string::npos) {
                    return {1, "interpolated"};
                }
                if (lowered == "clean") {
                    return {0, "clean"};
                }
            }
            return {source_flag != 0 ? 1 : 0, source_flag != 0 ? "interpolated" : "clean"};
        };

        auto countReason = [&](const std::string& reason) {
            if (reason == "manual") {
                ++preview.manual_rows;
            } else if (reason == "interpolated") {
                ++preview.interpolated_rows;
            } else {
                ++preview.clean_rows;
            }
        };

        preview.frame_counts.reserve(preview.total_frames);
        for (size_t frame_id = 0; frame_id < preview.total_frames; ++frame_id) {
            int32_t frame_count = 0;
            const bool has_override =
                g_zarr_bbox_edit_state.hasFrameOverride(static_cast<int>(frame_id));
            auto base_detections = zarr_loader.getRawDetections(frame_id, false, false);

            if (!has_override) {
                for (size_t det_idx = 0; det_idx < base_detections.boxes.size(); ++det_idx) {
                    const auto& box = base_detections.boxes[det_idx];
                    const float x_min = box[0];
                    const float y_min = box[1];
                    const float width = std::max(0.0f, box[2] - box[0]);
                    const float height = std::max(0.0f, box[3] - box[1]);

                    preview.frame_indices.push_back(static_cast<int32_t>(frame_id));
                    preview.bbox_norm_coords.push_back(
                        toNormalizedCxCyWh(x_min, y_min, width, height));
                    preview.scores.push_back(
                        det_idx < base_detections.scores.size()
                            ? base_detections.scores[det_idx]
                            : 1.0f);
                    preview.class_ids.push_back(
                        det_idx < base_detections.class_ids.size()
                            ? base_detections.class_ids[det_idx]
                            : 0);
                    const uint8_t source_flag =
                        det_idx < base_detections.detection_source.size()
                            ? base_detections.detection_source[det_idx]
                            : 0;
                    const std::string source_reason =
                        det_idx < base_detections.detection_reason.size()
                            ? base_detections.detection_reason[det_idx]
                            : std::string{};
                    auto [resolved_source, resolved_reason] =
                        resolveReasonAndSource(false, source_flag, source_reason);
                    preview.detection_source.push_back(resolved_source);
                    preview.reason.push_back(resolved_reason);
                    countReason(resolved_reason);
                    ++frame_count;
                }
            } else {
                auto override_it =
                    g_zarr_bbox_edit_state.frame_overrides.find(static_cast<int>(frame_id));
                if (override_it == g_zarr_bbox_edit_state.frame_overrides.end()) {
                    preview.error = "Dirty frame override state is inconsistent.";
                    return preview;
                }
                const auto& boxes = override_it->second;
                const auto* added_flags = [&]() -> const std::vector<uint8_t>* {
                    auto it = g_zarr_bbox_edit_state.frame_added_flags.find(
                        static_cast<int>(frame_id));
                    return (it != g_zarr_bbox_edit_state.frame_added_flags.end())
                               ? &it->second
                               : nullptr;
                }();
                const auto* manual_flags = [&]() -> const std::vector<uint8_t>* {
                    auto it = g_zarr_bbox_edit_state.frame_manual_flags.find(
                        static_cast<int>(frame_id));
                    return (it != g_zarr_bbox_edit_state.frame_manual_flags.end())
                               ? &it->second
                               : nullptr;
                }();
                const auto* source_detection = [&]() -> const std::vector<uint8_t>* {
                    auto it = g_zarr_bbox_edit_state.frame_source_detection_source.find(
                        static_cast<int>(frame_id));
                    return (it != g_zarr_bbox_edit_state.frame_source_detection_source.end())
                               ? &it->second
                               : nullptr;
                }();
                const auto* source_reason = [&]() -> const std::vector<std::string>* {
                    auto it = g_zarr_bbox_edit_state.frame_source_reason.find(
                        static_cast<int>(frame_id));
                    return (it != g_zarr_bbox_edit_state.frame_source_reason.end())
                               ? &it->second
                               : nullptr;
                }();

                for (size_t box_idx = 0; box_idx < boxes.size(); ++box_idx) {
                    const auto& box = boxes[box_idx];
                    preview.frame_indices.push_back(static_cast<int32_t>(frame_id));
                    preview.bbox_norm_coords.push_back(toNormalizedCxCyWh(
                        box.x_min, box.y_min, box.width, box.height));
                    preview.scores.push_back(
                        std::isfinite(box.confidence) ? box.confidence : 1.0f);
                    preview.class_ids.push_back(static_cast<int32_t>(box.class_id));

                    const bool is_added = added_flags && box_idx < added_flags->size() &&
                                          (*added_flags)[box_idx] != 0;
                    const bool is_manual = manual_flags && box_idx < manual_flags->size() &&
                                           (*manual_flags)[box_idx] != 0;
                    const uint8_t src_flag =
                        (source_detection && box_idx < source_detection->size())
                            ? (*source_detection)[box_idx]
                            : 0;
                    const std::string src_reason =
                        (source_reason && box_idx < source_reason->size())
                            ? (*source_reason)[box_idx]
                            : std::string{};

                    auto [resolved_source, resolved_reason] =
                        resolveReasonAndSource(is_added || is_manual,
                                               src_flag,
                                               src_reason);
                    preview.detection_source.push_back(resolved_source);
                    preview.reason.push_back(resolved_reason);
                    countReason(resolved_reason);
                    ++frame_count;
                }
            }

            preview.frame_counts.push_back(frame_count);
        }

        preview.n_detections = preview.frame_counts;
        preview.frame_mapping = preview.frame_indices;
        preview.total_detections = preview.frame_indices.size();

        const size_t total_rows = preview.total_detections;
        const bool lengths_match =
            preview.bbox_norm_coords.size() == total_rows &&
            preview.scores.size() == total_rows &&
            preview.class_ids.size() == total_rows &&
            preview.frame_mapping.size() == total_rows &&
            preview.detection_source.size() == total_rows &&
            preview.reason.size() == total_rows;
        if (!lengths_match) {
            preview.error = "Payload arrays have inconsistent detection-level lengths.";
            return preview;
        }

        const int64_t frame_sum = std::accumulate(
            preview.frame_counts.begin(), preview.frame_counts.end(), int64_t{0});
        if (frame_sum != static_cast<int64_t>(total_rows)) {
            preview.error = "sum(frame_counts) does not equal detection row count.";
            return preview;
        }

        preview.valid = true;
        return preview;
    };

    auto dumpDecodeBuffersToVideos = [&](const std::string& tag) -> std::optional<std::filesystem::path> {
        if (!video_loaded || scene->num_cams <= 0 || scene->size_of_buffer <= 0) {
            decode_debug_status = "Decode dump skipped: no decoded video buffers are available.";
            return std::nullopt;
        }

        const bool was_playing = ps.play_video;
        if (was_playing) {
            ps.play_video = false;
            ps.pause_selected = 0;
        }

        std::filesystem::path dump_root = default_buffer_dump_root;
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
            decode_debug_status =
                "Decode dump failed to create directory: " + dump_dir.string();
            return std::nullopt;
        }

        std::unordered_map<std::string, bool> prior_decode_requests;
        prior_decode_requests.reserve(camera_names.size());
        for (const auto& camera_name : camera_names) {
            auto it = window_need_decoding.find(camera_name);
            if (it == window_need_decoding.end()) {
                continue;
            }
            prior_decode_requests[camera_name] = it->second.load();
            it->second.store(false);
        }

        // Give decoder threads a short window to finish any in-flight writes.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        bool wrote_any_frames = false;
        size_t total_frames_written = 0;
        size_t total_candidate_frames = 0;

        for (int cam_idx = 0; cam_idx < scene->num_cams; ++cam_idx) {
            if (cam_idx >= static_cast<int>(camera_names.size())) {
                continue;
            }
            const int width = scene->cameras[cam_idx].image_width;
            const int height = scene->cameras[cam_idx].image_height;
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
            all_slots.reserve(scene->size_of_buffer);
            std::vector<BufferSample> samples;
            samples.reserve(scene->size_of_buffer);
            for (int slot_idx = 0; slot_idx < scene->size_of_buffer; ++slot_idx) {
                const auto& slot = scene->cameras[cam_idx].display_buffer[slot_idx];
                BufferSample sample;
                sample.slot = slot_idx;
                sample.frame_number = slot.frame_number;
                sample.available = slot.available_to_write;
                sample.has_frame_ptr = slot.frame != nullptr;
                all_slots.push_back(sample);

                if (sample.available || sample.frame_number < 0 || !sample.has_frame_ptr) {
                    continue;
                }
                samples.push_back(sample);
            }

            const std::string camera_stem =
                sanitizePathComponent(camera_names[cam_idx]);
            std::ofstream slot_file(
                (dump_dir / (camera_stem + "_slots.txt")).string());
            slot_file << "# slot_index,frame_number,available_to_write,has_frame_ptr\n";
            for (const auto& sample : all_slots) {
                slot_file << sample.slot << ","
                          << sample.frame_number << ","
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
            const double fps_for_dump = (video_fps > 0.0) ? video_fps : 30.0;
            writer.open(video_path.string(),
                        cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                        fps_for_dump,
                        cv::Size(width, height),
                        true);
            if (!writer.isOpened()) {
                video_path = dump_dir / (camera_stem + ".mp4");
                writer.open(video_path.string(),
                            cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                            fps_for_dump,
                            cv::Size(width, height),
                            true);
            }
            const bool video_writer_enabled = writer.isOpened();
            if (!video_writer_enabled) {
                std::cerr << "[DecodeDebug] VideoWriter unavailable for camera "
                          << camera_names[cam_idx]
                          << "; dumping PNG frames only." << std::endl;
            }

            std::ofstream mapping_file(
                (dump_dir / (camera_stem + "_frames.txt")).string());
            mapping_file << "# dump_frame_index,source_frame_number,slot_index,png_path\n";

            const size_t bytes =
                static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            std::vector<uint8_t> rgba_bytes(bytes);

            size_t local_written = 0;
            for (const auto& sample : samples) {
                if (sample.slot < 0 || sample.slot >= scene->size_of_buffer) {
                    continue;
                }
                const auto& slot = scene->cameras[cam_idx].display_buffer[sample.slot];
                const int frame_before = slot.frame_number;
                const bool available_before = slot.available_to_write;
                if (available_before || frame_before < 0 || !slot.frame) {
                    continue;
                }

                if (scene->use_cpu_buffer) {
                    std::memcpy(rgba_bytes.data(), slot.frame, bytes);
                } else {
                    cudaError_t copy_status = cudaMemcpy(
                        rgba_bytes.data(),
                        slot.frame,
                        bytes,
                        cudaMemcpyDeviceToHost);
                    if (copy_status != cudaSuccess) {
                        std::cerr << "[DecodeDebug] cudaMemcpy failed while dumping slot "
                                  << sample.slot << " for camera "
                                  << camera_names[cam_idx] << std::endl;
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
                png_name << camera_stem << "_f" << frame_after
                         << "_slot" << sample.slot << ".png";
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
                mapping_file << local_written << ","
                             << frame_after << ","
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
            auto it = window_need_decoding.find(camera_name);
            if (it != window_need_decoding.end()) {
                it->second.store(was_enabled);
            }
        }

        if (was_playing) {
            ps.play_video = true;
            ps.pause_seeked = false;
            ps.last_play_time_start = std::chrono::steady_clock::now();
        }

        if (!wrote_any_frames) {
            decode_debug_status =
                "Decode dump completed but no valid frames were captured: " +
                dump_dir.string();
        } else {
            decode_debug_status =
                "Decode dump wrote " + std::to_string(total_frames_written) +
                " frames (" + std::to_string(total_candidate_frames) +
                " candidates) to " + dump_dir.string();
        }
        return dump_dir;
    };

    auto randomSeekAndDumpBuffers = [&]() {
        const int max_frame = std::max(0, dc_context->total_num_frame - 1);
        if (!video_loaded || max_frame <= 0) {
            decode_debug_status = "Random seek + dump skipped: no loaded video timeline.";
            return;
        }
        std::uniform_int_distribution<int> frame_dist(0, max_frame);
        const int target_frame = frame_dist(debug_rng);

        const bool was_playing = ps.play_video;
        if (was_playing) {
            ps.play_video = false;
            ps.pause_selected = 0;
        }

        seekToFrame(target_frame, false);
        std::unordered_map<std::string, bool> prior_decode_requests;
        prior_decode_requests.reserve(window_need_decoding.size());
        for (auto& [camera_name, enabled] : window_need_decoding) {
            prior_decode_requests[camera_name] = enabled.load();
            enabled.store(true);
        }
        auto countCandidateSlots = [&]() -> size_t {
            if (!video_loaded || scene->num_cams <= 0 || scene->size_of_buffer <= 0) {
                return 0;
            }
            size_t count = 0;
            for (int cam_idx = 0; cam_idx < scene->num_cams; ++cam_idx) {
                for (int slot_idx = 0; slot_idx < scene->size_of_buffer; ++slot_idx) {
                    const auto& slot = scene->cameras[cam_idx].display_buffer[slot_idx];
                    if (!slot.available_to_write && slot.frame_number >= 0 &&
                        slot.frame != nullptr) {
                        ++count;
                    }
                }
            }
            return count;
        };
        const size_t target_slots_per_camera =
            static_cast<size_t>(std::clamp(scene->size_of_buffer, 4u, 8u));
        const size_t target_total_slots =
            target_slots_per_camera * static_cast<size_t>(std::max(1u, scene->num_cams));
        const auto fill_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
        while (std::chrono::steady_clock::now() < fill_deadline) {
            if (countCandidateSlots() >= target_total_slots) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        dumpDecodeBuffersToVideos("random_seek_" + std::to_string(target_frame));
        if (was_playing) {
            setCameraDecodeRequests(true);
            if (stimulus_player.loaded) {
                window_need_decoding[stimulus_player.window_name].store(true);
            }
        } else {
            for (const auto& [camera_name, was_enabled] : prior_decode_requests) {
                auto it = window_need_decoding.find(camera_name);
                if (it != window_need_decoding.end()) {
                    it->second.store(was_enabled);
                }
            }
        }
        decode_debug_status =
            "Random seek target frame " + std::to_string(target_frame) + ". " +
            decode_debug_status;

        if (was_playing) {
            ps.play_video = true;
            ps.pause_seeked = false;
            ps.last_play_time_start = std::chrono::steady_clock::now();
        }
    };

    auto invalidateReviewFrameCache = [&]() {
        review_frame_cache.valid = false;
        review_frame_cache.frames.clear();
    };

    auto frameHasNonCleanDetections = [&](int frame_id) -> bool {
        if (frame_id < 0) {
            return false;
        }
        if (zarr_loader.getDetectionsForFrame(static_cast<size_t>(frame_id)) <= 0) {
            return false;
        }

        auto detections =
            zarr_loader.getRawDetections(static_cast<size_t>(frame_id), false);
        const size_t detection_count = detections.boxes.size();
        for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
            bool is_interp_source = false;
            if (det_idx < detections.detection_source.size()) {
                is_interp_source = detections.detection_source[det_idx] != 0;
            }

            if (det_idx < detections.detection_reason.size()) {
                std::string reason =
                    ToLowerCopy(detections.detection_reason[det_idx]);
                if (!reason.empty() && reason != "clean") {
                    return true;
                }
                if (!reason.empty()) {
                    if (is_interp_source) {
                        return true;
                    }
                    continue;
                }
            }

            if (is_interp_source) {
                return true;
            }
        }
        return false;
    };

    auto ensureReviewFrameIndex = [&]() {
        const bool has_filters =
            review_frame_filters.include_interpolated ||
            review_frame_filters.include_non_clean ||
            review_frame_filters.include_empty;
        if (!zarr_loaded || !zarr_loader.hasDetectionData() || !has_filters) {
            review_frame_cache.valid = true;
            review_frame_cache.archive_path = zarr_loader.getArchivePath();
            review_frame_cache.dataset = zarr_loader.getActiveDetectionDataset();
            review_frame_cache.total_frames = zarr_loader.getTotalFrames();
            review_frame_cache.filters = review_frame_filters;
            review_frame_cache.frames.clear();
            return;
        }

        const auto current_dataset = zarr_loader.getActiveDetectionDataset();
        const size_t total_frames = zarr_loader.getTotalFrames();
        const std::string current_archive_path = zarr_loader.getArchivePath();

        if (review_frame_cache.valid &&
            review_frame_cache.archive_path == current_archive_path &&
            review_frame_cache.dataset == current_dataset &&
            review_frame_cache.total_frames == total_frames &&
            review_frame_cache.filters.include_interpolated ==
                review_frame_filters.include_interpolated &&
            review_frame_cache.filters.include_non_clean ==
                review_frame_filters.include_non_clean &&
            review_frame_cache.filters.include_empty ==
                review_frame_filters.include_empty) {
            return;
        }

        review_frame_cache.valid = true;
        review_frame_cache.archive_path = current_archive_path;
        review_frame_cache.dataset = current_dataset;
        review_frame_cache.total_frames = total_frames;
        review_frame_cache.filters = review_frame_filters;
        review_frame_cache.frames.clear();
        const bool non_clean_possible =
            current_dataset != ZarrDetectionLoader::DetectionDataset::RawDetect;

        for (size_t frame_idx = 0; frame_idx < total_frames; ++frame_idx) {
            bool matches = false;
            const int32_t det_count = zarr_loader.getDetectionsForFrame(frame_idx);

            if (review_frame_filters.include_empty && det_count <= 0) {
                matches = true;
            }
            if (!matches && review_frame_filters.include_interpolated &&
                zarr_loader.isFrameInterpolated(frame_idx)) {
                matches = true;
            }
            if (!matches && review_frame_filters.include_non_clean &&
                non_clean_possible && det_count > 0 &&
                frameHasNonCleanDetections(static_cast<int>(frame_idx))) {
                matches = true;
            }

            if (matches) {
                review_frame_cache.frames.push_back(static_cast<int>(frame_idx));
            }
        }
    };

    auto jumpToReviewFrame = [&](bool forward) -> bool {
        ensureReviewFrameIndex();
        if (review_frame_cache.frames.empty()) {
            review_frame_status =
                "No frames match the selected review filters.";
            return false;
        }

        const auto& review_frames = review_frame_cache.frames;
        int target_frame = current_frame_num;
        if (forward) {
            auto it = std::upper_bound(review_frames.begin(),
                                       review_frames.end(),
                                       current_frame_num);
            if (it == review_frames.end()) {
                it = review_frames.begin();
            }
            target_frame = *it;
        } else {
            auto it = std::lower_bound(review_frames.begin(),
                                       review_frames.end(),
                                       current_frame_num);
            if (it == review_frames.begin()) {
                target_frame = review_frames.back();
            } else {
                --it;
                target_frame = *it;
            }
        }

        review_frame_status.clear();
        seekToFrame(target_frame, true);
        return true;
    };

    auto reloadActiveZarrPreserveDataset =
        [&](std::string& reload_error,
            std::optional<ZarrDetectionLoader::DetectionDataset>
                preferred_dataset = std::nullopt) -> bool {
            const auto previous_dataset = zarr_loader.getActiveDetectionDataset();
            const std::string archive_path = zarr_loader.getArchivePath();
            if (archive_path.empty()) {
                reload_error = "No loaded Zarr archive.";
                return false;
            }

            if (!zarr_loader.loadZarrFile(archive_path, reload_error)) {
                zarr_loaded = false;
                return false;
            }

            zarr_loaded = true;
            const auto dataset_to_restore =
                preferred_dataset.value_or(previous_dataset);
            if (zarr_loader.isDatasetAvailable(dataset_to_restore)) {
                (void)zarr_loader.setActiveDetectionDataset(dataset_to_restore);
            }
            refreshDetectionDatasetOptions(zarr_loader);
            invalidateReviewFrameCache();
            review_frame_status.clear();
            if (zarr_loader.getTotalFrames() > 0 &&
                current_frame_num >=
                    static_cast<int>(zarr_loader.getTotalFrames())) {
                current_frame_num =
                    static_cast<int>(zarr_loader.getTotalFrames()) - 1;
            }
            return true;
        };

    while (!glfwWindowShouldClose(window->render_target)) {
        static FileBrowserWindowState file_browser_window_state;
        const auto frame_loop_start = std::chrono::steady_clock::now();
        double frame_camera_upload_ms = 0.0;
        int frame_camera_upload_count = 0;
        double frame_camera_texture_resize_ms = 0.0;
        double frame_camera_preview_resize_ms = 0.0;
        double frame_camera_display_convert_ms = 0.0;
        double frame_camera_pbo_copy_ms = 0.0;
        double frame_camera_texture_upload_ms = 0.0;
        double frame_camera_playback_front_path_ms = 0.0;
        double frame_camera_playback_stage_total_ms = 0.0;
        double frame_camera_playback_stage_upload_ms = 0.0;
        double frame_camera_playback_swap_ms = 0.0;
        double frame_camera_plot_image_ui_ms = 0.0;
        double frame_camera_overlay_ui_ms = 0.0;
        double frame_camera_scene_ui_ms = 0.0;
        double frame_file_browser_ui_ms = 0.0;
        double frame_frame_debug_ui_ms = 0.0;
        double frame_buffer_window_ui_ms = 0.0;
        double frame_crop_preview_ui_ms = 0.0;
        double frame_stimulus_buffer_window_ui_ms = 0.0;
        double frame_keypoints_window_ui_ms = 0.0;
        double frame_labeling_tool_ui_ms = 0.0;
        double frame_stimulus_window_ui_ms = 0.0;
        double frame_stimulus_timeline_ui_ms = 0.0;
        double frame_movement_timeline_ui_ms = 0.0;
        double frame_help_menu_ui_ms = 0.0;
        double frame_gl_draw_ms = 0.0;
        double frame_swap_ms = 0.0;
        double frame_ui_build_ms = 0.0;
        double frame_imgui_render_ms = 0.0;
        int frame_imgui_draw_cmd_count = 0;
        int frame_imgui_draw_list_count = 0;
        int frame_imgui_total_vtx_count = 0;
        int frame_imgui_total_idx_count = 0;
        double perf_camera_viewport_width_px =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_viewport_height_px =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_x_min =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_x_max =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_y_min =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_y_max =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_visible_fraction =
            std::numeric_limits<double>::quiet_NaN();
        int perf_camera_view_zoomed_in = -1;
        int perf_requested_camera_frame = -1;
        int perf_min_decoded_camera_frame = -1;

        // Poll and handle events (inputs, window resize, etc.)
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const auto ui_build_start = std::chrono::steady_clock::now();

        // --- Seek state machine polling ---
        if (seek_progress.state == SeekState::WaitingCameras) {
            int settled = poll_camera_seeks(scene, seek_progress.seek_id);
            seek_progress.cameras_settled = settled;
            if (settled >= seek_progress.cameras_total) {
                int settled_camera_frame = seek_progress.target_camera_frame;
                int settled_camera_index = getVisibleCameraIndex();
                if (settled_camera_index < 0 || settled_camera_index >= scene->num_cams) {
                    settled_camera_index = 0;
                }
                {
                    std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                    if (scene->num_cams > 0) {
                        const auto& settled_ctx =
                            scene->cameras[settled_camera_index].seek_context;
                        if (settled_ctx.seek_done &&
                            settled_ctx.settled_seek_id == seek_progress.seek_id) {
                            settled_camera_frame =
                                static_cast<int>(settled_ctx.seek_frame);
                        }
                    }
                }

                if (settled_camera_frame != seek_progress.target_camera_frame) {
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] id=" << seek_progress.seek_id
                              << " camera settled to " << settled_camera_frame
                              << " (requested " << seek_progress.target_camera_frame
                              << ")" << std::endl;
                }
                seek_progress.target_camera_frame = settled_camera_frame;
                ps.to_display_frame_number = settled_camera_frame;
                ps.slider_frame_number = settled_camera_frame;

                // All cameras settled — try to stabilize display from buffer
                if (!ps.play_video) {
                    stepPausedFrameFromBuffer(settled_camera_frame);
                }

                // Recompute stimulus target from the camera frame we actually settled on.
                int remapped_stimulus_frame = -1;
                if (stimulus_player.loaded && zarr_loader.hasStimulusAlignment()) {
                    auto stim_frame =
                        zarr_loader.getStimulusFrameForCameraFrame(settled_camera_frame);
                    if (stim_frame && *stim_frame >= 0) {
                        remapped_stimulus_frame = *stim_frame;
                    }
                }
                seek_progress.target_stimulus_frame = remapped_stimulus_frame;
                ps.current_stimulus_frame = remapped_stimulus_frame;

                // Transition to stimulus if needed
                if (stimulus_player.loaded && remapped_stimulus_frame >= 0) {
                    seek_progress.state = SeekState::WaitingStimulus;
                    seek_progress.deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    {
                        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                        stimulus_player.seek.seek_frame =
                            static_cast<uint64_t>(remapped_stimulus_frame);
                        stimulus_player.seek.seek_id = seek_progress.seek_id;
                        stimulus_player.seek.use_seek = true;
                        stimulus_player.seek.seek_done = false;
                        stimulus_player.seek.seek_accurate = seek_progress.accurate;
                    }
                    stimulus_player.last_displayed_frame = -1;
                    window_need_decoding[stimulus_player.window_name].store(true);
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] id=" << seek_progress.seek_id
                              << " issuing stimulus seek camera="
                              << settled_camera_frame << " -> stimulus="
                              << remapped_stimulus_frame
                              << " buffered_before="
                              << countBufferedStimulusFrames(stimulus_player)
                              << " newest_before="
                              << getNewestStimulusFrame(stimulus_player)
                              << std::endl;
                } else {
                    // No stimulus — seek is complete
                    seek_progress.state = SeekState::Ready;
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] id=" << seek_progress.seek_id
                              << " complete (no stimulus mapping)" << std::endl;
                }
            } else if (std::chrono::steady_clock::now() >= seek_progress.deadline) {
                std::cerr << "[Seek] id=" << seek_progress.seek_id
                          << " TIMEOUT in WaitingCameras ("
                          << settled << "/" << seek_progress.cameras_total
                          << " settled)" << std::endl;
                seek_progress.state = SeekState::TimedOut;
            }
        }
        if (seek_progress.state == SeekState::WaitingStimulus) {
            bool stim_done = false;
            int settled_stimulus_frame = -1;
            {
                std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                stim_done = stimulus_player.seek.seek_done &&
                            stimulus_player.seek.settled_seek_id == seek_progress.seek_id;
                if (stim_done) {
                    settled_stimulus_frame =
                        static_cast<int>(stimulus_player.seek.seek_frame);
                }
            }
            if (stim_done) {
                if (settled_stimulus_frame >= 0 &&
                    settled_stimulus_frame != seek_progress.target_stimulus_frame) {
                    if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] id=" << seek_progress.seek_id
                              << " stimulus settled to " << settled_stimulus_frame
                              << " (requested " << seek_progress.target_stimulus_frame
                              << ")" << std::endl;
                    seek_progress.target_stimulus_frame = settled_stimulus_frame;
                    ps.current_stimulus_frame = settled_stimulus_frame;
                }
                seek_progress.state = SeekState::Ready;
                if (crimson_seek_debug_logs_enabled()) std::cout << "[Seek] id=" << seek_progress.seek_id
                          << " stimulus settled buffered_after="
                          << countBufferedStimulusFrames(stimulus_player)
                          << " newest_after=" << getNewestStimulusFrame(stimulus_player)
                          << std::endl;
            } else if (std::chrono::steady_clock::now() >= seek_progress.deadline) {
                std::cerr << "[Seek] id=" << seek_progress.seek_id
                          << " TIMEOUT in WaitingStimulus"
                          << " target_stimulus_frame="
                          << seek_progress.target_stimulus_frame
                          << " buffered_now="
                          << countBufferedStimulusFrames(stimulus_player)
                          << " newest_now=" << getNewestStimulusFrame(stimulus_player)
                          << std::endl;
                seek_progress.state = SeekState::TimedOut;
            }
            // Keep stimulus decoder alive regardless of pause_seeked
            window_need_decoding[stimulus_player.window_name].store(true);
        }
        if (seek_progress.state == SeekState::Ready ||
            seek_progress.state == SeekState::TimedOut) {
            if (!ps.play_video && !ps.pause_seeked) {
                // Try once more to grab the frame from buffer
                stepPausedFrameFromBuffer(seek_progress.target_camera_frame);
            }
            seek_progress.state = SeekState::Idle;
        }

        // --- Update playback time ---
        auto now = std::chrono::steady_clock::now();

        if (ps.play_video) {
            ps.accumulated_play_time +=
                std::chrono::duration<double>(now - ps.last_play_time_start)
                    .count() *
                set_playback_speed;
            ps.last_play_time_start = now;
        }
        double playback_time_now = ps.accumulated_play_time;

        const auto file_browser_ui_start = std::chrono::steady_clock::now();
        if (video_loaded && !skeleton_chosen) {
            if (!skeleton) {
                skeleton = std::make_unique<SkeletonContext>();
            }
            if (skeleton_map.empty()) {
                skeleton_map = skeleton_get_all();
            }
        }
        const std::string active_skeleton_name =
            skeleton ? skeleton->name : std::string();
        FileBrowserWindowContext file_browser_context{
            ui_path_config,
            start_folder_name,
            root_dir,
            skeleton_dir,
            video_loaded,
            skeleton_chosen,
            active_skeleton_name,
            skeleton_map,
            cpu_buffer_toggle,
            scene->use_cpu_buffer,
            label_buffer_size,
            playback_preview_scale_mode,
            playback_renderer_mode,
            yolo_detection,
            ps.play_video,
            set_playback_speed,
            inst_speed,
            video_fps,
            current_frame_num,
            ps.last_frame_num_playspeed,
            ps.last_wall_time_playspeed,
            stimulus_player.loaded,
            stimulus_buffer_size,
            stimulus_use_cpu_buffer,
            stimulus_use_software_decode,
            stimulus_player.buffer_size,
            stimulus_player.use_cpu_buffer,
            stimulus_player.use_software_decode,
            dc_context->seek_interval,
        };
        FileBrowserWindowResult file_browser_result =
            drawFileBrowserWindow(file_browser_context,
                                  file_browser_window_state);
        if (file_browser_result.skeleton_selection.has_value()) {
            const auto& selection = *file_browser_result.skeleton_selection;
            bool load_calibration = true;
            if (scene->num_cams > 1) {
                for (u32 i = 0; i < scene->num_cams; i++) {
                    std::string cam_file = root_dir + "/calibration/" +
                                           camera_names[i] + ".yaml";

                    if (!std::filesystem::exists(cam_file)) {
                        load_calibration = false;
                        error_message = "Calibration file not found: " + cam_file;
                        show_error = true;
                        break;
                    }
                    if (!camera_load_params_from_yaml(cam_file,
                                                      camera_params[i],
                                                      error_message)) {
                        load_calibration = false;
                        camera_params.clear();
                        camera_params.resize(scene->num_cams);
                        show_error = true;
                        break;
                    }
                }
            }

            if (load_calibration) {
                skeleton_initialize(selection.name,
                                    root_dir,
                                    skeleton.get(),
                                    selection.primitive);
                plot_keypoints_flag = true;
                keypoints_root_folder = root_dir + "/labeled_data/";
                std::filesystem::create_directory(keypoints_root_folder);
                skeleton_chosen = true;
            }
        }
        switch (file_browser_result.detection_action) {
        case FileBrowserDetectionAction::YOLOv5: {
            std::string yolov5_onnx = root_dir + "/yolo/v5/best.onnx";
            std::string yolov5_labelname = root_dir + "/yolo/v5/label.names";
            read_yolo_labels(yolov5_labelname, &yolo_setting);

            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process,
                                                   yolov5_onnx,
                                                   &yolo_setting,
                                                   i));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::YOLOv8: {
            std::string engine_file_path =
                root_dir + "/yolo/yolorat_bbox/rat_bbox.engine";
            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process_trt,
                                                   engine_file_path,
                                                   i,
                                                   scene->cameras[i].image_width,
                                                   scene->cameras[i].image_height));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::YOLOv8Pose: {
            std::string engine_file_path =
                root_dir + "/yolo/yolopose/rat_pose.engine";
            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process_v8pose,
                                                   engine_file_path,
                                                   i,
                                                   scene->cameras[i].image_width,
                                                   scene->cameras[i].image_height));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::None:
            break;
        }
        if (file_browser_result.accurate_seek_target_frame.has_value()) {
            seekToFrame(*file_browser_result.accurate_seek_target_frame, false);
        }
        frame_file_browser_ui_ms +=
            durationMs(std::chrono::steady_clock::now() - file_browser_ui_start);

        if (video_loaded) {
            const auto frame_debug_ui_start = std::chrono::steady_clock::now();
            std::vector<LoggedBoundingBox> zarr_boxes;
            bool frame_is_interpolated = false;
            const bool frame_has_bbox_edits =
                g_zarr_bbox_edit_state.isFrameDirty(current_frame_num);
            bool dataset_has_synthetic_boxes = false;
            bool dataset_allows_bbox_edit = false;
            ZarrDetectionLoader::FrameDetections detection_details;
            const ZarrDetectionLoader::FrameDetections* detection_details_ptr =
                nullptr;
            if (zarr_loaded) {
                dataset_has_synthetic_boxes =
                    zarr_loader.activeDatasetHasSyntheticDetections();
                if (zarr_loader.hasInterpolation()) {
                    frame_is_interpolated =
                        zarr_loader.isFrameInterpolated(current_frame_num);
                }
                std::vector<LoggedBoundingBox> loaded_zarr_boxes =
                    zarr_loader.getBoundingBoxesForFrame(current_frame_num);
                zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(
                    current_frame_num, loaded_zarr_boxes);
                const bool active_dataset_is_raw_detect =
                    zarr_loader.hasDetectionData() &&
                    (zarr_loader.getActiveDetectionDataset() ==
                     ZarrDetectionLoader::DetectionDataset::RawDetect);
                dataset_allows_bbox_edit =
                    zarr_loader.hasDetectionData() && !active_dataset_is_raw_detect;
                if (!dataset_allows_bbox_edit) {
                    g_zarr_bbox_edit_state.draw_mode = false;
                    g_zarr_bbox_edit_state.cancelDraw();
                    g_zarr_bbox_edit_state.clearSelection();
                }
                const bool need_details =
                    zarr_loader.hasScores() ||
                    zarr_loader.hasHeadingData() ||
                    zarr_loader.hasKeypointData() || dataset_has_synthetic_boxes;
                if (need_details) {
                    detection_details =
                        zarr_loader.getRawDetections(current_frame_num, false);
                    detection_details_ptr = &detection_details;
                }
            }

            const FrameDebugWindowContext frame_debug_context{
                current_frame_num,
                ps.to_display_frame_number,
                ps.slider_frame_number,
                frame_sync_valid_slots,
                frame_sync_empty_slots,
                scene->size_of_buffer,
                frame_sync_recording_remaining,
                frame_sync_recording_total,
                frame_sync_latest_decoded,
                frame_sync_debug_line,
                plot_keypoints_flag,
                keypoints_map.count(current_frame_num) != 0,
                zarr_loaded,
                zarr_loader,
                detection_dataset_labels,
                detection_dataset_choice,
                dataset_has_synthetic_boxes,
                frame_is_interpolated,
                frame_has_bbox_edits,
                dataset_allows_bbox_edit,
                zarr_boxes,
                detection_details_ptr,
                review_frame_filters,
                review_frame_cache.valid,
                review_frame_cache.frames.size(),
                review_frame_status,
                decode_debug_status,
                default_buffer_dump_root.string(),
                g_zarr_bbox_edit_state,
                ps.play_video,
                bbox_payload_status,
                show_keypoint_markers,
                show_heading_arrows,
                show_eye_masks,
            };
            const FrameDebugWindowResult frame_debug_result =
                drawFrameDebugWindow(frame_debug_context, frame_debug_window_state);

            show_keypoint_markers = frame_debug_result.show_keypoint_markers;
            show_heading_arrows = frame_debug_result.show_heading_arrows;
            show_eye_masks = frame_debug_result.show_eye_masks;

            if (frame_debug_result.requested_detection_dataset_index >= 0 &&
                frame_debug_result.requested_detection_dataset_index <
                    static_cast<int>(detection_dataset_ids.size()) &&
                zarr_loader.setActiveDetectionDataset(
                    detection_dataset_ids[frame_debug_result
                                              .requested_detection_dataset_index])) {
                detection_dataset_choice =
                    frame_debug_result.requested_detection_dataset_index;
                refreshDetectionDatasetOptions(zarr_loader);
                g_zarr_bbox_edit_state.clearAll();
                invalidateReviewFrameCache();
                review_frame_status.clear();
                if (zarr_loader.getTotalFrames() > 0 &&
                    current_frame_num >=
                        static_cast<int>(zarr_loader.getTotalFrames())) {
                    current_frame_num =
                        static_cast<int>(zarr_loader.getTotalFrames()) - 1;
                }
            }

            if (frame_debug_result.review_filters_changed) {
                review_frame_filters =
                    frame_debug_result.review_frame_filters;
                invalidateReviewFrameCache();
                review_frame_status.clear();
            }
            if (frame_debug_result.request_prev_review_frame) {
                jumpToReviewFrame(false);
            }
            if (frame_debug_result.request_next_review_frame) {
                jumpToReviewFrame(true);
            }
            if (frame_debug_result.request_dump_decode_buffers) {
                dumpDecodeBuffersToVideos("manual_dump");
            }
            if (frame_debug_result.request_random_seek_dump) {
                randomSeekAndDumpBuffers();
            }
            if (frame_debug_result.request_reset_frame_bbox_edits) {
                g_zarr_bbox_edit_state.clearFrameEdits(current_frame_num);
            }
            if (frame_debug_result.request_clear_bbox_selection) {
                g_zarr_bbox_edit_state.clearSelection();
            }
            if (frame_debug_result.request_build_manual_payload_preview) {
                manual_payload_preview = buildManualDetectPayloadPreview();
                if (!manual_payload_preview->valid) {
                    bbox_payload_status =
                        "Manual payload preview failed: " +
                        manual_payload_preview->error;
                } else {
                    std::ostringstream payload_msg;
                    payload_msg << "Manual payload preview: frames="
                                << manual_payload_preview->total_frames
                                << " detections="
                                << manual_payload_preview->total_detections
                                << " clean="
                                << manual_payload_preview->clean_rows
                                << " interpolated="
                                << manual_payload_preview->interpolated_rows
                                << " manual="
                                << manual_payload_preview->manual_rows
                                << " dirty_frames="
                                << g_zarr_bbox_edit_state.dirtyFrameCount();
                    bbox_payload_status = payload_msg.str();
                }
            }
            if (frame_debug_result.request_write_manual_payload) {
                manual_payload_preview = buildManualDetectPayloadPreview();
                if (!manual_payload_preview->valid) {
                    bbox_payload_status =
                        "Manual write failed: payload preview invalid: " +
                        manual_payload_preview->error;
                } else {
                    std::string source_variant = "interpolated";
                    if (zarr_loader.getActiveDetectionDataset() ==
                        ZarrDetectionLoader::DetectionDataset::RefinedFiltered) {
                        source_variant = "filtered";
                    }

                    std::string write_error;
                    std::string resolved_refined_run;
                    ManualWriteReviewOptions review_opts;
                    const char* intended_use_items[] = {"full_recording",
                                                        "training"};
                    const char* review_state_items[] = {
                        "approved", "needs_review", "pending", "rejected"};
                    review_opts.intended_use =
                        intended_use_items
                            [frame_debug_window_state.manual_write_intended_use];
                    review_opts.state =
                        review_state_items
                            [frame_debug_window_state.manual_write_review_state];
                    const bool write_ok =
                        zarr_loader.writeManualRefinedDetections(
                            manual_payload_preview->frame_indices,
                            manual_payload_preview->bbox_norm_coords,
                            manual_payload_preview->scores,
                            manual_payload_preview->class_ids,
                            manual_payload_preview->frame_counts,
                            manual_payload_preview->detection_source,
                            manual_payload_preview->reason,
                            "manual",
                            source_variant,
                            write_error,
                            &resolved_refined_run,
                            review_opts);
                    if (!write_ok) {
                        bbox_payload_status =
                            "Manual write failed: " + write_error;
                    } else {
                        const size_t written_detections =
                            manual_payload_preview->total_detections;
                        std::string reload_error;
                        const std::string archive_path =
                            zarr_loader.getArchivePath();
                        if (!archive_path.empty() &&
                            zarr_loader.loadZarrFile(archive_path,
                                                     reload_error)) {
                            zarr_loaded = true;
                            if (zarr_loader.isDatasetAvailable(
                                    ZarrDetectionLoader::DetectionDataset::
                                        RefinedManual)) {
                                (void)zarr_loader.setActiveDetectionDataset(
                                    ZarrDetectionLoader::DetectionDataset::
                                        RefinedManual);
                            }
                            refreshDetectionDatasetOptions(zarr_loader);
                            g_zarr_bbox_edit_state.clearAll();
                            manual_payload_preview.reset();
                            invalidateReviewFrameCache();
                            review_frame_status.clear();
                            if (zarr_loader.getTotalFrames() > 0 &&
                                current_frame_num >= static_cast<int>(
                                                         zarr_loader
                                                             .getTotalFrames())) {
                                current_frame_num =
                                    static_cast<int>(
                                        zarr_loader.getTotalFrames()) -
                                    1;
                            }
                            std::ostringstream payload_msg;
                            payload_msg
                                << "Manual write complete: run="
                                << (resolved_refined_run.empty() ? "<latest>"
                                                                : resolved_refined_run)
                                << " group=manual"
                                << " detections=" << written_detections;
                            bbox_payload_status = payload_msg.str();
                        } else {
                            zarr_loaded = false;
                            g_zarr_bbox_edit_state.clearAll();
                            bbox_payload_status =
                                "Manual write succeeded but reload failed: " +
                                reload_error;
                        }
                    }
                }
            }
            frame_frame_debug_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - frame_debug_ui_start);
        }

        // file explorer display
        if (ImGuiFileDialog::Instance()->Display("ChooseMedia")) {
            if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
                auto selected_files =
                    ImGuiFileDialog::Instance()->GetSelection();
                root_dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                skeleton_dir = root_dir;

                // Reset any previously loaded stimulus video
                destroyStimulusPlayback(stimulus_player);
                window_need_decoding[stimulus_player.window_name].store(false);
                window_was_decoding[stimulus_player.window_name] = false;

                // Try to load Zarr detection file
                std::string zarr_error;
                if (loadZarrDetectionFromDirectory(root_dir, zarr_loader, zarr_error)) {
                    zarr_loaded = true;
                    refreshDetectionDatasetOptions(zarr_loader);
                    g_zarr_bbox_edit_state.clearAll();
                } else {
                    zarr_loaded = false;
                    g_zarr_bbox_edit_state.clearAll();
                    std::cout << "No Zarr detection file found (optional): " << zarr_error << std::endl;
                    detection_dataset_ids.clear();
                    detection_dataset_labels.clear();
                    detection_dataset_choice = 0;
                }

                // check if it is mp4, if it is mp4 files
                auto first_selection =
                    *selected_files.begin(); // Dereferencing iterator
                if (string_ends_with(first_selection.first, ".mp4")) {
                    for (const auto &elem : selected_files) {
                        std::string cam_string_full = elem.first;
                        std::size_t last_slash = cam_string_full.find_last_of("/\\");
                        if (last_slash != std::string::npos) {
                            cam_string_full = cam_string_full.substr(last_slash + 1);
                        }
                        std::size_t cam_string_mp4_position =
                            cam_string_full.find(".mp4");
                        std::string cam_string =
                            cam_string_full.substr(0, cam_string_mp4_position);
                        camera_names.push_back(cam_string);
                        std::cout << "camera names: " << cam_string
                                  << std::endl;
                        window_need_decoding[cam_string].store(true);
                        window_was_decoding[cam_string] = true;
                        std::map<std::string, std::string> m;
                        demuxers.push_back(
                            std::make_unique<FFmpegDemuxer>(elem.second.c_str(), m));
                    }
                    std::map<std::string, std::string> m;
                    FFmpegDemuxer dummy_dmuxer(
                        selected_files.begin()->second.c_str(), m);
                    dc_context->seek_interval =
                        (int)dummy_dmuxer
                            .FindKeyFrameInterval(); // get the seek interval
                    video_fps = dummy_dmuxer.GetFramerate();
                    scene->num_cams = selected_files.size();
                    scene->cameras.resize(scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        scene->cameras[j].image_width = demuxers[j]->GetWidth();
                        scene->cameras[j].image_height = demuxers[j]->GetHeight();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    // multiple threads for decoding for selected videos
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &decoder_process, dc_context, demuxers[i].get(),
                            camera_names[i], scene->cameras[i].display_buffer,
                            scene->size_of_buffer, &scene->cameras[i].seek_context,
                            scene->use_cpu_buffer));
                        is_view_focused.push_back(false);
                    }
                    video_loaded = true;
                } else {
                    input_is_imgs = true;
                    for (const auto &elem : selected_files) {
                        std::size_t cam_string_position = elem.first.find("_");
                        std::string cam_name =
                            elem.first.substr(0, cam_string_position);
                        std::string file_name =
                            elem.first.substr(cam_string_position + 1);

                        if (std::find(camera_names.begin(), camera_names.end(),
                                      cam_name) == camera_names.end()) {
                            camera_names.push_back(cam_name);
                        }

                        if (std::find(imgs_names.begin(), imgs_names.end(),
                                      file_name) == imgs_names.end()) {
                            imgs_names.push_back(file_name);
                        }
                    }

                    dc_context->seek_interval = 1;
                    scene->num_cams = camera_names.size();
                    scene->cameras.resize(scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        std::string file_name = root_dir + "/" +
                                                camera_names[j] + "_" +
                                                imgs_names[0];
                        cv::Mat image = cv::imread(file_name, cv::IMREAD_COLOR);
                        scene->cameras[j].image_width = image.cols;
                        scene->cameras[j].image_height = image.rows;
                    }
                    if (imgs_names.size() < label_buffer_size) {
                        label_buffer_size = imgs_names.size();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &image_loader, dc_context, imgs_names,
                            scene->cameras[i].display_buffer, scene->size_of_buffer,
                            &scene->cameras[i].seek_context, scene->use_cpu_buffer,
                            camera_names[i], root_dir));
                        is_view_focused.push_back(false);
                    }
                    video_loaded = true;
                }

                loadCameraCalibrationsForCurrentMedia();
                if (video_loaded && !input_is_imgs) {
                    int initial_frame = std::max(0, ps.to_display_frame_number);
                    double seek_fps = (video_fps > 0.0) ? video_fps : 30.0;
                    seek_all_cameras(scene, initial_frame, seek_fps, ps, true,
                                     &zarr_loader, &stimulus_player);
                }
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseZarrArchive")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                auto selection = ImGuiFileDialog::Instance()->GetSelection();
                std::string selected_zarr_path;
                if (!selection.empty()) {
                    selected_zarr_path = selection.begin()->second;
                } else {
                    selected_zarr_path = ImGuiFileDialog::Instance()->GetCurrentPath();
                }

                std::string zarr_error;
                if (loadZarrDetectionFromPath(selected_zarr_path, zarr_loader, zarr_error)) {
                    zarr_loaded = true;
                    refreshDetectionDatasetOptions(zarr_loader);
                    g_zarr_bbox_edit_state.clearAll();
                    invalidateReviewFrameCache();
                    review_frame_status.clear();
                    std::cout << "Loaded Zarr archive override: "
                              << zarr_loader.getArchivePath() << std::endl;
                    tryAutoLoadAffiliatedVideoFromZarr("Load Zarr Archive");
                    tryAutoLoadStimulusVideo("file-dialog");
                } else {
                    zarr_loaded = false;
                    g_zarr_bbox_edit_state.clearAll();
                    invalidateReviewFrameCache();
                    review_frame_status.clear();
                    std::cout << "Failed to load Zarr archive override: " << zarr_error << std::endl;
                    detection_dataset_ids.clear();
                    detection_dataset_labels.clear();
                    detection_dataset_choice = 0;
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseStimulus")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                auto selection = ImGuiFileDialog::Instance()->GetSelection();
                if (!selection.empty()) {
                    std::string stimulus_path = selection.begin()->second;
                    int selected_stimulus_buffer_size =
                        std::max(1, stimulus_buffer_size);
                    if (!initializeStimulusPlayback(stimulus_player, stimulus_path,
                                                    selected_stimulus_buffer_size,
                                                    stimulus_use_cpu_buffer,
                                                    stimulus_use_software_decode,
                                                    kCudaDeviceIndex)) {
                        show_error = true;
                        error_message = "Failed to load stimulus video: " + stimulus_path;
                    } else {
                        window_was_decoding[stimulus_player.window_name] = false;
                        window_need_decoding[stimulus_player.window_name].store(false);
                        if (zarr_loaded) {
                            scheduleStimulusSeek(stimulus_player, &zarr_loader,
                                                 ps.to_display_frame_number,
                                                 !ps.play_video);
                        }
                    }
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseSkeleton")) {
            if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
                auto skeleton_file =
                    ImGuiFileDialog::Instance()->GetSelection();

                if (!skeleton_file.empty()) {

                    bool load_calibration = true;
                    if (scene->num_cams > 1) {
                        for (u32 i = 0; i < scene->num_cams; i++) {
                            std::string cam_file = root_dir + "/calibration/" +
                                                   camera_names[i] + ".yaml";
                            if (!camera_load_params_from_yaml(cam_file, camera_params[i], error_message)) {
                                load_calibration = false;
                                camera_params.clear();
                                camera_params.resize(scene->num_cams);
                                show_error = true;
                                break;
                            }
                        }
                    }

                    if (load_calibration) {
                        skeleton_dir =
                            ImGuiFileDialog::Instance()->GetCurrentPath();
                        skeleton_initialize("", skeleton_file.begin()->second,
                                            skeleton.get(), SP_LOAD);
                        plot_keypoints_flag = true;
                        keypoints_root_folder = root_dir + "/labeled_data/";
                        skeleton_chosen = true;
                    }
                }
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        static int select_corr_head = 0;
        if (video_loaded && (!ps.play_video)) {
            int visible_idx = 0;
            if (!ps.pause_seeked) {
                for (int i = 0; i < scene->num_cams; i++) {
                    if (window_was_decoding[camera_names[i]]) {
                        visible_idx = i;
                        break;
                    }
                }
            }

            struct PausedBufferListItem {
                int slot = -1;
                int frame = -1;
            };
            std::vector<PausedBufferListItem> paused_buffer_items;
            paused_buffer_items.reserve(scene->size_of_buffer);
            for (int i = 0; i < scene->size_of_buffer; ++i) {
                const auto& slot = scene->cameras[visible_idx].display_buffer[i];
                if (slot.available_to_write || slot.frame_number < 0) {
                    continue;
                }
                paused_buffer_items.push_back({i, slot.frame_number});
            }
            std::sort(paused_buffer_items.begin(), paused_buffer_items.end(),
                      [](const PausedBufferListItem& a,
                         const PausedBufferListItem& b) {
                          if (a.frame == b.frame) {
                              return a.slot < b.slot;
                          }
                          return a.frame < b.frame;
                      });

            auto getPreferredPausedSlot = [&]() -> int {
                int exact_slot = -1;
                for (int i = 0; i < scene->size_of_buffer; ++i) {
                    const auto& slot = scene->cameras[visible_idx].display_buffer[i];
                    if (!slot.available_to_write &&
                        slot.frame_number == ps.to_display_frame_number) {
                        exact_slot = i;
                        break;
                    }
                }
                if (exact_slot >= 0) {
                    return exact_slot;
                }
                return findNearestPausedBufferSlot(
                    visible_idx, std::max(0, ps.to_display_frame_number));
            };

            ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
            const auto buffer_window_ui_start = std::chrono::steady_clock::now();
            if (ImGui::Begin("Frames in the buffer")) {
                ImGui::Text("Valid frames: %zu / %u",
                            paused_buffer_items.size(), scene->size_of_buffer);
                int selected_item = -1;
                int best_distance = std::numeric_limits<int>::max();
                int best_frame = std::numeric_limits<int>::min();
                for (int i = 0; i < static_cast<int>(paused_buffer_items.size()); ++i) {
                    const auto& item = paused_buffer_items[i];
                    if (item.frame == ps.to_display_frame_number) {
                        selected_item = i;
                        best_distance = 0;
                        best_frame = item.frame;
                        break;
                    }
                    const int distance = std::abs(item.frame - ps.to_display_frame_number);
                    if (distance < best_distance ||
                        (distance == best_distance && item.frame > best_frame)) {
                        best_distance = distance;
                        best_frame = item.frame;
                        selected_item = i;
                    }
                }
                if (paused_buffer_items.empty()) {
                    ImGui::TextDisabled("No decoded frames currently buffered.");
                } else {
                    for (int i = 0; i < static_cast<int>(paused_buffer_items.size()); ++i) {
                        const auto& item = paused_buffer_items[i];
                        char label[96];
                        const int delta = item.frame - ps.to_display_frame_number;
                        snprintf(label, sizeof(label), "Frame %d (slot %d, delta %+d)",
                                 item.frame, item.slot, delta);
                        ImGui::PushID(i);
                        if (ImGui::Selectable(label, selected_item == i)) {
                            selected_item = i;
                            ps.to_display_frame_number = item.frame;
                            ps.slider_frame_number = item.frame;
                            ps.pause_seeked = true;
                        }
                        ImGui::PopID();
                    }
                }

                if (ImGui::IsKeyPressed(ImGuiKey_Comma, true)) {
                    if (selected_item > 0 &&
                        selected_item <= static_cast<int>(paused_buffer_items.size()) - 1) {
                        --selected_item;
                        ps.to_display_frame_number =
                            paused_buffer_items[selected_item].frame;
                        ps.slider_frame_number = ps.to_display_frame_number;
                        ps.pause_seeked = true;
                    }
                };

                if (ImGui::IsKeyPressed(ImGuiKey_Period, true)) {
                    if (selected_item >= 0 &&
                        selected_item < static_cast<int>(paused_buffer_items.size()) - 1) {
                        ++selected_item;
                        ps.to_display_frame_number =
                            paused_buffer_items[selected_item].frame;
                        ps.slider_frame_number = ps.to_display_frame_number;
                        ps.pause_seeked = true;
                    }
                };
            }
            ImGui::End();
            frame_buffer_window_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - buffer_window_ui_start);
            select_corr_head = getPreferredPausedSlot();
            if (select_corr_head >= 0) {
                ps.read_head = select_corr_head;
                current_frame_num =
                    scene->cameras[visible_idx].display_buffer[select_corr_head]
                        .frame_number;
                // Keep paused seek target stable unless the user explicitly
                // selects/seeks a different frame.
            } else {
                current_frame_num = std::max(0, ps.to_display_frame_number);
            }
        }

        // Render a video frame
        if (video_loaded) {
            if (ps.play_video && scene->num_cams > 0 && scene->size_of_buffer > 0) {
                int live_frame =
                    scene->cameras[0].display_buffer[ps.read_head % scene->size_of_buffer].frame_number;
                if (live_frame >= 0) {
                    current_frame_num = live_frame;
                } else {
                    current_frame_num = ps.to_display_frame_number;
                }
            }
            if (zarr_loaded && zarr_loader.hasStimulusAlignment()) {
                int stim_source_frame = ps.play_video ? current_frame_num
                                                      : ps.to_display_frame_number;
                if (auto stim_frame = zarr_loader.getStimulusFrameForCameraFrame(stim_source_frame)) {
                    ps.current_stimulus_frame = *stim_frame;
                } else {
                    ps.current_stimulus_frame = -1;
                }
            } else {
                ps.current_stimulus_frame = -1;
            }
            const int paused_visible_idx = ps.play_video ? -1 : getVisibleCameraIndex();
            for (int j = 0; j < scene->num_cams; j++) {
                const std::string &win_name = camera_names[j];

                // layout
                ImGui::SetNextWindowSize(ImVec2(500, 400),
                                         ImGuiCond_FirstUseEver);
                ImVec2 window_pos;

                if (scene->num_cams < 8) {
                    if (j % 2 == 0) {
                        window_pos.y = 200.0;
                        window_pos.x = (j / 2.0) * 500;
                    } else {
                        window_pos.y = 600.0;
                        window_pos.x = (j - 1) / 2.0 * 500;
                    }
                } else {
                    int row = j % 4;
                    float base_x = (row == 0) ? j : (j - 1);
                    float x_group = (base_x / 4.0f) * 500;
                    switch (row) {
                    case 0:
                        window_pos = {x_group, 200.0f};
                        break;
                    case 1:
                        window_pos = {x_group, 600.0f};
                        break;
                    case 2:
                        window_pos = {x_group, 1000.0f};
                        break;
                    case 3:
                        window_pos = {x_group, 1400.0f};
                        break;
                    }
                }

                ImGui::SetNextWindowPos(window_pos, ImGuiCond_FirstUseEver);
                bool is_visible = ImGui::Begin(win_name.c_str());

                if (!window_was_decoding[win_name] && is_visible &&
                    ps.play_video) {
                    // seek if visibility has changed
                    seekToFrame(current_frame_num, true);
                }

                if (!window_was_decoding[win_name] && is_visible &&
                    !ps.play_video && !ps.pause_seeked) {
                    // seek if visibility has changed
                    seekToFrame(current_frame_num, true);
                    for (auto &[key, value] : window_need_decoding) {
                        value.store(true);
                    }
                }

                if (ps.play_video) {
                    window_need_decoding[win_name].store(is_visible);
                };

                if (is_visible) {
                    unsigned char *presented_rgba_cuda_buffer =
                        scene->cameras[j].pbo_cuda.cuda_buffer;
                    bool swap_playback_surface_after_draw = false;
                    auto uploadCameraFrameToTexture = [&](int slot_index) -> int {
                        if (slot_index < 0) {
                            return -1;
                        }
                        auto &camera = scene->cameras[j];
                        auto &slot = camera.display_buffer[slot_index];
                        const int source_frame_number = slot.frame_number;
                        const bool lightweight_playback_renderer_active =
                            playbackLightweightRendererIsActive();
                        const bool preview_active = playbackPreviewIsActive();
                        const bool preview_resize_active =
                            preview_active && scene->use_cpu_buffer;
                        const bool preview_sampling_active =
                            preview_active && !scene->use_cpu_buffer;
                        const bool direct_nv12_playback_present_active =
                            ps.play_video &&
                            lightweight_playback_renderer_active &&
                            !scene->use_cpu_buffer && !yolo_detection &&
                            slot.format == PictureBufferFormat::NV12;
                        const int desired_preview_sampling_mode =
                            preview_sampling_active
                                ? playback_preview_scale_mode
                                : 0;
                        const double preview_scale =
                            preview_resize_active
                                ? playbackPreviewScaleFactor()
                                : 1.0;
                        const int target_texture_width =
                            preview_resize_active
                                ? std::max(1, static_cast<int>(std::lround(
                                                  static_cast<double>(camera.image_width) *
                                                  preview_scale)))
                                : camera.image_width;
                        const int target_texture_height =
                            preview_resize_active
                                ? std::max(1, static_cast<int>(std::lround(
                                                  static_cast<double>(camera.image_height) *
                                                  preview_scale)))
                                : camera.image_height;
                        const bool texture_shape_changed =
                            camera.display_texture_width != target_texture_width ||
                            camera.display_texture_height != target_texture_height;
                        auto applyTexturePreviewSampling = [&](GLuint texture,
                                                              int *applied_mode,
                                                              bool regenerate_mips) {
                            bind_texture(&texture);
                            if (desired_preview_sampling_mode > 0) {
                                const int max_dim =
                                    std::max(target_texture_width,
                                             target_texture_height);
                                const int max_mip_level =
                                    max_dim > 0
                                        ? static_cast<int>(std::floor(std::log2(
                                              static_cast<double>(max_dim))))
                                        : 0;
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                                max_mip_level);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                                GL_LINEAR_MIPMAP_LINEAR);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                                GL_LINEAR);
                                glTexParameterf(
                                    GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS,
                                    desired_preview_sampling_mode == 1 ? 1.0f
                                                                       : 2.0f);
                                if (regenerate_mips) {
                                    const auto mip_start =
                                        std::chrono::steady_clock::now();
                                    glGenerateMipmap(GL_TEXTURE_2D);
                                    frame_camera_preview_resize_ms += durationMs(
                                        std::chrono::steady_clock::now() -
                                        mip_start);
                                }
                            } else {
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                                GL_LINEAR);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                                GL_LINEAR);
                                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS,
                                                0.0f);
                            }
                            unbind_texture();
                            if (applied_mode != nullptr) {
                                *applied_mode = desired_preview_sampling_mode;
                            }
                        };
                        auto uploadSurfaceToTexture = [&](PBO_CUDA &surface_pbo,
                                                          GLuint surface_texture,
                                                          int *surface_preview_mode) {
                            const auto texture_upload_start =
                                std::chrono::steady_clock::now();
                            GLuint upload_pbo = surface_pbo.pbo;
                            bind_pbo(&upload_pbo);
                            bind_texture(&surface_texture);
                            upload_image_pbo_to_texture(target_texture_width,
                                                        target_texture_height);
                            unbind_pbo();
                            unbind_texture();
                            frame_camera_texture_upload_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                texture_upload_start);
                            applyTexturePreviewSampling(
                                surface_texture,
                                surface_preview_mode,
                                desired_preview_sampling_mode > 0);
                        };
                        auto presentNv12SlotToTexture = [&](PBO_CUDA &surface_pbo,
                                                            GLuint destination_texture,
                                                            int *surface_preview_mode) {
                            const auto pbo_copy_start =
                                std::chrono::steady_clock::now();
                            ck(cudaMemcpy(surface_pbo.cuda_buffer, slot.frame,
                                          slot.frame_bytes,
                                          cudaMemcpyDeviceToDevice));
                            frame_camera_pbo_copy_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                pbo_copy_start);

                            const auto texture_upload_start =
                                std::chrono::steady_clock::now();
                            presentNv12PboToTexture(
                                camera, surface_pbo, destination_texture,
                                &nv12_playback_presenter,
                                slot.pitch_bytes > 0
                                    ? slot.pitch_bytes
                                    : static_cast<int>(camera.image_width),
                                slot.color_matrix);
                            frame_camera_texture_upload_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                texture_upload_start);
                            applyTexturePreviewSampling(
                                destination_texture, surface_preview_mode,
                                desired_preview_sampling_mode > 0);
                        };
                        const bool preview_sampling_changed =
                            camera.applied_preview_sampling_mode !=
                            desired_preview_sampling_mode;
                        const bool pipeline_playback_present =
                            ps.play_video && !scene->use_cpu_buffer &&
                            !preview_resize_active && !yolo_detection &&
                            !texture_shape_changed;
                        const bool direct_nv12_pipeline_present =
                            pipeline_playback_present &&
                            direct_nv12_playback_present_active;
                        if (camera.texture_has_valid_frame &&
                            camera.last_uploaded_frame == source_frame_number &&
                            !texture_shape_changed) {
                            const auto front_path_start =
                                std::chrono::steady_clock::now();
                            if (preview_sampling_changed) {
                                applyTexturePreviewSampling(
                                    camera.image_texture,
                                    &camera.applied_preview_sampling_mode,
                                    desired_preview_sampling_mode > 0);
                            }
                            frame_camera_playback_front_path_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                front_path_start);
                            presented_rgba_cuda_buffer =
                                camera.pbo_cuda.cuda_buffer;
                            return camera.last_uploaded_frame;
                        }
                        if (texture_shape_changed) {
                            const auto texture_resize_start =
                                std::chrono::steady_clock::now();
                            render_resize_camera_texture(&camera,
                                                         target_texture_width,
                                                         target_texture_height);
                            frame_camera_texture_resize_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                texture_resize_start);
                            camera.last_uploaded_frame = -1;
                            camera.texture_has_valid_frame = false;
                            camera.playback_staging_frame = -1;
                            camera.playback_staging_valid = false;
                            camera.playback_staging_preview_sampling_mode = -1;
                        }
                        if (pipeline_playback_present && camera.texture_has_valid_frame) {
                            const auto front_path_start =
                                std::chrono::steady_clock::now();
                            if (preview_sampling_changed) {
                                applyTexturePreviewSampling(
                                    camera.image_texture,
                                    &camera.applied_preview_sampling_mode,
                                    desired_preview_sampling_mode > 0);
                            }
                            frame_camera_playback_front_path_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                front_path_start);
                            if (!camera.playback_staging_valid ||
                                camera.playback_staging_frame != source_frame_number) {
                                const auto stage_total_start =
                                    std::chrono::steady_clock::now();
                                const auto stage_upload_start =
                                    std::chrono::steady_clock::now();
                                if (direct_nv12_pipeline_present) {
                                    presentNv12SlotToTexture(
                                        camera.playback_staging_pbo,
                                        camera.playback_staging_texture,
                                        &camera.playback_staging_preview_sampling_mode);
                                } else {
                                    if (slot.format == PictureBufferFormat::NV12) {
                                        const auto convert_start =
                                            std::chrono::steady_clock::now();
                                        Nv12ToColor32<RGBA32>(
                                            slot.frame,
                                            slot.pitch_bytes > 0
                                                ? slot.pitch_bytes
                                                : camera.image_width,
                                            camera.playback_staging_pbo.cuda_buffer,
                                            4 * static_cast<int>(camera.image_width),
                                            static_cast<int>(camera.image_width),
                                            static_cast<int>(camera.image_height),
                                            slot.color_matrix);
                                        frame_camera_display_convert_ms += durationMs(
                                            std::chrono::steady_clock::now() -
                                            convert_start);
                                    } else {
                                        const auto pbo_copy_start =
                                            std::chrono::steady_clock::now();
                                        ck(cudaMemcpy(
                                            camera.playback_staging_pbo.cuda_buffer,
                                            slot.frame,
                                            camera.image_width *
                                                camera.image_height * 4,
                                            cudaMemcpyDeviceToDevice));
                                        frame_camera_pbo_copy_ms += durationMs(
                                            std::chrono::steady_clock::now() -
                                            pbo_copy_start);
                                    }
                                    uploadSurfaceToTexture(
                                        camera.playback_staging_pbo,
                                        camera.playback_staging_texture,
                                        &camera.playback_staging_preview_sampling_mode);
                                }
                                frame_camera_playback_stage_upload_ms += durationMs(
                                    std::chrono::steady_clock::now() -
                                    stage_upload_start);
                                camera.playback_staging_frame = source_frame_number;
                                camera.playback_staging_valid = true;
                                const double stage_total_ms = durationMs(
                                    std::chrono::steady_clock::now() -
                                    stage_total_start);
                                frame_camera_playback_stage_total_ms +=
                                    stage_total_ms;
                                frame_camera_upload_ms += stage_total_ms;
                                frame_camera_upload_count++;
                                swap_playback_surface_after_draw = true;
                            }
                            presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
                            return camera.last_uploaded_frame;
                        }
                        const auto upload_start =
                            std::chrono::steady_clock::now();
                        if (preview_resize_active) {
                            const cv::Mat full_rgba(camera.image_height,
                                                    camera.image_width,
                                                    CV_8UC4,
                                                    slot.frame);
                            camera.playback_preview_rgba_cpu.resize(
                                static_cast<size_t>(target_texture_width) *
                                static_cast<size_t>(target_texture_height) * 4);
                            cv::Mat preview_rgba(
                                target_texture_height, target_texture_width, CV_8UC4,
                                camera.playback_preview_rgba_cpu.data());
                            const auto preview_resize_start =
                                std::chrono::steady_clock::now();
                            cv::resize(full_rgba, preview_rgba,
                                       cv::Size(target_texture_width,
                                                target_texture_height),
                                       0.0, 0.0, cv::INTER_AREA);
                            frame_camera_preview_resize_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                preview_resize_start);
                            const auto pbo_copy_start =
                                std::chrono::steady_clock::now();
                            ck(cudaMemcpy(
                                camera.pbo_cuda.cuda_buffer,
                                camera.playback_preview_rgba_cpu.data(),
                                static_cast<size_t>(target_texture_width) *
                                    static_cast<size_t>(target_texture_height) * 4,
                                cudaMemcpyHostToDevice));
                            frame_camera_pbo_copy_ms += durationMs(
                                std::chrono::steady_clock::now() - pbo_copy_start);
                            presented_rgba_cuda_buffer =
                                camera.pbo_cuda.cuda_buffer;
                        } else if (scene->use_cpu_buffer) {
                            const auto pbo_copy_start =
                                std::chrono::steady_clock::now();
                            ck(cudaMemcpy(
                                camera.pbo_cuda.cuda_buffer,
                                slot.frame,
                                camera.image_width * camera.image_height * 4,
                                cudaMemcpyHostToDevice));
                            frame_camera_pbo_copy_ms += durationMs(
                                std::chrono::steady_clock::now() - pbo_copy_start);
                            presented_rgba_cuda_buffer =
                                camera.pbo_cuda.cuda_buffer;
                        } else {
                            if (direct_nv12_playback_present_active) {
                                presentNv12SlotToTexture(
                                    camera.pbo_cuda,
                                    camera.image_texture,
                                    &camera.applied_preview_sampling_mode);
                                presented_rgba_cuda_buffer =
                                    camera.pbo_cuda.cuda_buffer;
                            } else if (slot.format == PictureBufferFormat::NV12) {
                                const auto convert_start =
                                    std::chrono::steady_clock::now();
                                Nv12ToColor32<RGBA32>(
                                    slot.frame,
                                    slot.pitch_bytes > 0 ? slot.pitch_bytes
                                                         : camera.image_width,
                                    camera.pbo_cuda.cuda_buffer,
                                    4 * static_cast<int>(camera.image_width),
                                    static_cast<int>(camera.image_width),
                                    static_cast<int>(camera.image_height),
                                    slot.color_matrix);
                                frame_camera_display_convert_ms += durationMs(
                                    std::chrono::steady_clock::now() -
                                    convert_start);
                            } else {
                                const auto pbo_copy_start =
                                    std::chrono::steady_clock::now();
                                ck(cudaMemcpy(
                                    camera.pbo_cuda.cuda_buffer, slot.frame,
                                    camera.image_width * camera.image_height * 4,
                                    cudaMemcpyDeviceToDevice));
                                frame_camera_pbo_copy_ms += durationMs(
                                    std::chrono::steady_clock::now() -
                                    pbo_copy_start);
                            }
                            presented_rgba_cuda_buffer =
                                camera.pbo_cuda.cuda_buffer;
                        }
                        if (!direct_nv12_playback_present_active) {
                            uploadSurfaceToTexture(
                                camera.pbo_cuda,
                                camera.image_texture,
                                &camera.applied_preview_sampling_mode);
                        }
                        camera.playback_staging_frame = -1;
                        camera.playback_staging_valid = false;
                        camera.playback_staging_preview_sampling_mode = -1;
                        presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
                        camera.last_uploaded_frame = source_frame_number;
                        camera.texture_has_valid_frame = true;
                        frame_camera_upload_ms += durationMs(
                            std::chrono::steady_clock::now() - upload_start);
                        frame_camera_upload_count++;
                        return source_frame_number;
                    };
                    auto clearCameraDisplayBuffer = [&]() {
                        auto &camera = scene->cameras[j];
                        const int clear_width =
                            camera.display_texture_width > 0
                                ? camera.display_texture_width
                                : camera.image_width;
                        const int clear_height =
                            camera.display_texture_height > 0
                                ? camera.display_texture_height
                                : camera.image_height;
                        const size_t bytes =
                            static_cast<size_t>(clear_width) *
                            static_cast<size_t>(clear_height) * 4;
                        if (bytes == 0) {
                            return;
                        }
                        ck(cudaMemset(camera.pbo_cuda.cuda_buffer, 0, bytes));
                        bind_pbo(&camera.pbo_cuda.pbo);
                        bind_texture(&camera.image_texture);
                        upload_image_pbo_to_texture(clear_width, clear_height);
                        unbind_pbo();
                        unbind_texture();
                        camera.last_uploaded_frame = -1;
                        camera.texture_has_valid_frame = false;
                        camera.applied_preview_sampling_mode = -1;
                        camera.playback_staging_frame = -1;
                        camera.playback_staging_valid = false;
                        camera.playback_staging_preview_sampling_mode = -1;
                    };
                    int presented_slot = -1;
                    int presented_frame = -1;
                    if (ps.play_video) {
                        // if the current frame is ready, upload for display,
                        // otherwise wait for the frame to get ready
                        // while (scene->cameras[j].display_buffer[ps.read_head]
                        //            .frame_number !=
                        //        ps.to_display_frame_number) {
                        //     std::cout
                        //         << win_name << " , read head: " <<
                        //         ps.read_head
                        //         << ", frame_number: "
                        //         << scene->cameras[j].display_buffer[ps.read_head]
                        //                .frame_number
                        //         << ", to_display_frame_number: "
                        //         << ps.to_display_frame_number << std::endl;
                        //     std::this_thread::sleep_for(
                        //         std::chrono::milliseconds(1));
                        // }

                        const int preferred_slot =
                            ps.read_head % scene->size_of_buffer;
                        const int display_slot =
                            findDisplaySlotForFrame(
                                j, ps.to_display_frame_number, preferred_slot);

                        int displayed_frame_num = -1;
                        presented_slot = display_slot;
                        if (display_slot >= 0) {
                            displayed_frame_num =
                                uploadCameraFrameToTexture(display_slot);
                            presented_frame = displayed_frame_num;
                            if (displayed_frame_num >= 0) {
                                current_frame_num = displayed_frame_num;
                            } else {
                                current_frame_num = ps.to_display_frame_number;
                            }
                        } else {
                            presented_frame = -1;
                            current_frame_num = ps.to_display_frame_number;
                            if (scene->cameras[j].texture_has_valid_frame) {
                                clearCameraDisplayBuffer();
                            }
                        }
                    } else {
                        if (ps.pause_seeked || select_corr_head >= 0) {
                            int paused_slot = select_corr_head;
                            if (paused_slot < 0) {
                                paused_slot = ps.read_head % scene->size_of_buffer;
                            }
                            paused_slot = findDisplaySlotForFrame(
                                j,
                                std::max(0, ps.to_display_frame_number),
                                paused_slot);

                            if (paused_slot >= 0) {
                                presented_slot = paused_slot;
                                presented_frame =
                                    uploadCameraFrameToTexture(paused_slot);
                                if (presented_frame >= 0) {
                                    current_frame_num = presented_frame;
                                } else {
                                    current_frame_num =
                                        std::max(0, ps.to_display_frame_number);
                                }
                            } else {
                                if (scene->cameras[j].texture_has_valid_frame) {
                                    clearCameraDisplayBuffer();
                                }
                            }
                        } else {
                            if (scene->cameras[j].texture_has_valid_frame) {
                                clearCameraDisplayBuffer();
                            }
                        }
                    }

                    // sync yolo detection
                    if (yolo_detection) {
                        std::unique_lock<std::mutex> lck(g_mutexes[j]);
                        // std::cout << "main_thread: acquire lock" <<
                        // std::endl;
                        yolo_input_frames_rgba[j] = presented_rgba_cuda_buffer;
                        g_ready[j] = true;
                        g_cvs[j].notify_one();
                    }

                    const auto scene_ui_build_start =
                        std::chrono::steady_clock::now();
                    ImGui::BeginGroup();
                    std::string scene_name = "scene view" + std::to_string(j);
                    ImGui::BeginChild(
                        scene_name.c_str(),
                        ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
                    ImVec2 avail_size = ImGui::GetContentRegionAvail();

                    // ImGui::Image((void*)(intptr_t)image_texture[j],
                    // avail_size);
                    //
                    if (plot_keypoints_flag) {
                        if (keypoints_map.find(current_frame_num) ==
                            keypoints_map.end()) {
                            keypoints_find = false;
                        } else {
                            keypoints_find = true;
                        }
                    }

                    ImPlotInputMap& plot_input_map = ImPlot::GetInputMap();
                    const int previous_plot_pan_mod = plot_input_map.PanMod;
                    bool restore_plot_pan_mod = false;
                    bool suppress_crosshairs = false;
                    if (zarr_loaded) {
                        const bool active_dataset_is_raw_detect_for_input =
                            zarr_loader.hasDetectionData() &&
                            (zarr_loader.getActiveDetectionDataset() ==
                             ZarrDetectionLoader::DetectionDataset::RawDetect);
                        const bool dataset_allows_bbox_edit_for_input =
                            zarr_loader.hasDetectionData() && !active_dataset_is_raw_detect_for_input;
                        const bool draw_mode_active_for_current_frame =
                            g_zarr_bbox_edit_state.draw_mode;
                        const bool has_bbox_selection_for_current_frame =
                            (g_zarr_bbox_edit_state.selected_frame == current_frame_num) &&
                            (g_zarr_bbox_edit_state.selected_box >= 0);
                        if (dataset_allows_bbox_edit_for_input &&
                            g_zarr_bbox_edit_state.enabled &&
                            (draw_mode_active_for_current_frame ||
                             has_bbox_selection_for_current_frame)) {
                            // While editing (move or draw), require Shift+drag to pan so
                            // Ctrl+left-drag can be used for bbox manipulation.
                            plot_input_map.PanMod = ImGuiMod_Shift;
                            restore_plot_pan_mod = true;
                            suppress_crosshairs = true;
                        }
                    }

                    const bool lightweight_playback_renderer_active =
                        playbackLightweightRendererIsActive();
                    ImPlotFlags scene_plot_flags = ImPlotFlags_Equal;
                    if (!suppress_crosshairs &&
                        !lightweight_playback_renderer_active) {
                        scene_plot_flags |= ImPlotFlags_Crosshairs;
                    }
                    if (lightweight_playback_renderer_active) {
                        scene_plot_flags |=
                            ImPlotFlags_CanvasOnly | ImPlotFlags_NoFrame;
                    } else {
                        scene_plot_flags |= ImPlotAxisFlags_AutoFit;
                    }
                    int scene_plot_style_var_count = 0;
                    int scene_plot_style_color_count = 0;
                    if (lightweight_playback_renderer_active) {
                        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding,
                                             ImVec2(0.0f, 0.0f));
                        scene_plot_style_var_count++;
                        ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding,
                                             ImVec2(0.0f, 0.0f));
                        scene_plot_style_var_count++;
                        ImPlot::PushStyleColor(ImPlotCol_PlotBg,
                                               ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        scene_plot_style_color_count++;
                        ImPlot::PushStyleColor(
                            ImPlotCol_PlotBorder,
                            ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        scene_plot_style_color_count++;
                    } else {
                        ImPlot::PushStyleVar(ImPlotStyleVar_LegendPadding,
                                             ImVec2(12.0f, 12.0f));
                        scene_plot_style_var_count++;
                    }
                    const auto camera_plot_image_ui_start =
                        std::chrono::steady_clock::now();
                    if (ImPlot::BeginPlot("##no_plot_name", avail_size, scene_plot_flags)) {
                        if (lightweight_playback_renderer_active) {
                            constexpr ImPlotAxisFlags kPlaybackAxisFlags =
                                ImPlotAxisFlags_NoDecorations |
                                ImPlotAxisFlags_NoMenus |
                                ImPlotAxisFlags_NoHighlight |
                                ImPlotAxisFlags_NoSideSwitch;
                            ImPlot::SetupAxes(nullptr, nullptr,
                                              kPlaybackAxisFlags,
                                              kPlaybackAxisFlags);
                            ImPlot::SetupAxesLimits(
                                0,
                                static_cast<double>(scene->cameras[j].image_width),
                                0,
                                static_cast<double>(scene->cameras[j].image_height),
                                ImGuiCond_Once);
                        } else {
                            ImPlot::SetupLegend(ImPlotLocation_SouthWest,
                                                ImPlotLegendFlags_None);
                        }
                        ImPlot::PlotImage(
                            "##no_image_name",
                            (ImTextureID)(intptr_t)scene->cameras[j].image_texture,
                            ImVec2(0, 0),
                            ImVec2(scene->cameras[j].image_width,
                                scene->cameras[j].image_height));
                        {
                            const ImPlotRect plot_limits =
                                ImPlot::GetPlotLimits();
                            const ImVec2 plot_size = ImPlot::GetPlotSize();
                            const double image_width =
                                static_cast<double>(
                                    scene->cameras[j].image_width);
                            const double image_height =
                                static_cast<double>(
                                    scene->cameras[j].image_height);
                            const double clamped_x_min = std::clamp(
                                plot_limits.X.Min, 0.0, image_width);
                            const double clamped_x_max = std::clamp(
                                plot_limits.X.Max, 0.0, image_width);
                            const double clamped_y_min = std::clamp(
                                plot_limits.Y.Min, 0.0, image_height);
                            const double clamped_y_max = std::clamp(
                                plot_limits.Y.Max, 0.0, image_height);
                            const double visible_width = std::max(
                                0.0, clamped_x_max - clamped_x_min);
                            const double visible_height = std::max(
                                0.0, clamped_y_max - clamped_y_min);
                            const double total_area = image_width * image_height;
                            const double visible_area =
                                visible_width * visible_height;
                            const double visible_fraction =
                                total_area > 0.0
                                    ? std::clamp(visible_area / total_area,
                                                 0.0, 1.0)
                                    : std::numeric_limits<double>::quiet_NaN();
                            const bool zoomed_in =
                                visible_width < (image_width - 1.0) ||
                                visible_height < (image_height - 1.0);
                            perf_camera_viewport_width_px =
                                static_cast<double>(plot_size.x);
                            perf_camera_viewport_height_px =
                                static_cast<double>(plot_size.y);
                            perf_camera_view_x_min = clamped_x_min;
                            perf_camera_view_x_max = clamped_x_max;
                            perf_camera_view_y_min = clamped_y_min;
                            perf_camera_view_y_max = clamped_y_max;
                            perf_camera_view_visible_fraction =
                                visible_fraction;
                            perf_camera_view_zoomed_in =
                                zoomed_in ? 1 : 0;
                        }
                        frame_camera_plot_image_ui_ms += durationMs(
                            std::chrono::steady_clock::now() -
                            camera_plot_image_ui_start);
                        const auto camera_overlay_ui_start =
                            std::chrono::steady_clock::now();

                        if (yolo_detection) {
                            draw_cv_contours(
                                yolo_boxes.at(j), yolo_labels.at(j),
                                yolo_classid.at(j), scene->cameras[j].image_height);
                        }

                        // === ZARR BOUNDING BOX RENDERING === //
                        if (zarr_loaded) {
                            const int zarr_bbox_query_frame = current_frame_num;
                            // Check interpolation status for this frame
                            bool is_zarr_interpolated = zarr_loader.hasInterpolation() &&
                                                        zarr_loader.isFrameInterpolated(zarr_bbox_query_frame);
                            
                            // Get bounding boxes from the active dataset
                            std::vector<LoggedBoundingBox> loaded_zarr_boxes =
                                zarr_loader.getBoundingBoxesForFrame(zarr_bbox_query_frame);
                            std::vector<LoggedBoundingBox> zarr_boxes =
                                g_zarr_bbox_edit_state.resolveFrameBoxes(zarr_bbox_query_frame,
                                                                         loaded_zarr_boxes);
                            ZarrDetectionLoader::FrameDetections detection_details =
                                zarr_loader.getRawDetections(zarr_bbox_query_frame, false);
                            {
                                int valid_slots = 0;
                                for (int slot_idx = 0; slot_idx < scene->size_of_buffer; ++slot_idx) {
                                    const auto& slot = scene->cameras[j].display_buffer[slot_idx];
                                    if (!slot.available_to_write && slot.frame_number >= 0) {
                                        ++valid_slots;
                                    }
                                }
                                const int total_slots = static_cast<int>(scene->size_of_buffer);
                                const int empty_slots = std::max(0, total_slots - valid_slots);
                                frame_sync_valid_slots = valid_slots;
                                frame_sync_empty_slots = empty_slots;
                                int latest_decoded = -1;
                                const auto latest_it = latest_decoded_frame.find(win_name);
                                if (latest_it != latest_decoded_frame.end()) {
                                    latest_decoded = latest_it->second.load();
                                }
                                const int total_recording_frames =
                                    std::max(dc_context->total_num_frame,
                                             dc_context->estimated_num_frames);
                                int recording_remaining = -1;
                                if (total_recording_frames > 0) {
                                    if (latest_decoded < 0) {
                                        recording_remaining = total_recording_frames;
                                    } else {
                                        recording_remaining = std::max(
                                            0, total_recording_frames - (latest_decoded + 1));
                                    }
                                }
                                frame_sync_latest_decoded = latest_decoded;
                                frame_sync_recording_remaining = recording_remaining;
                                frame_sync_recording_total = total_recording_frames;

                                std::ostringstream sync_debug;
                                sync_debug << "cam=" << win_name
                                           << " mode=" << (ps.play_video ? "play" : "pause")
                                           << " slot=" << presented_slot
                                           << " displayed=" << presented_frame
                                           << " current=" << current_frame_num
                                           << " target=" << ps.to_display_frame_number
                                           << " slider=" << ps.slider_frame_number
                                           << " bbox_query=" << zarr_bbox_query_frame
                                           << " empty_remaining=" << empty_slots
                                           << " recording_remaining=" << recording_remaining
                                           << " latest_decoded="
                                           << latest_decoded;
                                frame_sync_debug_line = sync_debug.str();
                            }
                            const float image_width_px =
                                static_cast<float>(scene->cameras[j].image_width);
                            const float image_height_px =
                                static_cast<float>(scene->cameras[j].image_height);
                            const bool plot_hovered = ImPlot::IsPlotHovered();
                            const bool active_dataset_is_raw_detect =
                                zarr_loader.hasDetectionData() &&
                                (zarr_loader.getActiveDetectionDataset() ==
                                 ZarrDetectionLoader::DetectionDataset::RawDetect);
                            const bool dataset_allows_bbox_edit =
                                zarr_loader.hasDetectionData() && !active_dataset_is_raw_detect;
                            if (!dataset_allows_bbox_edit) {
                                g_zarr_bbox_edit_state.draw_mode = false;
                                g_zarr_bbox_edit_state.cancelDraw();
                                g_zarr_bbox_edit_state.clearSelection();
                            }
                            const bool can_modify_boxes =
                                dataset_allows_bbox_edit &&
                                g_zarr_bbox_edit_state.enabled &&
                                (g_zarr_bbox_edit_state.allow_edit_while_playing ||
                                 !ps.play_video);

                            auto deleteSelectedBoxOnCurrentFrame = [&]() -> bool {
                                if (!can_modify_boxes) {
                                    return false;
                                }
                                if (g_zarr_bbox_edit_state.selected_frame != current_frame_num ||
                                    g_zarr_bbox_edit_state.selected_box < 0) {
                                    return false;
                                }
                                auto& editable_boxes =
                                    g_zarr_bbox_edit_state.ensureFrameOverride(
                                        current_frame_num,
                                        loaded_zarr_boxes,
                                        &detection_details);
                                auto& added_flags =
                                    g_zarr_bbox_edit_state.ensureAddedFlags(
                                        current_frame_num,
                                        editable_boxes.size());
                                auto& manual_flags =
                                    g_zarr_bbox_edit_state.ensureManualFlags(
                                        current_frame_num,
                                        editable_boxes.size());
                                g_zarr_bbox_edit_state.ensureSourceMetadata(
                                    current_frame_num,
                                    editable_boxes.size());
                                auto& source_indices =
                                    g_zarr_bbox_edit_state
                                        .frame_source_indices[current_frame_num];
                                auto& source_detection_source =
                                    g_zarr_bbox_edit_state
                                        .frame_source_detection_source[current_frame_num];
                                auto& source_reason =
                                    g_zarr_bbox_edit_state
                                        .frame_source_reason[current_frame_num];
                                const int selected_idx = g_zarr_bbox_edit_state.selected_box;
                                if (selected_idx < 0 ||
                                    selected_idx >= static_cast<int>(editable_boxes.size())) {
                                    g_zarr_bbox_edit_state.clearSelection();
                                    return false;
                                }
                                editable_boxes.erase(
                                    editable_boxes.begin() + selected_idx);
                                if (selected_idx < static_cast<int>(added_flags.size())) {
                                    added_flags.erase(added_flags.begin() + selected_idx);
                                } else {
                                    added_flags.assign(editable_boxes.size(), 0);
                                }
                                if (selected_idx < static_cast<int>(manual_flags.size())) {
                                    manual_flags.erase(manual_flags.begin() + selected_idx);
                                } else {
                                    manual_flags.assign(editable_boxes.size(), 0);
                                }
                                if (selected_idx < static_cast<int>(source_indices.size())) {
                                    source_indices.erase(source_indices.begin() + selected_idx);
                                } else {
                                    source_indices.assign(editable_boxes.size(), -1);
                                }
                                if (selected_idx < static_cast<int>(source_detection_source.size())) {
                                    source_detection_source.erase(
                                        source_detection_source.begin() + selected_idx);
                                } else {
                                    source_detection_source.assign(
                                        editable_boxes.size(), 0);
                                }
                                if (selected_idx < static_cast<int>(source_reason.size())) {
                                    source_reason.erase(source_reason.begin() + selected_idx);
                                } else {
                                    source_reason.assign(editable_boxes.size(), std::string{});
                                }
                                g_zarr_bbox_edit_state.dirty_frames.insert(current_frame_num);
                                g_zarr_bbox_edit_state.drag_active = false;
                                g_zarr_bbox_edit_state.drag_mouse_button = -1;
                                if (editable_boxes.empty()) {
                                    g_zarr_bbox_edit_state.clearSelection();
                                } else {
                                    g_zarr_bbox_edit_state.selected_frame = current_frame_num;
                                    g_zarr_bbox_edit_state.selected_box =
                                        std::min(selected_idx,
                                                 static_cast<int>(editable_boxes.size() - 1));
                                }
                                zarr_boxes = editable_boxes;
                                return true;
                            };

                            std::vector<FullFrameRect> editable_rects;
                            editable_rects.reserve(zarr_boxes.size());
                            for (const auto& box : zarr_boxes) {
                                editable_rects.push_back(
                                    {box.x_min, box.y_min, box.width, box.height});
                            }

                            FullFrameRectEditStateView full_frame_edit_state;
                            full_frame_edit_state.selected_frame =
                                g_zarr_bbox_edit_state.selected_frame;
                            full_frame_edit_state.selected_box =
                                g_zarr_bbox_edit_state.selected_box;
                            full_frame_edit_state.drag_active =
                                g_zarr_bbox_edit_state.drag_active;
                            full_frame_edit_state.drag_mouse_button =
                                g_zarr_bbox_edit_state.drag_mouse_button;
                            full_frame_edit_state.drag_offset_x =
                                g_zarr_bbox_edit_state.drag_offset_x;
                            full_frame_edit_state.drag_offset_y =
                                g_zarr_bbox_edit_state.drag_offset_y;
                            full_frame_edit_state.draw_mode =
                                g_zarr_bbox_edit_state.draw_mode;
                            full_frame_edit_state.draw_active =
                                g_zarr_bbox_edit_state.draw_active;
                            full_frame_edit_state.draw_frame =
                                g_zarr_bbox_edit_state.draw_frame;
                            full_frame_edit_state.draw_anchor_x =
                                g_zarr_bbox_edit_state.draw_anchor_x;
                            full_frame_edit_state.draw_anchor_y =
                                g_zarr_bbox_edit_state.draw_anchor_y;
                            full_frame_edit_state.draw_current_x =
                                g_zarr_bbox_edit_state.draw_current_x;
                            full_frame_edit_state.draw_current_y =
                                g_zarr_bbox_edit_state.draw_current_y;

                            const FullFrameRectEditContext full_frame_edit_context{
                                current_frame_num,
                                image_width_px,
                                image_height_px,
                                plot_hovered,
                                dataset_allows_bbox_edit,
                                can_modify_boxes,
                                &editable_rects,
                                6.0f,
                            };
                            const auto full_frame_edit_result =
                                processFullFrameRectEditInput(
                                    full_frame_edit_context, full_frame_edit_state);

                            g_zarr_bbox_edit_state.selected_frame =
                                full_frame_edit_result.state.selected_frame;
                            g_zarr_bbox_edit_state.selected_box =
                                full_frame_edit_result.state.selected_box;
                            g_zarr_bbox_edit_state.drag_active =
                                full_frame_edit_result.state.drag_active;
                            g_zarr_bbox_edit_state.drag_mouse_button =
                                full_frame_edit_result.state.drag_mouse_button;
                            g_zarr_bbox_edit_state.drag_offset_x =
                                full_frame_edit_result.state.drag_offset_x;
                            g_zarr_bbox_edit_state.drag_offset_y =
                                full_frame_edit_result.state.drag_offset_y;
                            g_zarr_bbox_edit_state.draw_mode =
                                full_frame_edit_result.state.draw_mode;
                            g_zarr_bbox_edit_state.draw_active =
                                full_frame_edit_result.state.draw_active;
                            g_zarr_bbox_edit_state.draw_frame =
                                full_frame_edit_result.state.draw_frame;
                            g_zarr_bbox_edit_state.draw_anchor_x =
                                full_frame_edit_result.state.draw_anchor_x;
                            g_zarr_bbox_edit_state.draw_anchor_y =
                                full_frame_edit_result.state.draw_anchor_y;
                            g_zarr_bbox_edit_state.draw_current_x =
                                full_frame_edit_result.state.draw_current_x;
                            g_zarr_bbox_edit_state.draw_current_y =
                                full_frame_edit_result.state.draw_current_y;

                            if (full_frame_edit_result.request_reset_frame) {
                                g_zarr_bbox_edit_state.clearFrameEdits(current_frame_num);
                                zarr_boxes = loaded_zarr_boxes;
                            }
                            if (full_frame_edit_result.request_delete_selected) {
                                deleteSelectedBoxOnCurrentFrame();
                            }
                            if (full_frame_edit_result.request_add_rect) {
                                auto& editable_boxes =
                                    g_zarr_bbox_edit_state.ensureFrameOverride(
                                        current_frame_num,
                                        loaded_zarr_boxes,
                                        &detection_details);
                                auto& added_flags =
                                    g_zarr_bbox_edit_state.ensureAddedFlags(
                                        current_frame_num,
                                        editable_boxes.size());
                                auto& manual_flags =
                                    g_zarr_bbox_edit_state.ensureManualFlags(
                                        current_frame_num,
                                        editable_boxes.size());
                                g_zarr_bbox_edit_state.ensureSourceMetadata(
                                    current_frame_num,
                                    editable_boxes.size());
                                auto& source_indices =
                                    g_zarr_bbox_edit_state
                                        .frame_source_indices[current_frame_num];
                                auto& source_detection_source =
                                    g_zarr_bbox_edit_state
                                        .frame_source_detection_source[current_frame_num];
                                auto& source_reason =
                                    g_zarr_bbox_edit_state
                                        .frame_source_reason[current_frame_num];

                                uint16_t new_class_id = 0;
                                float new_confidence = 1.0f;
                                if (g_zarr_bbox_edit_state.selected_frame ==
                                        current_frame_num &&
                                    g_zarr_bbox_edit_state.selected_box >= 0 &&
                                    g_zarr_bbox_edit_state.selected_box <
                                        static_cast<int>(zarr_boxes.size())) {
                                    const auto& selected_box =
                                        zarr_boxes[g_zarr_bbox_edit_state.selected_box];
                                    new_class_id = selected_box.class_id;
                                    new_confidence = selected_box.confidence;
                                } else if (!zarr_boxes.empty()) {
                                    new_class_id = zarr_boxes.front().class_id;
                                    new_confidence = zarr_boxes.front().confidence;
                                }

                                LoggedBoundingBox new_box{};
                                new_box.payload_timestamp_ns_epoch = 0;
                                new_box.received_timestamp_ns_epoch = 0;
                                new_box.payload_frame_id = static_cast<uint64_t>(
                                    std::max(0, current_frame_num));
                                new_box.payload_camera_id = 0;
                                new_box.box_index_in_payload =
                                    static_cast<uint8_t>(editable_boxes.size());
                                new_box.x_min =
                                    full_frame_edit_result.new_rect.x_min;
                                new_box.y_min =
                                    full_frame_edit_result.new_rect.y_min;
                                new_box.width =
                                    full_frame_edit_result.new_rect.width;
                                new_box.height =
                                    full_frame_edit_result.new_rect.height;
                                new_box.class_id = new_class_id;
                                new_box.confidence =
                                    std::isfinite(new_confidence)
                                        ? new_confidence
                                        : 1.0f;

                                editable_boxes.push_back(new_box);
                                added_flags.push_back(1);
                                manual_flags.push_back(1);
                                source_indices.push_back(-1);
                                source_detection_source.push_back(0);
                                source_reason.emplace_back("manual");
                                g_zarr_bbox_edit_state.dirty_frames.insert(
                                    current_frame_num);
                                g_zarr_bbox_edit_state.selected_frame =
                                    current_frame_num;
                                g_zarr_bbox_edit_state.selected_box =
                                    static_cast<int>(editable_boxes.size() - 1);
                                zarr_boxes = editable_boxes;
                            }
                            if (full_frame_edit_result.request_move_selected) {
                                auto& editable_boxes =
                                    g_zarr_bbox_edit_state.ensureFrameOverride(
                                        current_frame_num,
                                        loaded_zarr_boxes,
                                        &detection_details);
                                auto& manual_flags =
                                    g_zarr_bbox_edit_state.ensureManualFlags(
                                        current_frame_num,
                                        editable_boxes.size());
                                const int selected_idx =
                                    full_frame_edit_result.move_box_index;
                                if (selected_idx >= 0 &&
                                    selected_idx <
                                        static_cast<int>(editable_boxes.size())) {
                                    LoggedBoundingBox& moving_box =
                                        editable_boxes[selected_idx];
                                    const float max_x = std::max(
                                        0.0f, image_width_px - moving_box.width);
                                    const float max_y = std::max(
                                        0.0f, image_height_px - moving_box.height);
                                    moving_box.x_min = std::clamp(
                                        full_frame_edit_result.move_target_x,
                                        0.0f,
                                        max_x);
                                    moving_box.y_min = std::clamp(
                                        full_frame_edit_result.move_target_y,
                                        0.0f,
                                        max_y);
                                    if (selected_idx >= 0 &&
                                        selected_idx <
                                            static_cast<int>(manual_flags.size())) {
                                        manual_flags[selected_idx] = 1;
                                    }
                                    g_zarr_bbox_edit_state.dirty_frames.insert(
                                        current_frame_num);
                                    zarr_boxes = editable_boxes;
                                } else {
                                    g_zarr_bbox_edit_state.clearSelection();
                                }
                            }

                            const bool frame_has_bbox_edits =
                                g_zarr_bbox_edit_state.isFrameDirty(current_frame_num);

                            if (!zarr_boxes.empty()) {
                                std::vector<FullFrameRectOverlayItem> overlay_items =
                                    buildCameraViewBoundingBoxOverlayItems(
                                        zarr_boxes,
                                        detection_details,
                                        g_zarr_bbox_edit_state,
                                        current_frame_num,
                                        frame_has_bbox_edits,
                                        zarr_loader.activeDatasetHasSyntheticDetections(),
                                        is_zarr_interpolated);
                                drawFullFrameRectOverlays(overlay_items, image_height_px);
                            }

                            const std::string draft_label_suffix =
                                std::to_string(j);
                            drawFullFrameRectDraftOverlay(
                                full_frame_edit_result.state,
                                current_frame_num,
                                image_height_px,
                                draft_label_suffix.c_str());

                            // Draw chaser bounding boxes and target positions
                            if (zarr_loaded) {
                                auto chaser_bboxes = zarr_loader.getChaserBoundingBoxesForFrame(current_frame_num);
                                auto chaser_states =
                                    zarr_loader.getChaserInterpolatedStatesForCameraFrame(current_frame_num);
                                if (chaser_states.empty()) {
                                    if (ps.current_stimulus_frame >= 0 &&
                                        zarr_loader.hasStimulusFrameMapping()) {
                                        chaser_states =
                                            zarr_loader.getChaserStatesForStimulusFrame(ps.current_stimulus_frame);
                                    }
                                }
                                if (chaser_states.empty()) {
                                    chaser_states = zarr_loader.getChaserStatesForFrame(current_frame_num);
                                }

                                #if defined(CRIMSON_CHASER_DEBUG_LOGS)
                                // Debug: Print what we found
                                static bool debug_printed = false;
                                static int frames_with_data = 0;
                                if (chaser_bboxes.size() > 0 || chaser_states.size() > 0) {
                                    frames_with_data++;
                                    if (!debug_printed) {
                                        std::cout << "\n=== CHASER DATA DEBUG ===" << std::endl;
                                        std::cout << "Camera frame " << current_frame_num << ": Found " << chaser_bboxes.size()
                                                  << " chaser bboxes, " << chaser_states.size() << " chaser states" << std::endl;

                                        if (chaser_bboxes.size() > 0) {
                                            std::cout << "  First bbox: fish_id=" << chaser_bboxes[0].fish_id
                                                      << ", x=" << chaser_bboxes[0].x_px << ", y=" << chaser_bboxes[0].y_px
                                                      << ", w=" << chaser_bboxes[0].width_px << ", h=" << chaser_bboxes[0].height_px << std::endl;
                                        }

                                        if (chaser_states.size() > 0) {
                                            std::cout << "  First state: stimulus_frame=" << chaser_states[0].stimulus_frame_num
                                                      << ", camera_frame=" << chaser_states[0].camera_frame_id << std::endl;
                                            std::cout << "    chaser=(" << chaser_states[0].chaser_pos_x << "," << chaser_states[0].chaser_pos_y << ")"
                                                      << " target=(" << chaser_states[0].target_pos_x << "," << chaser_states[0].target_pos_y << ")" << std::endl;
                                            std::cout << "  Camera params: has_homography=" << camera_params[j].has_valid_homography
                                                      << ", offsetX=" << camera_params[j].stimulus_offset_x
                                                      << ", offsetY=" << camera_params[j].stimulus_offset_y << std::endl;
                                        }
                                        debug_printed = true;
                                    }
                                }

                                // Print summary after a while
                                static int last_frame_checked = -1;
                                if (current_frame_num > last_frame_checked + 1000) {
                                    std::cout << "Frames " << (last_frame_checked + 1) << "-" << current_frame_num
                                              << ": " << frames_with_data << " frames had chaser data" << std::endl;
                                    frames_with_data = 0;
                                    last_frame_checked = current_frame_num;
                                }
                                #endif
                                drawCameraViewChaserOverlay(
                                    chaser_bboxes,
                                    chaser_states,
                                    camera_params[j],
                                    static_cast<int>(scene->cameras[j].image_width),
                                    static_cast<int>(scene->cameras[j].image_height));
                            }

                            const bool heading_overlay_enabled = show_heading_arrows;
                            const bool heading_data_available = zarr_loader.hasHeadingData();
                            const bool eye_mask_overlay_enabled = show_eye_masks;
                            const bool eye_mask_data_available = zarr_loader.hasEyeMasks();
                            const bool can_draw_headings =
                                heading_overlay_enabled && heading_data_available;
                            const bool can_draw_eye_masks =
                                eye_mask_overlay_enabled && eye_mask_data_available;
                            const float scene_height_f =
                                static_cast<float>(scene->cameras[j].image_height);

                            if (kHeadingDebugLoggingEnabled) {
                                if (!heading_overlay_enabled) {
                                    if (!heading_debug_logged_toggle_disabled) {
                                        headingDebugLog("Heading overlay disabled via UI toggle; skipping arrow drawing.");
                                        heading_debug_logged_toggle_disabled = true;
                                    }
                                } else {
                                    if (heading_debug_logged_toggle_disabled) {
                                        headingDebugLog("Heading overlay toggle enabled; attempting to draw arrows.");
                                        heading_debug_logged_toggle_disabled = false;
                                    }

                                    if (!heading_data_available) {
                                        if (!heading_debug_logged_no_data) {
                                            headingDebugLog("Zarr loader reports no heading data; arrows will not be drawn.");
                                            heading_debug_logged_no_data = true;
                                        }
                                    } else if (heading_debug_logged_no_data) {
                                        headingDebugLog("Heading data detected; resuming arrow attempts.");
                                        heading_debug_logged_no_data = false;
                                    }

                                    if (zarr_loader.activeDatasetHasSyntheticDetections()) {
                                        if (!heading_debug_logged_interpolated) {
                                            headingDebugLog("Dataset contains synthetic detections; headings render only for real boxes.");
                                            heading_debug_logged_interpolated = true;
                                        }
                                    } else if (heading_debug_logged_interpolated) {
                                        headingDebugLog("Dataset now fully real; headings may render for all boxes.");
                                        heading_debug_logged_interpolated = false;
                                    }
                                }
                            }

                            if (kEyeMaskDebugLoggingEnabled) {
                                if (!show_eye_masks) {
                                    if (!eye_mask_debug_logged_toggle_disabled) {
                                        eyeMaskDebugLog("Eye mask overlay disabled via UI toggle; skipping mask drawing.");
                                        eye_mask_debug_logged_toggle_disabled = true;
                                    }
                                } else if (eye_mask_debug_logged_toggle_disabled) {
                                    eyeMaskDebugLog("Eye mask overlay toggle enabled; attempting to draw masks.");
                                    eye_mask_debug_logged_toggle_disabled = false;
                                }

                                if (!zarr_loader.hasEyeMasks()) {
                                    if (!eye_mask_debug_logged_no_data) {
                                        eyeMaskDebugLog("Zarr loader reports no eye mask data.");
                                        eye_mask_debug_logged_no_data = true;
                                    }
                                } else if (eye_mask_debug_logged_no_data) {
                                    eyeMaskDebugLog("Eye mask data detected; masks may render.");
                                    eye_mask_debug_logged_no_data = false;
                                }
                            }

                            if (can_draw_headings) {
                                ZarrDetectionLoader::FrameDetections heading_details =
                                    zarr_loader.getRawDetections(
                                        current_frame_num,
                                        /*use_interpolated=*/false,
                                        /*include_eye_masks=*/false);
                                drawCameraViewHeadingOverlay(
                                    heading_details,
                                    scene_height_f);
                            }

                            if (can_draw_eye_masks) {
                                ZarrDetectionLoader::FrameDetections mask_details =
                                    zarr_loader.getRawDetections(
                                        current_frame_num,
                                        /*use_interpolated=*/false,
                                        /*include_eye_masks=*/true);
                                drawCameraViewEyeMaskOverlay(
                                    mask_details,
                                    scene_height_f,
                                    zarr_loader.getEyeMaskRunName() + "|" +
                                        zarr_loader.getEyeAngleRunName());
                            }
                            drawCameraViewDetectionKeypointMarkers(
                                detection_details,
                                show_keypoint_markers,
                                image_height_px);
                        }

                        if (zarr_loaded && zarr_loader.hasStimulusEvents()) {
                            auto frame_events = zarr_loader.getStimulusEventsForFrame(current_frame_num);
                            drawCameraViewStimulusEventOverlay(
                                j, current_frame_num, frame_events);
                        }

                        if (plot_keypoints_flag) {
                            const CameraViewManualKeypointInputContext
                                keypoint_input_context{
                                    scene,
                                    skeleton.get(),
                                    &keypoints_map,
                                    current_frame_num,
                                    j,
                                    keypoints_find,
                                    ImPlot::IsPlotHovered(),
                                };
                            const CameraViewManualKeypointInputResult
                                keypoint_input_result =
                                    processCameraViewManualKeypointInput(
                                        keypoint_input_context);
                            keypoints_find = keypoint_input_result.keypoints_find;
                            is_view_focused[j] =
                                keypoint_input_result.view_focused;
                        }
                        ImPlot::EndPlot();
                        if (swap_playback_surface_after_draw) {
                            auto &camera = scene->cameras[j];
                            const auto swap_start =
                                std::chrono::steady_clock::now();
                            std::swap(camera.image_texture,
                                      camera.playback_staging_texture);
                            std::swap(camera.pbo_cuda,
                                      camera.playback_staging_pbo);
                            std::swap(camera.applied_preview_sampling_mode,
                                      camera.playback_staging_preview_sampling_mode);
                            const int previous_front_frame =
                                camera.last_uploaded_frame;
                            const bool previous_front_valid =
                                camera.texture_has_valid_frame;
                            camera.last_uploaded_frame =
                                camera.playback_staging_frame;
                            camera.texture_has_valid_frame =
                                camera.playback_staging_valid;
                            camera.playback_staging_frame =
                                previous_front_frame;
                            camera.playback_staging_valid =
                                previous_front_valid;
                            swap_playback_surface_after_draw = false;
                            frame_camera_playback_swap_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                swap_start);
                        }
                        frame_camera_overlay_ui_ms += durationMs(
                            std::chrono::steady_clock::now() -
                            camera_overlay_ui_start);
                    }
                    if (restore_plot_pan_mod) {
                        plot_input_map.PanMod = previous_plot_pan_mod;
                    }
                    if (scene_plot_style_color_count > 0) {
                        ImPlot::PopStyleColor(scene_plot_style_color_count);
                    }
                    if (scene_plot_style_var_count > 0) {
                        ImPlot::PopStyleVar(scene_plot_style_var_count);
                    }

                    ImGui::EndChild();

                    const CameraViewTransportControlsContext
                        camera_transport_context{
                            ps.to_display_frame_number,
                            dc_context->total_num_frame,
                            dc_context->estimated_num_frames,
                            video_fps,
                            ps.play_video,
                            ps.slider_frame_number,
                        };
                    const CameraViewTransportControlsResult
                        camera_transport_result =
                            drawCameraViewTransportControls(
                                camera_transport_context);
                    ps.slider_frame_number =
                        camera_transport_result.slider_frame_number;
                    ps.slider_just_changed =
                        camera_transport_result.slider_just_changed;
                    if (camera_transport_result.toggle_playback) {
                        applyPlaybackToggle();
                    }
                    if (camera_transport_result.step_delta != 0) {
                        stepFrames(camera_transport_result.step_delta);
                    }
                    if (camera_transport_result.seek_target_frame.has_value()) {
                        seekToFrame(
                            *camera_transport_result.seek_target_frame, true,
                            camera_transport_result.force_inaccurate_seek);
                    }

                    ImGui::EndGroup();
                    frame_camera_scene_ui_ms += durationMs(
                        std::chrono::steady_clock::now() -
                        scene_ui_build_start);
                }
                ImGui::End();
            }

            const CameraViewPlaybackShortcutsResult playback_shortcuts =
                handleCameraViewPlaybackShortcuts();
            if (playback_shortcuts.toggle_playback) {
                applyPlaybackToggle();
            }
            if (playback_shortcuts.step_delta != 0) {
                stepFrames(playback_shortcuts.step_delta);
            }

            for (const auto &[name, flag] : window_need_decoding) {
                window_was_decoding[name] = flag.load();
            }
        }

        if (zarr_loaded &&
            (zarr_loader.hasCropImages() || zarr_loader.hasKeypointData() ||
             zarr_loader.hasEyeMasks())) {
            const auto crop_preview_ui_start = std::chrono::steady_clock::now();
            RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
            CropFrameSource live_crop_frame_source;
            int crop_preview_frame_num = current_frame_num;
            if (video_loaded) {
                const int visible_idx = getVisibleCameraIndex();
                if (visible_idx >= 0 && scene->size_of_buffer > 0) {
                    const auto& camera = scene->cameras[visible_idx];
                    if (ps.play_video && camera.texture_has_valid_frame &&
                        camera.last_uploaded_frame >= 0) {
                        crop_preview_frame_num = camera.last_uploaded_frame;
                    }
                    const int preferred_slot = ps.read_head % scene->size_of_buffer;
                    const int slot_index = findDisplaySlotForFrame(
                        visible_idx, crop_preview_frame_num, preferred_slot);
                    if (camera.texture_has_valid_frame &&
                        camera.last_uploaded_frame == crop_preview_frame_num &&
                        camera.image_texture != 0) {
                        live_crop_frame_source.frame_number = crop_preview_frame_num;
                        live_crop_frame_source.width =
                            static_cast<int>(camera.image_width);
                        live_crop_frame_source.height =
                            static_cast<int>(camera.image_height);
                        live_crop_frame_source.texture_id = camera.image_texture;
                        live_crop_frame_source.texture_width =
                            camera.display_texture_width > 0
                                ? camera.display_texture_width
                                : static_cast<int>(camera.image_width);
                        live_crop_frame_source.texture_height =
                            camera.display_texture_height > 0
                                ? camera.display_texture_height
                                : static_cast<int>(camera.image_height);
                        live_crop_frame_source.texture_frame_number =
                            camera.last_uploaded_frame;
                    }
                    if (slot_index >= 0) {
                        const auto& slot = camera.display_buffer[slot_index];
                        if (!slot.available_to_write &&
                            slot.frame_number == crop_preview_frame_num &&
                            slot.frame != nullptr) {
                            live_crop_frame_source.frame = slot.frame;
                            live_crop_frame_source.frame_number =
                                slot.frame_number;
                            live_crop_frame_source.width =
                                static_cast<int>(camera.image_width);
                            live_crop_frame_source.height =
                                static_cast<int>(camera.image_height);
                            live_crop_frame_source.pitch_bytes =
                                slot.pitch_bytes;
                            live_crop_frame_source.color_matrix =
                                slot.color_matrix;
                            if (scene->use_cpu_buffer &&
                                slot.format == PictureBufferFormat::RGBA32) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::HostRGBA32;
                            } else if (slot.format ==
                                       PictureBufferFormat::RGBA32) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::DeviceRGBA32;
                            } else if (slot.format ==
                                       PictureBufferFormat::NV12) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::DeviceNV12;
                            }
                        }
                    }
                }
            }
            ZarrPersistedCropProvider persisted_crop_image_provider(zarr_loader);
            LiveCropImageProvider live_crop_image_provider(
                zarr_loader, live_crop_frame_source);
            ChainedCropImageProvider crop_image_provider(
                live_crop_image_provider, persisted_crop_image_provider);
            std::optional<CropSpec> selected_crop_spec;
            int selected_detection_index = -1;
            if (g_zarr_bbox_edit_state.selected_frame == crop_preview_frame_num &&
                g_zarr_bbox_edit_state.selected_box >= 0 &&
                zarr_loader.hasDetectionData()) {
                const auto loaded_crop_boxes =
                    zarr_loader.getBoundingBoxesForFrame(crop_preview_frame_num);
                const auto resolved_crop_boxes =
                    g_zarr_bbox_edit_state.resolveFrameBoxes(crop_preview_frame_num,
                                                             loaded_crop_boxes);
                if (g_zarr_bbox_edit_state.selected_box <
                    static_cast<int>(resolved_crop_boxes.size())) {
                    const auto& selected_box =
                        resolved_crop_boxes[static_cast<size_t>(
                            g_zarr_bbox_edit_state.selected_box)];
                    CropSpec crop_spec;
                    crop_spec.offset_x = selected_box.x_min;
                    crop_spec.offset_y = selected_box.y_min;
                    crop_spec.width_px = selected_box.width;
                    crop_spec.height_px = selected_box.height;
                    crop_spec.valid = selected_box.width > 0.0f &&
                                      selected_box.height > 0.0f;
                    if (crop_spec.valid) {
                        selected_crop_spec = crop_spec;
                    }

                    auto source_it =
                        g_zarr_bbox_edit_state.frame_source_indices.find(
                            crop_preview_frame_num);
                    if (source_it !=
                            g_zarr_bbox_edit_state.frame_source_indices.end() &&
                        g_zarr_bbox_edit_state.selected_box <
                            static_cast<int>(source_it->second.size())) {
                        selected_detection_index =
                            source_it->second[static_cast<size_t>(
                                g_zarr_bbox_edit_state.selected_box)];
                    } else if (g_zarr_bbox_edit_state.selected_box <
                               static_cast<int>(loaded_crop_boxes.size())) {
                        selected_detection_index =
                            g_zarr_bbox_edit_state.selected_box;
                    }
                }
            }
            const CropPreviewWindowContext crop_preview_context{
                crop_image_provider,
                zarr_loader,
                refined_keypoint_repo,
                crop_preview_frame_num,
                g_zarr_bbox_edit_state.selected_frame,
                g_zarr_bbox_edit_state.selected_box,
                selected_detection_index,
                selected_crop_spec,
                ps.play_video,
                &refined_keypoint_review_window_state.panel_state
                     .manual_write_status,
            };
            const auto crop_preview_result = drawCropPreviewWindow(
                crop_preview_context, crop_preview_window_state);

            applyCropPreviewKeypointWriteAction(
                refined_keypoint_repo,
                crop_preview_result.editor_action,
                crop_preview_result.selected_keypoint_selection,
                crop_preview_window_state.editor_state,
                refined_keypoint_review_window_state.panel_state
                    .manual_write_status,
                reloadActiveZarrPreserveDataset);
            frame_crop_preview_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - crop_preview_ui_start);
        }

        if (zarr_loaded && zarr_loader.hasKeypointData()) {
            const RefinedKeypointReviewWindowContext keypoint_review_window_context{
                zarr_loader,
                current_frame_num,
                g_zarr_bbox_edit_state.selected_frame,
                g_zarr_bbox_edit_state.selected_box,
            };
            const auto keypoint_review_window_result =
                drawRefinedKeypointReviewWindow(
                    keypoint_review_window_context,
                    refined_keypoint_review_window_state);
            if (keypoint_review_window_result.request_review_write) {
                RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
                const RefinedKeypointReviewWriteWorkflowResult
                    review_write_result = applyRefinedKeypointReviewWrite(
                        refined_keypoint_repo,
                        keypoint_review_window_result,
                        refined_keypoint_review_window_state.panel_state
                            .review_write_status,
                        reloadActiveZarrPreserveDataset);
                if (review_write_result.should_clear_zarr_loaded) {
                    zarr_loaded = false;
                }
            }
        }

        static StimulusPlaybackWindowsState stimulus_playback_windows_state;
        if (stimulus_player.loaded) {
            const auto stimulus_playback_windows_result =
                drawStimulusPlaybackWindows(
                    StimulusPlaybackWindowsContext{
                        stimulus_player,
                        zarr_loaded ? &zarr_loader : nullptr,
                        ps,
                        seek_progress,
                        current_frame_num,
                        video_fps,
                        latest_decoded_frame[stimulus_player.window_name].load(),
                        window_need_decoding[stimulus_player.window_name].load(),
                        &stimulus_catchup_seek_generation,
                    },
                    stimulus_playback_windows_state);
            window_need_decoding[stimulus_player.window_name].store(
                stimulus_playback_windows_result.decoder_requested);
            frame_stimulus_window_ui_ms +=
                stimulus_playback_windows_result.stimulus_window_ui_ms;
            frame_stimulus_buffer_window_ui_ms +=
                stimulus_playback_windows_result.stimulus_buffer_window_ui_ms;
        }

        if (plot_keypoints_flag) {
            const auto keypoints_window_ui_start =
                std::chrono::steady_clock::now();
            const KeypointsWindowContext keypoints_window_context{
                static_cast<int>(scene->num_cams),
                skeleton.get(),
                keypoints_map,
                current_frame_num,
                camera_names,
                is_view_focused,
                keypoints_find,
            };
            drawKeypointsWindow(keypoints_window_context);
            frame_keypoints_window_ui_ms += durationMs(
                std::chrono::steady_clock::now() - keypoints_window_ui_start);
        }

        if (plot_keypoints_flag) {
            const auto labeling_tool_ui_start =
                std::chrono::steady_clock::now();
            bool has_labeled_frames = !keypoints_map.empty();
            int next_labeled_frame = -1;
            if (has_labeled_frames) {
                auto upper_it = keypoints_map.upper_bound(current_frame_num);
                if (upper_it == keypoints_map.end()) {
                    upper_it = keypoints_map.begin();
                }
                next_labeled_frame = upper_it->first;
            }
#if CRIMSON_ENABLE_SFM
            constexpr bool triangulation_supported = true;
#else
            constexpr bool triangulation_supported = false;
#endif
            const LabelingToolWindowContext labeling_tool_context{
                root_dir,
                keypoints_root_folder,
                static_cast<int>(scene->num_cams),
                skeleton.get(),
                keypoints_map,
                current_frame_num,
                keypoints_find,
                triangulation_supported,
                last_saved,
                has_labeled_frames,
                next_labeled_frame,
                keypoints_map.size(),
            };
            const LabelingToolWindowResult labeling_tool_result =
                drawLabelingToolWindow(labeling_tool_context,
                                       labeling_tool_window_state);

            if (labeling_tool_result.request_triangulate) {
                auto keypoint_it = keypoints_map.find(current_frame_num);
                if (keypoint_it != keypoints_map.end()) {
                    reprojection(keypoint_it->second, skeleton.get(),
                                 camera_params, scene);
                }
            }

            if (labeling_tool_result.request_save) {
                save_keypoints(keypoints_map, skeleton.get(),
                               keypoints_root_folder, scene->num_cams,
                               camera_names, &input_is_imgs, imgs_names);
                last_saved = time(NULL);
            }

            if (labeling_tool_result.request_load_most_recent) {
                free_all_keypoints(keypoints_map, scene);
                if (labeling_tool_result.load_old_format) {
                    if (load_keypoints_depreciated(keypoints_map, skeleton.get(),
                                                   keypoints_root_folder, scene,
                                                   camera_names,
                                                   error_message)) {
                        free_all_keypoints(keypoints_map, scene);
                        show_error = true;
                    }
                } else {
                    std::string most_recent_folder;
                    if (find_most_recent_labels(keypoints_root_folder,
                                                most_recent_folder,
                                                error_message)) {
                        show_error = true;
                    } else if (load_keypoints(most_recent_folder, keypoints_map,
                                              skeleton.get(), scene,
                                              camera_names, error_message)) {
                        free_all_keypoints(keypoints_map, scene);
                        show_error = true;
                    }
                }
            }

            if (labeling_tool_result.selected_load_folder.has_value()) {
                free_all_keypoints(keypoints_map, scene);
                if (load_keypoints(*labeling_tool_result.selected_load_folder,
                                   keypoints_map, skeleton.get(), scene,
                                   camera_names, error_message)) {
                    free_all_keypoints(keypoints_map, scene);
                    show_error = true;
                }
            }

            if (labeling_tool_result.jump_target_frame.has_value()) {
                seekToFrame(*labeling_tool_result.jump_target_frame, true);
            }

            frame_labeling_tool_ui_ms += durationMs(
                std::chrono::steady_clock::now() - labeling_tool_ui_start);
        }

        static TimelineScrollState shared_timeline_scroll_state;
        static StimulusEventTimelineWindowState stimulus_timeline_window_state;
        static MovementTimelineWindowState movement_timeline_window_state;

        // Stimulus Event Timeline Window
        if (zarr_loaded) {
            const auto stimulus_timeline_ui_start =
                std::chrono::steady_clock::now();
            StimulusEventTimelineWindowContext stimulus_timeline_context{
                zarr_loader,
                shared_timeline_scroll_state,
                current_frame_num,
                video_fps,
            };
            StimulusEventTimelineWindowResult stimulus_timeline_result =
                drawStimulusEventTimelineWindow(stimulus_timeline_context,
                                               stimulus_timeline_window_state);
            if (stimulus_timeline_result.seek_target_frame.has_value()) {
                seekToFrame(*stimulus_timeline_result.seek_target_frame, true);
            }
            frame_stimulus_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - stimulus_timeline_ui_start);
        }

        // Movement timeline windows
        if (zarr_loaded && zarr_loader.hasMovementData()) {
            const auto movement_timeline_ui_start =
                std::chrono::steady_clock::now();
            MovementTimelineWindowContext movement_timeline_context{
                zarr_loader,
                shared_timeline_scroll_state,
                current_frame_num,
                video_fps,
            };
            drawMovementTimelineWindow(movement_timeline_context,
                                       movement_timeline_window_state);
            frame_movement_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - movement_timeline_ui_start);
        }

        shared_timeline_scroll_state.prev_enabled =
            shared_timeline_scroll_state.enabled;

        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
            show_help_window = !show_help_window;
        }

        if (show_help_window) {
            const auto help_menu_ui_start = std::chrono::steady_clock::now();
            if (ImGui::Begin("Help Menu")) {
                ImGui::Text("<Space>: toggle play and pause");
                ImGui::Text("<Left Arrow>    : Seek backward");
                ImGui::Text("<Shift+Left>    : Seek backward (×10)");
                ImGui::Text("<Right Arrow>   : Seek forward");
                ImGui::Text("<Shift+Right>   : Seek forward (×10)");

                ImGui::SeparatorText("When paused");
                ImGui::Text("<,>: previous image in buffer");
                ImGui::Text("<.>: next image in buffer");

                ImGui::SeparatorText("While hovering image");
                ImGui::Text("<c>: create keypoints on frame");
                ImGui::Text("<w>: drop active keypoint");
                ImGui::Text("<a>: active keypoint++ ");
                ImGui::Text("<d>: active keypoint--");
                ImGui::Text("<q>: active keypoint set to first node");
                ImGui::Text("<e>: active keypoint set to last node");
                ImGui::Text("<t> -> triangulate");
                ImGui::Text("<Backspace>: delete all keypoints");
                ImGui::Text("<Ctrl+s>         : Save labels");
                ImGui::Text("<Click+Drag>: move selected Zarr bbox (paused)");
                ImGui::Text("<N>: toggle draw-new-bbox mode");
                ImGui::Text("<Del>: delete selected Zarr bbox");
                ImGui::Text("<Esc>: cancel draw mode or clear selected Zarr bbox");
                ImGui::Text("<Shift+R>: reset current-frame Zarr bbox edits");
                ImGui::Text("Zarr Raw Detect is read-only");

                ImGui::SeparatorText("While hovering keypoints");
                ImGui::Text("<r>: delete active keypoint");
                ImGui::Text("<f>: delete active keypoint on all cameras");
                ImGui::Text("Click keypoint to active it");
            }
            ImGui::End();
            frame_help_menu_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - help_menu_ui_start);
        }

        if (show_error) {
            ImGui::OpenPopup("Error");
            show_error = false; // Reset the flag so it only opens once
        }

        if (ImGui::BeginPopupModal("Error", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", error_message.c_str());
            ImGui::Separator();

            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
                show_error = false;
            }

            ImGui::EndPopup();
        }

        // Rendering
        frame_ui_build_ms =
            durationMs(std::chrono::steady_clock::now() - ui_build_start);
        const auto imgui_render_start = std::chrono::steady_clock::now();
        ImGui::Render();
        frame_imgui_render_ms = durationMs(
            std::chrono::steady_clock::now() - imgui_render_start);
        ImDrawData* imgui_draw_data = ImGui::GetDrawData();
        if (imgui_draw_data != nullptr) {
            frame_imgui_draw_list_count = imgui_draw_data->CmdListsCount;
            frame_imgui_total_vtx_count = imgui_draw_data->TotalVtxCount;
            frame_imgui_total_idx_count = imgui_draw_data->TotalIdxCount;
            int draw_cmd_count = 0;
            for (int list_idx = 0; list_idx < imgui_draw_data->CmdListsCount;
                 ++list_idx) {
                const ImDrawList* draw_list = imgui_draw_data->CmdLists[list_idx];
                if (draw_list != nullptr) {
                    draw_cmd_count += draw_list->CmdBuffer.Size;
                }
            }
            frame_imgui_draw_cmd_count = draw_cmd_count;
        }
        int display_w, display_h;
        glfwGetFramebufferSize(window->render_target, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w,
                     clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        const auto gl_draw_start = std::chrono::steady_clock::now();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        frame_gl_draw_ms = durationMs(std::chrono::steady_clock::now() -
                                      gl_draw_start);

        // Update and Render additional Platform Windows
        // (Platform functions may change the current OpenGL context, so we
        // save/restore it to make it easier to paste this code elsewhere.
        //  For this specific demo app we could also call
        //  glfwMakeContextCurrent(window) directly)
//         if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
//             GLFWwindow *backup_current_context = glfwGetCurrentContext();
//             ImGui::UpdatePlatformWindows();
//             ImGui::RenderPlatformWindowsDefault();
//             glfwMakeContextCurrent(backup_current_context);
        const auto swap_start = std::chrono::steady_clock::now();
        glfwSwapBuffers(window->render_target);
        frame_swap_ms =
            durationMs(std::chrono::steady_clock::now() - swap_start);

        if (ps.just_seeked) {
            ps.just_seeked = false;
        } else {
            if (dc_context->decoding_flag && ps.play_video) {
                // always round up, mimimum 1
                int frame_to_show =
                    static_cast<int>(std::ceil(playback_time_now * video_fps));
                perf_requested_camera_frame = frame_to_show;

                int min_decoded_frame = INT_MAX;
                bool have_decode_bound = false;
                auto considerDecodeBound = [&](const std::string &stream_name) {
                    auto need_it = window_need_decoding.find(stream_name);
                    if (need_it == window_need_decoding.end() ||
                        !need_it->second.load()) {
                        return;
                    }
                    auto latest_it = latest_decoded_frame.find(stream_name);
                    if (latest_it == latest_decoded_frame.end()) {
                        return;
                    }
                    int decoded = latest_it->second.load();
                    if (decoded < 0) {
                        return;
                    }
                    min_decoded_frame = std::min(min_decoded_frame, decoded);
                    have_decode_bound = true;
                };
                for (const auto &cam_name : camera_names) {
                    considerDecodeBound(cam_name);
                }
                perf_min_decoded_camera_frame =
                    have_decode_bound ? min_decoded_frame : -1;
                if (have_decode_bound) {
                    frame_to_show = std::min(frame_to_show, min_decoded_frame);
                } else {
                    frame_to_show = ps.to_display_frame_number;
                }
                if (kPlaybackDebugLoggingEnabled) {
                    static int last_logged_display = -1;
                    if (frame_to_show != last_logged_display) {
                        int stim_latest =
                            latest_decoded_frame[stimulus_player.window_name].load();
                        bool stim_decode =
                            window_need_decoding[stimulus_player.window_name].load();
                        std::cout << "[Playback] request frame=" << frame_to_show
                                  << " min_decoded=" << min_decoded_frame
                                  << " stim_latest=" << stim_latest
                                  << " stim_decoder_active="
                                  << (stim_decode ? "true" : "false")
                                  << std::endl;
                        last_logged_display = frame_to_show;
                    }
                }

                int frame_delta = frame_to_show - ps.to_display_frame_number;
                if (frame_delta > 0) {
                    // Update frame number
                    ps.to_display_frame_number = frame_to_show;

                    // Mark all intermediate frames as available
                    for (int offset = 0; offset < frame_delta; ++offset) {
                        int index =
                            (ps.read_head + offset) % scene->size_of_buffer;
                        for (int j = 0; j < scene->num_cams; j++) {
                            scene->cameras[j].display_buffer[index].available_to_write =
                                true;
                        }
                    }

                    // Advance the read head
                    ps.read_head =
                        (ps.read_head + frame_delta) % scene->size_of_buffer;

                    // Optional: update slider/UI sync
                    ps.slider_frame_number = ps.to_display_frame_number;
                }
            }
        }

        if (perf_log_writer.enabled()) {
            const auto now_steady = std::chrono::steady_clock::now();
            if (now_steady - perf_log_writer.last_sample_steady >=
                kPerfLogSamplePeriod) {
                perf_log_writer.last_sample_steady = now_steady;
                const auto now_system = std::chrono::system_clock::now();
                const auto wall_epoch_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now_system.time_since_epoch())
                        .count();
                int visible_camera_count = 0;
                for (const auto& cam_name : camera_names) {
                    auto it = window_need_decoding.find(cam_name);
                    if (it != window_need_decoding.end() && it->second.load()) {
                        visible_camera_count++;
                    }
                }
                const int displayed_camera_frame = ps.to_display_frame_number;
                const int camera_decode_gap_frames =
                    (perf_requested_camera_frame >= 0 &&
                     perf_min_decoded_camera_frame >= 0)
                        ? (perf_requested_camera_frame -
                           perf_min_decoded_camera_frame)
                        : -1;
                double perf_camera_decode_convert_ms =
                    std::numeric_limits<double>::quiet_NaN();
                double perf_camera_decode_wait_ms =
                    std::numeric_limits<double>::quiet_NaN();
                double perf_camera_decode_write_ms =
                    std::numeric_limits<double>::quiet_NaN();
                double perf_camera_decode_pipeline_ms =
                    std::numeric_limits<double>::quiet_NaN();
                auto updateMaxFinite = [](double& dst, double value) {
                    if (!std::isfinite(value)) {
                        return;
                    }
                    if (!std::isfinite(dst) || value > dst) {
                        dst = value;
                    }
                };
                {
                    std::lock_guard<std::mutex> lock(g_decoder_perf_mutex);
                    for (const auto& cam_name : camera_names) {
                        auto need_it = window_need_decoding.find(cam_name);
                        if (need_it == window_need_decoding.end() ||
                            !need_it->second.load()) {
                            continue;
                        }
                        auto perf_it = decoder_perf_samples.find(cam_name);
                        if (perf_it == decoder_perf_samples.end() ||
                            !perf_it->second) {
                            continue;
                        }
                        const auto& perf = perf_it->second;
                        updateMaxFinite(
                            perf_camera_decode_convert_ms,
                            perf->nv12_to_rgba_ms.load());
                        updateMaxFinite(
                            perf_camera_decode_wait_ms,
                            perf->buffer_wait_ms.load());
                        updateMaxFinite(
                            perf_camera_decode_write_ms,
                            perf->frame_write_ms.load());
                        updateMaxFinite(
                            perf_camera_decode_pipeline_ms,
                            perf->frame_total_ms.load());
                    }
                }
                const int stimulus_latest_decoded =
                    latest_decoded_frame[stimulus_player.window_name].load();
                const int stimulus_last_displayed =
                    stimulus_player.last_displayed_frame;
                const int stimulus_progress_frame =
                    std::max(stimulus_latest_decoded, stimulus_last_displayed);
                const int stimulus_target_frame = ps.current_stimulus_frame;
                const int stimulus_progress_gap_frames =
                    (stimulus_target_frame >= 0 && stimulus_progress_frame >= 0)
                        ? (stimulus_target_frame - stimulus_progress_frame)
                        : -1;
                const int stimulus_buffered_frames =
                    stimulus_player.loaded
                        ? countBufferedStimulusFrames(stimulus_player)
                        : 0;
                const double elapsed_s =
                    std::chrono::duration<double>(
                        now_steady - perf_log_writer.start_steady)
                        .count();
                const double frame_loop_ms =
                    durationMs(now_steady - frame_loop_start);
                perf_log_writer.stream
                    << elapsed_s << "," << wall_epoch_ms << ","
                    << (ps.play_video ? 1 : 0) << "," << set_playback_speed
                    << "," << inst_speed << "," << video_fps << ","
                    << perf_requested_camera_frame << ","
                    << displayed_camera_frame << "," << current_frame_num << ","
                    << perf_min_decoded_camera_frame << ","
                    << camera_decode_gap_frames << ","
                    << perf_camera_decode_convert_ms << ","
                    << perf_camera_decode_wait_ms << ","
                    << perf_camera_decode_write_ms << ","
                    << perf_camera_decode_pipeline_ms << ","
                    << visible_camera_count
                    << "," << (scene->use_cpu_buffer ? "cpu" : "gpu") << ","
                    << playbackPreviewScaleLabel() << ","
                    << (playbackPreviewIsActive() ? 1 : 0) << ","
                    << playbackRendererModeLabel() << ","
                    << perf_camera_viewport_width_px << ","
                    << perf_camera_viewport_height_px << ","
                    << perf_camera_view_x_min << ","
                    << perf_camera_view_x_max << ","
                    << perf_camera_view_y_min << ","
                    << perf_camera_view_y_max << ","
                    << perf_camera_view_visible_fraction << ","
                    << perf_camera_view_zoomed_in << ","
                    << frame_camera_upload_count << ","
                    << frame_camera_upload_ms << ","
                    << frame_camera_texture_resize_ms << ","
                    << frame_camera_preview_resize_ms << ","
                    << frame_camera_display_convert_ms << ","
                    << frame_camera_pbo_copy_ms << ","
                    << frame_camera_texture_upload_ms << ","
                    << frame_camera_playback_front_path_ms << ","
                    << frame_camera_playback_stage_total_ms << ","
                    << frame_camera_playback_stage_upload_ms << ","
                    << frame_camera_playback_swap_ms << ","
                    << frame_camera_plot_image_ui_ms << ","
                    << frame_camera_overlay_ui_ms << ","
                    << frame_camera_scene_ui_ms << ","
                    << frame_file_browser_ui_ms << ","
                    << frame_frame_debug_ui_ms << ","
                    << frame_buffer_window_ui_ms << ","
                    << frame_crop_preview_ui_ms << ","
                    << frame_stimulus_buffer_window_ui_ms << ","
                    << frame_keypoints_window_ui_ms << ","
                    << frame_labeling_tool_ui_ms << ","
                    << frame_stimulus_window_ui_ms << ","
                    << frame_stimulus_timeline_ui_ms << ","
                    << frame_movement_timeline_ui_ms << ","
                    << frame_help_menu_ui_ms << ","
                    << frame_gl_draw_ms << ","
                    << frame_swap_ms << "," << frame_loop_ms << ","
                    << frame_ui_build_ms << ","
                    << frame_imgui_render_ms << ","
                    << frame_imgui_draw_cmd_count << ","
                    << frame_imgui_draw_list_count << ","
                    << frame_imgui_total_vtx_count << ","
                    << frame_imgui_total_idx_count << ","
                    << (stimulus_player.loaded ? 1 : 0) << ","
                    << ((stimulus_player.loaded
                             ? stimulus_player.use_software_decode
                             : stimulus_use_software_decode)
                            ? "software"
                            : "gpu")
                    << ","
                    << ((stimulus_player.loaded ? stimulus_player.use_cpu_buffer
                                                : stimulus_use_cpu_buffer)
                            ? "cpu"
                            : "gpu")
                    << "," << stimulus_target_frame << ","
                    << stimulus_latest_decoded << ","
                    << stimulus_last_displayed << ","
                    << stimulus_buffered_frames << ","
                    << stimulus_progress_gap_frames << "\n";
                perf_log_writer.stream.flush();

                json metadata = {
                    {"format", "crimson_perf_metadata_v1"},
                    {"generated_wall_epoch_ms", wall_epoch_ms},
                    {"perf_csv_path", perf_log_writer.csv_path.string()},
                    {"cwd", cwd.string()},
                    {"argv0_path", argv0_path.string()},
                    {"recording_path",
                     cli_recording_path.empty() ? json(nullptr)
                                                : json(cli_recording_path)},
                    {"zarr_override_path",
                     cli_zarr_override_path.empty()
                         ? json(nullptr)
                         : json(cli_zarr_override_path)},
                    {"window",
                     {{"swap_interval", window->swap_interval},
                      {"width", window->width},
                      {"height", window->height}}},
                    {"main_video",
                     {{"loaded", video_loaded},
                      {"fps", video_fps},
                      {"buffer_mode", scene->use_cpu_buffer ? "cpu" : "gpu"},
                      {"buffer_storage_format",
                       scene->use_cpu_buffer ? "rgba32" : "nv12"},
                      {"playback_preview_scale", playbackPreviewScaleLabel()},
                      {"playback_preview_active", playbackPreviewIsActive()},
                      {"playback_renderer_mode", playbackRendererModeLabel()},
                      {"viewport_width_px", perf_camera_viewport_width_px},
                      {"viewport_height_px", perf_camera_viewport_height_px},
                      {"view_x_min", perf_camera_view_x_min},
                      {"view_x_max", perf_camera_view_x_max},
                      {"view_y_min", perf_camera_view_y_min},
                      {"view_y_max", perf_camera_view_y_max},
                      {"view_visible_fraction",
                       perf_camera_view_visible_fraction},
                      {"view_zoomed_in", perf_camera_view_zoomed_in},
                      {"buffer_size",
                       video_loaded ? static_cast<int>(scene->size_of_buffer)
                                    : label_buffer_size},
                      {"requested_playback_speed", set_playback_speed},
                      {"measured_playback_speed", inst_speed},
                      {"requested_camera_frame", perf_requested_camera_frame},
                      {"displayed_camera_frame", displayed_camera_frame},
                      {"current_frame_num", current_frame_num},
                      {"min_decoded_camera_frame", perf_min_decoded_camera_frame},
                      {"camera_decode_gap_frames", camera_decode_gap_frames},
                      {"visible_camera_count", visible_camera_count},
                      {"camera_names", camera_names}}},
                    {"stimulus",
                     {{"loaded", stimulus_player.loaded},
                      {"decode_backend",
                       ((stimulus_player.loaded
                             ? stimulus_player.use_software_decode
                             : stimulus_use_software_decode)
                            ? "software"
                            : "gpu")},
                      {"buffer_mode",
                       ((stimulus_player.loaded ? stimulus_player.use_cpu_buffer
                                                : stimulus_use_cpu_buffer)
                            ? "cpu"
                            : "gpu")},
                      {"buffer_size",
                       stimulus_player.loaded ? stimulus_player.buffer_size
                                              : stimulus_buffer_size},
                      {"target_frame", stimulus_target_frame},
                      {"latest_decoded_frame", stimulus_latest_decoded},
                      {"last_displayed_frame", stimulus_last_displayed},
                      {"buffered_frames", stimulus_buffered_frames},
                      {"progress_gap_frames", stimulus_progress_gap_frames}}}
                };
                perf_log_writer.writeMetadata(metadata);
            }
        }
    }

    // Cleanup
    destroyStimulusPlayback(stimulus_player);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window->render_target);
    glfwTerminate();

    dc_context->stop_flag = true;
    // wait for threads to join
    for (auto &t : decoder_threads)
        t.join();

    return 0;
}
