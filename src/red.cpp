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
#include "zarr_loader.h"
#include "gui_interpolation.h"
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
    glUniformMatrix3fv(presenter->yuv_matrix_location, 1, GL_FALSE,
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

struct EyeOrientationSmoother {
    struct History {
        std::deque<double> angles;
        double last_unwrapped = std::numeric_limits<double>::quiet_NaN();
    };

    static constexpr size_t kMaxSamples = 60;

    void resetIfRunChanged(const std::string& run_id) {
        if (run_id != current_run_) {
            histories_.clear();
            current_run_ = run_id;
        }
    }

    ImVec2 smoothDirection(int32_t roi_index, int eye, const ImVec2& raw_dir) {
        if (roi_index < 0 || eye < 0 || eye >= 2) {
            return raw_dir;
        }
        double raw_angle = std::atan2(raw_dir.y, raw_dir.x);
        if (!std::isfinite(raw_angle)) {
            return raw_dir;
        }

        auto& history = histories_[roi_index][eye];

        double unwrapped = raw_angle;
        if (std::isfinite(history.last_unwrapped)) {
            while (unwrapped - history.last_unwrapped > M_PI) {
                unwrapped -= 2.0 * M_PI;
            }
            while (unwrapped - history.last_unwrapped < -M_PI) {
                unwrapped += 2.0 * M_PI;
            }
        }
        history.last_unwrapped = unwrapped;

        history.angles.push_back(unwrapped);
        if (history.angles.size() > kMaxSamples) {
            history.angles.pop_front();
        }

        std::vector<double> sorted(history.angles.begin(), history.angles.end());
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];
        if ((sorted.size() % 2) == 0 && sorted.size() >= 2) {
            median = 0.5 * (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]);
        }

        double wrapped = std::fmod(median, 2.0 * M_PI);
        if (wrapped <= -M_PI) wrapped += 2.0 * M_PI;
        if (wrapped > M_PI) wrapped -= 2.0 * M_PI;

        ImVec2 smoothed(static_cast<float>(std::cos(wrapped)),
                        static_cast<float>(std::sin(wrapped)));
        float len = std::sqrt(smoothed.x * smoothed.x + smoothed.y * smoothed.y);
        if (len > 1e-6f) {
            smoothed.x /= len;
            smoothed.y /= len;
            return smoothed;
        }
        return raw_dir;
    }

private:
    std::unordered_map<int32_t, std::array<History, 2>> histories_;
    std::string current_run_;
};

static EyeOrientationSmoother g_eye_orientation_smoother;

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

    while (!glfwWindowShouldClose(window->render_target)) {
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
        if (ImGui::Begin("File Browser", NULL, ImGuiWindowFlags_MenuBar)) {
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("Open")) {
                        IGFD::FileDialogConfig config;
                        config.countSelectionMax = 0;
                        config.path = start_folder_name;
                        config.flags = ImGuiFileDialogFlags_Modal;
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "ChooseMedia", "Choose Media",
                            ".mp4,.tiff,.jpeg,.jpg,.png", config);
                    };
                    if (ImGui::MenuItem("Load Zarr Archive")) {
                        IGFD::FileDialogConfig config;
                        config.countSelectionMax = 1;
                        config.path = root_dir.empty() ? start_folder_name : root_dir;
                        config.flags = ImGuiFileDialogFlags_Modal;
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "ChooseZarrArchive", "Choose Zarr Archive Directory",
                            nullptr, config);
                    }
                    if (video_loaded) {
                        if (ImGui::MenuItem("Load Stimulus Video")) {
                            IGFD::FileDialogConfig config;
                            config.countSelectionMax = 1;
                            config.path = root_dir;
                            config.flags = ImGuiFileDialogFlags_Modal;
                            ImGuiFileDialog::Instance()->OpenDialog(
                                "ChooseStimulus", "Choose Stimulus Video",
                                ".mp4", config);
                        }
                    }
                    if (!ui_path_config.preferred_roots.empty() &&
                        ImGui::BeginMenu("Path Preset")) {
                        for (const auto& preset_path : ui_path_config.preferred_roots) {
                            bool selected = (preset_path == start_folder_name);
                            if (ImGui::MenuItem(preset_path.c_str(), nullptr, selected)) {
                                start_folder_name = preset_path;
                                std::cout << "[UIPathConfig] Start path set to: "
                                          << start_folder_name << std::endl;
                            }
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }

                if (video_loaded) {
                    if (ImGui::BeginMenu("Skeleton")) {
                        if (!skeleton_chosen) {
                            skeleton = std::make_unique<SkeletonContext>();
                            skeleton_map = skeleton_get_all();
                        }

                        for (auto &element : skeleton_map) {

                            if (ImGui::MenuItem(element.first.c_str(), NULL,
                                                skeleton->name == element.first,
                                                !skeleton_chosen)) {
                                if (element.second == SP_LOAD) {
                                    IGFD::FileDialogConfig config;
                                    config.countSelectionMax = 1;
                                    config.path = skeleton_dir;
                                    config.flags = ImGuiFileDialogFlags_Modal;
                                    ImGuiFileDialog::Instance()->OpenDialog(
                                        "ChooseSkeleton", "Choose Skeleton",
                                        ".json", config);
                                } else {

                                    bool load_calibration = true;
                                    if (scene->num_cams > 1) {
                                        for (u32 i = 0; i < scene->num_cams;
                                             i++) {
                                            std::string cam_file =
                                                root_dir + "/calibration/" +
                                                camera_names[i] + ".yaml";

                                            if (!std::filesystem::exists(cam_file)) {
                                                load_calibration = false;
                                                error_message = "Calibration file not found: " + cam_file;
                                                show_error = true;
                                                break;
                                            }
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
                                        skeleton_initialize(element.first,
                                                            root_dir, skeleton.get(),
                                                            element.second);
                                        plot_keypoints_flag = true;
                                        keypoints_root_folder =
                                            root_dir + "/labeled_data/";
                                        // create folders
                                        std::filesystem::create_directory(
                                            keypoints_root_folder);
                                        skeleton_chosen = true;
                                    }
                                }
                            }

                        }
                        ImGui::EndMenu();
                    }

                    if (ImGui::BeginMenu("Detection")) {
                        if (cpu_buffer_toggle) {
                            if (ImGui::MenuItem("YOLOv5")) {
                                std::string yolov5_onnx =
                                    root_dir + "/yolo/v5/best.onnx";
                                std::string yolov5_labelname =
                                    root_dir + "/yolo/v5/label.names";
                                read_yolo_labels(yolov5_labelname,
                                                 &yolo_setting);

                                for (int i = 0; i < scene->num_cams; i++) {
                                    yolo_threads.push_back(
                                        std::thread(&yolo_process, yolov5_onnx,
                                                    &yolo_setting, i));
                                }
                                yolo_detection = true;
                            }
                        } else {
                            if (ImGui::MenuItem("YOLOv8")) {
                                std::string engine_file_path =
                                    root_dir +
                                    "/yolo/yolorat_bbox/rat_bbox.engine";
                                for (int i = 0; i < scene->num_cams; i++) {
                                    yolo_threads.push_back(std::thread(
                                        &yolo_process_trt, engine_file_path, i,
                                        scene->cameras[i].image_width,
                                        scene->cameras[i].image_height));
                                }
                                yolo_detection = true;
                            }

                            if (ImGui::MenuItem("YOLOv8Pose")) {
                                std::string engine_file_path =
                                    root_dir + "/yolo/yolopose/rat_pose.engine";
                                for (int i = 0; i < scene->num_cams; i++) {
                                    yolo_threads.push_back(std::thread(
                                        &yolo_process_v8pose, engine_file_path,
                                        i, scene->cameras[i].image_width,
                                        scene->cameras[i].image_height));
                                }
                                yolo_detection = true;
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
                ImGui::EndMenuBar();
            }
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                        1000.0f / ImGui::GetIO().Framerate,
                        ImGui::GetIO().Framerate);

            if (!video_loaded) {
                {
                    const char *items[] = {"CPU Buffer", "GPU Buffer"};
                    static int item_current = 0;
                    ImGui::Combo("Buffer Type", &item_current, items,
                                 IM_ARRAYSIZE(items));
                    if (item_current == 0) {
                        scene->use_cpu_buffer = true;
                    } else {
                        scene->use_cpu_buffer = false;
                    }
                }

                ImGui::InputInt("Buffer Size", &label_buffer_size);
                label_buffer_size = std::max(1, label_buffer_size);
            }
            {
                const char *items[] = {"Full Resolution (1x)",
                                       "Half-Resolution Preview (1/2)",
                                       "Quarter-Resolution Preview (1/4)"};
                ImGui::Combo("Playback Preview Scale", &playback_preview_scale_mode,
                             items, IM_ARRAYSIZE(items));
                if (playback_preview_scale_mode != 0) {
                    if (yolo_detection) {
                        ImGui::TextDisabled(
                            "Preview scaling is temporarily disabled while YOLO inference is active.");
                    } else if (!ps.play_video) {
                        ImGui::TextDisabled(
                            "Preview scaling applies only during playback; paused inspection remains full resolution.");
                    } else if (scene->use_cpu_buffer) {
                        ImGui::Text("Effective preview scale: %s (CPU resized preview)",
                                    playbackPreviewScaleLabel());
                    } else {
                        ImGui::Text("Effective preview scale: %s (GPU mip preview)",
                                    playbackPreviewScaleLabel());
                    }
                }
            }
            {
                const char *items[] = {"Standard Renderer",
                                       "Lightweight Playback Renderer"};
                ImGui::Combo("Playback Renderer", &playback_renderer_mode,
                             items, IM_ARRAYSIZE(items));
                if (playback_renderer_mode == 1) {
                    if (!ps.play_video) {
                        ImGui::TextDisabled(
                            "The lightweight renderer applies only during playback; paused inspection keeps the full plot path.");
                    } else {
                        ImGui::Text(
                            "Active playback renderer: %s",
                            playbackRendererModeLabel());
                    }
                }
            }
            if (!stimulus_player.loaded) {
                ImGui::InputInt("Stimulus Buffer Size", &stimulus_buffer_size);
                stimulus_buffer_size = std::max(1, stimulus_buffer_size);
                {
                    const char *items[] = {"Stimulus GPU Buffer", "Stimulus CPU Buffer"};
                    int stimulus_buffer_mode = stimulus_use_cpu_buffer ? 1 : 0;
                    ImGui::Combo("Stimulus Buffer Type", &stimulus_buffer_mode, items,
                                 IM_ARRAYSIZE(items));
                    stimulus_use_cpu_buffer = (stimulus_buffer_mode == 1);
                }
                {
                    const char *items[] = {"Stimulus Software Decode",
                                           "Stimulus GPU Decode"};
                    int stimulus_decode_mode =
                        stimulus_use_software_decode ? 0 : 1;
                    ImGui::Combo("Stimulus Decode Backend", &stimulus_decode_mode,
                                 items, IM_ARRAYSIZE(items));
                    stimulus_use_software_decode =
                        (stimulus_decode_mode == 0);
                }
                ImGui::Text("Stimulus Buffer Size: %d", stimulus_buffer_size);
                ImGui::Text("Stimulus Decode Backend: %s",
                            stimulus_use_software_decode ? "Software"
                                                         : "GPU");
                ImGui::Text("Stimulus Buffer Mode: %s",
                            stimulus_use_cpu_buffer ? "CPU" : "GPU");
            } else {
                ImGui::Text("Stimulus Buffer Size: %d", stimulus_player.buffer_size);
                ImGui::Text("Stimulus Decode Backend: %s",
                            stimulus_player.use_software_decode ? "Software"
                                                                : "GPU");
                ImGui::Text("Stimulus Buffer Mode: %s",
                            stimulus_player.use_cpu_buffer ? "CPU" : "GPU");
            }
            if (video_loaded) {
                ImGui::InputInt("Seek Step", &dc_context->seek_interval, 10,
                                100);
                static int seek_accurate_frame_num = 0;
                ImGui::InputInt("Seek Accurate", &seek_accurate_frame_num, 1,
                                100);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    seekToFrame(seek_accurate_frame_num, false);
                }

                auto now_wall = std::chrono::steady_clock::now();
                double wall_seconds =
                    std::chrono::duration<double>(now_wall -
                                                  ps.last_wall_time_playspeed)
                        .count();
                int frame_delta =
                    current_frame_num - ps.last_frame_num_playspeed;
                if (wall_seconds > 0.5 && ps.play_video) {
                    inst_speed =
                        frame_delta /
                        (video_fps * wall_seconds); // Real-time normalized
                    ps.last_frame_num_playspeed = current_frame_num;
                    ps.last_wall_time_playspeed = now_wall;
                }

                // Always draw the latest value
                if (ps.play_video) {
                    ImGui::Text("Video FPS: %.1f", video_fps);
                    ImGui::SliderFloat("Set Playback Speed",
                                       &set_playback_speed, 0.1f, 1.0f,
                                       "%.1fx");
                    ImGui::Text("Current Playback Speed: %.2fx", inst_speed);
                    ImGui::Text("Tip: If playback is slower than real-time, \n"
                                "collapse camera views to improve speed.");
                }
            }
        }
        ImGui::End();
        frame_file_browser_ui_ms +=
            durationMs(std::chrono::steady_clock::now() - file_browser_ui_start);

        if (video_loaded) {
            const auto frame_debug_ui_start = std::chrono::steady_clock::now();
            ImGui::Begin("Frame Debug");
            ImGui::Text("Inspecting Frame: %d", current_frame_num);
            ImGui::Text("Display target frame: %d", ps.to_display_frame_number);
            ImGui::Text("Slider frame: %d", ps.slider_frame_number);
            if (frame_sync_valid_slots >= 0 && frame_sync_empty_slots >= 0) {
                ImGui::Text("Buffer frames: valid=%d empty_remaining=%d total=%u",
                            frame_sync_valid_slots, frame_sync_empty_slots,
                            scene->size_of_buffer);
            }
            if (frame_sync_recording_remaining >= 0 && frame_sync_recording_total > 0) {
                ImGui::Text("Recording decode: latest=%d remaining=%d total=%d",
                            frame_sync_latest_decoded,
                            frame_sync_recording_remaining,
                            frame_sync_recording_total);
            }
            if (!frame_sync_debug_line.empty()) {
                ImGui::TextWrapped("Frame sync: %s", frame_sync_debug_line.c_str());
            }
            ImGui::Separator();

            // Check for Labeled Keypoints
            if (plot_keypoints_flag) {
                if (keypoints_map.count(current_frame_num)) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[Manual] Keypoints:   Found");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "[Manual] Keypoints:   None");
                }
            }

            if (zarr_loaded) {
                const bool dataset_has_synthetic_boxes = zarr_loader.activeDatasetHasSyntheticDetections();
                if (!detection_dataset_labels.empty()) {
                    ImGui::Text("Detection dataset:");
                    const char* current_label =
                        detection_dataset_labels[std::min<int>(detection_dataset_choice,
                                                               static_cast<int>(detection_dataset_labels.size()) - 1)].c_str();
                    bool dataset_changed = false;
                    if (ImGui::BeginCombo("##detection_dataset_combo", current_label)) {
                        for (int i = 0; i < static_cast<int>(detection_dataset_labels.size()); ++i) {
                            bool selected = (i == detection_dataset_choice);
                            if (ImGui::Selectable(detection_dataset_labels[i].c_str(), selected)) {
                                if (zarr_loader.setActiveDetectionDataset(detection_dataset_ids[i])) {
                                    detection_dataset_choice = i;
                                    dataset_changed = true;
                                }
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                        if (dataset_changed) {
                            refreshDetectionDatasetOptions(zarr_loader);
                            g_zarr_bbox_edit_state.clearAll();
                            invalidateReviewFrameCache();
                            review_frame_status.clear();
                            if (zarr_loader.getTotalFrames() > 0 &&
                                current_frame_num >= static_cast<int>(zarr_loader.getTotalFrames())) {
                                current_frame_num = static_cast<int>(zarr_loader.getTotalFrames()) - 1;
                            }
                        }
                    }
                }
                if (zarr_loader.hasReviewStatus()) {
                    const auto& rs = zarr_loader.getReviewState();
                    ImVec4 status_color = (rs == "approved")
                        ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                        : (rs == "rejected")
                            ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                            : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
                    ImGui::TextColored(status_color, "Review: %s", rs.c_str());
                    ImGui::SameLine();
                    ImGui::Text("| Use: %s | Method: %s",
                                zarr_loader.getReviewIntendedUse().c_str(),
                                zarr_loader.getReviewMethod().c_str());
                    if (!zarr_loader.getReviewTimestamp().empty()) {
                        ImGui::Text("  Reviewed: %s", zarr_loader.getReviewTimestamp().c_str());
                    }
                    if (!zarr_loader.getReviewReviewer().empty()) {
                        ImGui::Text("  Reviewer: %s", zarr_loader.getReviewReviewer().c_str());
                    }
                    if (!zarr_loader.getReviewNotes().empty()) {
                        ImGui::Text("  Notes: %s", zarr_loader.getReviewNotes().c_str());
                    }
                }
                if (zarr_loader.hasKeypointReviewStatus()) {
                    const auto& krs = zarr_loader.getKeypointReviewState();
                    ImVec4 kp_status_color = (krs == "approved")
                        ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                        : (krs == "rejected")
                            ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                            : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
                    ImGui::TextColored(kp_status_color, "KP Review: %s", krs.c_str());
                    ImGui::SameLine();
                    ImGui::Text("| Use: %s | Method: %s",
                                zarr_loader.getKeypointReviewIntendedUse().c_str(),
                                zarr_loader.getKeypointReviewMethod().c_str());
                    if (!zarr_loader.getKeypointReviewTimestamp().empty()) {
                        ImGui::Text("  KP Reviewed: %s", zarr_loader.getKeypointReviewTimestamp().c_str());
                    }
                    if (!zarr_loader.getKeypointReviewReviewer().empty()) {
                        ImGui::Text("  KP Reviewer: %s", zarr_loader.getKeypointReviewReviewer().c_str());
                    }
                    if (!zarr_loader.getKeypointReviewNotes().empty()) {
                        ImGui::Text("  KP Notes: %s", zarr_loader.getKeypointReviewNotes().c_str());
                    }
                }
                if (!zarr_loader.hasDetectionData()) {
                    ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.25f, 1.0f),
                                       "[Zarr] Detection runs: unavailable (metadata/stimulus-only mode)");
                }

                    if (zarr_loader.hasStimulusAlignment()) {
                        ImGui::Separator();
                        ImGui::Text("Stimulus Alignment:");
                        if (zarr_loader.hasStimulusFrameMapping()) {
                            ImGui::Text("  Mapping variant: %s",
                                        zarr_loader.hasCorrectedStimulusFrameMapping()
                                            ? "corrected"
                                            : "legacy");
                            if (ps.current_stimulus_frame >= 0) {
                                ImGui::Text("  Current stimulus frame: %d", ps.current_stimulus_frame);
                            } else {
                                ImGui::Text("  Current stimulus frame: (not mapped)");
                            }
                            if (auto metadata_index =
                                    zarr_loader.getStimulusMetadataIndexForCameraFrame(current_frame_num)) {
                                ImGui::Text("  Frame metadata index: %d", *metadata_index);
                            }
                            if (auto first_cam = zarr_loader.getFirstCameraFrameWithStimulus()) {
                                if (auto first_stim = zarr_loader.getFirstStimulusFrameNumber()) {
                                    ImGui::Text("  First mapped camera frame: %d -> Stim %d",
                                                *first_cam, *first_stim);
                                } else {
                                    ImGui::Text("  First mapped camera frame: %d", *first_cam);
                                }
                            }
                            ImGui::Text("  Camera frame offset: %lld",
                                        static_cast<long long>(zarr_loader.getStimulusCameraFrameOffset()));
                        } else {
                            ImGui::Text("  Mapping data not available");
                        }
                    }

                std::vector<LoggedBoundingBox> zarr_boxes;
                bool frame_is_interpolated = false;
                const bool frame_has_bbox_edits =
                    g_zarr_bbox_edit_state.isFrameDirty(current_frame_num);

                if (zarr_loader.hasInterpolation()) {
                    frame_is_interpolated = zarr_loader.isFrameInterpolated(current_frame_num);
                }

                std::vector<LoggedBoundingBox> loaded_zarr_boxes =
                    zarr_loader.getBoundingBoxesForFrame(current_frame_num);
                zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(current_frame_num,
                                                                      loaded_zarr_boxes);
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

                const bool need_details =
                    zarr_loader.hasScores() ||
                    zarr_loader.hasHeadingData() ||
                    zarr_loader.hasKeypointData() ||
                    dataset_has_synthetic_boxes;
                ZarrDetectionLoader::FrameDetections detection_details;
                if (need_details) {
                    detection_details = zarr_loader.getRawDetections(current_frame_num, false);
                }

                if (!zarr_boxes.empty()) {
                    if (frame_is_interpolated && dataset_has_synthetic_boxes) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    Found %zu (INTERPOLATED)",
                                           zarr_boxes.size());
                    } else if (frame_is_interpolated && !dataset_has_synthetic_boxes) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                           "[Zarr] Detections:    Found %zu (original, interp available)",
                                           zarr_boxes.size());
                    } else {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    Found %zu", zarr_boxes.size());
                    }

                    if (zarr_loader.hasScores() && !detection_details.scores.empty()) {
                        float max_score = *std::max_element(
                            detection_details.scores.begin(), detection_details.scores.end());
                        ImGui::Text("  Max confidence: %.2f", max_score);
                    }

                    if (zarr_loader.hasClassIDs()) {
                        ImGui::Text("  Has class IDs: Yes");
                    }

                    if (zarr_loader.hasHeadingData()) {
                        if (!dataset_has_synthetic_boxes &&
                            !detection_details.heading_valid.empty()) {
                            size_t valid_headings =
                                std::count(detection_details.heading_valid.begin(),
                                           detection_details.heading_valid.end(), 1);
                            ImGui::Text("  Heading vectors: %zu valid", valid_headings);
                        } else {
                            ImGui::Text("  Heading vectors available (use original detections)");
                        }
                    }

                    if (zarr_loader.hasInterpolation()) {
                        ImGui::Text("  Interpolation available: Yes");
                        ImGui::Text("  Current frame interpolated: %s",
                                    frame_is_interpolated ? "Yes" : "No");
                        ImGui::Text("  Using interpolation: %s",
                                    dataset_has_synthetic_boxes ? "Yes" : "No");
                        ImGui::Text("  Method: %s",
                                    zarr_loader.getInterpolationMethod().c_str());
                    }
                } else {
                    if (zarr_loader.hasInterpolation() && frame_is_interpolated) {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    None (frame is interpolated)");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                           "[Zarr] Detections:    None");
                    }
	                }

                ImGui::Separator();
                ImGui::Text("Review Navigation:");
                bool review_filter_changed = false;
                review_filter_changed |=
                    ImGui::Checkbox("Interpolated##review_filter_interpolated",
                                    &review_frame_filters.include_interpolated);
                ImGui::SameLine();
                review_filter_changed |=
                    ImGui::Checkbox("Non-clean##review_filter_non_clean",
                                    &review_frame_filters.include_non_clean);
                ImGui::SameLine();
                review_filter_changed |=
                    ImGui::Checkbox("Empty##review_filter_empty",
                                    &review_frame_filters.include_empty);
                if (review_filter_changed) {
                    invalidateReviewFrameCache();
                    review_frame_status.clear();
                }

                const bool review_filter_enabled =
                    review_frame_filters.include_interpolated ||
                    review_frame_filters.include_non_clean ||
                    review_frame_filters.include_empty;
                ImGui::BeginDisabled(!review_filter_enabled);
                if (ImGui::Button("Prev Review Frame")) {
                    jumpToReviewFrame(false);
                }
                ImGui::SameLine();
                if (ImGui::Button("Next Review Frame")) {
                    jumpToReviewFrame(true);
                }
                ImGui::EndDisabled();
                if (!review_filter_enabled) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                        "Enable at least one review filter to jump frames.");
                } else if (review_frame_cache.valid) {
                    ImGui::Text("  Indexed review frames: %zu",
                                review_frame_cache.frames.size());
                }
                if (!review_frame_status.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "%s",
                                       review_frame_status.c_str());
                }

                ImGui::Separator();
                ImGui::Text("Decode Debug:");
                if (ImGui::Button("Dump Decode Buffers")) {
                    dumpDecodeBuffersToVideos("manual_dump");
                }
                ImGui::SameLine();
                if (ImGui::Button("Random Seek + Dump")) {
                    randomSeekAndDumpBuffers();
                }
                ImGui::TextWrapped(
                    "  Output dir: CRIMSON_BUFFER_DUMP_DIR (default %s)",
                    default_buffer_dump_root.string().c_str());
                if (!decode_debug_status.empty()) {
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                                       "%s",
                                       decode_debug_status.c_str());
                }

                ImGui::TextWrapped(
                    "  Box colors: clean=blue, interpolated=orange, manual=teal");

	                ImGui::Separator();
	                ImGui::Text("BBox Edit (in-memory):");
                if (!dataset_allows_bbox_edit) {
                    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                       "Read-only: Zarr Raw Detect (read-only) cannot be edited.");
                }
                ImGui::BeginDisabled(!dataset_allows_bbox_edit);
                ImGui::Checkbox("Enable bbox drag editing", &g_zarr_bbox_edit_state.enabled);
                bool draw_mode_enabled = g_zarr_bbox_edit_state.draw_mode;
                if (ImGui::Checkbox("Draw new boxes (N)", &draw_mode_enabled)) {
                    g_zarr_bbox_edit_state.draw_mode = draw_mode_enabled;
                    if (!draw_mode_enabled) {
                        g_zarr_bbox_edit_state.cancelDraw();
                    } else {
                        g_zarr_bbox_edit_state.clearSelection();
                    }
                }
                if (ps.play_video && g_zarr_bbox_edit_state.enabled &&
                    !g_zarr_bbox_edit_state.allow_edit_while_playing) {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                       "Pause playback to drag boxes.");
                }
                if (g_zarr_bbox_edit_state.draw_mode) {
                    ImGui::Text("  Draw mode active: Ctrl + left-drag to place; Esc cancels.");
                }
                ImGui::Text("  Cycle/select bbox: B (Shift+B reverse)");
                ImGui::Text("  Move selected bbox: Ctrl + left-drag");
                ImGui::Text("  Pan while editing: Shift + drag");
                ImGui::Text("  Delete selected bbox: Del");
                ImGui::Text("  Current frame: %s",
                            frame_has_bbox_edits ? "edited (unsaved)" : "unchanged");
                ImGui::Text("  Pending edited frames: %zu",
                            g_zarr_bbox_edit_state.dirtyFrameCount());
                if (ImGui::Button("Reset Frame BBox Edits (Shift+R)")) {
                    g_zarr_bbox_edit_state.clearFrameEdits(current_frame_num);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear BBox Selection (Esc)")) {
                    g_zarr_bbox_edit_state.clearSelection();
                }
                if (ImGui::Button("Build Manual Payload Preview")) {
                    manual_payload_preview = buildManualDetectPayloadPreview();
                    if (!manual_payload_preview->valid) {
                        bbox_payload_status = "Manual payload preview failed: " +
                                              manual_payload_preview->error;
                    } else {
                        std::ostringstream payload_msg;
                        payload_msg << "Manual payload preview: frames="
                                    << manual_payload_preview->total_frames
                                    << " detections="
                                    << manual_payload_preview->total_detections
                                    << " clean=" << manual_payload_preview->clean_rows
                                    << " interpolated="
                                    << manual_payload_preview->interpolated_rows
                                    << " manual=" << manual_payload_preview->manual_rows
                                    << " dirty_frames="
                                    << g_zarr_bbox_edit_state.dirtyFrameCount();
                        bbox_payload_status = payload_msg.str();
                    }
                }
                const char* intended_use_items[] = {"full_recording", "training"};
                ImGui::Combo("Intended Use##manual_write",
                             &manual_write_intended_use,
                             intended_use_items,
                             IM_ARRAYSIZE(intended_use_items));
                const char* review_state_items[] = {"approved", "needs_review", "pending", "rejected"};
                ImGui::Combo("Review State##manual_write",
                             &manual_write_review_state,
                             review_state_items,
                             IM_ARRAYSIZE(review_state_items));
                if (ImGui::Button("Write Manual Payload to Zarr")) {
                    manual_payload_preview = buildManualDetectPayloadPreview();

                    if (!manual_payload_preview->valid) {
                        bbox_payload_status = "Manual write failed: payload preview invalid: " +
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
                        review_opts.intended_use = intended_use_items[manual_write_intended_use];
                        review_opts.state = review_state_items[manual_write_review_state];
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
                            const std::string archive_path = zarr_loader.getArchivePath();
                            if (!archive_path.empty() &&
                                zarr_loader.loadZarrFile(archive_path, reload_error)) {
                                zarr_loaded = true;
                                if (zarr_loader.isDatasetAvailable(
                                        ZarrDetectionLoader::DetectionDataset::RefinedManual)) {
                                    (void)zarr_loader.setActiveDetectionDataset(
                                        ZarrDetectionLoader::DetectionDataset::RefinedManual);
                                }
                                refreshDetectionDatasetOptions(zarr_loader);
                                g_zarr_bbox_edit_state.clearAll();
                                manual_payload_preview.reset();
                                invalidateReviewFrameCache();
                                review_frame_status.clear();
                                if (zarr_loader.getTotalFrames() > 0 &&
                                    current_frame_num >= static_cast<int>(zarr_loader.getTotalFrames())) {
                                    current_frame_num =
                                        static_cast<int>(zarr_loader.getTotalFrames()) - 1;
                                }
                                std::ostringstream payload_msg;
                                payload_msg
                                    << "Manual write complete: run="
                                    << (resolved_refined_run.empty() ? "<latest>" : resolved_refined_run)
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
                ImGui::TextWrapped(
                    "  Writes refined_detect_runs/<latest>/manual and updates manual pointers/status.");
                ImGui::EndDisabled();
                if (!bbox_payload_status.empty()) {
                    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                                       "%s",
                                       bbox_payload_status.c_str());
                }

                if (zarr_loader.hasKeypointData() || zarr_loader.hasHeadingData()) {
                    ImGui::Separator();
                    if (zarr_loader.hasKeypointData()) {
                        ImGui::Text("Keypoint Overlay:");
                        ImGui::Checkbox("Show keypoint markers", &show_keypoint_markers);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Overlay swim bladder and eye keypoints on the video frame.");
                        }
                        if (!zarr_loader.getKeypointsRunName().empty()) {
                            ImGui::Text("  Keypoints run: %s (%s)",
                                        zarr_loader.getKeypointsRunName().c_str(),
                                        zarr_loader.isRefinedKeypoints() ? "refined" : "raw");
                        }
                        if (detection_details.is_refined_keypoints &&
                            !detection_details.keypoint_usable.empty()) {
                            size_t usable_count = 0;
                            size_t flip_count = 0;
                            size_t det_count = detection_details.keypoint_usable.size();
                            for (size_t qi = 0; qi < det_count; ++qi) {
                                if (detection_details.keypoint_usable[qi] != 0) usable_count++;
                                if (qi < detection_details.keypoint_flip_corrected.size() &&
                                    detection_details.keypoint_flip_corrected[qi] != 0) flip_count++;
                            }
                            ImGui::Text("  Quality: %zu/%zu usable (%zu flip-corrected)",
                                        usable_count, det_count, flip_count);
                            if (!detection_details.keypoint_reason.empty() &&
                                !detection_details.keypoint_reason[0].empty()) {
                                ImGui::TextWrapped("  Reason: %s",
                                                   detection_details.keypoint_reason[0].c_str());
                            }
                        }
                        if (detection_details.keypoints_per_detection > 0 &&
                            !detection_details.keypoint_labels.empty()) {
                            std::string label_list;
                            for (size_t i = 0; i < detection_details.keypoint_labels.size(); ++i) {
                                if (i > 0) {
                                    label_list += ", ";
                                }
                                label_list += detection_details.keypoint_labels[i];
                                if (label_list.size() > 72 &&
                                    i + 1 < detection_details.keypoint_labels.size()) {
                                    label_list += "...";
                                    break;
                                }
                            }
                            if (!label_list.empty()) {
                                ImGui::TextWrapped("  Labels: %s", label_list.c_str());
                            }
                        }
                        if (zarr_loader.activeDatasetHasSyntheticDetections()) {
                            ImGui::TextWrapped("Synthetic detections are present; interpolated boxes draw with hollow keypoint markers.");
                        }
                    }
                    if (zarr_loader.hasHeadingData()) {
                        if (zarr_loader.hasKeypointData()) {
                            ImGui::Spacing();
                        }
                        ImGui::Text("Heading Overlay:");
                        ImGui::Checkbox("Show heading arrows", &show_heading_arrows);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Visualize swim bladder headings from the keypoints run.");
                        }
                        if (!zarr_loader.getKeypointsRunName().empty() &&
                            !zarr_loader.hasKeypointData()) {
                            ImGui::Text("  Keypoints run: %s (%s)",
                                        zarr_loader.getKeypointsRunName().c_str(),
                                        zarr_loader.isRefinedKeypoints() ? "refined" : "raw");
                        }
                        if (zarr_loader.activeDatasetHasSyntheticDetections()) {
                            ImGui::TextWrapped("Synthetic detections are present; arrows render only for real boxes.");
                        }
                    }
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "[Zarr] Detections:    Not loaded");
            }

            if (zarr_loaded && zarr_loader.hasInterpolation()) {
                ImGui::Separator();
                ImGui::Text("Interpolation Status:");
                bool current_interpolated = zarr_loader.isFrameInterpolated(current_frame_num);
                ImGui::Text("  Current frame interpolated: %s", current_interpolated ? "Yes" : "No");
                ImGui::Text("  Dataset uses interpolation: %s",
                            zarr_loader.activeDatasetHasSyntheticDetections() ? "Yes" : "No");
                ImGui::Text("  Method: %s", zarr_loader.getInterpolationMethod().c_str());
            }

            if (zarr_loaded && zarr_loader.hasEyeMasks()) {
                ImGui::Separator();
                ImGui::Text("Eye Mask Overlay:");
                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_logged_toggle_disabled) {
                    eyeMaskDebugLog("Eye mask overlay toggle re-enabled; attempting to draw masks.");
                    eye_mask_debug_logged_toggle_disabled = false;
                }
                ImGui::Checkbox("Show refined eye masks", &show_eye_masks);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Visualize refined eye masks as semi-transparent overlays.");
                }
                if (!zarr_loader.getEyeMaskRunName().empty()) {
                    ImGui::Text("  Eye mask run: %s",
                                zarr_loader.getEyeMaskRunName().c_str());
                }
                if (zarr_loader.activeDatasetHasSyntheticDetections()) {
                    ImGui::TextWrapped("Synthetic detections are present; masks are skipped for interpolated boxes.");
                }
            }

            ImGui::End();
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
                            auto draw_keypoint_markers = [&]() {
                                if (!(show_keypoint_markers &&
                                      detection_details.has_keypoints &&
                                      !detection_details.keypoints_pixels.empty() &&
                                      detection_details.keypoints_per_detection > 0)) {
                                    return;
                                }

                                const size_t kp_per_det = detection_details.keypoints_per_detection;
                                std::vector<std::string> lowered_labels(kp_per_det);
                                for (size_t kp_idx = 0; kp_idx < kp_per_det; ++kp_idx) {
                                    if (kp_idx < detection_details.keypoint_labels.size()) {
                                        lowered_labels[kp_idx] = detection_details.keypoint_labels[kp_idx];
                                        std::transform(lowered_labels[kp_idx].begin(),
                                                       lowered_labels[kp_idx].end(),
                                                       lowered_labels[kp_idx].begin(),
                                                       [](unsigned char c) {
                                                           return static_cast<char>(std::tolower(c));
                                                       });
                                    } else {
                                        lowered_labels[kp_idx].clear();
                                    }
                                }

                                auto chooseColor = [&](size_t kp_idx) -> ImVec4 {
                                    const std::string& label = lowered_labels[kp_idx];
                                    if (label.find("swim") != std::string::npos ||
                                        label.find("bladder") != std::string::npos) {
                                        return ImVec4(1.0f, 0.85f, 0.15f, 1.0f);
                                    }
                                    if (label.find("left") != std::string::npos) {
                                        return ImVec4(0.3f, 0.95f, 0.4f, 1.0f);
                                    }
                                    if (label.find("right") != std::string::npos) {
                                        return ImVec4(0.75f, 0.4f, 0.95f, 1.0f);
                                    }
                                    static const ImVec4 fallback_colors[] = {
                                        ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                        ImVec4(0.35f, 0.85f, 0.55f, 1.0f),
                                        ImVec4(0.6f, 0.5f, 0.95f, 1.0f),
                                        ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
                                        ImVec4(0.4f, 0.75f, 0.95f, 1.0f)
                                    };
                                    return fallback_colors[kp_idx % (sizeof(fallback_colors) / sizeof(fallback_colors[0]))];
                                };

                                auto chooseMarker = [&](size_t kp_idx) -> ImPlotMarker {
                                    const std::string& label = lowered_labels[kp_idx];
                                    if (label.find("swim") != std::string::npos ||
                                        label.find("bladder") != std::string::npos) {
                                        return ImPlotMarker_Circle;
                                    }
                                    if (label.find("left") != std::string::npos) {
                                        return ImPlotMarker_Square;
                                    }
                                    if (label.find("right") != std::string::npos) {
                                        return ImPlotMarker_Diamond;
                                    }
                                    static const ImPlotMarker fallback_markers[] = {
                                        ImPlotMarker_Circle,
                                        ImPlotMarker_Square,
                                        ImPlotMarker_Diamond,
                                        ImPlotMarker_Cross,
                                        ImPlotMarker_Plus,
                                        ImPlotMarker_Up,
                                        ImPlotMarker_Down
                                    };
                                    return fallback_markers[kp_idx % (sizeof(fallback_markers) / sizeof(fallback_markers[0]))];
                                };

                                auto chooseSize = [&](size_t kp_idx) -> float {
                                    const std::string& label = lowered_labels[kp_idx];
                                    if (label.find("swim") != std::string::npos ||
                                        label.find("bladder") != std::string::npos) {
                                        return 4.5f;
                                    }
                                    if (label.find("left") != std::string::npos ||
                                        label.find("right") != std::string::npos) {
                                        return 4.5f;
                                    }
                                    return 7.0f;
                                };

                                size_t detection_count = std::min(detection_details.keypoints_pixels.size(),
                                                                  detection_details.boxes.size());
                                const double img_h = static_cast<double>(scene->cameras[j].image_height);
                                // Draw skeleton edges before markers so markers render on top
                                if (!detection_details.skeleton_edges.empty()) {
                                    ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(1.0f, 1.0f, 1.0f, 0.63f));
                                    for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
                                        const auto& keypoints = detection_details.keypoints_pixels[det_idx];
                                        if (keypoints.size() != kp_per_det) continue;
                                        for (const auto& edge : detection_details.skeleton_edges) {
                                            size_t a = edge[0], b = edge[1];
                                            if (a >= kp_per_det || b >= kp_per_det) continue;
                                            float ax = keypoints[a][0], ay = keypoints[a][1];
                                            float bx = keypoints[b][0], by = keypoints[b][1];
                                            if (!std::isfinite(ax) || !std::isfinite(ay) ||
                                                !std::isfinite(bx) || !std::isfinite(by)) continue;
                                            double xs[2] = {static_cast<double>(ax), static_cast<double>(bx)};
                                            double ys[2] = {img_h - static_cast<double>(ay),
                                                            img_h - static_cast<double>(by)};
                                            std::string lbl = "##edge_" + std::to_string(det_idx) + "_" +
                                                              std::to_string(a) + "_" + std::to_string(b);
                                            ImPlot::PlotLine(lbl.c_str(), xs, ys, 2);
                                        }
                                    }
                                    ImPlot::PopStyleColor();
                                }
                                for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
                                    const auto& keypoints = detection_details.keypoints_pixels[det_idx];
                                    if (keypoints.size() != kp_per_det) {
                                        continue;
                                    }
                                    bool detection_is_interp = false;
                                    if (!detection_details.detection_source.empty() &&
                                        det_idx < detection_details.detection_source.size()) {
                                        detection_is_interp = detection_details.detection_source[det_idx] != 0;
                                    }
                                    uint8_t heading_valid_flag = 1;
                                    if (!detection_details.heading_valid.empty() &&
                                        det_idx < detection_details.heading_valid.size()) {
                                        heading_valid_flag = detection_details.heading_valid[det_idx];
                                    }

                                    for (size_t kp_idx = 0; kp_idx < kp_per_det; ++kp_idx) {
                                        const auto& kp = keypoints[kp_idx];
                                        float kp_x = kp[0];
                                        float kp_y = kp[1];
                                        if (!std::isfinite(kp_x) || !std::isfinite(kp_y)) {
                                            continue;
                                        }

                                        double plot_x = static_cast<double>(kp_x);
                                        double plot_y = img_h - static_cast<double>(kp_y);

                                        ImVec4 base_color = chooseColor(kp_idx);
                                        float alpha_scale = 1.0f;
                                        if (heading_valid_flag == 0) {
                                            alpha_scale *= 0.4f;
                                        }
                                        if (detection_is_interp) {
                                            alpha_scale *= 0.65f;
                                        }

                                        // Refined keypoint quality-based dimming
                                        bool kp_unusable = false;
                                        bool kp_flip_corrected = false;
                                        bool kp_det_source_interp = false;
                                        if (detection_details.is_refined_keypoints) {
                                            if (det_idx < detection_details.keypoint_usable.size() &&
                                                detection_details.keypoint_usable[det_idx] == 0) {
                                                alpha_scale *= 0.35f;
                                                kp_unusable = true;
                                            }
                                            if (det_idx < detection_details.keypoint_detection_source.size() &&
                                                detection_details.keypoint_detection_source[det_idx] != 0) {
                                                alpha_scale *= 0.65f;
                                                kp_det_source_interp = true;
                                            }
                                            if (det_idx < detection_details.keypoint_flip_corrected.size() &&
                                                detection_details.keypoint_flip_corrected[det_idx] != 0) {
                                                kp_flip_corrected = true;
                                            }
                                        }

                                        alpha_scale = std::clamp(alpha_scale, 0.25f, 1.0f);

                                        ImVec4 fill_color = base_color;
                                        fill_color.w *= alpha_scale;
                                        ImVec4 outline_color = base_color;
                                        outline_color.w = std::max(alpha_scale, 0.6f);

                                        // Quality-based outline color overrides
                                        if (kp_flip_corrected) {
                                            outline_color = ImVec4(0.0f, 0.9f, 0.9f, outline_color.w);
                                        }
                                        if (kp_unusable) {
                                            outline_color = ImVec4(0.95f, 0.3f, 0.3f, outline_color.w);
                                        }

                                        ImPlot::SetNextMarkerStyle(chooseMarker(kp_idx),
                                                                   chooseSize(kp_idx),
                                                                   fill_color,
                                                                   2.0f,
                                                                   outline_color);
                                        std::string label = "##kp_" + std::to_string(det_idx) + "_" +
                                                            std::to_string(kp_idx);
                                        ImPlot::PlotScatter(label.c_str(), &plot_x, &plot_y, 1);
                                    }
                                }
                            };
                            
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

                            if (g_zarr_bbox_edit_state.selected_frame == current_frame_num &&
                                (g_zarr_bbox_edit_state.selected_box < 0 ||
                                 g_zarr_bbox_edit_state.selected_box >=
                                     static_cast<int>(zarr_boxes.size()))) {
                                g_zarr_bbox_edit_state.clearSelection();
                            }

                            auto clampPlotPointToImage = [&](ImPlotPoint plot_point) -> ImVec2 {
                                const float clamped_x = std::clamp(
                                    static_cast<float>(plot_point.x), 0.0f, image_width_px);
                                const float clamped_y = std::clamp(
                                    image_height_px - static_cast<float>(plot_point.y),
                                    0.0f,
                                    image_height_px);
                                return ImVec2(clamped_x, clamped_y);
                            };

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

                            if (plot_hovered) {
                                if (dataset_allows_bbox_edit &&
                                    ImGui::IsKeyPressed(ImGuiKey_N, false)) {
                                    g_zarr_bbox_edit_state.draw_mode =
                                        !g_zarr_bbox_edit_state.draw_mode;
                                    g_zarr_bbox_edit_state.cancelDraw();
                                    if (g_zarr_bbox_edit_state.draw_mode) {
                                        g_zarr_bbox_edit_state.clearSelection();
                                    }
                                }
                                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                                    if (g_zarr_bbox_edit_state.draw_active) {
                                        g_zarr_bbox_edit_state.cancelDraw();
                                    } else if (g_zarr_bbox_edit_state.draw_mode) {
                                        g_zarr_bbox_edit_state.draw_mode = false;
                                    } else {
                                        g_zarr_bbox_edit_state.clearSelection();
                                    }
                                }
                                if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
                                    ImGui::GetIO().KeyShift) {
                                    g_zarr_bbox_edit_state.clearFrameEdits(current_frame_num);
                                    zarr_boxes = loaded_zarr_boxes;
                                }
                                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                                    deleteSelectedBoxOnCurrentFrame();
                                }
                                if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
                                    if (zarr_boxes.empty()) {
                                        g_zarr_bbox_edit_state.clearSelection();
                                    } else {
                                        if (g_zarr_bbox_edit_state.draw_mode) {
                                            g_zarr_bbox_edit_state.draw_mode = false;
                                            g_zarr_bbox_edit_state.cancelDraw();
                                        }
                                        int next_idx = 0;
                                        const bool reverse_cycle = ImGui::GetIO().KeyShift;
                                        if (g_zarr_bbox_edit_state.selected_frame == current_frame_num &&
                                            g_zarr_bbox_edit_state.selected_box >= 0 &&
                                            g_zarr_bbox_edit_state.selected_box <
                                                static_cast<int>(zarr_boxes.size())) {
                                            const int box_count =
                                                static_cast<int>(zarr_boxes.size());
                                            const int current_idx =
                                                g_zarr_bbox_edit_state.selected_box;
                                            if (reverse_cycle) {
                                                next_idx = (current_idx - 1 + box_count) % box_count;
                                            } else {
                                                next_idx = (current_idx + 1) % box_count;
                                            }
                                        }
                                        g_zarr_bbox_edit_state.selected_frame = current_frame_num;
                                        g_zarr_bbox_edit_state.selected_box = next_idx;
                                        g_zarr_bbox_edit_state.drag_active = false;
                                        g_zarr_bbox_edit_state.drag_mouse_button = -1;
                                    }
                                }
                            }

                            if (g_zarr_bbox_edit_state.draw_mode &&
                                g_zarr_bbox_edit_state.drag_active) {
                                g_zarr_bbox_edit_state.drag_active = false;
                                g_zarr_bbox_edit_state.drag_mouse_button = -1;
                            }

                            if (g_zarr_bbox_edit_state.draw_active) {
                                if (!g_zarr_bbox_edit_state.draw_mode ||
                                    !can_modify_boxes ||
                                    g_zarr_bbox_edit_state.draw_frame != current_frame_num) {
                                    g_zarr_bbox_edit_state.cancelDraw();
                                } else {
                                    ImVec2 current_img = clampPlotPointToImage(
                                        ImPlot::GetPlotMousePos());
                                    g_zarr_bbox_edit_state.draw_current_x = current_img.x;
                                    g_zarr_bbox_edit_state.draw_current_y = current_img.y;
                                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                        const float x_min = std::min(
                                            g_zarr_bbox_edit_state.draw_anchor_x,
                                            g_zarr_bbox_edit_state.draw_current_x);
                                        const float y_min = std::min(
                                            g_zarr_bbox_edit_state.draw_anchor_y,
                                            g_zarr_bbox_edit_state.draw_current_y);
                                        const float width = std::fabs(
                                            g_zarr_bbox_edit_state.draw_current_x -
                                            g_zarr_bbox_edit_state.draw_anchor_x);
                                        const float height = std::fabs(
                                            g_zarr_bbox_edit_state.draw_current_y -
                                            g_zarr_bbox_edit_state.draw_anchor_y);
                                        if (width >= 2.0f && height >= 2.0f) {
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
                                            new_box.payload_frame_id =
                                                static_cast<uint64_t>(
                                                    std::max(0, current_frame_num));
                                            new_box.payload_camera_id = 0;
                                            new_box.box_index_in_payload =
                                                static_cast<uint8_t>(editable_boxes.size());
                                            new_box.x_min = x_min;
                                            new_box.y_min = y_min;
                                            new_box.width = width;
                                            new_box.height = height;
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
                                        g_zarr_bbox_edit_state.cancelDraw();
                                    }
                                }
                            }

                            if (!g_zarr_bbox_edit_state.draw_mode &&
                                !g_zarr_bbox_edit_state.drag_active &&
                                plot_hovered &&
                                can_modify_boxes &&
                                ImGui::GetIO().KeyCtrl &&
                                ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                                g_zarr_bbox_edit_state.selected_frame == current_frame_num &&
                                g_zarr_bbox_edit_state.selected_box >= 0 &&
                                g_zarr_bbox_edit_state.selected_box <
                                    static_cast<int>(zarr_boxes.size())) {
                                const int selected_idx =
                                    g_zarr_bbox_edit_state.selected_box;
                                const LoggedBoundingBox& selected_box =
                                    zarr_boxes[selected_idx];
                                ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
                                if (IsPlotPointInsideZarrBox(selected_box,
                                                             mouse_plot.x,
                                                             mouse_plot.y,
                                                             image_height_px,
                                                             6.0f)) {
                                    g_zarr_bbox_edit_state.drag_active = true;
                                    g_zarr_bbox_edit_state.drag_mouse_button =
                                        ImGuiMouseButton_Left;
                                    g_zarr_bbox_edit_state.drag_offset_x =
                                        static_cast<float>(mouse_plot.x) -
                                        selected_box.x_min;
                                    const float mouse_y_img =
                                        image_height_px -
                                        static_cast<float>(mouse_plot.y);
                                    g_zarr_bbox_edit_state.drag_offset_y =
                                        mouse_y_img - selected_box.y_min;
                                }
                            }

                            if (g_zarr_bbox_edit_state.drag_active) {
                                const int held_button = g_zarr_bbox_edit_state.drag_mouse_button;
                                if (held_button < 0 ||
                                    !ImGui::IsMouseDown(static_cast<ImGuiMouseButton>(held_button)) ||
                                    g_zarr_bbox_edit_state.selected_frame != current_frame_num ||
                                    g_zarr_bbox_edit_state.selected_box < 0) {
                                    g_zarr_bbox_edit_state.drag_active = false;
                                    g_zarr_bbox_edit_state.drag_mouse_button = -1;
                                } else if (can_modify_boxes) {
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
                                        g_zarr_bbox_edit_state.selected_box;
                                    if (selected_idx >= 0 &&
                                        selected_idx <
                                            static_cast<int>(editable_boxes.size())) {
                                        ImPlotPoint mouse_plot =
                                            ImPlot::GetPlotMousePos();
                                        LoggedBoundingBox& moving_box =
                                            editable_boxes[selected_idx];
                                        const float mouse_y_img =
                                            image_height_px -
                                            static_cast<float>(mouse_plot.y);
                                        const float target_x =
                                            static_cast<float>(mouse_plot.x) -
                                            g_zarr_bbox_edit_state.drag_offset_x;
                                        const float target_y =
                                            mouse_y_img -
                                            g_zarr_bbox_edit_state.drag_offset_y;
                                        const float max_x = std::max(
                                            0.0f, image_width_px - moving_box.width);
                                        const float max_y = std::max(
                                            0.0f, image_height_px - moving_box.height);
                                        moving_box.x_min =
                                            std::clamp(target_x, 0.0f, max_x);
                                        moving_box.y_min =
                                            std::clamp(target_y, 0.0f, max_y);
                                        if (selected_idx >= 0 &&
                                            selected_idx < static_cast<int>(manual_flags.size())) {
                                            manual_flags[selected_idx] = 1;
                                        }
                                        g_zarr_bbox_edit_state.dirty_frames.insert(
                                            current_frame_num);
                                        zarr_boxes = editable_boxes;
                                    } else {
                                        g_zarr_bbox_edit_state.clearSelection();
                                    }
                                } else {
                                    g_zarr_bbox_edit_state.drag_active = false;
                                    g_zarr_bbox_edit_state.drag_mouse_button = -1;
                                }
                            }

                            int click_button = -1;
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
                                click_button = ImGuiMouseButton_Left;
                            }
                            if (plot_hovered && click_button >= 0) {
                                if (g_zarr_bbox_edit_state.draw_mode) {
                                    if (click_button == ImGuiMouseButton_Left &&
                                        can_modify_boxes &&
                                        ImGui::GetIO().KeyCtrl) {
                                        ImVec2 start_img = clampPlotPointToImage(
                                            ImPlot::GetPlotMousePos());
                                        g_zarr_bbox_edit_state.draw_active = true;
                                        g_zarr_bbox_edit_state.draw_frame =
                                            current_frame_num;
                                        g_zarr_bbox_edit_state.draw_anchor_x = start_img.x;
                                        g_zarr_bbox_edit_state.draw_anchor_y = start_img.y;
                                        g_zarr_bbox_edit_state.draw_current_x = start_img.x;
                                        g_zarr_bbox_edit_state.draw_current_y = start_img.y;
                                        g_zarr_bbox_edit_state.drag_active = false;
                                        g_zarr_bbox_edit_state.drag_mouse_button = -1;
                                    } else if (!can_modify_boxes) {
                                        g_zarr_bbox_edit_state.cancelDraw();
                                    }
                                } else {
                                    ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
                                    bool started_selected_drag = false;
                                    if (can_modify_boxes &&
                                        g_zarr_bbox_edit_state.selected_frame ==
                                            current_frame_num &&
                                        g_zarr_bbox_edit_state.selected_box >= 0 &&
                                        g_zarr_bbox_edit_state.selected_box <
                                            static_cast<int>(zarr_boxes.size())) {
                                        const int selected_idx =
                                            g_zarr_bbox_edit_state.selected_box;
                                        const LoggedBoundingBox& selected_box =
                                            zarr_boxes[selected_idx];
                                        if (IsPlotPointInsideZarrBox(selected_box,
                                                                     mouse_plot.x,
                                                                     mouse_plot.y,
                                                                     image_height_px,
                                                                     6.0f)) {
                                            g_zarr_bbox_edit_state.selected_frame =
                                                current_frame_num;
                                            g_zarr_bbox_edit_state.selected_box =
                                                selected_idx;
                                            g_zarr_bbox_edit_state.drag_active = true;
                                            g_zarr_bbox_edit_state.drag_mouse_button =
                                                click_button;
                                            g_zarr_bbox_edit_state.drag_offset_x =
                                                static_cast<float>(mouse_plot.x) -
                                                selected_box.x_min;
                                            const float mouse_y_img =
                                                image_height_px -
                                                static_cast<float>(mouse_plot.y);
                                            g_zarr_bbox_edit_state.drag_offset_y =
                                                mouse_y_img - selected_box.y_min;
                                            started_selected_drag = true;
                                        }
                                    }

                                    if (!started_selected_drag) {
                                        int hit_idx = HitTestZarrBoxAtPlotPoint(
                                            zarr_boxes,
                                            mouse_plot.x,
                                            mouse_plot.y,
                                            image_height_px,
                                            6.0f);
                                        if (hit_idx >= 0 &&
                                            hit_idx <
                                                static_cast<int>(zarr_boxes.size())) {
                                            g_zarr_bbox_edit_state.selected_frame =
                                                current_frame_num;
                                            g_zarr_bbox_edit_state.selected_box =
                                                hit_idx;
                                            if (can_modify_boxes) {
                                                g_zarr_bbox_edit_state.drag_active =
                                                    true;
                                                g_zarr_bbox_edit_state
                                                    .drag_mouse_button =
                                                    click_button;
                                                const LoggedBoundingBox&
                                                    selected_box =
                                                        zarr_boxes[hit_idx];
                                                g_zarr_bbox_edit_state
                                                    .drag_offset_x =
                                                    static_cast<float>(mouse_plot.x) -
                                                    selected_box.x_min;
                                                const float mouse_y_img =
                                                    image_height_px -
                                                    static_cast<float>(
                                                        mouse_plot.y);
                                                g_zarr_bbox_edit_state
                                                    .drag_offset_y =
                                                    mouse_y_img -
                                                    selected_box.y_min;
                                            } else {
                                                g_zarr_bbox_edit_state
                                                    .drag_active = false;
                                                g_zarr_bbox_edit_state
                                                    .drag_mouse_button = -1;
                                            }
                                        } else {
                                            g_zarr_bbox_edit_state
                                                .clearSelection();
                                        }
                                    }
                                }
                            }

                            const bool frame_has_bbox_edits =
                                g_zarr_bbox_edit_state.isFrameDirty(current_frame_num);

                            // Draw the boxes
                            if (!zarr_boxes.empty()) {
                                enum class BoxProvenance {
                                    Clean = 0,
                                    Interpolated = 1,
                                    Manual = 2
                                };
                                auto classify_box_provenance =
                                    [&](size_t box_idx) -> BoxProvenance {
                                    if (!detection_details.detection_reason.empty() &&
                                        box_idx < detection_details.detection_reason.size()) {
                                        std::string reason = ToLowerCopy(
                                            detection_details.detection_reason[box_idx]);
                                        if (reason == "manual" ||
                                            reason.find("manual") != std::string::npos) {
                                            return BoxProvenance::Manual;
                                        }
                                        if (reason == "interpolated" ||
                                            reason.find("interp") != std::string::npos) {
                                            return BoxProvenance::Interpolated;
                                        }
                                        if (reason == "clean") {
                                            return BoxProvenance::Clean;
                                        }
                                    }

                                    bool detection_is_interp =
                                        zarr_loader.activeDatasetHasSyntheticDetections();
                                    if (!detection_details.detection_source.empty()) {
                                        if (box_idx < detection_details.detection_source.size()) {
                                            detection_is_interp =
                                                detection_details.detection_source[box_idx] != 0;
                                        } else {
                                            detection_is_interp = false;
                                        }
                                    }
                                    if (is_zarr_interpolated &&
                                        zarr_loader.activeDatasetHasSyntheticDetections() &&
                                        detection_details.detection_source.empty()) {
                                        detection_is_interp = true;
                                    }
                                    return detection_is_interp ? BoxProvenance::Interpolated
                                                               : BoxProvenance::Clean;
                                };

                                for (size_t box_idx = 0; box_idx < zarr_boxes.size(); ++box_idx) {
                                    const auto& box = zarr_boxes[box_idx];
                                    double x_coords[5] = {
                                        box.x_min, 
                                        box.x_min + box.width, 
                                        box.x_min + box.width, 
                                        box.x_min, 
                                        box.x_min
                                    };
                                    
                                    double y_coords[5] = {
                                        (double)scene->cameras[j].image_height - box.y_min,
                                        (double)scene->cameras[j].image_height - box.y_min,
                                        (double)scene->cameras[j].image_height - (box.y_min + box.height),
                                        (double)scene->cameras[j].image_height - (box.y_min + box.height),
                                        (double)scene->cameras[j].image_height - box.y_min
                                    };

                                    BoxProvenance box_provenance =
                                        classify_box_provenance(box_idx);
                                    ImVec4 box_color = ImVec4(0.2f, 0.6f, 1.0f, 1.0f);  // clean
                                    float line_width = 2.0f;
                                    if (box_provenance == BoxProvenance::Interpolated) {
                                        box_color = ImVec4(1.0f, 0.7f, 0.0f, 0.9f);
                                        line_width = 2.5f;
                                    } else if (box_provenance == BoxProvenance::Manual) {
                                        box_color = ImVec4(0.0f, 0.85f, 0.65f, 1.0f);
                                        line_width = 2.75f;
                                    }

                                    const bool box_selected =
                                        (g_zarr_bbox_edit_state.selected_frame ==
                                             current_frame_num) &&
                                        (g_zarr_bbox_edit_state.selected_box ==
                                         static_cast<int>(box_idx));
                                    const bool box_is_added =
                                        g_zarr_bbox_edit_state.isAddedBox(
                                            current_frame_num,
                                            static_cast<int>(box_idx));
                                    const bool box_is_manual =
                                        g_zarr_bbox_edit_state.isManualBox(
                                            current_frame_num,
                                            static_cast<int>(box_idx));
                                    if (box_is_manual) {
                                        box_provenance = BoxProvenance::Manual;
                                    }
                                    if (box_selected) {
                                        box_color = ImVec4(1.0f, 0.25f, 0.95f, 1.0f);
                                        line_width = 3.5f;
                                    } else if (box_is_added) {
                                        box_color = ImVec4(0.95f, 0.35f, 0.15f, 1.0f);
                                        line_width = std::max(line_width, 3.0f);
                                    } else if (frame_has_bbox_edits) {
                                        line_width = std::max(line_width, 2.5f);
                                    }
                                    
                                    ImPlot::SetNextLineStyle(box_color, line_width);
                                    
                                    std::string label = "Zarr_" + std::to_string(box.class_id);
                                    if (box_provenance == BoxProvenance::Interpolated) {
                                        label += " [I]";
                                    } else if (box_provenance == BoxProvenance::Manual) {
                                        label += " [MAN]";
                                    }
                                    if (box_is_added) {
                                        label += " [A]";
                                    }
                                    if (box_selected) {
                                        label += " [S]";
                                    } else if (frame_has_bbox_edits) {
                                        label += " [M]";
                                    }
                                    
                                    ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);
                                }
                            }

                            if (g_zarr_bbox_edit_state.draw_active &&
                                g_zarr_bbox_edit_state.draw_frame == current_frame_num) {
                                const double draft_x0 = std::min(
                                    g_zarr_bbox_edit_state.draw_anchor_x,
                                    g_zarr_bbox_edit_state.draw_current_x);
                                const double draft_x1 = std::max(
                                    g_zarr_bbox_edit_state.draw_anchor_x,
                                    g_zarr_bbox_edit_state.draw_current_x);
                                const double draft_y0 = std::min(
                                    g_zarr_bbox_edit_state.draw_anchor_y,
                                    g_zarr_bbox_edit_state.draw_current_y);
                                const double draft_y1 = std::max(
                                    g_zarr_bbox_edit_state.draw_anchor_y,
                                    g_zarr_bbox_edit_state.draw_current_y);
                                if ((draft_x1 - draft_x0) >= 1.0 &&
                                    (draft_y1 - draft_y0) >= 1.0) {
                                    double x_coords[5] = {
                                        draft_x0,
                                        draft_x1,
                                        draft_x1,
                                        draft_x0,
                                        draft_x0
                                    };
                                    double y_coords[5] = {
                                        static_cast<double>(scene->cameras[j].image_height) - draft_y0,
                                        static_cast<double>(scene->cameras[j].image_height) - draft_y0,
                                        static_cast<double>(scene->cameras[j].image_height) - draft_y1,
                                        static_cast<double>(scene->cameras[j].image_height) - draft_y1,
                                        static_cast<double>(scene->cameras[j].image_height) - draft_y0
                                    };
                                    ImPlot::SetNextLineStyle(
                                        ImVec4(1.0f, 1.0f, 0.2f, 0.95f), 2.0f);
                                    std::string draft_label =
                                        "ZarrDrawDraft##" + std::to_string(j);
                                    ImPlot::PlotLine(
                                        draft_label.c_str(), x_coords, y_coords, 5);
                                }
                            }

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

struct StateOverlay {
                                   int chaser_index = -1;
                                   double target_plot_x = 0.0;
                                   double target_plot_y = 0.0;
                                   double target_world_x = std::numeric_limits<double>::quiet_NaN();
                                   double target_world_y = std::numeric_limits<double>::quiet_NaN();
                                   double chaser_plot_x = 0.0;
                                   double chaser_plot_y = 0.0;
                                   double chaser_world_x = std::numeric_limits<double>::quiet_NaN();
                                   double chaser_world_y = std::numeric_limits<double>::quiet_NaN();
                                   size_t target_bbox_index = std::numeric_limits<size_t>::max();
                                   bool has_target = false;
                                   bool has_chaser = false;
                               };

                                std::vector<uint8_t> target_bbox_usage(chaser_bboxes.size(), 0);
                                std::vector<StateOverlay> state_overlays;
                                state_overlays.reserve(chaser_states.size());

                                const size_t kInvalidBBoxIndex = std::numeric_limits<size_t>::max();

                                auto selectBoundingBoxForTarget = [&](double cam_x, double cam_y) -> size_t {
                                    auto choose = [&](auto predicate) -> size_t {
                                        double best_distance = std::numeric_limits<double>::infinity();
                                        size_t best_index = kInvalidBBoxIndex;
                                        for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
                                            const auto& box = chaser_bboxes[idx];
                                            if (!predicate(box)) {
                                                continue;
                                            }
                                            if (!std::isfinite(box.centroid_x) || !std::isfinite(box.centroid_y)) {
                                                continue;
                                            }
                                            double dx = cam_x - static_cast<double>(box.centroid_x);
                                            double dy = cam_y - static_cast<double>(box.centroid_y);
                                            double dist_sq = dx * dx + dy * dy;
                                            if (dist_sq < best_distance) {
                                                best_distance = dist_sq;
                                                best_index = idx;
                                            }
                                        }
                                        return best_index;
                                    };

                                    size_t idx = choose([](const auto& box) { return box.is_target; });
                                    if (idx != kInvalidBBoxIndex) {
                                        return idx;
                                    }
                                    idx = choose([](const auto& box) { return box.fish_id < 0; });
                                    if (idx != kInvalidBBoxIndex) {
                                        return idx;
                                    }
                                    return choose([](const auto&) { return true; });
                                };

                                if (!chaser_states.empty()) {
                                    auto projectStimulusToCamera = [&](const ZarrDetectionLoader::ChaserState& state,
                                                                       float stim_x, float stim_y,
                                                                       bool is_target,
                                                                       double& out_x, double& out_y) -> bool {
                                        if (state.has_camera_coords) {
                                            double cx = is_target ? state.target_camera_x : state.chaser_camera_x;
                                            double cy = is_target ? state.target_camera_y : state.chaser_camera_y;
                                            if (!std::isfinite(cx) || !std::isfinite(cy)) {
                                                return false;
                                            }
                                            out_x = cx;
                                            out_y = static_cast<double>(scene->cameras[j].image_height) - cy;
                                            return true;
                                        }

                                        if (!std::isfinite(stim_x) || !std::isfinite(stim_y)) {
                                            return false;
                                        }

                                        float offsetX = camera_params[j].stimulus_offset_x;
                                        float offsetY = camera_params[j].stimulus_offset_y;

                                        if (camera_params[j].has_valid_homography) {
                                            std::vector<cv::Point2f> src_points(1);
                                            std::vector<cv::Point2f> dst_points;
                                            src_points[0] = cv::Point2f(stim_x + offsetX, stim_y + offsetY);
                                            cv::perspectiveTransform(src_points, dst_points, camera_params[j].inverse_homography_matrix);
                                            if (dst_points.empty()) {
                                                return false;
                                            }
                                            out_x = dst_points[0].x;
                                            out_y = static_cast<double>(scene->cameras[j].image_height) - dst_points[0].y;
                                            return true;
                                        }

                                        constexpr double kProjectorExtent = 358.0;
                                        double projector_w = kProjectorExtent;
                                        double projector_h = kProjectorExtent;
                                        double tex_x = stim_x + offsetX;
                                        double tex_y = stim_y + offsetY;
                                        out_x = (tex_x / projector_w) * static_cast<double>(scene->cameras[j].image_width);
                                        double y = (tex_y / projector_h) * static_cast<double>(scene->cameras[j].image_height);
                                        out_y = static_cast<double>(scene->cameras[j].image_height) - y;
                                        return true;
                                    };

                                    for (const auto& state : chaser_states) {
                                        StateOverlay overlay;
                                        overlay.chaser_index = state.chaser_index;

                                        double target_plot_x = 0.0;
                                        double target_plot_y = 0.0;
                                        if (projectStimulusToCamera(state,
                                                                    state.target_pos_x,
                                                                    state.target_pos_y,
                                                                    true,
                                                                    target_plot_x,
                                                                    target_plot_y)) {
                                            overlay.has_target = true;
                                            double target_cam_x = target_plot_x;
                                            double target_cam_y = static_cast<double>(scene->cameras[j].image_height) - target_plot_y;
                                            size_t bbox_idx = chaser_bboxes.empty()
                                                                 ? kInvalidBBoxIndex
                                                                 : selectBoundingBoxForTarget(target_cam_x, target_cam_y);
                                            if (bbox_idx != kInvalidBBoxIndex) {
                                                auto& bbox = chaser_bboxes[bbox_idx];
                                                overlay.target_plot_x = static_cast<double>(bbox.centroid_x);
                                                overlay.target_plot_y =
                                                    static_cast<double>(scene->cameras[j].image_height) - static_cast<double>(bbox.centroid_y);
                                                overlay.target_bbox_index = bbox_idx;
                                                target_bbox_usage[bbox_idx] = 1;
                                                bbox.is_target = true;
                                                if (bbox.chaser_index < 0 && state.chaser_index >= 0) {
                                                    bbox.chaser_index = state.chaser_index;
                                                }
                                                overlay.target_world_x = bbox.centroid_x;
                                                overlay.target_world_y = static_cast<double>(scene->cameras[j].image_height) - bbox.centroid_y;
                                            } else {
                                                overlay.target_plot_x = target_plot_x;
                                                overlay.target_plot_y = target_plot_y;
                                                overlay.target_world_x = target_plot_x;
                                                overlay.target_world_y = target_plot_y;
                                            }
                                        }

                                        double chaser_plot_x = 0.0;
                                        double chaser_plot_y = 0.0;
                                        if (projectStimulusToCamera(state,
                                                                    state.chaser_pos_x,
                                                                    state.chaser_pos_y,
                                                                    false,
                                                                    chaser_plot_x,
                                                                    chaser_plot_y)) {
                                            overlay.has_chaser = true;
                                            overlay.chaser_plot_x = chaser_plot_x;
                                            overlay.chaser_plot_y = chaser_plot_y;
                                            overlay.chaser_world_x = chaser_plot_x;
                                            overlay.chaser_world_y = chaser_plot_y;
                                        }

                                        state_overlays.push_back(overlay);
                                    }
                                }

                                for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
                                    const auto& bbox = chaser_bboxes[idx];
                                    if (!std::isfinite(bbox.x_px) || !std::isfinite(bbox.y_px) ||
                                        !std::isfinite(bbox.width_px) || !std::isfinite(bbox.height_px)) {
                                        continue;
                                    }

                                    bool highlight_target =
                                        bbox.is_target ||
                                        (idx < target_bbox_usage.size() && target_bbox_usage[idx] != 0);

                                    ImVec4 box_color = highlight_target
                                                           ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                                                           : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                                    float line_width = highlight_target ? 2.75f : 2.0f;

                                    double x0 = static_cast<double>(bbox.x_px);
                                    double x1 = static_cast<double>(bbox.x_px + bbox.width_px);
                                    double y0 = static_cast<double>(bbox.y_px);
                                    double y1 = static_cast<double>(bbox.y_px + bbox.height_px);

                                    double x_coords[5] = {x0, x1, x1, x0, x0};
                                    double y_coords[5] = {
                                        static_cast<double>(scene->cameras[j].image_height) - y0,
                                        static_cast<double>(scene->cameras[j].image_height) - y0,
                                        static_cast<double>(scene->cameras[j].image_height) - y1,
                                        static_cast<double>(scene->cameras[j].image_height) - y1,
                                        static_cast<double>(scene->cameras[j].image_height) - y0};

                                    ImPlot::SetNextLineStyle(box_color, line_width);
                                    auto format_label_id = [&](int32_t candidate, size_t fallback) -> int32_t {
                                        if (candidate >= 0) {
                                            return candidate;
                                        }
                                        if (bbox.fish_id >= 0) {
                                            return bbox.fish_id;
                                        }
                                        return static_cast<int32_t>(fallback);
                                    };
                                    int32_t label_id = format_label_id(bbox.chaser_index, idx);
                                    std::string label = highlight_target
                                                            ? "Target BBox " + std::to_string(label_id) +
                                                                  "##target_bbox_" + std::to_string(idx)
                                                            : "Chaser BBox " + std::to_string(label_id) +
                                                                  "##chaser_bbox_" + std::to_string(idx);
                                    ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);

                                    if (!highlight_target) {
                                        if (std::isfinite(bbox.centroid_x) && std::isfinite(bbox.centroid_y)) {
                                            double centroid_x = static_cast<double>(bbox.centroid_x);
                                            double centroid_y =
                                                static_cast<double>(scene->cameras[j].image_height) - static_cast<double>(bbox.centroid_y);
                                            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                                                       5.0f,
                                                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                                                       IMPLOT_AUTO,
                                                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
                                            std::string centroid_label = highlight_target
                                                                            ? "Target BBox " + std::to_string(label_id) + " Centroid##target_centroid_" + std::to_string(idx)
                                                                            : "Chaser BBox " + std::to_string(label_id) + " Centroid##chaser_centroid_" + std::to_string(idx);
                                            ImPlot::PlotScatter(centroid_label.c_str(), &centroid_x, &centroid_y, 1);
                                        }
                                    }
                                }

                                for (const auto& overlay : state_overlays) {
                                    if (overlay.has_target) {
                                        double plot_x = overlay.target_plot_x;
                                        double plot_y = overlay.target_plot_y;
                                        ImVec4 target_color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                                        float outline = overlay.target_bbox_index != kInvalidBBoxIndex ? 2.5f : 2.0f;
                                        float target_marker_size = 3.0f;
                                        float target_outline = outline * 0.25f;
                                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                                                   target_marker_size,
                                                                   target_color,
                                                                   target_outline,
                                                                   target_color);
                                        std::string target_label = "Target_" + std::to_string(overlay.chaser_index);
                                        ImPlot::PlotScatter(target_label.c_str(), &plot_x, &plot_y, 1);
                                    }

                                    if (overlay.has_chaser && overlay.has_target) {
                                        double line_x[2] = {overlay.chaser_plot_x, overlay.target_plot_x};
                                        double line_y[2] = {overlay.chaser_plot_y, overlay.target_plot_y};
                                        double dx = overlay.target_plot_x - overlay.chaser_plot_x;
                                        double dy = overlay.target_plot_y - overlay.chaser_plot_y;
                                        double dist = std::sqrt(dx * dx + dy * dy);
                                        double max_dim = static_cast<double>(std::max(scene->cameras[j].image_width, scene->cameras[j].image_height));
                                        double max_dist = (max_dim > 0.0) ? (max_dim * (2.0 / 3.0)) : 200.0;
                                        double t = std::clamp(dist / max_dist, 0.0, 1.0);
                                        ImVec4 close_color(1.0f, 0.15f, 0.1f, 1.0f);
                                        ImVec4 far_color(0.15f, 0.9f, 0.2f, 1.0f);
                                        ImVec4 line_color(
                                            close_color.x * static_cast<float>(1.0 - t) + far_color.x * static_cast<float>(t),
                                            close_color.y * static_cast<float>(1.0 - t) + far_color.y * static_cast<float>(t),
                                            close_color.z * static_cast<float>(1.0 - t) + far_color.z * static_cast<float>(t),
                                            1.0f);
                                        ImPlot::SetNextLineStyle(line_color, 2.5f);
                                        std::string line_label = "ChaserTargetLine##" +
                                                                 std::to_string(reinterpret_cast<uintptr_t>(&overlay));
                                        ImPlot::PlotLine(line_label.c_str(), line_x, line_y, 2);
                                    }

                                    if (overlay.has_chaser) {
                                        double plot_x = overlay.chaser_plot_x;
                                        double plot_y = overlay.chaser_plot_y;
                                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                                                   8.0f,
                                                                   ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                                                   2.0f,
                                                                   ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
                                        std::string chaser_label = "Chaser_state_" + std::to_string(overlay.chaser_index);
                                        ImPlot::PlotScatter(chaser_label.c_str(), &plot_x, &plot_y, 1);
                                    }
                                }

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
                                if (kHeadingDebugLoggingEnabled &&
                                    heading_debug_entry_log_count < 200 &&
                                    heading_debug_last_frame_logged != current_frame_num) {
                                    headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                    ": entering heading draw path (overlay_enabled=" +
                                                    (heading_overlay_enabled ? "1" : "0") +
                                                    ", heading_data=" +
                                                    (heading_data_available ? "1" : "0") +
                                                    ", dataset_interp=" +
                                                    (zarr_loader.activeDatasetHasSyntheticDetections() ? "1" : "0") + ").");
                                    heading_debug_last_frame_logged = current_frame_num;
                                    heading_debug_entry_log_count++;
                                }

                                ZarrDetectionLoader::FrameDetections heading_details =
                                    zarr_loader.getRawDetections(
                                        current_frame_num,
                                        /*use_interpolated=*/false,
                                        /*include_eye_masks=*/false);

                                const size_t det_count = heading_details.boxes.size();
                                const size_t valid_count = heading_details.heading_valid.size();
                                const size_t heading_count = heading_details.headings_deg.size();
                                const size_t swim_bladder_count = heading_details.swim_bladder_pixels.size();

                                if (det_count > 0 &&
                                    valid_count == det_count &&
                                    heading_count == det_count) {
                                    if (kHeadingDebugLoggingEnabled) {
                                        headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                        ": processing " + std::to_string(det_count) +
                                                        " detections (heading_valid=" + std::to_string(valid_count) +
                                                        ", headings_deg=" + std::to_string(heading_count) +
                                                        ", swim_bladder=" + std::to_string(swim_bladder_count) + ").");
                                    }

                                    ImDrawList* plot_draw_list = ImPlot::GetPlotDrawList();
                                    const ImU32 arrow_color =
                                        ImGui::GetColorU32(ImVec4(1.0f, 0.25f, 0.1f, 0.95f));
                                    const float arrow_thickness = 2.0f;

                                    for (size_t det_idx = 0; det_idx < det_count; ++det_idx) {
                                        if (heading_details.heading_valid[det_idx] == 0) {
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " skipped (heading_valid == 0).");
                                                heading_debug_draw_log_count++;
                                            }
                                            continue;
                                        }

                                        if (!heading_details.detection_source.empty() &&
                                            det_idx < heading_details.detection_source.size() &&
                                            heading_details.detection_source[det_idx] != 0) {
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " skipped (synthetic detection). ");
                                                heading_debug_draw_log_count++;
                                            }
                                            continue;
                                        }

                                        const auto& box = heading_details.boxes[det_idx];
                                        if (det_idx >= heading_details.swim_bladder_pixels.size()) {
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " lacks swim bladder pixel data (available=" +
                                                                std::to_string(swim_bladder_count) + ").");
                                                heading_debug_draw_log_count++;
                                            }
                                            continue;
                                        }

                                        float box_width = std::max(0.0f, box[2] - box[0]);
                                        float box_height = std::max(0.0f, box[3] - box[1]);
                                        float base_x = heading_details.swim_bladder_pixels[det_idx][0];
                                        float base_y = heading_details.swim_bladder_pixels[det_idx][1];

                                        if (!std::isfinite(base_x) || !std::isfinite(base_y)) {
                                            float fallback_x = 0.5f * (box[0] + box[2]);
                                            float fallback_y = 0.5f * (box[1] + box[3]);
                                            if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 120) {
                                                headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                ": detection " + std::to_string(det_idx) +
                                                                " had invalid swim bladder coords (" +
                                                                std::to_string(base_x) + ", " +
                                                                std::to_string(base_y) +
                                                                "); using bounding box center (" +
                                                                std::to_string(fallback_x) + ", " +
                                                                std::to_string(fallback_y) + ").");
                                                heading_debug_draw_log_count++;
                                            }
                                            base_x = fallback_x;
                                            base_y = fallback_y;
                                        }

                                        if (kHeadingDebugLoggingEnabled &&
                                            heading_debug_draw_log_count < 120) {
                                            headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                            ": detection " + std::to_string(det_idx) +
                                                            " bbox=[" + std::to_string(box[0]) + ", " +
                                                            std::to_string(box[1]) + ", " +
                                                            std::to_string(box[2]) + ", " +
                                                            std::to_string(box[3]) + "] base=(" +
                                                            std::to_string(base_x) + ", " +
                                                            std::to_string(base_y) + ").");
                                            heading_debug_draw_log_count++;
                                        }

                                        float heading_deg = heading_details.headings_deg[det_idx];
                                        float heading_rad =
                                            heading_deg * static_cast<float>(M_PI) / 180.0f;

                                        float bbox_scale =
                                            std::max(box_width, box_height) * 1.25f;
                                        float frame_scale = scene_height_f * 0.02f;
                                        float arrow_len =
                                            std::max(60.0f, std::max(bbox_scale, frame_scale));
                                        float end_x = base_x + std::cos(heading_rad) * arrow_len;
                                        float end_y = base_y - std::sin(heading_rad) * arrow_len;

                                        if (kHeadingDebugLoggingEnabled && heading_debug_draw_log_count < 80) {
                                            headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                            ": drawing heading arrow det " +
                                                            std::to_string(det_idx) + " base=(" +
                                                            std::to_string(base_x) + ", " +
                                                            std::to_string(base_y) + ") heading_deg=" +
                                                            std::to_string(heading_deg) + " arrow_len=" +
                                                            std::to_string(arrow_len) + " end=(" +
                                                            std::to_string(end_x) + ", " +
                                                            std::to_string(end_y) + ").");
                                            heading_debug_draw_log_count++;
                                        }

                                        ImPlotPoint plot_start(base_x, scene_height_f - base_y);
                                        ImPlotPoint plot_end(end_x, scene_height_f - end_y);
                                        ImVec2 p0 = ImPlot::PlotToPixels(plot_start);
                                        ImVec2 p1 = ImPlot::PlotToPixels(plot_end);

                                        plot_draw_list->AddLine(p0, p1, arrow_color, arrow_thickness);

                                        ImVec2 dir = ImVec2(p0.x - p1.x, p0.y - p1.y);
                                        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                                        if (len > 1e-3f) {
                                            dir.x /= len;
                                            dir.y /= len;
                                            float head_size = 8.0f;
                                            ImVec2 left = ImVec2(
                                                p1.x + dir.x * head_size + dir.y * head_size * 0.5f,
                                                p1.y + dir.y * head_size - dir.x * head_size * 0.5f);
                                            ImVec2 right = ImVec2(
                                                p1.x + dir.x * head_size - dir.y * head_size * 0.5f,
                                                p1.y + dir.y * head_size + dir.x * head_size * 0.5f);
                                            plot_draw_list->AddTriangleFilled(p1, left, right, arrow_color);
                                        }
                                    }
                                } else if (kHeadingDebugLoggingEnabled && can_draw_headings) {
                                    headingDebugLog("Frame " + std::to_string(current_frame_num) +
                                                    ": heading data mismatch (boxes=" + std::to_string(det_count) +
                                                    ", heading_valid=" + std::to_string(valid_count) +
                                                    ", headings_deg=" + std::to_string(heading_count) +
                                                    ", swim_bladder=" + std::to_string(swim_bladder_count) + ").");
                                }
                            }

                            if (can_draw_eye_masks) {
                                g_eye_orientation_smoother.resetIfRunChanged(
                                    zarr_loader.getEyeMaskRunName() + "|" +
                                    zarr_loader.getEyeAngleRunName());
                                ZarrDetectionLoader::FrameDetections mask_details =
                                    zarr_loader.getRawDetections(
                                        current_frame_num,
                                        /*use_interpolated=*/false,
                                        /*include_eye_masks=*/true);
                                if (mask_details.includes_eye_masks) {
                                    size_t mask_count =
                                        std::min(mask_details.eye_masks.size(),
                                                 mask_details.boxes.size());
                                    if (mask_count > 0) {
                                        if (kEyeMaskDebugLoggingEnabled &&
                                            eye_mask_debug_entry_log_count < 200 &&
                                            eye_mask_debug_last_frame_logged != current_frame_num) {
                                            eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                            ": entering eye mask draw path (masks=" +
                                                            std::to_string(mask_count) + ").");
                                            eye_mask_debug_last_frame_logged = current_frame_num;
                                            eye_mask_debug_entry_log_count++;
                                        }
                                        for (size_t det_idx = 0; det_idx < mask_count; ++det_idx) {
                                            const auto& mask_info = mask_details.eye_masks[det_idx];
                                            if (!mask_details.detection_source.empty() &&
                                                det_idx < mask_details.detection_source.size() &&
                                                mask_details.detection_source[det_idx] != 0) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " marked synthetic; skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (!mask_info.valid) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " marked invalid; skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (!std::isfinite(mask_info.offset_x) ||
                                                !std::isfinite(mask_info.offset_y)) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has non-finite offsets; skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (mask_info.roi_width <= 0.0f || mask_info.roi_height <= 0.0f) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has invalid ROI size (" +
                                                                    std::to_string(mask_info.roi_width) + "x" +
                                                                    std::to_string(mask_info.roi_height) + "); skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (mask_info.rows <= 0 || mask_info.cols <= 0) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has invalid matrix dimensions (" +
                                                                    std::to_string(mask_info.rows) + "x" +
                                                                    std::to_string(mask_info.cols) + "); skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            if (mask_info.rows <= 0 || mask_info.cols <= 0) {
                                                if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": mask entry " + std::to_string(det_idx) +
                                                                    " has invalid matrix dimensions (" +
                                                                    std::to_string(mask_info.rows) + "x" +
                                                                    std::to_string(mask_info.cols) + "); skipping.");
                                                    eye_mask_debug_draw_log_count++;
                                                }
                                                continue;
                                            }
                                            double cell_w = mask_info.roi_width / static_cast<double>(mask_info.cols);
                                            double cell_h = mask_info.roi_height / static_cast<double>(mask_info.rows);
                                            for (int eye = 0; eye < 2; ++eye) {
                                                const auto& pixel_indices = mask_info.pixel_indices[eye];
                                                bool has_pixels = !pixel_indices.empty();
                                                if (!has_pixels) {
                                                    if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                        eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                        ": mask entry " + std::to_string(det_idx) +
                                                                        " eye " + std::to_string(eye) +
                                                                        " has no non-zero pixels.");
                                                        eye_mask_debug_draw_log_count++;
                                                    }
                                                } else if (kEyeMaskDebugLoggingEnabled && eye_mask_debug_draw_log_count < 80) {
                                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                                    ": drawing eye mask det " + std::to_string(det_idx) +
                                                                    " eye=" + std::to_string(eye) +
                                                                    " offset=(" + std::to_string(mask_info.offset_x) + ", " +
                                                                    std::to_string(mask_info.offset_y) + ") size=(" +
                                                                    std::to_string(mask_info.roi_width) + ", " +
                                                                    std::to_string(mask_info.roi_height) + ").");
                                                    eye_mask_debug_draw_log_count++;
                                                }

                                                std::string base_id = (eye == 0)
                                                                          ? "##eye_mask_left_" + std::to_string(det_idx)
                                                                          : "##eye_mask_right_" + std::to_string(det_idx);
                                                ImVec4 base_color = (eye == 0)
                                                                        ? ImVec4(0.2f, 0.6f, 1.0f, 0.35f)
                                                                        : ImVec4(1.0f, 0.3f, 0.6f, 0.35f);

                                                std::vector<double> xs;
                                                std::vector<double> ys;
                                                if (has_pixels) {
                                                    xs.reserve(pixel_indices.size());
                                                    ys.reserve(pixel_indices.size());
                                                    for (uint16_t linear : pixel_indices) {
                                                        uint16_t row = linear / static_cast<uint16_t>(mask_info.cols);
                                                        uint16_t col = linear % static_cast<uint16_t>(mask_info.cols);
                                                        double px = mask_info.offset_x +
                                                                    (static_cast<double>(col) + 0.5) * cell_w;
                                                        double py = mask_info.offset_y +
                                                                    (static_cast<double>(row) + 0.5) * cell_h;
                                                        xs.push_back(px);
                                                        ys.push_back(scene_height_f - py);
                                                    }
                                                    if (!xs.empty()) {
                                                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, base_color, 1.0f,
                                                                                    base_color);
                                                        ImPlot::PlotScatter((base_id + "_pts").c_str(), xs.data(), ys.data(),
                                                                            static_cast<int>(xs.size()));
                                                    }
                                                }

                                                if (mask_info.has_feret_axes && cell_w > 0.0 && cell_h > 0.0) {
                                                    auto roiToWorld = [&](float roi_x, float roi_y) -> std::pair<double, double> {
                                                        double px = mask_info.offset_x +
                                                                    static_cast<double>(roi_x) * cell_w;
                                                        double py = mask_info.offset_y +
                                                                    static_cast<double>(roi_y) * cell_h;
                                                        return {px, py};
                                                    };
                                                    auto worldToScene = [&](double world_x, double world_y) -> std::pair<double, double> {
                                                        return {world_x, scene_height_f - world_y};
                                                    };
                                                    auto roiToScene = [&](float roi_x, float roi_y) -> std::pair<double, double> {
                                                        auto world = roiToWorld(roi_x, roi_y);
                                                        return worldToScene(world.first, world.second);
                                                    };
                                                    auto draw_axis =
                                                        [&](const ZarrDetectionLoader::FrameDetections::EyeMask::AxisSegment& axis,
                                                            const std::string& label, const ImVec4& color, float thickness) {
                                                            if (!axis.valid) {
                                                                return;
                                                            }
                                                            auto p0 = roiToScene(axis.x0, axis.y0);
                                                            auto p1 = roiToScene(axis.x1, axis.y1);
                                                            double x_vals[2] = {p0.first, p1.first};
                                                            double y_vals[2] = {p0.second, p1.second};
                                                            ImPlot::SetNextLineStyle(color, thickness);
                                                            ImPlot::PlotLine(label.c_str(), x_vals, y_vals, 2);
                                                        };

                                                    ImVec4 major_color = base_color;
                                                    major_color.w = 0.9f;
                                                        ImVec4 minor_color = base_color;
                                                        minor_color.x = std::min(1.0f, minor_color.x + 0.15f);
                                                        minor_color.y = std::min(1.0f, minor_color.y + 0.15f);
                                                        minor_color.z = std::min(1.0f, minor_color.z + 0.15f);
                                                        minor_color.w = 0.75f;

                                                    draw_axis(mask_info.feret_major[eye],
                                                              base_id + "_feret_major",
                                                              major_color,
                                                              2.5f);
                                                    draw_axis(mask_info.feret_minor[eye],
                                                              base_id + "_feret_minor",
                                                              minor_color,
                                                              1.8f);

                                                    const auto& minor_axis = mask_info.feret_minor[eye];
                                                    if (minor_axis.valid) {
                                                        auto endpoint0_world = roiToWorld(minor_axis.x0, minor_axis.y0);
                                                        auto endpoint1_world = roiToWorld(minor_axis.x1, minor_axis.y1);
                                                        auto center_world = roiToWorld(
                                                            0.5f * (minor_axis.x0 + minor_axis.x1),
                                                            0.5f * (minor_axis.y0 + minor_axis.y1));
                                                        if (mask_info.has_eye_angles && mask_info.feret_angle_valid[eye]) {
                                                            auto center_scene = worldToScene(center_world.first, center_world.second);
                                                            char angle_label[32];
                                                            std::snprintf(angle_label, sizeof(angle_label), "%+.1f°",
                                                                          mask_info.feret_minor_angle_deg[eye]);
                                                            ImPlot::PlotText(angle_label,
                                                                             center_scene.first,
                                                                             center_scene.second,
                                                                             ImVec2(0.0f, -12.0f));
                                                        }

                                                        double det_center_x =
                                                            0.5 * (mask_details.boxes[det_idx][0] + mask_details.boxes[det_idx][2]);
                                                        double det_center_y =
                                                            0.5 * (mask_details.boxes[det_idx][1] + mask_details.boxes[det_idx][3]);

                                                        auto squaredDistance = [](double ax, double ay, double bx, double by) -> double {
                                                            double dx = ax - bx;
                                                            double dy = ay - by;
                                                            return dx * dx + dy * dy;
                                                        };

                                                        double dist0 = squaredDistance(endpoint0_world.first, endpoint0_world.second,
                                                                                       det_center_x, det_center_y);
                                                        double dist1 = squaredDistance(endpoint1_world.first, endpoint1_world.second,
                                                                                       det_center_x, det_center_y);
                                                        auto outward_endpoint = (dist0 >= dist1) ? endpoint0_world : endpoint1_world;

                                                        ImVec2 dir_world = ImVec2(
                                                            static_cast<float>(outward_endpoint.first - center_world.first),
                                                            static_cast<float>(outward_endpoint.second - center_world.second));
                                                        float dir_len = std::sqrt(dir_world.x * dir_world.x +
                                                                                  dir_world.y * dir_world.y);
                                                        if (dir_len > 1e-3f) {
                                                            dir_world.x /= dir_len;
                                                            dir_world.y /= dir_len;

                                                            ImVec2 smoothed_dir =
                                                                g_eye_orientation_smoother.smoothDirection(
                                                                    mask_info.roi_index, eye, dir_world);
                                                            float smooth_len = std::sqrt(smoothed_dir.x * smoothed_dir.x +
                                                                                          smoothed_dir.y * smoothed_dir.y);
                                                            if (smooth_len > 1e-3f) {
                                                                smoothed_dir.x /= smooth_len;
                                                                smoothed_dir.y /= smooth_len;
                                                                dir_world = smoothed_dir;
                                                            }

                                                            float roi_span = std::max(mask_info.roi_width, mask_info.roi_height);
                                                            float beam_length = std::max(roi_span * 3.5f, 80.0f);
                                                            float beam_width = std::max(roi_span * 0.75f, 25.0f);

                                                            ImVec2 apex_world = ImVec2(
                                                                static_cast<float>(center_world.first - dir_world.x * (roi_span * 0.15f)),
                                                                static_cast<float>(center_world.second - dir_world.y * (roi_span * 0.15f)));
                                                            ImVec2 base_center_world = ImVec2(
                                                                apex_world.x + dir_world.x * beam_length,
                                                                apex_world.y + dir_world.y * beam_length);

                                                            ImVec2 perp_world = ImVec2(-dir_world.y, dir_world.x);
                                                            float perp_len = std::sqrt(perp_world.x * perp_world.x +
                                                                                        perp_world.y * perp_world.y);
                                                            if (perp_len > 1e-3f) {
                                                                perp_world.x /= perp_len;
                                                                perp_world.y /= perp_len;
                                                            }

                                                            ImVec2 left_world = ImVec2(
                                                                base_center_world.x + perp_world.x * (beam_width * 0.5f),
                                                                base_center_world.y + perp_world.y * (beam_width * 0.5f));
                                                            ImVec2 right_world = ImVec2(
                                                                base_center_world.x - perp_world.x * (beam_width * 0.5f),
                                                                base_center_world.y - perp_world.y * (beam_width * 0.5f));

                                                            auto left_scene_pair = worldToScene(left_world.x, left_world.y);
                                                            auto right_scene_pair = worldToScene(right_world.x, right_world.y);
                                                            auto apex_scene_pair = worldToScene(apex_world.x, apex_world.y);

                                                            ImVec2 tri_points[3];
                                                            tri_points[0] = ImPlot::PlotToPixels(
                                                                ImPlotPoint(left_scene_pair.first, left_scene_pair.second));
                                                            tri_points[1] = ImPlot::PlotToPixels(
                                                                ImPlotPoint(right_scene_pair.first, right_scene_pair.second));
                                                            tri_points[2] = ImPlot::PlotToPixels(
                                                                ImPlotPoint(apex_scene_pair.first, apex_scene_pair.second));

                                                            ImVec4 beam_color = base_color;
                                                            beam_color.w = 0.16f;

                                                            ImDrawList* beam_draw_list = ImPlot::GetPlotDrawList();
                                                            beam_draw_list->AddConvexPolyFilled(
                                                                tri_points,
                                                                3,
                                                                ImGui::ColorConvertFloat4ToU32(beam_color));
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                } else if (kEyeMaskDebugLoggingEnabled) {
                                    eyeMaskDebugLog("Frame " + std::to_string(current_frame_num) +
                                                    ": eye mask data unavailable in detection results.");
                                }
                            }
                            draw_keypoint_markers();
                        }

                        if (zarr_loaded && zarr_loader.hasStimulusEvents()) {
                            struct StimulusOverlayState {
                                std::string text;
                                int last_event_frame = std::numeric_limits<int>::min();
                            };
                            static std::array<StimulusOverlayState, MAX_VIEWS> s_overlay_cache;

                            auto frame_events = zarr_loader.getStimulusEventsForFrame(current_frame_num);
                            if (!frame_events.empty()) {
                                std::string events_text;
                                for (size_t i = 0; i < frame_events.size(); ++i) {
                                    if (i > 0) {
                                        events_text += "\n";
                                    }
                                    events_text += frame_events[i];
                                }
                                s_overlay_cache[j].text = std::move(events_text);
                                s_overlay_cache[j].last_event_frame = current_frame_num;
                            }

                            if (!s_overlay_cache[j].text.empty() &&
                                current_frame_num >= s_overlay_cache[j].last_event_frame) {
                                ImVec2 plot_pos = ImPlot::GetPlotPos();
                                ImDrawList* draw_list = ImPlot::GetPlotDrawList();
                                ImVec2 overlay_origin = ImVec2(plot_pos.x + 12.0f, plot_pos.y + 12.0f);
                                ImVec2 text_size = ImGui::CalcTextSize(s_overlay_cache[j].text.c_str(), nullptr, false, -1.0f);
                                ImVec2 box_min = overlay_origin;
                                ImVec2 box_max = ImVec2(box_min.x + text_size.x + 12.0f,
                                                        box_min.y + text_size.y + 8.0f);

                                draw_list->AddRectFilled(box_min, box_max,
                                                         IM_COL32(0, 0, 0, 180), 4.0f);
                                draw_list->AddRect(box_min, box_max,
                                                   IM_COL32(80, 180, 255, 220), 4.0f);
                                draw_list->AddText(ImVec2(box_min.x + 6.0f, box_min.y + 4.0f),
                                                   IM_COL32(200, 220, 255, 255),
                                                   s_overlay_cache[j].text.c_str());
                            }
                        }

                        if (plot_keypoints_flag) {
                            // plot arena for testing camera parameters
                            // gui_plot_perimeter(&camera_params[j],
                            // scene->cameras[j].image_height); if (scene->num_cams > 1)
                            // {
                            //     gui_plot_world_coordinates(&camera_params[j],
                            //     j, scene->cameras[j].image_height);
                            // }

                            // labeling
                            if (ImPlot::IsPlotHovered()) {
                                is_view_focused[j] = true;

                                if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                                    // create keypoints
                                    if (!keypoints_find) {
                                        // not found
                                        KeyPoints *keypoints =
                                            (KeyPoints *)malloc(
                                                sizeof(KeyPoints));
                                        allocate_keypoints(keypoints, scene,
                                                           skeleton.get());
                                        keypoints_map[current_frame_num] =
                                            keypoints;
                                    }
                                }

                                if (keypoints_find) {
                                    u32 *kp = &(keypoints_map[current_frame_num]
                                                    ->active_id[j]);
                                    if (ImGui::IsKeyPressed(ImGuiKey_W,
                                                            false)) {
                                        // labeling sequentially each view
                                        ImPlotPoint mouse =
                                            ImPlot::GetPlotMousePos();
                                        keypoints_map[current_frame_num]
                                            ->keypoints2d[j][*kp]
                                            .position = {mouse.x, mouse.y};
                                        keypoints_map[current_frame_num]
                                            ->keypoints2d[j][*kp]
                                            .is_labeled = true;
                                        keypoints_map[current_frame_num]
                                            ->keypoints2d[j][*kp]
                                            .is_triangulated = false;
                                        if (*kp < (skeleton->num_nodes - 1)) {
                                            (*kp)++;
                                        }
                                    }

                                    if (ImGui::IsKeyPressed(ImGuiKey_A, true)) {
                                        if (*kp <= 0) {
                                            *kp = 0;
                                        } else
                                            (*kp)--;
                                    }

                                    if (ImGui::IsKeyPressed(ImGuiKey_D, true)) {
                                        if (*kp >= skeleton->num_nodes - 1) {
                                            *kp = skeleton->num_nodes - 1;
                                        } else
                                            (*kp)++;
                                    }

                                    if (ImGui::IsKeyPressed(
                                            ImGuiKey_E,
                                            false)) // skip to the last keypoint
                                    {
                                        *kp = skeleton->num_nodes - 1;
                                    }

                                    if (ImGui::IsKeyPressed(
                                            ImGuiKey_Q,
                                            false)) // go to the first keypoint
                                    {
                                        *kp = 0;
                                    }

                                    // delete all keypoint on a frame
                                    if (ImGui::IsKeyPressed(ImGuiKey_Backspace,
                                                            false)) {
                                        free_keypoints(
                                            keypoints_map[current_frame_num],
                                            scene);
                                        keypoints_map.erase(current_frame_num);
                                        keypoints_find = false;
                                    }
                                }
                            } else {
                                is_view_focused[j] = false;
                            }

                            if (keypoints_find) {
                                gui_plot_keypoints(
                                    keypoints_map.at(current_frame_num),
                                    skeleton.get(), j, scene->num_cams);
                                // think more general solution of multiple sets
                                // of keypoints
                                if (skeleton->name == "Rat4Box" ||
                                    skeleton->name == "Rat4Box3Ball") {
                                    gui_plot_bbox_from_keypoints(
                                        keypoints_map.at(current_frame_num),
                                        skeleton.get(), j, 4, 5);
                                }
                            }
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

                    float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                    if (ImGui::Button(ICON_FK_FAST_BACKWARD)) {
                        stepFrames(-10);
                    }
                    ImGui::SameLine(0.0f, spacing);
                    if (ImGui::Button(ICON_FK_STEP_BACKWARD)) {
                        stepFrames(-1);
                    }
                    ImGui::SameLine(0.0f, spacing);

                    if (ps.to_display_frame_number ==
                        (dc_context->total_num_frame - 1)) {
                        ImVec4 repeat_normal = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
                        ImVec4 repeat_hover = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
                        ImVec4 repeat_active = ImVec4(1.0f, 0.9f, 0.1f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, repeat_normal);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                              repeat_hover);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                              repeat_active);

                        if (ImGui::Button(ICON_FK_REPEAT)) {
                            // seek to zero
                            seekToFrame(0, true);
                        }
                        ImGui::PopStyleColor(3);
                    } else {
                        ImVec4 normal, hover, active;
                        if (ps.play_video) {
                            normal = ImVec4(0.8f, 0.3f, 0.3f, 1.0f);
                            hover = ImVec4(0.9f, 0.4f, 0.4f, 1.0f);
                            active = ImVec4(0.7f, 0.2f, 0.2f, 1.0f);
                        } else {
                            // green
                            normal = ImVec4(0.2f, 0.6f, 0.2f, 1.0f);
                            hover = ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
                            active = ImVec4(0.3f, 0.75f, 0.3f, 1.0f);
                        }
                        ImGui::PushStyleColor(ImGuiCol_Button, normal);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
                        if (ImGui::Button(ps.play_video ? ICON_FK_PAUSE
                                                        : ICON_FK_PLAY)) {
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
                        }
                        ImGui::PopStyleColor(3);
                    }

                    ImGui::SameLine(0.0f, spacing);
                    if (ImGui::Button(ICON_FK_STEP_FORWARD)) {
                        stepFrames(1);
                    }
                    ImGui::SameLine(0.0f, spacing);
                    if (ImGui::Button(ICON_FK_FAST_FORWARD)) {
                        stepFrames(10);
                    }
                    ImGui::SameLine();
                    ps.slider_just_changed = ImGui::SliderInt(
                        "##frame count", &ps.slider_frame_number, 0,
                        dc_context->estimated_num_frames);
                    const bool slider_active = ImGui::IsItemActive();
                    const bool slider_released = ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SameLine();
                    float current_time_sec = ps.slider_frame_number / video_fps;
                    float total_time_sec =
                        dc_context->estimated_num_frames / video_fps;

                    std::string current_str = format_time(current_time_sec);
                    std::string total_str = format_time(total_time_sec);
                    ImGui::Text("%s / %s", current_str.c_str(),
                                total_str.c_str());

                    if (ps.slider_just_changed && slider_active) {
                        // Dragging — fast keyframe-only seek
                        seekToFrame(ps.slider_frame_number, true, /*force_inaccurate=*/true);
                    }
                    if (slider_released) {
                        // Released — one final accurate seek for exact frame
                        seekToFrame(ps.slider_frame_number, true, /*force_inaccurate=*/false);
                    }

                    ImGui::EndGroup();
                    frame_camera_scene_ui_ms += durationMs(
                        std::chrono::steady_clock::now() -
                        scene_ui_build_start);
                }
                ImGui::End();
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
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
            }

            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
                if (ImGui::GetIO().KeyShift) {
                    stepFrames(-10);
                } else {
                    stepFrames(-1);
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
                if (ImGui::GetIO().KeyShift) {
                    stepFrames(10);
                } else {
                    stepFrames(1);
                }
            }

            for (const auto &[name, flag] : window_need_decoding) {
                window_was_decoding[name] = flag.load();
            }
        }

        if (zarr_loaded && zarr_loader.hasCropImages()) {
            ImGui::SetNextWindowSize(ImVec2(300.0f, 300.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(120.0f, 120.0f),
                                                ImVec2(420.0f, 700.0f));
            const auto crop_preview_ui_start = std::chrono::steady_clock::now();
            bool crop_window_open = ImGui::Begin("Crop Preview");
            if (crop_window_open) {
                const auto& movement_frames = zarr_loader.getMovementFrameIndices();
                const auto& detection_indices = zarr_loader.getMovementDetectionIndices();
                int32_t crop_roi_index = -1;
                if (!movement_frames.empty() &&
                    movement_frames.size() == detection_indices.size()) {
                    auto it = std::lower_bound(movement_frames.begin(),
                                               movement_frames.end(),
                                               current_frame_num);
                    if (it != movement_frames.end() && *it == current_frame_num) {
                        size_t idx = static_cast<size_t>(std::distance(movement_frames.begin(), it));
                        if (idx < detection_indices.size()) {
                            crop_roi_index = detection_indices[idx];
                        }
                    }
                }
                // Fallback: use crop frame_indices directly when movement
                // data is absent (frame_indices maps roi_index → frame).
                if (crop_roi_index < 0) {
                    const auto& crop_frames = zarr_loader.getCropFrameIndices();
                    auto it = std::find(crop_frames.begin(),
                                        crop_frames.end(),
                                        current_frame_num);
                    if (it != crop_frames.end()) {
                        crop_roi_index = static_cast<int32_t>(
                            std::distance(crop_frames.begin(), it));
                    }
                }

                static GLuint crop_texture = 0;
                static std::vector<uint8_t> crop_rgba_buffer;
                static int last_roi_index = -1;
                static size_t last_width = 0;
                static size_t last_height = 0;
                static size_t last_channels = 0;
                static int last_crop_preview_source_frame = -1;
                static auto last_crop_preview_refresh_time =
                    std::chrono::steady_clock::time_point{};
                static std::vector<std::array<float, 2>> crop_kp_positions;
                static std::vector<std::string> crop_kp_labels;
                static std::vector<std::array<size_t, 2>> crop_kp_edges;

                static GLuint rotated_crop_texture = 0;
                static std::vector<uint8_t> rotated_rgba_buffer;
                static unsigned int rotated_width = 0;
                static unsigned int rotated_height = 0;
                static bool rotated_valid = false;
                static std::vector<std::array<float, 2>> rotated_kp_positions;
                static std::vector<std::string> rotated_kp_labels;
                static std::vector<std::array<size_t, 2>> rotated_kp_edges;
                static float stored_heading_deg = 0.0f;
                static bool stored_heading_valid = false;
                static std::array<float, 2> arrow_origin_crop = {0, 0};   // in crop-local px
                static std::array<float, 2> arrow_origin_rotated = {0, 0}; // in rotated-crop-local px
                static bool arrow_origin_valid = false;
                static int displayed_crop_roi_index = -1;
                static int displayed_crop_source_frame = -1;

                if (crop_roi_index >= 0) {
                    constexpr auto kCropPreviewPlaybackRefreshInterval =
                        std::chrono::milliseconds(100);
                    const auto now_steady = std::chrono::steady_clock::now();
                    const bool playback_refresh_due =
                        !ps.play_video ||
                        last_crop_preview_refresh_time ==
                            std::chrono::steady_clock::time_point{} ||
                        (now_steady - last_crop_preview_refresh_time) >=
                            kCropPreviewPlaybackRefreshInterval;
                    const bool frame_changed =
                        current_frame_num != last_crop_preview_source_frame;
                    const bool should_refresh_preview =
                        !ps.play_video || !frame_changed || playback_refresh_due;

                    ZarrDetectionLoader::CropImageView crop_view;
                    bool crop_preview_available = false;
                    if (should_refresh_preview &&
                        zarr_loader.getCropImageForIndex(crop_roi_index, crop_view)) {
                        bool needs_upload =
                            crop_roi_index != last_roi_index ||
                            crop_view.width != last_width ||
                            crop_view.height != last_height ||
                            crop_view.channels != last_channels ||
                            frame_changed;

                        if (crop_texture == 0) {
                            create_texture(&crop_texture);
                            needs_upload = true;
                        }

                        if (needs_upload) {
                            size_t pixel_count = crop_view.width * crop_view.height;
                            crop_rgba_buffer.resize(pixel_count * 4);
                            const uint8_t* src = crop_view.data;
                            uint8_t* dst = crop_rgba_buffer.data();
                            if (crop_view.channels == 4) {
                                std::memcpy(dst, src, pixel_count * 4);
                            } else if (crop_view.channels == 3) {
                                for (size_t p = 0; p < pixel_count; ++p) {
                                    dst[4 * p + 0] = src[3 * p + 0];
                                    dst[4 * p + 1] = src[3 * p + 1];
                                    dst[4 * p + 2] = src[3 * p + 2];
                                    dst[4 * p + 3] = 255;
                                }
                            } else {
                                for (size_t p = 0; p < pixel_count; ++p) {
                                    uint8_t v = src[p];
                                    dst[4 * p + 0] = v;
                                    dst[4 * p + 1] = v;
                                    dst[4 * p + 2] = v;
                                    dst[4 * p + 3] = 255;
                                }
                            }
                            upload_texture(&crop_texture,
                                           crop_rgba_buffer.data(),
                                           static_cast<unsigned int>(crop_view.width),
                                           static_cast<unsigned int>(crop_view.height));
                            last_roi_index = crop_roi_index;
                            last_width = crop_view.width;
                            last_height = crop_view.height;
                            last_channels = crop_view.channels;
                            displayed_crop_roi_index = crop_roi_index;
                            displayed_crop_source_frame = current_frame_num;
                            last_crop_preview_source_frame = current_frame_num;
                            last_crop_preview_refresh_time = now_steady;
                            crop_kp_positions.clear();
                            crop_kp_labels.clear();
                            crop_kp_edges.clear();

                            // Compute heading-normalized rotated crop
                            rotated_valid = false;
                            stored_heading_valid = false;
                            if (zarr_loader.hasKeypointData()) {
                                auto det = zarr_loader.getRawDetections(
                                    static_cast<size_t>(current_frame_num), false, true);
                                size_t matched = SIZE_MAX;
                                for (size_t di = 0; di < det.eye_masks.size(); ++di) {
                                    if (det.eye_masks[di].roi_index == crop_roi_index) {
                                    matched = di; break;
                                    }
                                }
                                if (matched != SIZE_MAX && matched < det.headings_deg.size() &&
                                    matched < det.heading_valid.size() && det.heading_valid[matched]) {
                                    float heading = det.headings_deg[matched];
                                    stored_heading_deg = heading;
                                    stored_heading_valid = true;
                                    float angle = -heading;
                                    int w = static_cast<int>(crop_view.width);
                                    int h = static_cast<int>(crop_view.height);
                                    cv::Point2f center(w / 2.0f, h / 2.0f);
                                    cv::Mat rot_mat = cv::getRotationMatrix2D(center, angle, 1.0);
                                    cv::Rect2f bbox = cv::RotatedRect(center, cv::Size2f(static_cast<float>(w), static_cast<float>(h)), angle)
                                                          .boundingRect2f();
                                    rot_mat.at<double>(0, 2) += bbox.width / 2.0 - center.x;
                                    rot_mat.at<double>(1, 2) += bbox.height / 2.0 - center.y;
                                    int new_w = static_cast<int>(std::ceil(bbox.width));
                                    int new_h = static_cast<int>(std::ceil(bbox.height));
                                    cv::Mat src(h, w, CV_8UC4, crop_rgba_buffer.data());
                                    cv::Mat dst;
                                    cv::warpAffine(src, dst, rot_mat, cv::Size(new_w, new_h),
                                                    cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                                                    cv::Scalar(0, 0, 0, 0));
                                    // Apply circular mask: radius = half the smaller original dimension
                                    float radius = std::min(w, h) / 2.0f;
                                    cv::Point2f new_center(new_w / 2.0f, new_h / 2.0f);
                                    for (int row = 0; row < new_h; ++row) {
                                        uint8_t* ptr = dst.ptr<uint8_t>(row);
                                        for (int col = 0; col < new_w; ++col) {
                                            float dx = col + 0.5f - new_center.x;
                                            float dy = row + 0.5f - new_center.y;
                                            if (dx * dx + dy * dy > radius * radius) {
                                                ptr[col * 4 + 0] = 0;
                                                ptr[col * 4 + 1] = 0;
                                                ptr[col * 4 + 2] = 0;
                                                ptr[col * 4 + 3] = 0;
                                            }
                                        }
                                    }
                                    // Crop to circle's bounding square so canvas size is
                                    // constant regardless of rotation angle.
                                    int crop_side = std::min(w, h);
                                    int cx = new_w / 2 - crop_side / 2;
                                    int cy = new_h / 2 - crop_side / 2;
                                    dst = dst(cv::Rect(cx, cy, crop_side, crop_side)).clone();
                                    new_w = crop_side;
                                    new_h = crop_side;
                                    rotated_rgba_buffer.assign(dst.data, dst.data + dst.total() * 4);
                                    rotated_width = new_w;
                                    rotated_height = new_h;
                                    if (rotated_crop_texture == 0) create_texture(&rotated_crop_texture);
                                    upload_texture(&rotated_crop_texture, rotated_rgba_buffer.data(),
                                                   rotated_width, rotated_height);
                                    rotated_valid = true;

                                    // Transform keypoints into rotated-crop-local coords
                                    rotated_kp_positions.clear();
                                    rotated_kp_labels.clear();
                                    rotated_kp_edges = det.skeleton_edges;
                                    crop_kp_edges = det.skeleton_edges;
                                    if (det.has_keypoints && matched < det.keypoints_pixels.size() &&
                                        det.includes_eye_masks) {
                                        const auto& kps = det.keypoints_pixels[matched];
                                        const auto& mask = det.eye_masks[matched];
                                        float off_x = mask.offset_x;
                                        float off_y = mask.offset_y;
                                        if (std::isfinite(off_x) && std::isfinite(off_y)) {
                                            double r0 = rot_mat.at<double>(0, 0);
                                            double r1 = rot_mat.at<double>(0, 1);
                                            double r2 = rot_mat.at<double>(0, 2);
                                            double r3 = rot_mat.at<double>(1, 0);
                                            double r4 = rot_mat.at<double>(1, 1);
                                            double r5 = rot_mat.at<double>(1, 2);
                                            for (size_t ki = 0; ki < kps.size(); ++ki) {
                                                if (!std::isfinite(kps[ki][0]) ||
                                                    !std::isfinite(kps[ki][1])) {
                                                    crop_kp_positions.push_back({NAN, NAN});
                                                    rotated_kp_positions.push_back({NAN, NAN});
                                                } else {
                                                    float px = kps[ki][0] - off_x;
                                                    float py = kps[ki][1] - off_y;
                                                    crop_kp_positions.push_back({px, py});
                                                    float rx = static_cast<float>(r0 * px + r1 * py + r2) - cx;
                                                    float ry = static_cast<float>(r3 * px + r4 * py + r5) - cy;
                                                    rotated_kp_positions.push_back({rx, ry});
                                                }
                                                crop_kp_labels.push_back(
                                                    ki < det.keypoint_labels.size()
                                                        ? det.keypoint_labels[ki] : "");
                                                rotated_kp_labels.push_back(
                                                    ki < det.keypoint_labels.size()
                                                        ? det.keypoint_labels[ki] : "");
                                            }
                                            // Compute arrow origin: midpoint between eye keypoints
                                            arrow_origin_valid = false;
                                            float left_x = NAN, left_y = NAN;
                                            float right_x = NAN, right_y = NAN;
                                            float left_rx = NAN, left_ry = NAN;
                                            float right_rx = NAN, right_ry = NAN;
                                            for (size_t ki = 0; ki < kps.size(); ++ki) {
                                                const std::string& lbl =
                                                    ki < det.keypoint_labels.size()
                                                        ? det.keypoint_labels[ki] : "";
                                                bool is_left = lbl.find("left") != std::string::npos;
                                                bool is_right = lbl.find("right") != std::string::npos;
                                                if ((is_left || is_right) &&
                                                    std::isfinite(kps[ki][0]) && std::isfinite(kps[ki][1])) {
                                                    float px = kps[ki][0] - off_x;
                                                    float py = kps[ki][1] - off_y;
                                                    if (is_left)  { left_x = px; left_y = py; }
                                                    if (is_right) { right_x = px; right_y = py; }
                                                    if (ki < rotated_kp_positions.size()) {
                                                        if (is_left) { left_rx = rotated_kp_positions[ki][0]; left_ry = rotated_kp_positions[ki][1]; }
                                                        if (is_right) { right_rx = rotated_kp_positions[ki][0]; right_ry = rotated_kp_positions[ki][1]; }
                                                    }
                                                }
                                            }
                                            if (std::isfinite(left_x) && std::isfinite(right_x)) {
                                                arrow_origin_crop = {(left_x + right_x) / 2.0f,
                                                                     (left_y + right_y) / 2.0f};
                                                arrow_origin_rotated = {(left_rx + right_rx) / 2.0f,
                                                                        (left_ry + right_ry) / 2.0f};
                                                arrow_origin_valid = true;
                                            }
                                        }
                                    }
                                } else {
                                    crop_kp_positions.clear();
                                    crop_kp_labels.clear();
                                    crop_kp_edges.clear();
                                }
                            }
                        }

                        crop_preview_available =
                            crop_texture != 0 && last_width > 0 && last_height > 0;
                        if (crop_preview_available) {
                            static bool show_crop_keypoints = true;
                            static bool show_rotated_crop = true;
                            static bool show_heading_arrow = false;
                            ImGui::Checkbox("Keypoints", &show_crop_keypoints);
                            ImGui::SameLine();
                            ImGui::Checkbox("Rotated", &show_rotated_crop);
                            ImGui::SameLine();
                            ImGui::Checkbox("Heading", &show_heading_arrow);
                            if (ps.play_video) {
                                ImGui::TextDisabled(
                                    "Playback preview throttled to 10 Hz");
                            }

                            ImVec2 img_size(static_cast<float>(last_width),
                                            static_cast<float>(last_height));
                            float max_dim = std::max(img_size.x, img_size.y);
                            const float preview_max = 260.0f;
                            float scale = 1.0f;
                            if (max_dim > preview_max && max_dim > 0.0f) {
                                scale = preview_max / max_dim;
                                img_size.x *= scale;
                                img_size.y *= scale;
                            }
                            ImVec2 image_tl = ImGui::GetCursorScreenPos();
                            ImGui::Image((ImTextureID)(intptr_t)crop_texture, img_size);
                            ImGui::Text("ROI #%d", displayed_crop_roi_index);
                            if (ps.play_video && displayed_crop_source_frame >= 0 &&
                                displayed_crop_source_frame != current_frame_num) {
                                ImGui::TextDisabled("Preview frame %d", displayed_crop_source_frame);
                            }

                            if (show_crop_keypoints && !crop_kp_positions.empty()) {
                                ImDrawList* draw_list = ImGui::GetWindowDrawList();

                                auto kpColor = [&](size_t kp_idx) -> ImU32 {
                                    const std::string& lbl =
                                        kp_idx < crop_kp_labels.size()
                                            ? crop_kp_labels[kp_idx] : "";
                                    if (lbl.find("swim") != std::string::npos ||
                                        lbl.find("bladder") != std::string::npos)
                                        return IM_COL32(255, 217, 38, 220);
                                    if (lbl.find("left") != std::string::npos)
                                        return IM_COL32(77, 242, 102, 220);
                                    if (lbl.find("right") != std::string::npos)
                                        return IM_COL32(191, 102, 242, 220);
                                    return IM_COL32(242, 153, 51, 220);
                                };

                                for (const auto& edge : crop_kp_edges) {
                                    size_t a = edge[0], b = edge[1];
                                    if (a >= crop_kp_positions.size() ||
                                        b >= crop_kp_positions.size()) continue;
                                    float ax = crop_kp_positions[a][0];
                                    float ay = crop_kp_positions[a][1];
                                    float bx = crop_kp_positions[b][0];
                                    float by = crop_kp_positions[b][1];
                                    if (!std::isfinite(ax) || !std::isfinite(ay) ||
                                        !std::isfinite(bx) || !std::isfinite(by)) continue;
                                    ImVec2 pa(image_tl.x + ax * scale,
                                              image_tl.y + ay * scale);
                                    ImVec2 pb(image_tl.x + bx * scale,
                                              image_tl.y + by * scale);
                                    draw_list->AddLine(pa, pb, IM_COL32(255, 255, 255, 160), 1.5f);
                                }
                                for (size_t ki = 0; ki < crop_kp_positions.size(); ++ki) {
                                    float kx = crop_kp_positions[ki][0];
                                    float ky = crop_kp_positions[ki][1];
                                    if (!std::isfinite(kx) || !std::isfinite(ky)) continue;
                                    ImVec2 center(image_tl.x + kx * scale,
                                                  image_tl.y + ky * scale);
                                    draw_list->AddCircleFilled(center, 4.0f * scale,
                                                               kpColor(ki));
                                    draw_list->AddCircle(center, 4.0f * scale,
                                                         IM_COL32(255, 255, 255, 180), 0, 1.5f);
                                }
                            }

                            if (show_heading_arrow && stored_heading_valid && arrow_origin_valid) {
                                float rad = stored_heading_deg * (3.14159265f / 180.0f);
                                float arrow_len = std::min(img_size.x, img_size.y) * 0.2f;
                                ImVec2 center(image_tl.x + arrow_origin_crop[0] * scale,
                                              image_tl.y + arrow_origin_crop[1] * scale);
                                // 0° = right (+X), CCW positive, screen Y is flipped
                                float dx = std::cos(rad) * arrow_len;
                                float dy = -std::sin(rad) * arrow_len;
                                ImVec2 tip(center.x + dx, center.y + dy);
                                ImDrawList* dl = ImGui::GetWindowDrawList();
                                dl->AddLine(center, tip, IM_COL32(255, 50, 50, 220), 2.5f);
                                // Arrowhead
                                float head_len = 8.0f * scale;
                                float head_angle = 2.6f; // ~150° from shaft
                                ImVec2 h1(tip.x + head_len * std::cos(rad + head_angle),
                                          tip.y - head_len * std::sin(rad + head_angle));
                                ImVec2 h2(tip.x + head_len * std::cos(rad - head_angle),
                                          tip.y - head_len * std::sin(rad - head_angle));
                                dl->AddTriangleFilled(tip, h1, h2, IM_COL32(255, 50, 50, 220));
                            }

                            if (show_rotated_crop && rotated_valid && rotated_crop_texture != 0) {
                                ImGui::Separator();
                                ImVec2 rot_size(static_cast<float>(rotated_width),
                                                static_cast<float>(rotated_height));
                                float rot_max = std::max(rot_size.x, rot_size.y);
                                float rot_scale = 1.0f;
                                if (rot_max > preview_max && rot_max > 0.0f) {
                                    rot_scale = preview_max / rot_max;
                                    rot_size.x *= rot_scale;
                                    rot_size.y *= rot_scale;
                                }
                                ImVec2 rot_image_tl = ImGui::GetCursorScreenPos();
                                ImGui::Image((ImTextureID)(intptr_t)rotated_crop_texture, rot_size);

                                if (show_crop_keypoints && !rotated_kp_positions.empty()) {
                                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                                    auto kpColor = [&](size_t kp_idx) -> ImU32 {
                                        const std::string& lbl =
                                            kp_idx < rotated_kp_labels.size()
                                                ? rotated_kp_labels[kp_idx] : "";
                                        if (lbl.find("swim") != std::string::npos ||
                                            lbl.find("bladder") != std::string::npos)
                                            return IM_COL32(255, 217, 38, 220);
                                        if (lbl.find("left") != std::string::npos)
                                            return IM_COL32(77, 242, 102, 220);
                                        if (lbl.find("right") != std::string::npos)
                                            return IM_COL32(191, 102, 242, 220);
                                        return IM_COL32(242, 153, 51, 220);
                                    };
                                    for (const auto& edge : rotated_kp_edges) {
                                        size_t a = edge[0], b = edge[1];
                                        if (a >= rotated_kp_positions.size() || b >= rotated_kp_positions.size()) continue;
                                        float ax = rotated_kp_positions[a][0], ay = rotated_kp_positions[a][1];
                                        float bx = rotated_kp_positions[b][0], by = rotated_kp_positions[b][1];
                                        if (!std::isfinite(ax) || !std::isfinite(ay) ||
                                            !std::isfinite(bx) || !std::isfinite(by)) continue;
                                        ImVec2 pa(rot_image_tl.x + ax * rot_scale, rot_image_tl.y + ay * rot_scale);
                                        ImVec2 pb(rot_image_tl.x + bx * rot_scale, rot_image_tl.y + by * rot_scale);
                                        draw_list->AddLine(pa, pb, IM_COL32(255, 255, 255, 160), 1.5f);
                                    }
                                    for (size_t ki = 0; ki < rotated_kp_positions.size(); ++ki) {
                                        float rx = rotated_kp_positions[ki][0];
                                        float ry = rotated_kp_positions[ki][1];
                                        if (!std::isfinite(rx) || !std::isfinite(ry)) continue;
                                        float sx = rx * rot_scale;
                                        float sy = ry * rot_scale;
                                        ImVec2 pt(rot_image_tl.x + sx, rot_image_tl.y + sy);
                                        draw_list->AddCircleFilled(pt, 4.0f * rot_scale, kpColor(ki));
                                        draw_list->AddCircle(pt, 4.0f * rot_scale,
                                                             IM_COL32(255, 255, 255, 180), 0, 1.5f);
                                    }
                                }
                                if (show_heading_arrow && stored_heading_valid && arrow_origin_valid) {
                                    // Normalized heading always points right (0°)
                                    float arrow_len = std::min(rot_size.x, rot_size.y) * 0.2f;
                                    ImVec2 center(rot_image_tl.x + arrow_origin_rotated[0] * rot_scale,
                                                  rot_image_tl.y + arrow_origin_rotated[1] * rot_scale);
                                    ImVec2 tip(center.x + arrow_len, center.y);
                                    ImDrawList* dl = ImGui::GetWindowDrawList();
                                    dl->AddLine(center, tip, IM_COL32(255, 50, 50, 220), 2.5f);
                                    float head_len = 8.0f * rot_scale;
                                    float head_angle = 2.6f;
                                    ImVec2 h1(tip.x + head_len * std::cos(head_angle),
                                              tip.y - head_len * std::sin(head_angle));
                                    ImVec2 h2(tip.x + head_len * std::cos(-head_angle),
                                              tip.y - head_len * std::sin(-head_angle));
                                    dl->AddTriangleFilled(tip, h1, h2, IM_COL32(255, 50, 50, 220));
                                }
                                ImGui::Text("Heading-normalized");
                            }
                        }
                    } else if (!should_refresh_preview) {
                        crop_preview_available =
                            crop_texture != 0 && displayed_crop_roi_index >= 0 &&
                            last_width > 0 && last_height > 0;
                        if (crop_preview_available) {
                            static bool show_crop_keypoints = true;
                            static bool show_rotated_crop = true;
                            static bool show_heading_arrow = false;
                            ImGui::Checkbox("Keypoints", &show_crop_keypoints);
                            ImGui::SameLine();
                            ImGui::Checkbox("Rotated", &show_rotated_crop);
                            ImGui::SameLine();
                            ImGui::Checkbox("Heading", &show_heading_arrow);
                            ImGui::TextDisabled(
                                "Playback preview throttled to 10 Hz");

                            ImVec2 img_size(static_cast<float>(last_width),
                                            static_cast<float>(last_height));
                            float max_dim = std::max(img_size.x, img_size.y);
                            const float preview_max = 260.0f;
                            float scale = 1.0f;
                            if (max_dim > preview_max && max_dim > 0.0f) {
                                scale = preview_max / max_dim;
                                img_size.x *= scale;
                                img_size.y *= scale;
                            }
                            ImVec2 image_tl = ImGui::GetCursorScreenPos();
                            ImGui::Image((ImTextureID)(intptr_t)crop_texture, img_size);
                            ImGui::Text("ROI #%d", displayed_crop_roi_index);
                            if (displayed_crop_source_frame >= 0 &&
                                displayed_crop_source_frame != current_frame_num) {
                                ImGui::TextDisabled("Preview frame %d",
                                                    displayed_crop_source_frame);
                            }

                            if (show_crop_keypoints && !crop_kp_positions.empty()) {
                                ImDrawList* draw_list = ImGui::GetWindowDrawList();

                                auto kpColor = [&](size_t kp_idx) -> ImU32 {
                                    const std::string& lbl =
                                        kp_idx < crop_kp_labels.size()
                                            ? crop_kp_labels[kp_idx] : "";
                                    if (lbl.find("swim") != std::string::npos ||
                                        lbl.find("bladder") != std::string::npos)
                                        return IM_COL32(255, 217, 38, 220);
                                    if (lbl.find("left") != std::string::npos)
                                        return IM_COL32(77, 242, 102, 220);
                                    if (lbl.find("right") != std::string::npos)
                                        return IM_COL32(191, 102, 242, 220);
                                    return IM_COL32(242, 153, 51, 220);
                                };

                                for (const auto& edge : crop_kp_edges) {
                                    size_t a = edge[0], b = edge[1];
                                    if (a >= crop_kp_positions.size() ||
                                        b >= crop_kp_positions.size()) continue;
                                    float ax = crop_kp_positions[a][0];
                                    float ay = crop_kp_positions[a][1];
                                    float bx = crop_kp_positions[b][0];
                                    float by = crop_kp_positions[b][1];
                                    if (!std::isfinite(ax) || !std::isfinite(ay) ||
                                        !std::isfinite(bx) || !std::isfinite(by)) continue;
                                    ImVec2 pa(image_tl.x + ax * scale,
                                              image_tl.y + ay * scale);
                                    ImVec2 pb(image_tl.x + bx * scale,
                                              image_tl.y + by * scale);
                                    draw_list->AddLine(pa, pb, IM_COL32(255, 255, 255, 160), 1.5f);
                                }
                                for (size_t ki = 0; ki < crop_kp_positions.size(); ++ki) {
                                    float kx = crop_kp_positions[ki][0];
                                    float ky = crop_kp_positions[ki][1];
                                    if (!std::isfinite(kx) || !std::isfinite(ky)) continue;
                                    ImVec2 center(image_tl.x + kx * scale,
                                                  image_tl.y + ky * scale);
                                    draw_list->AddCircleFilled(center, 4.0f * scale,
                                                               kpColor(ki));
                                    draw_list->AddCircle(center, 4.0f * scale,
                                                         IM_COL32(255, 255, 255, 180), 0, 1.5f);
                                }
                            }

                            if (show_heading_arrow && stored_heading_valid &&
                                arrow_origin_valid) {
                                float rad = stored_heading_deg *
                                    (3.14159265f / 180.0f);
                                float arrow_len =
                                    std::min(img_size.x, img_size.y) * 0.2f;
                                ImVec2 center(image_tl.x + arrow_origin_crop[0] * scale,
                                              image_tl.y + arrow_origin_crop[1] * scale);
                                float dx = std::cos(rad) * arrow_len;
                                float dy = -std::sin(rad) * arrow_len;
                                ImVec2 tip(center.x + dx, center.y + dy);
                                ImDrawList* dl = ImGui::GetWindowDrawList();
                                dl->AddLine(center, tip, IM_COL32(255, 50, 50, 220), 2.5f);
                                float head_len = 8.0f * scale;
                                float head_angle = 2.6f;
                                ImVec2 h1(tip.x + head_len * std::cos(rad + head_angle),
                                          tip.y - head_len * std::sin(rad + head_angle));
                                ImVec2 h2(tip.x + head_len * std::cos(rad - head_angle),
                                          tip.y - head_len * std::sin(rad - head_angle));
                                dl->AddTriangleFilled(tip, h1, h2, IM_COL32(255, 50, 50, 220));
                            }

                            if (show_rotated_crop && rotated_valid &&
                                rotated_crop_texture != 0) {
                                ImGui::Separator();
                                ImVec2 rot_size(static_cast<float>(rotated_width),
                                                static_cast<float>(rotated_height));
                                float rot_max = std::max(rot_size.x, rot_size.y);
                                float rot_scale = 1.0f;
                                if (rot_max > preview_max && rot_max > 0.0f) {
                                    rot_scale = preview_max / rot_max;
                                    rot_size.x *= rot_scale;
                                    rot_size.y *= rot_scale;
                                }
                                ImVec2 rot_image_tl = ImGui::GetCursorScreenPos();
                                ImGui::Image((ImTextureID)(intptr_t)rotated_crop_texture, rot_size);

                                if (show_crop_keypoints &&
                                    !rotated_kp_positions.empty()) {
                                    ImDrawList* draw_list =
                                        ImGui::GetWindowDrawList();
                                    auto kpColor = [&](size_t kp_idx) -> ImU32 {
                                        const std::string& lbl =
                                            kp_idx < rotated_kp_labels.size()
                                                ? rotated_kp_labels[kp_idx] : "";
                                        if (lbl.find("swim") != std::string::npos ||
                                            lbl.find("bladder") != std::string::npos)
                                            return IM_COL32(255, 217, 38, 220);
                                        if (lbl.find("left") != std::string::npos)
                                            return IM_COL32(77, 242, 102, 220);
                                        if (lbl.find("right") != std::string::npos)
                                            return IM_COL32(191, 102, 242, 220);
                                        return IM_COL32(242, 153, 51, 220);
                                    };
                                    for (const auto& edge : rotated_kp_edges) {
                                        size_t a = edge[0], b = edge[1];
                                        if (a >= rotated_kp_positions.size() ||
                                            b >= rotated_kp_positions.size()) continue;
                                        float ax = rotated_kp_positions[a][0], ay = rotated_kp_positions[a][1];
                                        float bx = rotated_kp_positions[b][0], by = rotated_kp_positions[b][1];
                                        if (!std::isfinite(ax) || !std::isfinite(ay) ||
                                            !std::isfinite(bx) || !std::isfinite(by)) continue;
                                        ImVec2 pa(rot_image_tl.x + ax * rot_scale, rot_image_tl.y + ay * rot_scale);
                                        ImVec2 pb(rot_image_tl.x + bx * rot_scale, rot_image_tl.y + by * rot_scale);
                                        draw_list->AddLine(pa, pb, IM_COL32(255, 255, 255, 160), 1.5f);
                                    }
                                    for (size_t ki = 0; ki < rotated_kp_positions.size(); ++ki) {
                                        float rx = rotated_kp_positions[ki][0];
                                        float ry = rotated_kp_positions[ki][1];
                                        if (!std::isfinite(rx) || !std::isfinite(ry)) continue;
                                        float sx = rx * rot_scale;
                                        float sy = ry * rot_scale;
                                        ImVec2 pt(rot_image_tl.x + sx, rot_image_tl.y + sy);
                                        draw_list->AddCircleFilled(pt, 4.0f * rot_scale, kpColor(ki));
                                        draw_list->AddCircle(pt, 4.0f * rot_scale,
                                                             IM_COL32(255, 255, 255, 180), 0, 1.5f);
                                    }
                                }
                                if (show_heading_arrow && stored_heading_valid &&
                                    arrow_origin_valid) {
                                    float arrow_len = std::min(rot_size.x, rot_size.y) * 0.2f;
                                    ImVec2 center(rot_image_tl.x + arrow_origin_rotated[0] * rot_scale,
                                                  rot_image_tl.y + arrow_origin_rotated[1] * rot_scale);
                                    ImVec2 tip(center.x + arrow_len, center.y);
                                    ImDrawList* dl = ImGui::GetWindowDrawList();
                                    dl->AddLine(center, tip, IM_COL32(255, 50, 50, 220), 2.5f);
                                    float head_len = 8.0f * rot_scale;
                                    float head_angle = 2.6f;
                                    ImVec2 h1(tip.x + head_len * std::cos(head_angle),
                                              tip.y - head_len * std::sin(head_angle));
                                    ImVec2 h2(tip.x + head_len * std::cos(-head_angle),
                                              tip.y - head_len * std::sin(-head_angle));
                                    dl->AddTriangleFilled(tip, h1, h2, IM_COL32(255, 50, 50, 220));
                                }
                                ImGui::Text("Heading-normalized");
                            }
                        }
                    } else {
                        ImGui::TextUnformatted("No crop available for current frame.");
                        last_roi_index = -1;
                        displayed_crop_roi_index = -1;
                        displayed_crop_source_frame = -1;
                        crop_kp_positions.clear();
                        crop_kp_labels.clear();
                        crop_kp_edges.clear();
                    }
                } else {
                    ImGui::TextUnformatted("No crop available for current frame.");
                    last_roi_index = -1;
                    displayed_crop_roi_index = -1;
                    displayed_crop_source_frame = -1;
                    crop_kp_positions.clear();
                    crop_kp_labels.clear();
                    crop_kp_edges.clear();
                }
            }
            ImGui::End();
            frame_crop_preview_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - crop_preview_ui_start);
        }

        if (stimulus_player.loaded) {
            const auto stimulus_window_ui_start =
                std::chrono::steady_clock::now();
            ImGui::SetNextWindowSize(ImVec2(480.0f, 360.0f), ImGuiCond_FirstUseEver);
            bool stimulus_visible = ImGui::Begin(stimulus_player.window_name.c_str());

            bool seek_needs_stimulus =
                (seek_progress.state == SeekState::WaitingStimulus);
            bool base_decode_request =
                ps.play_video || !ps.pause_seeked || seek_needs_stimulus;
            bool decoder_requested = base_decode_request;
            static bool last_decoder_logged = false;

            int target_stimulus_frame = ps.current_stimulus_frame;
            int effective_target_frame = target_stimulus_frame;
            if (effective_target_frame < 0) {
                std::optional<int32_t> first_stim;
                if (zarr_loaded && zarr_loader.hasStimulusAlignment()) {
                    auto opt_first = zarr_loader.getFirstStimulusFrameNumber();
                    if (opt_first) {
                        first_stim = *opt_first;
                    }
                }
                effective_target_frame = first_stim.value_or(0);
            }
            // Keep a one-frame cushion so target-1 fallback frames remain
            // available when timestamp rounding lands just before target.
            constexpr int kStimulusDiscardSlackFrames = 1;
            auto isStimulusFrameClose = [&](int candidate_frame,
                                            int target_frame) -> bool {
                if (candidate_frame < 0 || target_frame < 0) {
                    return false;
                }
                return std::abs(candidate_frame - target_frame) <=
                       kStimulusDiscardSlackFrames;
            };
            int discard_threshold = effective_target_frame - kStimulusDiscardSlackFrames;
            if (discard_threshold < 0) {
                discard_threshold = 0;
            }
            discardStimulusFramesOlderThan(stimulus_player, discard_threshold);

            bool decoder_active_now = window_need_decoding[stimulus_player.window_name].load();
            if (stimulus_player.playback_catchup_seek_in_flight) {
                std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                if (stimulus_player.seek.seek_done &&
                    stimulus_player.seek.settled_seek_id ==
                        stimulus_player.playback_catchup_seek_id) {
                    stimulus_player.playback_catchup_seek_in_flight = false;
                }
            }

            if (ps.play_video && zarr_loaded && zarr_loader.hasStimulusAlignment() &&
                target_stimulus_frame >= 0 &&
                seek_progress.state != SeekState::WaitingCameras &&
                seek_progress.state != SeekState::WaitingStimulus) {
                const int latest_stimulus_frame =
                    latest_decoded_frame[stimulus_player.window_name].load();
                const int stimulus_progress_frame =
                    std::max(latest_stimulus_frame, stimulus_player.last_displayed_frame);
                const int stimulus_lag_frames =
                    (stimulus_progress_frame >= 0)
                        ? (target_stimulus_frame - stimulus_progress_frame)
                        : target_stimulus_frame;
                const int catchup_threshold =
                    std::max(8, static_cast<int>(std::ceil(stimulus_player.fps * 0.35)));
                const int catchup_seek_backoff =
                    std::max(1, static_cast<int>(std::ceil(stimulus_player.fps * 0.05)));
                const auto now = std::chrono::steady_clock::now();
                const bool catchup_cooldown_elapsed =
                    !stimulus_player.playback_catchup_seek_in_flight ||
                    (now - stimulus_player.playback_catchup_last_request) >=
                        std::chrono::milliseconds(200);
                const bool target_has_advanced =
                    stimulus_player.playback_catchup_target_frame < 0 ||
                    target_stimulus_frame >
                        (stimulus_player.playback_catchup_target_frame +
                         std::max(2, catchup_threshold / 2));

                if (stimulus_lag_frames > catchup_threshold &&
                    catchup_cooldown_elapsed && target_has_advanced) {
                    const int catchup_seek_frame =
                        std::max(0, target_stimulus_frame - catchup_seek_backoff);
                    const uint64_t catchup_seek_id =
                        stimulus_catchup_seek_generation++;
                    {
                        std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                        stimulus_player.seek.seek_frame =
                            static_cast<uint64_t>(catchup_seek_frame);
                        stimulus_player.seek.seek_id = catchup_seek_id;
                        stimulus_player.seek.use_seek = true;
                        stimulus_player.seek.seek_done = false;
                        stimulus_player.seek.seek_accurate = false;
                    }
                    stimulus_player.last_displayed_frame = -1;
                    stimulus_player.throttled = false;
                    stimulus_player.throttle_resume_frame = -1;
                    stimulus_player.playback_catchup_seek_in_flight = true;
                    stimulus_player.playback_catchup_seek_id = catchup_seek_id;
                    stimulus_player.playback_catchup_target_frame = catchup_seek_frame;
                    stimulus_player.playback_catchup_last_request = now;
                    decoder_requested = true;
                    if (crimson_seek_debug_logs_enabled()) std::cout
                        << "[Stimulus] catch-up seek target=" << target_stimulus_frame
                        << " progress=" << stimulus_progress_frame
                        << " lag=" << stimulus_lag_frames
                        << " seek_frame=" << catchup_seek_frame
                        << " seek_id=" << catchup_seek_id << std::endl;
                }
            }

            if (base_decode_request) {
                double fps_ratio = (video_fps > 0.0) ? (stimulus_player.fps / video_fps) : 1.0;
                double high_headroom = fps_ratio * 6.0 + stimulus_player.fps * 0.20;
                double low_headroom = fps_ratio * 2.0 + stimulus_player.fps * 0.05;
                int high_threshold = effective_target_frame + static_cast<int>(high_headroom);
                int low_threshold = effective_target_frame + static_cast<int>(low_headroom);
                int newest_frame = getNewestStimulusFrame(stimulus_player);

                if (decoder_active_now && newest_frame >= 0 &&
                    newest_frame > high_threshold) {
                    if (!stimulus_player.throttled)
                        if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus] throttling decode: newest=" << newest_frame
                                  << " high_threshold=" << high_threshold << std::endl;
                    decoder_requested = false;
                    stimulus_player.throttled = true;
                    stimulus_player.throttle_resume_frame = low_threshold;
                } else if (!decoder_active_now && stimulus_player.throttled) {
                    int resume_target = std::max(low_threshold, stimulus_player.throttle_resume_frame);
                    if (newest_frame <= resume_target) {
                        if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus] resuming decode: newest=" << newest_frame
                                  << " resume_target=" << resume_target << std::endl;
                        decoder_requested = true;
                        stimulus_player.throttled = false;
                        stimulus_player.throttle_resume_frame = -1;
                    } else {
                        decoder_requested = false;
                    }
                }
            } else {
                stimulus_player.throttled = false;
                stimulus_player.throttle_resume_frame = -1;
                decoder_requested = false;
            }

            if (!decoder_requested && target_stimulus_frame >= 0) {
                int candidate_index =
                    findStimulusBuffer(stimulus_player, target_stimulus_frame);
                int candidate_frame = -1;
                if (candidate_index >= 0 && stimulus_player.display_buffer) {
                    candidate_frame =
                        stimulus_player.display_buffer[candidate_index].frame_number;
                }
                const bool candidate_is_close =
                    isStimulusFrameClose(candidate_frame, target_stimulus_frame);
                if (!candidate_is_close) {
                    decoder_requested = true;
                }
            }

            if (decoder_requested != last_decoder_logged) {
                if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus] decoder_should_run="
                          << (decoder_requested ? "true" : "false")
                          << " (play=" << (ps.play_video ? "true" : "false")
                          << " pause_seeked=" << (ps.pause_seeked ? "true" : "false")
                          << ")" << std::endl;
                last_decoder_logged = decoder_requested;
            }
            window_need_decoding[stimulus_player.window_name].store(decoder_requested);

            if (stimulus_visible) {
                bool mapping_available =
                    zarr_loaded && zarr_loader.hasStimulusAlignment();

                if (mapping_available && target_stimulus_frame >= 0 &&
                    target_stimulus_frame != stimulus_player.last_displayed_frame) {
                    int buffer_index =
                        findStimulusBuffer(stimulus_player, target_stimulus_frame);
                    if (buffer_index != -1) {
                        const int buffered_frame =
                            stimulus_player.display_buffer[buffer_index].frame_number;
                        const bool buffered_frame_is_close =
                            isStimulusFrameClose(buffered_frame,
                                                 target_stimulus_frame);
                        if (buffered_frame_is_close) {
                            uploadStimulusFrameToTexture(stimulus_player, buffer_index);
                            stimulus_player.last_displayed_frame = buffered_frame;
                            if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus] uploaded frame "
                                      << buffered_frame
                                      << " for target " << target_stimulus_frame
                                      << " (buffer " << buffer_index << ")"
                                      << std::endl;
                        }
                    }
                }

                ImVec2 avail = ImGui::GetContentRegionAvail();
                float aspect = (stimulus_player.width > 0 && stimulus_player.height > 0)
                                   ? static_cast<float>(stimulus_player.height) /
                                         static_cast<float>(stimulus_player.width)
                                   : 1.0f;
                float display_width = avail.x;
                float display_height = display_width * aspect;
                if (display_height > avail.y && avail.y > 0.0f) {
                    display_height = avail.y;
                    display_width = display_height / std::max(aspect, 1e-3f);
                }
                if (display_width <= 0.0f || display_height <= 0.0f) {
                    display_width = static_cast<float>(stimulus_player.width);
                    display_height = static_cast<float>(stimulus_player.height);
                }

                if (stimulus_player.last_displayed_frame >= 0) {
                    ImGui::Image((ImTextureID)(intptr_t)stimulus_player.texture,
                                 ImVec2(display_width, display_height));
                } else {
                    ImGui::Dummy(ImVec2(display_width, display_height));
                    ImGui::TextUnformatted("Waiting for stimulus frame...");
                }

                ImGui::Separator();
                const bool displayed_frame_is_close =
                    isStimulusFrameClose(stimulus_player.last_displayed_frame,
                                         target_stimulus_frame);
                if (mapping_available && target_stimulus_frame >= 0 &&
                    !displayed_frame_is_close) {
                    ImGui::TextUnformatted("Awaiting stimulus frame decode...");
                }
                if (!mapping_available) {
                    ImGui::TextUnformatted("Stimulus alignment not available.");
                } else if (target_stimulus_frame < 0) {
                    ImGui::Text("Stimulus inactive for camera frame %d", current_frame_num);
                } else {
                    ImGui::Text("Camera frame %d -> Stimulus frame %d",
                                current_frame_num, target_stimulus_frame);
                }
                ImGui::Text("Latest decoded stimulus frame: %d",
                            latest_decoded_frame[stimulus_player.window_name].load());
                ImGui::Separator();
                ImGui::Text("Seek id: %lu  State: %s",
                            seek_progress.seek_id,
                            seekStateName(seek_progress.state));
                if (seek_progress.state == SeekState::WaitingCameras) {
                    ImGui::Text("Cameras: %d / %d settled",
                                seek_progress.cameras_settled,
                                seek_progress.cameras_total);
                }
                if (seek_progress.state == SeekState::WaitingStimulus) {
                    ImGui::Text("Stimulus target: %d",
                                seek_progress.target_stimulus_frame);
                }
                ImGui::Separator();
                ImGui::Text("Stimulus video: %s", stimulus_player.video_path.c_str());
                ImGui::Text("Resolution: %u x %u  |  %.2f fps",
                            stimulus_player.width, stimulus_player.height, stimulus_player.fps);
            }
            ImGui::End();
            frame_stimulus_window_ui_ms += durationMs(
                std::chrono::steady_clock::now() - stimulus_window_ui_start);

            ImGui::SetNextWindowSize(ImVec2(500.0f, 440.0f), ImGuiCond_FirstUseEver);
            const auto stimulus_buffer_window_ui_start =
                std::chrono::steady_clock::now();
            if (ImGui::Begin("Stimulus Frames in Buffer")) {
                struct StimulusBufferListItem {
                    int slot = -1;
                    int frame = -1;
                };
                std::vector<StimulusBufferListItem> stimulus_buffer_items;
                if (stimulus_player.display_buffer && stimulus_player.buffer_size > 0) {
                    stimulus_buffer_items.reserve(stimulus_player.buffer_size);
                    for (int i = 0; i < stimulus_player.buffer_size; ++i) {
                        const auto& slot = stimulus_player.display_buffer[i];
                        if (slot.available_to_write || slot.frame_number < 0) {
                            continue;
                        }
                        stimulus_buffer_items.push_back({i, slot.frame_number});
                    }
                }
                std::sort(stimulus_buffer_items.begin(), stimulus_buffer_items.end(),
                          [](const StimulusBufferListItem& a,
                             const StimulusBufferListItem& b) {
                              if (a.frame == b.frame) {
                                  return a.slot < b.slot;
                              }
                              return a.frame < b.frame;
                          });

                ImGui::Text("Valid frames: %zu / %d", stimulus_buffer_items.size(),
                            std::max(0, stimulus_player.buffer_size));
                if (target_stimulus_frame >= 0) {
                    ImGui::Text("Target stimulus frame: %d", target_stimulus_frame);
                } else {
                    ImGui::TextDisabled("Target stimulus frame: (none)");
                }
                ImGui::Text("Last displayed frame: %d", stimulus_player.last_displayed_frame);
                ImGui::Text("Latest decoded frame: %d",
                            latest_decoded_frame[stimulus_player.window_name].load());
                ImGui::Separator();

                int selected_item = -1;
                if (target_stimulus_frame >= 0) {
                    int best_distance = std::numeric_limits<int>::max();
                    int best_frame = std::numeric_limits<int>::min();
                    for (int i = 0; i < static_cast<int>(stimulus_buffer_items.size()); ++i) {
                        const auto& item = stimulus_buffer_items[i];
                        if (item.frame == target_stimulus_frame) {
                            selected_item = i;
                            break;
                        }
                        const int distance =
                            std::abs(item.frame - target_stimulus_frame);
                        if (distance < best_distance ||
                            (distance == best_distance && item.frame > best_frame)) {
                            best_distance = distance;
                            best_frame = item.frame;
                            selected_item = i;
                        }
                    }
                }

                if (stimulus_buffer_items.empty()) {
                    ImGui::TextDisabled("No decoded stimulus frames currently buffered.");
                } else {
                    ImGui::TextDisabled("Click an item to upload that buffered frame.");
                    for (int i = 0; i < static_cast<int>(stimulus_buffer_items.size()); ++i) {
                        const auto& item = stimulus_buffer_items[i];
                        char label[128];
                        if (target_stimulus_frame >= 0) {
                            const int delta = item.frame - target_stimulus_frame;
                            snprintf(label, sizeof(label),
                                     "Frame %d (slot %d, delta %+d)",
                                     item.frame, item.slot, delta);
                        } else {
                            snprintf(label, sizeof(label), "Frame %d (slot %d)",
                                     item.frame, item.slot);
                        }
                        if (ImGui::Selectable(label, selected_item == i)) {
                            uploadStimulusFrameToTexture(stimulus_player, item.slot);
                            stimulus_player.last_displayed_frame = item.frame;
                            if (crimson_seek_debug_logs_enabled()) std::cout << "[Stimulus] debug upload frame "
                                      << item.frame << " (slot " << item.slot
                                      << ")" << std::endl;
                        }
                    }
                }
            }
            ImGui::End();
            frame_stimulus_buffer_window_ui_ms += durationMs(
                std::chrono::steady_clock::now() - stimulus_buffer_window_ui_start);
        }

        if (plot_keypoints_flag) {
            const auto keypoints_window_ui_start =
                std::chrono::steady_clock::now();
            if (ImGui::Begin("Keypoints")) {

                const float TEXT_BASE_HEIGHT =
                    ImGui::GetTextLineHeightWithSpacing();
                {
                    const int rows_count = scene->num_cams;
                    const int columns_count = skeleton->num_nodes + 1;

                    static ImGuiTableFlags table_flags =
                        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingFixedFit |
                        ImGuiTableFlags_BordersOuter |
                        ImGuiTableFlags_BordersInnerH |
                        ImGuiTableFlags_Hideable | ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_HighlightHoveredColumn;

                    if (ImGui::BeginTable(
                            "table_angled_headers", columns_count, table_flags,
                            ImVec2(0.0f, TEXT_BASE_HEIGHT * 12))) {
                        ImGui::TableSetupColumn(
                            "Name", ImGuiTableColumnFlags_NoHide |
                                        ImGuiTableColumnFlags_NoReorder);
                        for (int column = 1; column < columns_count; column++)
                            ImGui::TableSetupColumn(
                                skeleton->node_names[column - 1].c_str(),
                                ImGuiTableColumnFlags_AngledHeader |
                                    ImGuiTableColumnFlags_WidthFixed);
                        ImGui::TableSetupScrollFreeze(1, 2);

                        ImGui::
                            TableAngledHeadersRow(); // Draw angled headers
                                                     // for all columns with
                                                     // the
                                                     // ImGuiTableColumnFlags_AngledHeader
                                                     // flag.
                        ImGui::TableHeadersRow(); // Draw remaining headers
                                                  // and allow access to
                                                  // context-menu and other
                                                  // functions.

                        for (int row = 0; row < rows_count; row++) {
                            ImGui::PushID(row);
                            ImGui::TableNextRow();

                            if (is_view_focused[row] && keypoints_find) {
                                ImU32 row_bg_color = ImGui::GetColorU32(
                                    ImVec4(0.7f, 0.3f, 0.3f, 0.65f));
                                ImGui::TableSetBgColor(
                                    ImGuiTableBgTarget_RowBg0, row_bg_color);
                            }

                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::Text("%s", camera_names[row].c_str());
                            for (int column = 1; column < columns_count;
                                 column++)
                                if (ImGui::TableSetColumnIndex(column)) {
                                    if (keypoints_find) {
                                        ImVec4 node_color;
                                        if (keypoints_map[current_frame_num]
                                                ->active_id[row] ==
                                            column - 1) {
                                            node_color = (ImVec4)ImColor::HSV(
                                                0.8, 1.0f, 1.0f);
                                        } else {
                                            if (keypoints_map[current_frame_num]
                                                    ->keypoints2d[row]
                                                                 [column - 1]
                                                    .is_labeled) {
                                                node_color =
                                                    skeleton
                                                        ->node_colors[column -
                                                                      1];
                                                node_color.w = 0.9;
                                            }
                                        }

                                        if (keypoints_map[current_frame_num]
                                                ->keypoints2d[row][column - 1]
                                                .is_triangulated) {
                                            ImGui::TextColored(
                                                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                                "T");
                                        }

                                        ImU32 cell_bg_color =
                                            ImGui::GetColorU32(node_color);
                                        ImGui::TableSetBgColor(
                                            ImGuiTableBgTarget_CellBg,
                                            cell_bg_color);
                                    }
                                }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
            }
            ImGui::End();
            frame_keypoints_window_ui_ms += durationMs(
                std::chrono::steady_clock::now() - keypoints_window_ui_start);
        }

        if (plot_keypoints_flag) {
            const auto labeling_tool_ui_start =
                std::chrono::steady_clock::now();
            if (ImGui::Begin("Labeling Tool")) {

                if (scene->num_cams > 1) {
                    bool keypoint_triangulated_all = true;
#if CRIMSON_ENABLE_SFM
                    constexpr bool triangulation_supported = true;
#else
                    constexpr bool triangulation_supported = false;
#endif
                    if (keypoints_find) {
                        for (int i = 0; i < scene->num_cams; i++) {
                            for (int j = 0; j < skeleton->num_nodes; j++) {
                                if (!keypoints_map.at(current_frame_num)
                                         ->keypoints2d[i][j]
                                         .is_triangulated) {
                                    keypoint_triangulated_all = false;
                                }
                            }
                        }
                    } else {
                        keypoint_triangulated_all = false;
                    }

                    bool enabled = keypoints_find && triangulation_supported;
                    bool apply_color =
                        triangulation_supported && !keypoint_triangulated_all &&
                        enabled;
                    if (apply_color) {
                        ImGui::PushStyleColor(
                            ImGuiCol_Button,
                            (ImVec4)ImColor::HSV(0.8, 1.0f, 1.0f));
                        ImGui::PushStyleColor(
                            ImGuiCol_ButtonHovered,
                            (ImVec4)ImColor::HSV(0.8, 0.9f, 0.8f));
                        ImGui::PushStyleColor(
                            ImGuiCol_ButtonActive,
                            (ImVec4)ImColor::HSV(0.8, 0.9f, 0.5f));
                    }

                    ImGui::BeginDisabled(!enabled);
                    if (ImGui::Button("Triangulate")) {
                        reprojection(keypoints_map.at(current_frame_num),
                                     skeleton.get(), camera_params, scene);
                    }
                    ImGui::EndDisabled();

                    if (apply_color) {
                        ImGui::PopStyleColor(3);
                    }

                    if (!triangulation_supported) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("SFM disabled in this build");
                    }

                    if (enabled) {
                        if (ImGui::IsKeyPressed(ImGuiKey_T,
                                                false)) // triangulate
                        {
                            reprojection(keypoints_map.at(current_frame_num),
                                         skeleton.get(), camera_params, scene);
                        }
                    }
                }

                if (ImGui::Button("Update keypoints working directory")) {
                    IGFD::FileDialogConfig config;
                    config.countSelectionMax = 1;
                    config.path = root_dir;
                    config.flags = ImGuiFileDialogFlags_Modal;
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "ChooseKeypointsFolder",
                        "Choose keypoints working directory", nullptr, config);
                }
                ImGui::SameLine();
                ImGui::Text("%s", keypoints_root_folder.c_str());

                if (ImGui::Button("Save Labeled Data") ||
                    (ImGui::GetIO().KeyCtrl &&
                     ImGui::IsKeyPressed(ImGuiKey_S, false))) {
                    save_keypoints(keypoints_map, skeleton.get(),
                                   keypoints_root_folder, scene->num_cams,
                                   camera_names, &input_is_imgs, imgs_names);
                    last_saved = time(NULL);
                }
                if (last_saved != static_cast<std::time_t>(-1)) {
                    ImGui::SameLine();
                    ImGui::Text("Last saved: %s", ctime(&last_saved));
                }

                static bool load_old_format = false;
                if (ImGui::Button("Load Most Recent Labels")) {
                    free_all_keypoints(keypoints_map, scene);
                    if (load_old_format) {
                        if (load_keypoints_depreciated(
                                keypoints_map, skeleton.get(), keypoints_root_folder,
                                scene, camera_names, error_message)) {
                            free_all_keypoints(keypoints_map, scene);
                            show_error = true;
                        }

                    } else {
                        std::string most_recent_folder;
                        if (find_most_recent_labels(keypoints_root_folder,
                                                    most_recent_folder,
                                                    error_message)) {
                            show_error = true;
                        } else {
                            if (load_keypoints(most_recent_folder,
                                               keypoints_map, skeleton.get(), scene,
                                               camera_names, error_message)) {
                                free_all_keypoints(keypoints_map, scene);
                                show_error = true;
                            }
                        }
                    }
                }
                ImGui::SameLine();
                ImGui::Checkbox("Old format", &load_old_format);

                if (ImGui::Button("Load From Selected")) {
                    IGFD::FileDialogConfig config;
                    config.countSelectionMax = 1;
                    config.path = keypoints_root_folder;
                    config.flags = ImGuiFileDialogFlags_Modal;
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "LoadFromSelected", "Load from selected", nullptr,
                        config);
                }

                auto upper_it = keypoints_map.upper_bound(current_frame_num);
                if (upper_it == keypoints_map.end()) {
                    upper_it = keypoints_map.begin();
                }

                ImGui::Separator();
                ImGui::Text("Next labeled frame : %d", (*upper_it).first);
                if (ImGui::Button("Jump to Next Labeled Frame")) {
                    seekToFrame((*upper_it).first, true);
                }
                ImGui::Text("Total labeled frames : %zu", keypoints_map.size());
            }
            ImGui::End();
            frame_labeling_tool_ui_ms += durationMs(
                std::chrono::steady_clock::now() - labeling_tool_ui_start);
        }

        static bool s_timeline_scrolling_enabled = false;
        static float s_timeline_window_half_span_s = 5.0f;
        static bool s_timeline_scrolling_prev = false;

        // Stimulus Event Timeline Window
        if (zarr_loaded) {
            const auto stimulus_timeline_ui_start =
                std::chrono::steady_clock::now();
            if (ImGui::Begin("Stimulus Event Timeline")) {
                auto timeline = zarr_loader.getStimulusEventTimeline();
                static size_t last_logged_timeline_count = std::numeric_limits<size_t>::max();
                if (timeline.size() != last_logged_timeline_count) {
                    size_t missing_camera = 0;
                    for (const auto& evt : timeline) {
                        if (evt.camera_frame_id < 0) {
                            ++missing_camera;
                        }
                    }
                    std::cout << "  [StimulusTimeline] Entries=" << timeline.size()
                              << ", missing_camera_ids=" << missing_camera << std::endl;
                    const size_t preview = std::min<size_t>(timeline.size(), 5);
                    for (size_t i = 0; i < preview; ++i) {
                        const auto& evt = timeline[i];
                        std::cout << "    [" << i << "] stim_frame=" << evt.stimulus_frame_num
                                  << ", cam_frame=" << evt.camera_frame_id
                                  << ", type=" << evt.event_type_id
                                  << ", label='" << evt.label << "'" << std::endl;
                    }
                    last_logged_timeline_count = timeline.size();
                }
                if (timeline.empty()) {
                    ImGui::TextUnformatted("No stimulus events found.");
                } else {
                    // Helper function to generate consistent colors for event types
                    auto getEventTypeColor = [](int32_t event_type_id) -> ImVec4 {
                        // Use hash to generate consistent color for each event type
                        uint32_t hash = static_cast<uint32_t>(event_type_id) * 2654435761u;
                        float h = (hash % 360) / 360.0f;  // Hue
                        float s = 0.7f + 0.25f * ((hash >> 8) % 100) / 100.0f;  // Saturation 0.7-0.95
                        float v = 0.8f + 0.2f * ((hash >> 16) % 100) / 100.0f;  // Value 0.8-1.0

                        // Convert HSV to RGB
                        float c = v * s;
                        float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
                        float m = v - c;
                        float r, g, b;
                        if (h < 1.0f/6.0f) { r = c; g = x; b = 0; }
                        else if (h < 2.0f/6.0f) { r = x; g = c; b = 0; }
                        else if (h < 3.0f/6.0f) { r = 0; g = c; b = x; }
                        else if (h < 4.0f/6.0f) { r = 0; g = x; b = c; }
                        else if (h < 5.0f/6.0f) { r = x; g = 0; b = c; }
                        else { r = c; g = 0; b = x; }
                        return ImVec4(r + m, g + m, b + m, 0.9f);
                    };

                    // Collect unique event types and create filter state
                    static std::unordered_map<int32_t, bool> event_type_filter;
                    static bool filter_initialized = false;
                    std::unordered_map<int32_t, std::string> event_type_labels;
                    for (const auto& evt : timeline) {
                        if (event_type_labels.find(evt.event_type_id) == event_type_labels.end()) {
                            // Extract just the event type name (before any " - " context)
                            std::string type_name = evt.label;
                            size_t dash_pos = type_name.find(" - ");
                            if (dash_pos != std::string::npos) {
                                type_name = type_name.substr(0, dash_pos);
                            }
                            event_type_labels[evt.event_type_id] = type_name;

                            if (!filter_initialized) {
                                event_type_filter[evt.event_type_id] = true;  // All enabled by default
                            }
                        }
                    }
                    filter_initialized = true;

                    static int selected_event_idx = -1;
                    if (selected_event_idx >= static_cast<int>(timeline.size())) {
                        selected_event_idx = -1;
                    }

                    auto resolveTimelineTargetFrame =
                        [&](const auto& evt) -> int32_t {
                        if (evt.stimulus_frame_num >= 0) {
                            if (auto mapped = zarr_loader.getCameraFrameForStimulusFrame(
                                    evt.stimulus_frame_num, true)) {
                                return *mapped;
                            }
                        }
                        if (evt.camera_frame_id >= 0) {
                            return evt.camera_frame_id;
                        }
                        return evt.stimulus_frame_num;
                    };

                    std::vector<double> x_values(timeline.size());
                    std::vector<double> y_values(timeline.size());
                    std::vector<int32_t> display_frames(timeline.size());
                    for (size_t i = 0; i < timeline.size(); ++i) {
                        const auto& evt = timeline[i];
                        int32_t frame = resolveTimelineTargetFrame(evt);
                        display_frames[i] = frame;
                        int32_t clamped = frame >= 0 ? frame : 0;
                        if (video_fps > 0.0) {
                            x_values[i] = static_cast<double>(clamped) / video_fps;
                        } else {
                            x_values[i] = static_cast<double>(clamped);
                        }
                        y_values[i] = 0.0;  // All events on same line
                    }

                    size_t timeline_signature = timeline.size();
                    if (!timeline.empty()) {
                        auto signature_value = [&](int32_t frame) -> size_t {
                            return static_cast<size_t>(std::max(frame, 0));
                        };
                        timeline_signature = timeline_signature * 1315423911u +
                                             signature_value(display_frames.front());
                        timeline_signature = timeline_signature * 2654435761u +
                                             signature_value(display_frames.back());
                    }
                    static size_t cached_timeline_signature = 0;

                    double default_max_time =
                        (video_fps > 0.0)
                            ? static_cast<double>(std::max<size_t>(1, zarr_loader.getTotalFrames())) / video_fps
                            : static_cast<double>(std::max<size_t>(1, zarr_loader.getTotalFrames()));
                    for (int32_t frame : display_frames) {
                        if (frame >= 0) {
                            double candidate = (video_fps > 0.0)
                                                   ? static_cast<double>(frame + 1) / video_fps
                                                   : static_cast<double>(frame + 1);
                            default_max_time = std::max(default_max_time, candidate);
                        }
                    }
                    ImGui::Spacing();
                    ImGui::Checkbox("Scrolling Window (±s)##stimulus", &s_timeline_scrolling_enabled);
                    if (s_timeline_scrolling_enabled) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::DragFloat("Half-span##stimulus_window_span",
                                             &s_timeline_window_half_span_s,
                                             0.1f,
                                             0.5f,
                                             60.0f,
                                             "%.1f s")) {
                            s_timeline_window_half_span_s = std::max(0.1f, s_timeline_window_half_span_s);
                        } else {
                            s_timeline_window_half_span_s = std::max(0.1f, s_timeline_window_half_span_s);
                        }
                    }

                    ImVec2 plot_size = ImVec2(ImGui::GetContentRegionAvail().x, 170.0f);
                    if (ImPlot::BeginPlot("##stimulus_timeline_plot", plot_size,
                                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                        ImPlot::SetupAxes("Time (s)", nullptr,
                                          ImPlotAxisFlags_NoHighlight,
                                          ImPlotAxisFlags_NoDecorations);
                        ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                                          ImPlotAxisFlags_NoDecorations |
                                              ImPlotAxisFlags_Lock);
                        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, 0.5,
                                                ImGuiCond_Always);

                        double current_time =
                            (video_fps > 0.0)
                                ? static_cast<double>(current_frame_num) / video_fps
                                : static_cast<double>(current_frame_num);

                        double timeline_min_time = 0.0;
                        double timeline_max_time = default_max_time;
                        if (!x_values.empty()) {
                            auto minmax =
                                std::minmax_element(x_values.begin(), x_values.end());
                            timeline_min_time = *minmax.first;
                            timeline_max_time =
                                std::max(default_max_time, *minmax.second);
                        }
                        if (timeline_max_time <= timeline_min_time) {
                            timeline_max_time = timeline_min_time + 0.5;
                        }

                        bool reset_limits =
                            (!s_timeline_scrolling_enabled &&
                             s_timeline_scrolling_prev) ||
                            (timeline_signature != cached_timeline_signature);

                        if (s_timeline_scrolling_enabled && current_time >= 0.0 &&
                            timeline_max_time > timeline_min_time) {
                            double half_span =
                                static_cast<double>(std::max(
                                    0.1f, s_timeline_window_half_span_s));
                            double window_min = current_time - half_span;
                            double window_max = current_time + half_span;
                            if (!x_values.empty()) {
                                window_min =
                                    std::max(window_min, timeline_min_time);
                                window_max =
                                    std::min(window_max, timeline_max_time);
                            } else {
                                window_min = std::max(window_min, 0.0);
                                window_max =
                                    std::min(window_max, timeline_max_time);
                            }
                            if (window_max - window_min < 0.1) {
                                double pad = std::max(0.1, half_span);
                                window_min = std::max(
                                    timeline_min_time, current_time - pad);
                                window_max = std::min(
                                    timeline_max_time, current_time + pad);
                                if (window_max <= window_min) {
                                    window_min = std::max(
                                        timeline_min_time,
                                        timeline_max_time - pad);
                                    window_max = timeline_max_time;
                                }
                            }
                            ImPlot::SetupAxisLimits(ImAxis_X1, window_min,
                                                    window_max, ImGuiCond_Always);
                            cached_timeline_signature = timeline_signature;
                        } else if (reset_limits) {
                            ImPlot::SetupAxisLimits(ImAxis_X1, timeline_min_time,
                                                    timeline_max_time,
                                                    ImGuiCond_Always);
                            cached_timeline_signature = timeline_signature;
                        }

                        // Plot events grouped by type with consistent colors
                        for (const auto& [event_type_id, type_label] : event_type_labels) {
                            // Skip filtered out event types
                            if (!event_type_filter[event_type_id]) {
                                continue;
                            }

                            // Collect events of this type
                            std::vector<double> type_x_values;
                            std::vector<double> type_y_values;
                            for (size_t i = 0; i < timeline.size(); ++i) {
                                if (timeline[i].event_type_id == event_type_id) {
                                    type_x_values.push_back(x_values[i]);
                                    type_y_values.push_back(y_values[i]);
                                }
                            }

                            if (!type_x_values.empty()) {
                                ImVec4 color = getEventTypeColor(event_type_id);
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6.0f, color, 1.5f,
                                                           ImVec4(0, 0, 0, 0));
                                ImPlot::PlotScatter(type_label.c_str(), type_x_values.data(),
                                                   type_y_values.data(),
                                                   static_cast<int>(type_x_values.size()));
                            }
                        }

                        double current_line_x[2] = {current_time, current_time};
                        double current_line_y[2] = {-1.0, 1.0};
                        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 2.0f);
                        ImPlot::PlotLine("Current Frame", current_line_x, current_line_y, 2);

                        int hovered_event_idx = -1;
                        constexpr float kSelectionRadiusPx = 12.0f;
                        if (ImPlot::IsPlotHovered()) {
                            ImVec2 mouse_pos = ImGui::GetIO().MousePos;
                            float best_distance = kSelectionRadiusPx;
                            for (size_t i = 0; i < x_values.size(); ++i) {
                                ImVec2 event_pixels =
                                    ImPlot::PlotToPixels(ImPlotPoint(x_values[i], y_values[i]));
                                float dx = mouse_pos.x - event_pixels.x;
                                float dy = mouse_pos.y - event_pixels.y;
                                float distance = std::sqrt(dx * dx + dy * dy);
                                if (distance < best_distance) {
                                    best_distance = distance;
                                    hovered_event_idx = static_cast<int>(i);
                                }
                            }

                            if (hovered_event_idx != -1 &&
                                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                selected_event_idx = hovered_event_idx;
                                const int target_frame = display_frames[hovered_event_idx];
                                if (target_frame >= 0) {
                                    seekToFrame(target_frame, true);
                                }
                            }
                        }

                        if (hovered_event_idx != -1) {
                            const auto& hovered_evt = timeline[hovered_event_idx];
                            const int32_t resolved_frame =
                                display_frames[hovered_event_idx];
                            const int32_t camera_frame = hovered_evt.camera_frame_id;
                            const int32_t stim_frame = hovered_evt.stimulus_frame_num;
                            if (resolved_frame >= 0) {
                                double event_time = x_values[hovered_event_idx];
                                if (camera_frame >= 0 &&
                                    camera_frame != resolved_frame) {
                                    ImGui::SetTooltip("Camera Frame %d (resolved)\nEvent Camera Frame %d\nStimulus Frame %d\nTime %.3f s\n%s",
                                                      resolved_frame,
                                                      camera_frame,
                                                      std::max(stim_frame, 0),
                                                      event_time,
                                                      hovered_evt.label.c_str());
                                } else if (stim_frame >= 0 &&
                                           stim_frame != resolved_frame) {
                                    ImGui::SetTooltip("Camera Frame %d\nStimulus Frame %d\nTime %.3f s\n%s",
                                                      resolved_frame,
                                                      stim_frame,
                                                      event_time,
                                                      hovered_evt.label.c_str());
                                } else {
                                    ImGui::SetTooltip("Camera Frame %d\nTime %.3f s\n%s",
                                                      resolved_frame,
                                                      event_time,
                                                      hovered_evt.label.c_str());
                                }
                            } else {
                            ImGui::SetTooltip("Frame %d\nTime %.3f s\n%s",
                                             std::max(stim_frame, 0),
                                             x_values[hovered_event_idx],
                                             hovered_evt.label.c_str());
                            }
                        }

                        if (selected_event_idx >= 0 &&
                            selected_event_idx < static_cast<int>(x_values.size())) {
                            double selected_x = x_values[selected_event_idx];
                            double selected_y = y_values[selected_event_idx];
                            ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond, 9.0f,
                                                       ImVec4(1.0f, 0.5f, 0.2f, 1.0f), 2.0f,
                                                       ImVec4(0, 0, 0, 0));
                            ImPlot::PlotScatter("Selected Event", &selected_x, &selected_y, 1);
                        }
                        ImPlot::EndPlot();
                    }

                    // Legend and Filter UI
                    ImGui::SeparatorText("Event Type Legend & Filter");
                    ImGui::BeginChild("##event_type_legend", ImVec2(0, 120), true);

                    // Add "Show All" / "Hide All" buttons
                    if (ImGui::SmallButton("Show All")) {
                        for (auto& [type_id, enabled] : event_type_filter) {
                            enabled = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Hide All")) {
                        for (auto& [type_id, enabled] : event_type_filter) {
                            enabled = false;
                        }
                    }

                    ImGui::Separator();

                    // Display legend with checkboxes for filtering
                    int col_count = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 250.0f));
                    if (ImGui::BeginTable("##legend_table", col_count, ImGuiTableFlags_SizingStretchSame)) {
                        int col_idx = 0;
                        for (const auto& [event_type_id, type_label] : event_type_labels) {
                            if (col_idx % col_count == 0) {
                                ImGui::TableNextRow();
                            }
                            ImGui::TableNextColumn();

                            // Push unique ID for this event type
                            ImGui::PushID(event_type_id);

                            ImVec4 color = getEventTypeColor(event_type_id);

                            // Color swatch
                            ImGui::PushStyleColor(ImGuiCol_Button, color);
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
                            ImGui::SmallButton("  ");
                            ImGui::PopStyleColor(3);

                            ImGui::SameLine();

                            // Checkbox for filtering
                            bool& enabled = event_type_filter[event_type_id];
                            ImGui::Checkbox(type_label.c_str(), &enabled);

                            ImGui::PopID();

                            col_idx++;
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndChild();

                    ImGui::SeparatorText("Event List");
                    ImGui::BeginChild("##stimulus_event_list", ImVec2(0, 200), true);
                    for (size_t i = 0; i < timeline.size(); ++i) {
                        const auto& evt = timeline[i];
                        int32_t resolved_frame = display_frames[i];
                        std::ostringstream row_label;
                        if (resolved_frame >= 0) {
                            row_label << "Cam " << resolved_frame;
                            if (evt.camera_frame_id >= 0 &&
                                evt.camera_frame_id != resolved_frame) {
                                row_label << " (Event Cam " << evt.camera_frame_id << ")";
                            }
                            if (evt.stimulus_frame_num >= 0 &&
                                evt.stimulus_frame_num != resolved_frame) {
                                row_label << " (Stim " << evt.stimulus_frame_num << ")";
                            }
                        } else {
                            row_label << "Stim " << evt.stimulus_frame_num;
                        }
                        row_label << "  " << evt.label;
                        ImGui::PushID(static_cast<int>(i));
                        bool is_selected = (selected_event_idx == static_cast<int>(i));
                        if (ImGui::Selectable(row_label.str().c_str(), is_selected)) {
                            selected_event_idx = static_cast<int>(i);
                            int target_frame = display_frames[i];
                            if (target_frame >= 0) {
                                seekToFrame(target_frame, true);
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    if (selected_event_idx >= 0 &&
                        selected_event_idx < static_cast<int>(timeline.size())) {
                        const auto& evt = timeline[selected_event_idx];
                        const int32_t resolved_frame = display_frames[selected_event_idx];
                        ImGui::Separator();
                        ImGui::Text("Selected Event:");
                        if (resolved_frame >= 0) {
                            ImGui::BulletText("Camera Frame: %d", resolved_frame);
                        }
                        if (evt.camera_frame_id >= 0 &&
                            evt.camera_frame_id != resolved_frame) {
                            ImGui::BulletText("Event Camera Frame: %d",
                                              evt.camera_frame_id);
                        }
                        if (evt.stimulus_frame_num >= 0) {
                            ImGui::BulletText("Stimulus Frame: %d", evt.stimulus_frame_num);
                        }
                        ImGui::BulletText("Type ID: %d", evt.event_type_id);
                        ImGui::BulletText("%s", evt.label.c_str());
                    }
                }
            }
            ImGui::End();
            frame_stimulus_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - stimulus_timeline_ui_start);
        }

        // Movement timeline windows
        if (zarr_loaded && zarr_loader.hasMovementData()) {
            auto renderMovementDatasetUI = [&](const char* combo_label) -> const ZarrDetectionData::MovementSeries* {
                size_t series_count = zarr_loader.getMovementSeriesCount();
                size_t selected_index = zarr_loader.getSelectedMovementSeriesIndex();
                const auto* selected_series = zarr_loader.getMovementSeries(selected_index);

                if (series_count > 1) {
                    std::ostringstream summary;
                    if (selected_series) {
                        summary << selected_series->category << "/" << selected_series->run_name
                                << " (track " << selected_series->track_id << ")";
                    } else {
                        summary << "Select dataset";
                    }
                    if (ImGui::BeginCombo(combo_label, summary.str().c_str())) {
                        for (size_t i = 0; i < series_count; ++i) {
                            const auto* series = zarr_loader.getMovementSeries(i);
                            if (!series) {
                                continue;
                            }
                            std::ostringstream label;
                            label << series->category << "/" << series->run_name
                                  << " (track " << series->track_id << ")";
                            bool is_selected = (i == selected_index);
                            if (ImGui::Selectable(label.str().c_str(), is_selected)) {
                                if (zarr_loader.selectMovementSeries(i)) {
                                    selected_index = i;
                                    selected_series = zarr_loader.getMovementSeries(i);
                                }
                            }
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else if (selected_series) {
                    ImGui::Text("Dataset: %s/%s (track %s)",
                                selected_series->category.c_str(),
                                selected_series->run_name.c_str(),
                                selected_series->track_id.c_str());
                }

                if (selected_series) {
                    if (!selected_series->detection_variant.empty() ||
                        !selected_series->source_detect_run.empty()) {
                        if (!selected_series->source_detect_run.empty()) {
                            ImGui::Text("Variant: %s | Source run: %s",
                                        selected_series->detection_variant.empty()
                                            ? "unknown"
                                            : selected_series->detection_variant.c_str(),
                                        selected_series->source_detect_run.c_str());
                        } else {
                            ImGui::Text("Variant: %s",
                                        selected_series->detection_variant.empty()
                                            ? "unknown"
                                            : selected_series->detection_variant.c_str());
                        }
                    }
                    if (selected_series->fps > 0.0 || selected_series->smoothing_seconds > 0.0) {
                        if (selected_series->fps > 0.0 && selected_series->smoothing_seconds > 0.0) {
                            ImGui::Text("FPS: %.2f | Smoothing: %.2f s",
                                        selected_series->fps,
                                        selected_series->smoothing_seconds);
                        } else if (selected_series->fps > 0.0) {
                            ImGui::Text("FPS: %.2f", selected_series->fps);
                        } else {
                            ImGui::Text("Smoothing: %.2f s", selected_series->smoothing_seconds);
                        }
                    }
                    if (selected_series->video_width > 0 && selected_series->video_height > 0) {
                        ImGui::Text("Camera size: %dx%d",
                                    selected_series->video_width,
                                    selected_series->video_height);
                    }
                }

                return selected_series;
            };

            const auto movement_timeline_ui_start =
                std::chrono::steady_clock::now();
            if (ImGui::Begin("Speed & Distance Timeline")) {
                const auto* selected_series = renderMovementDatasetUI("Dataset");

                const auto& time_data = zarr_loader.getMovementTimeSeconds();
                const auto& smoothed_speed = zarr_loader.getMovementSmoothedSpeedMm();
                const auto& instant_speed = zarr_loader.getMovementInstantaneousSpeedMm();
                const auto& distance_mm = zarr_loader.getMovementDistanceToTargetMm();
                const auto& heading_degrees = zarr_loader.getMovementHeadingDegrees();
                const auto& smoothed_heading_degrees = zarr_loader.getMovementSmoothedHeadingDegrees();
                const auto& heading_keypoint_success = zarr_loader.getMovementHeadingKeypointSuccess();
                const auto& heading_per_second_degrees = zarr_loader.getMovementHeadingPerSecondDegrees();
                const auto& heading_per_second_resultant = zarr_loader.getMovementHeadingPerSecondResultant();
                const auto& heading_per_second_time = zarr_loader.getMovementHeadingPerSecondTimeSeconds();
                const auto& frame_indices = zarr_loader.getMovementFrameIndices();
                static bool show_smoothed = true;
                static bool show_instantaneous = false;
                static bool show_vergence = true;
                static bool show_heading_raw = false;
                static bool show_heading_smoothed = true;
                static bool show_heading_per_second = false;

                bool smoothed_available = !smoothed_speed.empty();
                bool instant_available = !instant_speed.empty();
                bool distance_available = !distance_mm.empty();
                bool heading_sample_available = !heading_degrees.empty() || !smoothed_heading_degrees.empty();
                bool heading_per_second_available = !heading_per_second_degrees.empty() &&
                                                    !heading_per_second_time.empty() &&
                                                    heading_per_second_degrees.size() == heading_per_second_time.size();

                if (!selected_series || time_data.empty() ||
                    (!smoothed_available && !instant_available && !distance_available &&
                     !heading_sample_available && !heading_per_second_available)) {
                    ImGui::TextUnformatted("No movement data available.");
                } else {
                    if (!smoothed_available && show_smoothed) {
                        show_smoothed = false;
                    }
                    if (!instant_available && show_instantaneous) {
                        show_instantaneous = false;
                    }
                    if (!heading_sample_available) {
                        show_heading_raw = false;
                        show_heading_smoothed = false;
                    }
                    if (!heading_per_second_available) {
                        show_heading_per_second = false;
                    }

                    if (!zarr_loader.getMovementCategory().empty()) {
                        ImGui::Text("Movement Run: %s/%s | Track: %s",
                                    zarr_loader.getMovementCategory().c_str(),
                                    zarr_loader.getMovementRunName().c_str(),
                                    zarr_loader.getMovementTrackId().c_str());
                    } else {
                        ImGui::Text("Movement Run: %s | Track: %s",
                                    zarr_loader.getMovementRunName().c_str(),
                                    zarr_loader.getMovementTrackId().c_str());
                    }
                    ImGui::Text("Data points: %zu", time_data.size());

                    ImGui::Checkbox("Scrolling Window (±s)##movement",
                                    &s_timeline_scrolling_enabled);
                    if (s_timeline_scrolling_enabled) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(140.0f);
                        if (ImGui::DragFloat("Half-span##movement_window_span",
                                             &s_timeline_window_half_span_s,
                                             0.1f,
                                             0.5f,
                                             60.0f,
                                             "%.1f s")) {
                            s_timeline_window_half_span_s =
                                std::max(0.1f, s_timeline_window_half_span_s);
                        } else {
                            s_timeline_window_half_span_s =
                                std::max(0.1f, s_timeline_window_half_span_s);
                        }
                    }

                    ImGui::Checkbox("Show Smoothed Speed", &show_smoothed);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Instantaneous Speed", &show_instantaneous);

                    ImGui::SeparatorText("Heading Options");
                    ImGui::BeginDisabled(!heading_sample_available);
                    ImGui::Checkbox("Show Raw Heading", &show_heading_raw);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Smoothed Heading", &show_heading_smoothed);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!heading_per_second_available);
                    ImGui::Checkbox("Show Heading (per-second)", &show_heading_per_second);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!zarr_loader.hasEyeVergenceFrame());
                    ImGui::Checkbox("Show Vergence", &show_vergence);
                    ImGui::EndDisabled();

                    std::vector<double> time_plot;
                    std::vector<double> smoothed_plot;
                    std::vector<double> instant_plot;
                    time_plot.reserve(time_data.size());
                    smoothed_plot.reserve(smoothed_speed.size());
                    if (!instant_speed.empty()) {
                        instant_plot.reserve(instant_speed.size());
                    }

                    for (size_t i = 0; i < time_data.size(); ++i) {
                        time_plot.push_back(static_cast<double>(time_data[i]));
                        if (smoothed_available && i < smoothed_speed.size()) {
                            smoothed_plot.push_back(static_cast<double>(smoothed_speed[i]));
                        }
                        if (instant_available && i < instant_speed.size()) {
                            instant_plot.push_back(static_cast<double>(instant_speed[i]));
                        }
                    }

                    constexpr double kMmPerPlotUnit = 10.0;
                    std::vector<double> distance_time;
                    std::vector<double> distance_units;
                    double sum_distance = 0.0;
                    double max_distance_mm = 0.0;
                    double min_distance_mm = std::numeric_limits<double>::infinity();
                    size_t valid_distance_count = 0;

                    size_t distance_samples = std::min(time_data.size(), distance_mm.size());
                    distance_time.reserve(distance_samples);
                    distance_units.reserve(distance_samples);
                    for (size_t i = 0; i < distance_samples; ++i) {
                        float raw_distance = distance_mm[i];
                        if (!IsFiniteFloat(raw_distance)) {
                            continue;
                        }
                        double t = static_cast<double>(time_data[i]);
                        double value_mm = static_cast<double>(raw_distance);
                        distance_time.push_back(t);
                        distance_units.push_back(value_mm / kMmPerPlotUnit);
                        sum_distance += value_mm;
                        max_distance_mm = std::max(max_distance_mm, value_mm);
                        min_distance_mm = std::min(min_distance_mm, value_mm);
                        ++valid_distance_count;
                    }

                    std::vector<double> heading_time_raw;
                    std::vector<double> heading_raw_plot;
                    std::vector<double> heading_time_smoothed;
                    std::vector<double> heading_smoothed_plot;
                    heading_time_raw.reserve(std::min(time_data.size(), heading_degrees.size()));
                    heading_raw_plot.reserve(std::min(time_data.size(), heading_degrees.size()));
                    heading_time_smoothed.reserve(std::min(time_data.size(), smoothed_heading_degrees.size()));
                    heading_smoothed_plot.reserve(std::min(time_data.size(), smoothed_heading_degrees.size()));

                    auto headingSampleAllowed = [&](size_t index) {
                        if (!heading_keypoint_success.empty() && index < heading_keypoint_success.size()) {
                            return heading_keypoint_success[index] != 0;
                        }
                        return true;
                    };

                    double heading_y_min = std::numeric_limits<double>::infinity();
                    double heading_y_max = -std::numeric_limits<double>::infinity();
                    auto extend_heading_range = [&](double value) {
                        heading_y_min = std::min(heading_y_min, value);
                        heading_y_max = std::max(heading_y_max, value);
                    };

                    size_t raw_samples = std::min(time_data.size(), heading_degrees.size());
                    for (size_t i = 0; i < raw_samples; ++i) {
                        if (!headingSampleAllowed(i)) {
                            continue;
                        }
                        float heading_raw_val = heading_degrees[i];
                        if (!IsFiniteFloat(heading_raw_val)) {
                            continue;
                        }
                        double t = static_cast<double>(time_data[i]);
                        double v = static_cast<double>(heading_raw_val);
                        heading_time_raw.push_back(t);
                        heading_raw_plot.push_back(v);
                        extend_heading_range(v);
                    }

                    size_t smoothed_samples = std::min(time_data.size(), smoothed_heading_degrees.size());
                    for (size_t i = 0; i < smoothed_samples; ++i) {
                        if (!headingSampleAllowed(i)) {
                            continue;
                        }
                        float heading_smooth_val = smoothed_heading_degrees[i];
                        if (!IsFiniteFloat(heading_smooth_val)) {
                            continue;
                        }
                        double t = static_cast<double>(time_data[i]);
                        double v = static_cast<double>(heading_smooth_val);
                        heading_time_smoothed.push_back(t);
                        heading_smoothed_plot.push_back(v);
                        extend_heading_range(v);
                    }

                    std::vector<double> heading_per_second_time_plot;
                    std::vector<double> heading_per_second_plot;
                    std::vector<double> heading_per_second_resultant_plot;
                    if (heading_per_second_available) {
                        size_t per_samples = std::min(heading_per_second_degrees.size(), heading_per_second_time.size());
                        heading_per_second_time_plot.reserve(per_samples);
                        heading_per_second_plot.reserve(per_samples);
                        if (!heading_per_second_resultant.empty()) {
                            heading_per_second_resultant_plot.reserve(per_samples);
                        }
                        for (size_t i = 0; i < per_samples; ++i) {
                            float heading_val = heading_per_second_degrees[i];
                            float heading_time_val = heading_per_second_time[i];
                            if (!IsFiniteFloat(heading_val) || !IsFiniteFloat(heading_time_val)) {
                                continue;
                            }
                            double t = static_cast<double>(heading_time_val);
                            double v = static_cast<double>(heading_val);
                            heading_per_second_time_plot.push_back(t);
                            heading_per_second_plot.push_back(v);
                            extend_heading_range(v);
                            if (!heading_per_second_resultant.empty() && i < heading_per_second_resultant.size()) {
                                float resultant = heading_per_second_resultant[i];
                                if (IsFiniteFloat(resultant)) {
                                    heading_per_second_resultant_plot.push_back(static_cast<double>(resultant));
                                } else {
                                    heading_per_second_resultant_plot.push_back(std::numeric_limits<double>::quiet_NaN());
                                }
                            }
                        }
                        if (!heading_per_second_resultant.empty() &&
                            heading_per_second_resultant_plot.size() != heading_per_second_plot.size()) {
                            heading_per_second_resultant_plot.resize(
                                heading_per_second_plot.size(),
                                std::numeric_limits<double>::quiet_NaN());
                        }
                    }

                    std::vector<double> vergence_time_plot;
                    std::vector<double> vergence_value_plot;
                    size_t vergence_valid_count = 0;
                    double vergence_sum = 0.0;
                    double vergence_min = std::numeric_limits<double>::infinity();
                    double vergence_max = -std::numeric_limits<double>::infinity();
                    if (zarr_loader.hasEyeVergenceFrame()) {
                        const auto& verg_time = zarr_loader.getEyeVergenceFrameTimeSeconds();
                        const auto& verg_values = zarr_loader.getEyeVergenceFrameSignedDeg();
                        const auto& verg_valid = zarr_loader.getEyeVergenceFrameValidMask();
                        size_t count = std::min(verg_time.size(), verg_values.size());
                        vergence_time_plot.reserve(count);
                        vergence_value_plot.reserve(count);
                        for (size_t i = 0; i < count; ++i) {
                            double t = static_cast<double>(verg_time[i]);
                            double value = static_cast<double>(verg_values[i]);
                            bool valid = verg_valid.empty() || (i < verg_valid.size() && verg_valid[i] != 0);
                            vergence_time_plot.push_back(t);
                            if (valid && std::isfinite(value)) {
                                vergence_value_plot.push_back(value);
                                vergence_min = std::min(vergence_min, value);
                                vergence_max = std::max(vergence_max, value);
                                vergence_sum += value;
                                ++vergence_valid_count;
                            } else {
                                vergence_value_plot.push_back(std::numeric_limits<double>::quiet_NaN());
                            }
                        }
                    }

                    auto compute_heading_axis = [&](double& min_out, double& max_out) {
                        if (heading_y_min == std::numeric_limits<double>::infinity() ||
                            heading_y_max == -std::numeric_limits<double>::infinity()) {
                            min_out = -180.0;
                            max_out = 180.0;
                        } else {
                            min_out = heading_y_min;
                            max_out = heading_y_max;
                            double span = std::max(10.0, max_out - min_out);
                            double padding = std::max(5.0, span * 0.1);
                            min_out -= padding;
                            max_out += padding;
                            if (min_out >= max_out) {
                                min_out -= 1.0;
                                max_out += 1.0;
                            }
                        }
                    };
                    double vergence_axis_min = -60.0;
                    double vergence_axis_max = 60.0;
                    if (!vergence_time_plot.empty()) {
                        if (vergence_min == std::numeric_limits<double>::infinity() ||
                            vergence_max == -std::numeric_limits<double>::infinity()) {
                            vergence_axis_min = -60.0;
                            vergence_axis_max = 60.0;
                        } else {
                            double span = std::max(5.0, vergence_max - vergence_min);
                            double padding = span * 0.1;
                            vergence_axis_min = vergence_min - padding;
                            vergence_axis_max = vergence_max + padding;
                            if (vergence_axis_min >= vergence_axis_max) {
                                vergence_axis_min -= 1.0;
                                vergence_axis_max += 1.0;
                            }
                        }
                    }



                    auto computeCurrentTime = [&]() -> double {
                        if (current_frame_num < 0) {
                            return -1.0;
                        }
                        double resolved_time = -1.0;

                        if (!frame_indices.empty()) {
                            auto it = std::find(frame_indices.begin(), frame_indices.end(), current_frame_num);
                            if (it != frame_indices.end()) {
                                size_t idx = std::distance(frame_indices.begin(), it);
                                if (idx < time_data.size()) {
                                    resolved_time = static_cast<double>(time_data[idx]);
                                }
                            } else {
                                auto upper = std::lower_bound(frame_indices.begin(), frame_indices.end(), current_frame_num);
                                if (upper != frame_indices.end() && upper != frame_indices.begin()) {
                                    auto lower = upper - 1;
                                    size_t lower_idx = std::distance(frame_indices.begin(), lower);
                                    size_t upper_idx = std::distance(frame_indices.begin(), upper);

                                    if (upper_idx < time_data.size() && lower_idx < time_data.size()) {
                                        int32_t f0 = *lower;
                                        int32_t f1 = *upper;
                                        float t0 = time_data[lower_idx];
                                        float t1 = time_data[upper_idx];
                                        float delta_f = static_cast<float>(f1 - f0);
                                        if (delta_f != 0.0f) {
                                            float alpha = static_cast<float>(current_frame_num - f0) / delta_f;
                                            resolved_time = static_cast<double>(t0 + alpha * (t1 - t0));
                                        }
                                    }
                                } else if (upper == frame_indices.begin() && !time_data.empty()) {
                                    resolved_time = static_cast<double>(time_data.front());
                                } else if (upper == frame_indices.end() && !time_data.empty()) {
                                    resolved_time = static_cast<double>(time_data.back());
                                }
                            }
                        }

                        if (resolved_time < 0.0 && video_fps > 0.0) {
                            double estimated_time = static_cast<double>(current_frame_num) / video_fps;
                            if (!time_data.empty()) {
                                double min_time = static_cast<double>(time_data.front());
                                double max_time = static_cast<double>(time_data.back());
                                resolved_time = std::clamp(estimated_time, min_time, max_time);
                            } else {
                                resolved_time = estimated_time;
                            }
                        }

                        return resolved_time;
                    };

                    double current_time_line = computeCurrentTime();

                    double time_axis_min = time_plot.front();
                    double time_axis_max = time_plot.back();
                    if (time_axis_max <= time_axis_min) {
                        time_axis_max = time_axis_min + 0.5;
                    }
                    bool has_time_span = time_axis_max > time_axis_min;
                    double half_span_seconds = static_cast<double>(
                        std::max(0.1f, s_timeline_window_half_span_s));
                    bool use_time_window =
                        s_timeline_scrolling_enabled && current_time_line >= 0.0 &&
                        has_time_span;
                    double window_min = time_axis_min;
                    double window_max = time_axis_max;
                    if (use_time_window) {
                        window_min =
                            std::max(time_axis_min, current_time_line - half_span_seconds);
                        window_max =
                            std::min(time_axis_max, current_time_line + half_span_seconds);
                        if (window_max - window_min < 0.1) {
                            double pad = std::max(0.1, half_span_seconds);
                            window_min =
                                std::max(time_axis_min, current_time_line - pad);
                            window_max =
                                std::min(time_axis_max, current_time_line + pad);
                            if (window_max <= window_min) {
                                window_min = std::max(time_axis_min, time_axis_max - pad);
                                window_max = time_axis_max;
                            }
                        }
                    }
                    bool reset_time_axis =
                        (!s_timeline_scrolling_enabled && s_timeline_scrolling_prev);

                    auto apply_time_axis_limits = [&](ImGuiCond fallback_cond) {
                        if (use_time_window) {
                            ImPlot::SetupAxisLimits(ImAxis_X1, window_min, window_max,
                                                    ImGuiCond_Always);
                        } else if (reset_time_axis) {
                            ImPlot::SetupAxisLimits(ImAxis_X1, time_axis_min,
                                                    time_axis_max, ImGuiCond_Always);
                        } else {
                            ImPlot::SetupAxisLimits(ImAxis_X1, time_axis_min,
                                                    time_axis_max, fallback_cond);
                        }
                    };

                    ImVec2 subplot_size = ImVec2(-1, 920);
                    if (!time_plot.empty() &&
                        ImPlot::BeginSubplots("##movement_plots", 4, 1, subplot_size,
                                              ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle)) {
                        if (ImPlot::BeginPlot("##speed_plot")) {
                            ImPlot::SetupAxes(nullptr, "Speed (mm/s)");
                            apply_time_axis_limits(ImGuiCond_Once);

                            double max_speed = 0.0;
                            if (show_smoothed && !smoothed_plot.empty()) {
                                max_speed = std::max(max_speed, *std::max_element(smoothed_plot.begin(), smoothed_plot.end()));
                            }
                            if (show_instantaneous && !instant_plot.empty()) {
                                max_speed = std::max(max_speed, *std::max_element(instant_plot.begin(), instant_plot.end()));
                            }
                            double y_max_speed = (max_speed > 0.0) ? max_speed * 1.1 : 1.0;
                            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max_speed, ImGuiCond_Once);

                            if (show_smoothed && !smoothed_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Smoothed Speed",
                                                 time_plot.data(),
                                                 smoothed_plot.data(),
                                                 static_cast<int>(time_plot.size()));
                            }

                            if (show_instantaneous && !instant_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.5f, 0.2f, 0.6f), 1.0f);
                                ImPlot::PlotLine("Instantaneous Speed",
                                                 time_plot.data(),
                                                 instant_plot.data(),
                                                 static_cast<int>(instant_plot.size()));
                            }

                            if (current_time_line >= 0.0) {
                                ImPlotRect limits = ImPlot::GetPlotLimits();
                                double current_line_x[2] = {current_time_line, current_time_line};
                                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                                ImPlot::PlotLine("##current_time_speed", current_line_x, current_line_y, 2);
                            }

                            ImPlot::EndPlot();
                        }

                        if (ImPlot::BeginPlot("##heading_plot")) {
                            ImPlot::SetupAxes(nullptr, "Heading (deg)");
                            if (!time_plot.empty()) {
                                apply_time_axis_limits(ImGuiCond_Once);
                            }
                            double heading_axis_min;
                            double heading_axis_max;
                            compute_heading_axis(heading_axis_min, heading_axis_max);
                            ImPlot::SetupAxisLimits(ImAxis_Y1, heading_axis_min, heading_axis_max, ImGuiCond_Once);

                            bool drew_heading = false;
                            if (show_heading_raw && !heading_raw_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.35f, 0.2f, 0.9f), 1.5f);
                                ImPlot::PlotLine("Heading (raw)",
                                                 heading_time_raw.data(),
                                                 heading_raw_plot.data(),
                                                 static_cast<int>(heading_raw_plot.size()));
                                drew_heading = true;
                            }
                            if (show_heading_smoothed && !heading_smoothed_plot.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.7f, 0.4f, 1.0f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Heading (smoothed)",
                                                 heading_time_smoothed.data(),
                                                 heading_smoothed_plot.data(),
                                                 static_cast<int>(heading_smoothed_plot.size()));
                                drew_heading = true;
                            }

                            bool drew_per_second = show_heading_per_second && !heading_per_second_plot.empty();
                            if (drew_per_second) {
                                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.8f, 0.8f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Heading (per-second)",
                                                 heading_per_second_time_plot.data(),
                                                 heading_per_second_plot.data(),
                                                 static_cast<int>(heading_per_second_plot.size()));
                            }

                            bool drew_resultant = drew_per_second && !heading_per_second_resultant_plot.empty();
                            if (drew_resultant) {
                                ImPlot::SetupAxis(ImAxis_Y2, "Resultant");
                                ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 1.0, ImGuiCond_Once);
                                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                                ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.6f, 0.6f, 0.7f), 1.5f);
                                ImPlot::PlotLine("Resultant",
                                                 heading_per_second_time_plot.data(),
                                                 heading_per_second_resultant_plot.data(),
                                                 static_cast<int>(heading_per_second_resultant_plot.size()));
                                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                            }

                            if ((drew_heading || drew_per_second) && current_time_line >= 0.0) {
                                ImPlotRect limits = ImPlot::GetPlotLimits();
                                double current_line_x[2] = {current_time_line, current_time_line};
                                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                                ImPlot::PlotLine("##current_time_heading", current_line_x, current_line_y, 2);
                            }

                            ImPlot::EndPlot();
                        }

                        if (ImPlot::BeginPlot("##distance_plot")) {
                            ImPlot::SetupAxes("Time (s)", "Distance (10 mm)");
                            apply_time_axis_limits(ImGuiCond_Once);
                            double y_max_units = (max_distance_mm > 0.0)
                                                     ? (max_distance_mm * 1.1) / kMmPerPlotUnit
                                                     : 1.0;
                            if (y_max_units <= 0.0) {
                                y_max_units = 1.0;
                            }
                            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max_units, ImGuiCond_Once);

                            if (!distance_time.empty()) {
                                ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), 2.0f);
                                ImPlot::PlotLine("Distance to Target (10 mm)",
                                                 distance_time.data(),
                                                 distance_units.data(),
                                                 static_cast<int>(distance_time.size()));
                            }

                            if (current_time_line >= 0.0) {
                                ImPlotRect limits = ImPlot::GetPlotLimits();
                                double current_line_x[2] = {current_time_line, current_time_line};
                                double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                                ImPlot::PlotLine("##current_time_distance", current_line_x, current_line_y, 2);
                            }

                            ImPlot::EndPlot();
                        }

                        if (ImPlot::BeginPlot("##vergence_plot")) {
                            ImPlot::SetupAxes(nullptr, "Vergence (deg)");
                            if (!vergence_time_plot.empty()) {
                                ImPlot::SetupAxisLimits(ImAxis_Y1, vergence_axis_min, vergence_axis_max, ImGuiCond_Once);
                                if (show_vergence) {
                                    ImVec4 vergence_color = ImVec4(0.85f, 0.2f, 0.7f, 1.0f);
                                    ImPlot::SetNextLineStyle(vergence_color, 2.0f);
                                    ImPlot::PlotLine("Vergence",
                                                     vergence_time_plot.data(),
                                                     vergence_value_plot.data(),
                                                     static_cast<int>(vergence_time_plot.size()));
                                }
                            } else {
                                ImGui::TextUnformatted("No vergence data available.");
                            }
                            ImPlot::EndPlot();
                        }

                        ImPlot::EndSubplots();
                    }

                    ImGui::SeparatorText("Speed Statistics");
                    if (!smoothed_speed.empty()) {
                        float avg_speed = std::accumulate(smoothed_speed.begin(), smoothed_speed.end(), 0.0f) /
                                          smoothed_speed.size();
                        float max_spd = *std::max_element(smoothed_speed.begin(), smoothed_speed.end());
                        ImGui::BulletText("Average Speed (smoothed): %.2f mm/s", avg_speed);
                        ImGui::BulletText("Max Speed (smoothed): %.2f mm/s", max_spd);
                    } else {
                        ImGui::TextUnformatted("No smoothed speed data available.");
                    }

                    ImGui::SeparatorText("Distance Statistics");
                    if (valid_distance_count == 0) {
                        ImGui::TextUnformatted("No valid distance samples available.");
                    } else {
                        ImGui::Text("Valid points: %zu / %zu", valid_distance_count, distance_samples);
                        double avg_distance = sum_distance / static_cast<double>(valid_distance_count);
                        ImGui::BulletText("Average Distance: %.2f mm", avg_distance);
                        ImGui::BulletText("Max Distance: %.2f mm", max_distance_mm);
                        if (min_distance_mm < std::numeric_limits<double>::infinity()) {
                            ImGui::BulletText("Min Distance: %.2f mm", min_distance_mm);
                        }
                    }

                    ImGui::SeparatorText("Vergence Statistics");
                    if (vergence_time_plot.empty()) {
                        ImGui::TextUnformatted("No vergence data available.");
                    } else if (vergence_valid_count == 0) {
                        ImGui::TextUnformatted("No valid vergence samples.");
                    } else {
                        double avg_vergence = vergence_sum / static_cast<double>(vergence_valid_count);
                        ImGui::BulletText("Valid samples: %zu", vergence_valid_count);
                        ImGui::BulletText("Average Vergence: %.2f deg", avg_vergence);
                        if (vergence_min != std::numeric_limits<double>::infinity() &&
                            vergence_max != -std::numeric_limits<double>::infinity()) {
                            ImGui::BulletText("Range: %.2f .. %.2f deg", vergence_min, vergence_max);
                        }
                    }

                    ImGui::SeparatorText("Heading Statistics");
                    const std::vector<double>* heading_for_stats = nullptr;
                    if (!heading_smoothed_plot.empty()) {
                        heading_for_stats = &heading_smoothed_plot;
                    } else if (!heading_raw_plot.empty()) {
                        heading_for_stats = &heading_raw_plot;
                    }
                    if (heading_for_stats && !heading_for_stats->empty()) {
                        double sum_cos = 0.0;
                        double sum_sin = 0.0;
                        for (double deg : *heading_for_stats) {
                            double rad = deg * static_cast<double>(M_PI) / 180.0;
                            sum_cos += std::cos(rad);
                            sum_sin += std::sin(rad);
                        }
                        size_t count = heading_for_stats->size();
                        double mean_rad = std::atan2(sum_sin, sum_cos);
                        double mean_deg = mean_rad * 180.0 / static_cast<double>(M_PI);
                        double resultant =
                            std::sqrt(sum_cos * sum_cos + sum_sin * sum_sin) / static_cast<double>(count);
                        ImGui::Text("Valid samples: %zu", count);
                        ImGui::BulletText("Circular Mean: %.1f deg", mean_deg);
                        ImGui::BulletText("Mean Resultant Length: %.2f", resultant);
                    } else {
                        ImGui::TextUnformatted("No valid heading samples available.");
                    }
                    if (!heading_per_second_resultant_plot.empty()) {
                        size_t finite_count = 0;
                        double sum_res = 0.0;
                        for (double value : heading_per_second_resultant_plot) {
                            if (value == value) {
                                sum_res += value;
                                ++finite_count;
                            }
                        }
                        if (finite_count > 0) {
                            ImGui::BulletText("Average per-second resultant: %.2f",
                                              sum_res / static_cast<double>(finite_count));
                        }
                    }
                }
            }
            ImGui::End();
            frame_movement_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - movement_timeline_ui_start);
        }

        s_timeline_scrolling_prev = s_timeline_scrolling_enabled;

        if (ImGuiFileDialog::Instance()->Display("ChooseKeypointsFolder")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                keypoints_root_folder =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("LoadFromSelected")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                auto selected_folder =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
                free_all_keypoints(keypoints_map, scene);
                if (load_keypoints(selected_folder, keypoints_map, skeleton.get(),
                                   scene, camera_names, error_message)) {
                    free_all_keypoints(keypoints_map, scene);
                    show_error = true;
                }
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

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
