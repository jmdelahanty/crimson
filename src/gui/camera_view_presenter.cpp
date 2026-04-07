#include "gui/camera_view_presenter.h"

#include "NvCodecUtils.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {

struct Nv12PlaybackPresenter {
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLint luma_texture_location = -1;
    GLint chroma_texture_location = -1;
    GLint yuv_matrix_location = -1;
};

double durationMs(std::chrono::steady_clock::duration delta) {
    return std::chrono::duration<double, std::milli>(delta).count();
}

GLuint compileGlShader(GLenum shader_type, const char* source) {
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

Nv12PlaybackPresenter& getNv12PlaybackPresenter() {
    static Nv12PlaybackPresenter presenter;
    return presenter;
}

void ensureNv12PlaybackPresenter(Nv12PlaybackPresenter* presenter) {
    if (presenter == nullptr || presenter->program != 0) {
        return;
    }

    static const char* kVertexShader = R"GLSL(
        #version 130
        attribute vec2 aPos;
        attribute vec2 aUV;
        varying vec2 vUV;

        void main() {
            vUV = aUV;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )GLSL";

    static const char* kFragmentShader = R"GLSL(
        #version 130
        uniform sampler2D uLumaTex;
        uniform sampler2D uChromaTex;
        uniform mat3 uYuvToRgb;
        varying vec2 vUV;

        void main() {
            float y = texture2D(uLumaTex, vUV).r * 255.0 - 16.0;
            vec2 uv = texture2D(uChromaTex, vUV).rg * 255.0 -
                      vec2(128.0, 128.0);
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
        glGetProgramInfoLog(presenter->program, log_length, nullptr,
                            log.data());
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
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,  -1.0f, 1.0f, 0.0f,
        -1.0f, 1.0f,  0.0f, 1.0f, 1.0f,  1.0f,  1.0f, 1.0f,
    };

    glGenVertexArrays(1, &presenter->vao);
    glBindVertexArray(presenter->vao);
    glGenBuffers(1, &presenter->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, presenter->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void fillNv12YuvToRgbMatrix(int color_matrix, float matrix_out[9]) {
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

void presentNv12PboToTexture(const CameraResources& camera,
                             const PBO_CUDA& nv12_pbo,
                             GLuint destination_texture,
                             Nv12PlaybackPresenter* presenter,
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
                    GL_UNSIGNED_BYTE, reinterpret_cast<void*>(0));

    glBindTexture(GL_TEXTURE_2D, camera.nv12_chroma_texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, nv12_pitch_bytes / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chroma_width, chroma_height, GL_RG,
                    GL_UNSIGNED_BYTE,
                    reinterpret_cast<void*>(chroma_offset));

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

void applyTexturePreviewSampling(GLuint texture,
                                 int desired_preview_sampling_mode,
                                 int target_texture_width,
                                 int target_texture_height,
                                 int* applied_mode,
                                 bool regenerate_mips,
                                 CameraViewPresenterPerfMetrics* perf) {
    bind_texture(&texture);
    if (desired_preview_sampling_mode > 0) {
        const int max_dim = std::max(target_texture_width, target_texture_height);
        const int max_mip_level =
            max_dim > 0
                ? static_cast<int>(
                      std::floor(std::log2(static_cast<double>(max_dim))))
                : 0;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_mip_level);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS,
                        desired_preview_sampling_mode == 1 ? 1.0f : 2.0f);
        if (regenerate_mips) {
            const auto mip_start = std::chrono::steady_clock::now();
            glGenerateMipmap(GL_TEXTURE_2D);
            if (perf != nullptr) {
                perf->preview_resize_ms +=
                    durationMs(std::chrono::steady_clock::now() - mip_start);
            }
        }
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 0.0f);
    }
    unbind_texture();
    if (applied_mode != nullptr) {
        *applied_mode = desired_preview_sampling_mode;
    }
}

void uploadSurfaceToTexture(PBO_CUDA& surface_pbo,
                            GLuint surface_texture,
                            int* surface_preview_mode,
                            int target_texture_width,
                            int target_texture_height,
                            int desired_preview_sampling_mode,
                            CameraViewPresenterPerfMetrics* perf) {
    const auto texture_upload_start = std::chrono::steady_clock::now();
    GLuint upload_pbo = surface_pbo.pbo;
    bind_pbo(&upload_pbo);
    bind_texture(&surface_texture);
    upload_image_pbo_to_texture(target_texture_width, target_texture_height);
    unbind_pbo();
    unbind_texture();
    if (perf != nullptr) {
        perf->texture_upload_ms +=
            durationMs(std::chrono::steady_clock::now() - texture_upload_start);
    }
    applyTexturePreviewSampling(surface_texture, desired_preview_sampling_mode,
                                target_texture_width, target_texture_height,
                                surface_preview_mode,
                                desired_preview_sampling_mode > 0, perf);
}

void presentNv12SlotToTexture(CameraResources& camera,
                              const PictureBuffer& slot,
                              PBO_CUDA& surface_pbo,
                              GLuint destination_texture,
                              int* surface_preview_mode,
                              int target_texture_width,
                              int target_texture_height,
                              int desired_preview_sampling_mode,
                              CameraViewPresenterPerfMetrics* perf) {
    const auto pbo_copy_start = std::chrono::steady_clock::now();
    ck(cudaMemcpy(surface_pbo.cuda_buffer, slot.frame, slot.frame_bytes,
                  cudaMemcpyDeviceToDevice));
    if (perf != nullptr) {
        perf->pbo_copy_ms +=
            durationMs(std::chrono::steady_clock::now() - pbo_copy_start);
    }

    const auto texture_upload_start = std::chrono::steady_clock::now();
    presentNv12PboToTexture(
        camera, surface_pbo, destination_texture, &getNv12PlaybackPresenter(),
        slot.pitch_bytes > 0 ? slot.pitch_bytes
                             : static_cast<int>(camera.image_width),
        slot.color_matrix);
    if (perf != nullptr) {
        perf->texture_upload_ms +=
            durationMs(std::chrono::steady_clock::now() - texture_upload_start);
    }
    applyTexturePreviewSampling(destination_texture, desired_preview_sampling_mode,
                                target_texture_width, target_texture_height,
                                surface_preview_mode,
                                desired_preview_sampling_mode > 0, perf);
}

void clearCameraDisplayBuffer(CameraResources& camera) {
    const int clear_width =
        camera.display_texture_width > 0 ? camera.display_texture_width
                                         : static_cast<int>(camera.image_width);
    const int clear_height =
        camera.display_texture_height > 0 ? camera.display_texture_height
                                          : static_cast<int>(camera.image_height);
    const size_t bytes = static_cast<size_t>(clear_width) *
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
}

}  // namespace

int findCameraDisplaySlotForFrame(const render_scene& scene,
                                  int cam_idx,
                                  int target_frame,
                                  int preferred_slot) {
    if (scene.num_cams <= 0 || scene.size_of_buffer <= 0 || cam_idx < 0 ||
        cam_idx >= static_cast<int>(scene.num_cams)) {
        return -1;
    }

    auto slotIsValid = [&](int slot_idx) -> bool {
        return slot_idx >= 0 && slot_idx < static_cast<int>(scene.size_of_buffer) &&
               !scene.cameras[cam_idx].display_buffer[slot_idx].available_to_write &&
               scene.cameras[cam_idx].display_buffer[slot_idx].frame_number >= 0;
    };

    if (slotIsValid(preferred_slot)) {
        const int preferred_frame =
            scene.cameras[cam_idx].display_buffer[preferred_slot].frame_number;
        if (target_frame < 0 || preferred_frame == target_frame) {
            return preferred_slot;
        }
    }

    int exact_slot = -1;
    int best_lower_slot = -1;
    int best_lower_frame = std::numeric_limits<int>::min();
    int best_abs_slot = -1;
    int best_abs_distance = std::numeric_limits<int>::max();

    for (int i = 0; i < static_cast<int>(scene.size_of_buffer); ++i) {
        if (!slotIsValid(i)) {
            continue;
        }
        const int frame_num = scene.cameras[cam_idx].display_buffer[i].frame_number;
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
}

CameraViewPresenterResult presentCameraViewFrame(
    const CameraViewPresenterContext& context) {
    CameraViewPresenterResult result;
    result.resolved_current_frame_num = context.current_frame_num;

    if (context.scene == nullptr || context.view_idx < 0 ||
        context.view_idx >= static_cast<int>(context.scene->num_cams)) {
        return result;
    }

    auto& camera = context.scene->cameras[context.view_idx];
    result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;

    auto uploadCameraFrameToTexture = [&](int slot_index) -> int {
        if (slot_index < 0) {
            return -1;
        }

        auto& slot = camera.display_buffer[slot_index];
        const int source_frame_number = slot.frame_number;
        const bool preview_resize_active =
            context.preview_active && context.scene->use_cpu_buffer;
        const bool preview_sampling_active =
            context.preview_active && !context.scene->use_cpu_buffer;
        const bool direct_nv12_playback_present_active =
            context.play_video && context.lightweight_playback_renderer_active &&
            !context.scene->use_cpu_buffer && !context.yolo_detection &&
            slot.format == PictureBufferFormat::NV12;
        const int desired_preview_sampling_mode =
            preview_sampling_active ? context.preview_scale_mode : 0;
        const double preview_scale =
            preview_resize_active ? context.preview_scale : 1.0;
        const int target_texture_width =
            preview_resize_active
                ? std::max(1, static_cast<int>(std::lround(
                                  static_cast<double>(camera.image_width) *
                                  preview_scale)))
                : static_cast<int>(camera.image_width);
        const int target_texture_height =
            preview_resize_active
                ? std::max(1, static_cast<int>(std::lround(
                                  static_cast<double>(camera.image_height) *
                                  preview_scale)))
                : static_cast<int>(camera.image_height);
        const bool texture_shape_changed =
            camera.display_texture_width != target_texture_width ||
            camera.display_texture_height != target_texture_height;
        const bool preview_sampling_changed =
            camera.applied_preview_sampling_mode !=
            desired_preview_sampling_mode;
        const bool pipeline_playback_present =
            context.play_video && !context.scene->use_cpu_buffer &&
            !preview_resize_active && !context.yolo_detection &&
            !texture_shape_changed;
        const bool direct_nv12_pipeline_present =
            pipeline_playback_present && direct_nv12_playback_present_active;

        if (camera.texture_has_valid_frame &&
            camera.last_uploaded_frame == source_frame_number &&
            !texture_shape_changed) {
            const auto front_path_start = std::chrono::steady_clock::now();
            if (preview_sampling_changed) {
                applyTexturePreviewSampling(
                    camera.image_texture, desired_preview_sampling_mode,
                    target_texture_width, target_texture_height,
                    &camera.applied_preview_sampling_mode,
                    desired_preview_sampling_mode > 0, &result.perf);
            }
            result.perf.playback_front_path_ms +=
                durationMs(std::chrono::steady_clock::now() - front_path_start);
            result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
            return camera.last_uploaded_frame;
        }

        if (texture_shape_changed) {
            const auto texture_resize_start = std::chrono::steady_clock::now();
            render_resize_camera_texture(&camera, target_texture_width,
                                         target_texture_height);
            result.perf.texture_resize_ms += durationMs(
                std::chrono::steady_clock::now() - texture_resize_start);
            camera.last_uploaded_frame = -1;
            camera.texture_has_valid_frame = false;
            camera.playback_staging_frame = -1;
            camera.playback_staging_valid = false;
            camera.playback_staging_preview_sampling_mode = -1;
        }

        if (pipeline_playback_present && camera.texture_has_valid_frame) {
            const auto front_path_start = std::chrono::steady_clock::now();
            if (preview_sampling_changed) {
                applyTexturePreviewSampling(
                    camera.image_texture, desired_preview_sampling_mode,
                    target_texture_width, target_texture_height,
                    &camera.applied_preview_sampling_mode,
                    desired_preview_sampling_mode > 0, &result.perf);
            }
            result.perf.playback_front_path_ms +=
                durationMs(std::chrono::steady_clock::now() - front_path_start);

            if (!camera.playback_staging_valid ||
                camera.playback_staging_frame != source_frame_number) {
                const auto stage_total_start = std::chrono::steady_clock::now();
                const auto stage_upload_start =
                    std::chrono::steady_clock::now();
                if (direct_nv12_pipeline_present) {
                    presentNv12SlotToTexture(
                        camera, slot, camera.playback_staging_pbo,
                        camera.playback_staging_texture,
                        &camera.playback_staging_preview_sampling_mode,
                        target_texture_width, target_texture_height,
                        desired_preview_sampling_mode, &result.perf);
                } else {
                    if (slot.format == PictureBufferFormat::NV12) {
                        const auto convert_start =
                            std::chrono::steady_clock::now();
                        Nv12ToColor32<RGBA32>(
                            slot.frame,
                            slot.pitch_bytes > 0 ? slot.pitch_bytes
                                                 : static_cast<int>(camera.image_width),
                            camera.playback_staging_pbo.cuda_buffer,
                            4 * static_cast<int>(camera.image_width),
                            static_cast<int>(camera.image_width),
                            static_cast<int>(camera.image_height),
                            slot.color_matrix);
                        result.perf.display_convert_ms += durationMs(
                            std::chrono::steady_clock::now() - convert_start);
                    } else {
                        const auto pbo_copy_start =
                            std::chrono::steady_clock::now();
                        ck(cudaMemcpy(camera.playback_staging_pbo.cuda_buffer,
                                      slot.frame,
                                      camera.image_width * camera.image_height * 4,
                                      cudaMemcpyDeviceToDevice));
                        result.perf.pbo_copy_ms += durationMs(
                            std::chrono::steady_clock::now() - pbo_copy_start);
                    }
                    uploadSurfaceToTexture(
                        camera.playback_staging_pbo,
                        camera.playback_staging_texture,
                        &camera.playback_staging_preview_sampling_mode,
                        target_texture_width, target_texture_height,
                        desired_preview_sampling_mode, &result.perf);
                }
                result.perf.playback_stage_upload_ms += durationMs(
                    std::chrono::steady_clock::now() - stage_upload_start);
                camera.playback_staging_frame = source_frame_number;
                camera.playback_staging_valid = true;
                const double stage_total_ms =
                    durationMs(std::chrono::steady_clock::now() -
                               stage_total_start);
                result.perf.playback_stage_total_ms += stage_total_ms;
                result.perf.upload_ms += stage_total_ms;
                result.perf.upload_count++;
                result.swap_playback_surface_after_draw = true;
            }
            result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
            return camera.last_uploaded_frame;
        }

        const auto upload_start = std::chrono::steady_clock::now();
        if (preview_resize_active) {
            const cv::Mat full_rgba(camera.image_height, camera.image_width,
                                    CV_8UC4, slot.frame);
            camera.playback_preview_rgba_cpu.resize(
                static_cast<size_t>(target_texture_width) *
                static_cast<size_t>(target_texture_height) * 4);
            cv::Mat preview_rgba(target_texture_height, target_texture_width,
                                 CV_8UC4,
                                 camera.playback_preview_rgba_cpu.data());
            const auto preview_resize_start = std::chrono::steady_clock::now();
            cv::resize(full_rgba, preview_rgba,
                       cv::Size(target_texture_width, target_texture_height),
                       0.0, 0.0, cv::INTER_AREA);
            result.perf.preview_resize_ms += durationMs(
                std::chrono::steady_clock::now() - preview_resize_start);
            const auto pbo_copy_start = std::chrono::steady_clock::now();
            ck(cudaMemcpy(
                camera.pbo_cuda.cuda_buffer,
                camera.playback_preview_rgba_cpu.data(),
                static_cast<size_t>(target_texture_width) *
                    static_cast<size_t>(target_texture_height) * 4,
                cudaMemcpyHostToDevice));
            result.perf.pbo_copy_ms +=
                durationMs(std::chrono::steady_clock::now() - pbo_copy_start);
            result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
        } else if (context.scene->use_cpu_buffer) {
            const auto pbo_copy_start = std::chrono::steady_clock::now();
            ck(cudaMemcpy(camera.pbo_cuda.cuda_buffer, slot.frame,
                          camera.image_width * camera.image_height * 4,
                          cudaMemcpyHostToDevice));
            result.perf.pbo_copy_ms +=
                durationMs(std::chrono::steady_clock::now() - pbo_copy_start);
            result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
        } else {
            if (direct_nv12_playback_present_active) {
                presentNv12SlotToTexture(
                    camera, slot, camera.pbo_cuda, camera.image_texture,
                    &camera.applied_preview_sampling_mode, target_texture_width,
                    target_texture_height, desired_preview_sampling_mode,
                    &result.perf);
                result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
            } else if (slot.format == PictureBufferFormat::NV12) {
                const auto convert_start = std::chrono::steady_clock::now();
                Nv12ToColor32<RGBA32>(
                    slot.frame,
                    slot.pitch_bytes > 0 ? slot.pitch_bytes
                                         : static_cast<int>(camera.image_width),
                    camera.pbo_cuda.cuda_buffer,
                    4 * static_cast<int>(camera.image_width),
                    static_cast<int>(camera.image_width),
                    static_cast<int>(camera.image_height), slot.color_matrix);
                result.perf.display_convert_ms +=
                    durationMs(std::chrono::steady_clock::now() - convert_start);
            } else {
                const auto pbo_copy_start = std::chrono::steady_clock::now();
                ck(cudaMemcpy(camera.pbo_cuda.cuda_buffer, slot.frame,
                              camera.image_width * camera.image_height * 4,
                              cudaMemcpyDeviceToDevice));
                result.perf.pbo_copy_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               pbo_copy_start);
            }
            result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
        }

        if (!direct_nv12_playback_present_active) {
            uploadSurfaceToTexture(
                camera.pbo_cuda, camera.image_texture,
                &camera.applied_preview_sampling_mode, target_texture_width,
                target_texture_height, desired_preview_sampling_mode,
                &result.perf);
        }

        camera.playback_staging_frame = -1;
        camera.playback_staging_valid = false;
        camera.playback_staging_preview_sampling_mode = -1;
        result.presented_rgba_cuda_buffer = camera.pbo_cuda.cuda_buffer;
        camera.last_uploaded_frame = source_frame_number;
        camera.texture_has_valid_frame = true;
        result.perf.upload_ms +=
            durationMs(std::chrono::steady_clock::now() - upload_start);
        result.perf.upload_count++;
        return source_frame_number;
    };

    if (context.play_video) {
        const int preferred_slot =
            context.read_head % static_cast<int>(context.scene->size_of_buffer);
        const int display_slot = findCameraDisplaySlotForFrame(
            *context.scene, context.view_idx, context.target_display_frame,
            preferred_slot);

        result.presented_slot = display_slot;
        if (display_slot >= 0) {
            result.presented_frame = uploadCameraFrameToTexture(display_slot);
            if (result.presented_frame >= 0) {
                result.resolved_current_frame_num = result.presented_frame;
            } else {
                result.resolved_current_frame_num = context.target_display_frame;
            }
        } else {
            result.presented_frame = -1;
            result.resolved_current_frame_num = context.target_display_frame;
            if (camera.texture_has_valid_frame) {
                clearCameraDisplayBuffer(camera);
            }
        }
        return result;
    }

    if (context.pause_seeked || context.preferred_paused_slot >= 0) {
        int paused_slot = context.preferred_paused_slot;
        if (paused_slot < 0) {
            paused_slot =
                context.read_head % static_cast<int>(context.scene->size_of_buffer);
        }
        paused_slot = findCameraDisplaySlotForFrame(
            *context.scene, context.view_idx,
            std::max(0, context.target_display_frame), paused_slot);

        if (paused_slot >= 0) {
            result.presented_slot = paused_slot;
            result.presented_frame = uploadCameraFrameToTexture(paused_slot);
            if (result.presented_frame >= 0) {
                result.resolved_current_frame_num = result.presented_frame;
            } else {
                result.resolved_current_frame_num =
                    std::max(0, context.target_display_frame);
            }
        } else if (camera.texture_has_valid_frame) {
            clearCameraDisplayBuffer(camera);
        }
    } else if (camera.texture_has_valid_frame) {
        clearCameraDisplayBuffer(camera);
    }

    return result;
}
