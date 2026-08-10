#include "gui/crop_preview_window.h"

#include "imgui.h"
#include "render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>

namespace {

struct ResolvedCropPreviewSelection {
    int32_t crop_roi_index = -1;
    std::optional<CropSpec> crop_spec;
    std::string crop_roi_source;
    std::optional<RefinedKeypointSelection> selected_keypoint_selection;
};

struct CropTexturePresenter {
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint fbo = 0;
    GLint src_texture_location = -1;
    GLint src_size_location = -1;
    GLint dst_size_location = -1;
    GLint src_rect_location = -1;
    GLint dst_rect_location = -1;
};

struct RotatedCropPresenter {
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint fbo = 0;
    GLint src_texture_location = -1;
    GLint src_size_location = -1;
    GLint dst_size_location = -1;
    GLint rotation_location = -1;
};

struct SourceRotatedCropPresenter {
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint fbo = 0;
    GLint src_texture_location = -1;
    GLint src_size_location = -1;
    GLint crop_size_location = -1;
    GLint dst_size_location = -1;
    GLint src_rect_location = -1;
    GLint dst_rect_location = -1;
    GLint rotation_location = -1;
};

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

const char* cropOriginSourceLabel(CropImageView::Origin origin,
                                  bool texture_path) {
    switch (origin) {
    case CropImageView::Origin::LiveFrame:
        return texture_path ? "live_texture" : "live_image";
    case CropImageView::Origin::PersistedZarr:
        return "persisted_zarr";
    case CropImageView::Origin::Unknown:
    default:
        return "unknown";
    }
}

GLuint compileCropShader(GLenum shader_type, const char* source) {
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
        std::cerr << "Crop preview shader compilation failed: " << log
                  << std::endl;
    }
    return shader;
}

void ensureCropTexturePresenter(CropTexturePresenter* presenter) {
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
        uniform sampler2D uSrcTex;
        uniform vec2 uSrcSize;
        uniform vec2 uDstSize;
        uniform vec4 uSrcRect;
        uniform vec4 uDstRect;
        varying vec2 vUV;

        void main() {
            vec2 dstPx = vUV * uDstSize;
            if (dstPx.x < uDstRect.x || dstPx.y < uDstRect.y ||
                dstPx.x >= (uDstRect.x + uDstRect.z) ||
                dstPx.y >= (uDstRect.y + uDstRect.w)) {
                gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
                return;
            }

            vec2 localUv = (dstPx - uDstRect.xy) / uDstRect.zw;
            vec2 srcPx = uSrcRect.xy + localUv * uSrcRect.zw;
            vec2 srcUv = (srcPx + vec2(0.5, 0.5)) / uSrcSize;
            gl_FragColor = texture2D(uSrcTex, srcUv);
        }
    )GLSL";

    GLuint vertex_shader = compileCropShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment_shader =
        compileCropShader(GL_FRAGMENT_SHADER, kFragmentShader);

    presenter->program = glCreateProgram();
    glAttachShader(presenter->program, vertex_shader);
    glAttachShader(presenter->program, fragment_shader);
    glBindAttribLocation(presenter->program, 0, "aPos");
    glBindAttribLocation(presenter->program, 1, "aUV");
    glLinkProgram(presenter->program);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    presenter->src_texture_location =
        glGetUniformLocation(presenter->program, "uSrcTex");
    presenter->src_size_location =
        glGetUniformLocation(presenter->program, "uSrcSize");
    presenter->dst_size_location =
        glGetUniformLocation(presenter->program, "uDstSize");
    presenter->src_rect_location =
        glGetUniformLocation(presenter->program, "uSrcRect");
    presenter->dst_rect_location =
        glGetUniformLocation(presenter->program, "uDstRect");

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
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glGenFramebuffers(1, &presenter->fbo);
}

void ensureRotatedCropPresenter(RotatedCropPresenter* presenter) {
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
        uniform sampler2D uSrcTex;
        uniform vec2 uSrcSize;
        uniform vec2 uDstSize;
        uniform vec2 uRotation;
        varying vec2 vUV;

        void main() {
            vec2 dstPx = vUV * uDstSize;
            vec2 dstCenter = uDstSize * 0.5;
            vec2 delta = dstPx - dstCenter;
            float radius = min(uDstSize.x, uDstSize.y) * 0.5;
            if (dot(delta, delta) > radius * radius) {
                gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
                return;
            }

            float c = uRotation.x;
            float s = uRotation.y;
            vec2 srcCenter = uSrcSize * 0.5;
            vec2 srcDelta = vec2(c * delta.x - s * delta.y,
                                 s * delta.x + c * delta.y);
            vec2 srcPx = srcCenter + srcDelta;
            if (srcPx.x < 0.0 || srcPx.y < 0.0 ||
                srcPx.x >= uSrcSize.x || srcPx.y >= uSrcSize.y) {
                gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
                return;
            }

            vec2 srcUv = (srcPx + vec2(0.5, 0.5)) / uSrcSize;
            gl_FragColor = texture2D(uSrcTex, srcUv);
        }
    )GLSL";

    GLuint vertex_shader = compileCropShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment_shader =
        compileCropShader(GL_FRAGMENT_SHADER, kFragmentShader);

    presenter->program = glCreateProgram();
    glAttachShader(presenter->program, vertex_shader);
    glAttachShader(presenter->program, fragment_shader);
    glBindAttribLocation(presenter->program, 0, "aPos");
    glBindAttribLocation(presenter->program, 1, "aUV");
    glLinkProgram(presenter->program);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    presenter->src_texture_location =
        glGetUniformLocation(presenter->program, "uSrcTex");
    presenter->src_size_location =
        glGetUniformLocation(presenter->program, "uSrcSize");
    presenter->dst_size_location =
        glGetUniformLocation(presenter->program, "uDstSize");
    presenter->rotation_location =
        glGetUniformLocation(presenter->program, "uRotation");

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
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glGenFramebuffers(1, &presenter->fbo);
}

void ensureSourceRotatedCropPresenter(SourceRotatedCropPresenter* presenter) {
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
        uniform sampler2D uSrcTex;
        uniform vec2 uSrcSize;
        uniform vec2 uCropSize;
        uniform vec2 uDstSize;
        uniform vec4 uSrcRect;
        uniform vec4 uDstRect;
        uniform vec2 uRotation;
        varying vec2 vUV;

        void main() {
            vec2 dstPx = vUV * uDstSize;
            vec2 dstCenter = uDstSize * 0.5;
            vec2 delta = dstPx - dstCenter;
            float radius = min(uDstSize.x, uDstSize.y) * 0.5;
            if (dot(delta, delta) > radius * radius) {
                gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
                return;
            }

            float c = uRotation.x;
            float s = uRotation.y;
            vec2 cropCenter = uCropSize * 0.5;
            vec2 cropPx = cropCenter + vec2(c * delta.x - s * delta.y,
                                            s * delta.x + c * delta.y);

            if (cropPx.x < uDstRect.x || cropPx.y < uDstRect.y ||
                cropPx.x >= (uDstRect.x + uDstRect.z) ||
                cropPx.y >= (uDstRect.y + uDstRect.w)) {
                gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
                return;
            }

            vec2 localUv = (cropPx - uDstRect.xy) / uDstRect.zw;
            vec2 srcPx = uSrcRect.xy + localUv * uSrcRect.zw;
            vec2 srcUv = (srcPx + vec2(0.5, 0.5)) / uSrcSize;
            gl_FragColor = texture2D(uSrcTex, srcUv);
        }
    )GLSL";

    GLuint vertex_shader = compileCropShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment_shader =
        compileCropShader(GL_FRAGMENT_SHADER, kFragmentShader);

    presenter->program = glCreateProgram();
    glAttachShader(presenter->program, vertex_shader);
    glAttachShader(presenter->program, fragment_shader);
    glBindAttribLocation(presenter->program, 0, "aPos");
    glBindAttribLocation(presenter->program, 1, "aUV");
    glLinkProgram(presenter->program);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    presenter->src_texture_location =
        glGetUniformLocation(presenter->program, "uSrcTex");
    presenter->src_size_location =
        glGetUniformLocation(presenter->program, "uSrcSize");
    presenter->crop_size_location =
        glGetUniformLocation(presenter->program, "uCropSize");
    presenter->dst_size_location =
        glGetUniformLocation(presenter->program, "uDstSize");
    presenter->src_rect_location =
        glGetUniformLocation(presenter->program, "uSrcRect");
    presenter->dst_rect_location =
        glGetUniformLocation(presenter->program, "uDstRect");
    presenter->rotation_location =
        glGetUniformLocation(presenter->program, "uRotation");

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
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glGenFramebuffers(1, &presenter->fbo);
}

void ensureCropTexture(CropPreviewWindowState& state,
                       unsigned int width,
                       unsigned int height) {
    if (state.crop_texture == 0) {
        create_texture(&state.crop_texture);
    }
    if (state.last_width == width && state.last_height == height &&
        state.last_channels == 4) {
        return;
    }
    bind_texture(&state.crop_texture);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height),
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    unbind_texture();
}

void ensureRotatedCropTexture(CropPreviewWindowState& state,
                              unsigned int side) {
    if (state.rotated_crop_texture == 0) {
        create_texture(&state.rotated_crop_texture);
        state.rotated_width = 0;
        state.rotated_height = 0;
    }
    if (state.rotated_width == side && state.rotated_height == side) {
        return;
    }
    bind_texture(&state.rotated_crop_texture);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 static_cast<GLsizei>(side),
                 static_cast<GLsizei>(side),
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    unbind_texture();
    state.rotated_width = side;
    state.rotated_height = side;
}

bool renderCropTexture(CropPreviewWindowState& state,
                       const CropTextureView& crop_texture_view) {
    if (!crop_texture_view.valid()) {
        return false;
    }

    static CropTexturePresenter presenter;
    ensureCropTexturePresenter(&presenter);
    ensureCropTexture(state,
                      static_cast<unsigned int>(crop_texture_view.output_width),
                      static_cast<unsigned int>(crop_texture_view.output_height));

    GLint previous_framebuffer = 0;
    GLint previous_program = 0;
    GLint previous_vertex_array = 0;
    GLint previous_array_buffer = 0;
    GLint previous_active_texture = 0;
    GLint previous_texture0 = 0;
    GLint previous_viewport[4] = {0, 0, 0, 0};
    GLfloat previous_clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previous_clear_color);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture0);

    glBindFramebuffer(GL_FRAMEBUFFER, presenter.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           state.crop_texture,
                           0);
    glViewport(0, 0, crop_texture_view.output_width, crop_texture_view.output_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(presenter.program);
    glUniform1i(presenter.src_texture_location, 0);
    glUniform2f(presenter.src_size_location,
                static_cast<float>(crop_texture_view.source_texture_width),
                static_cast<float>(crop_texture_view.source_texture_height));
    glUniform2f(presenter.dst_size_location,
                static_cast<float>(crop_texture_view.output_width),
                static_cast<float>(crop_texture_view.output_height));
    glUniform4f(presenter.src_rect_location,
                static_cast<float>(crop_texture_view.source_rect.x),
                static_cast<float>(crop_texture_view.source_rect.y),
                static_cast<float>(crop_texture_view.source_rect.width),
                static_cast<float>(crop_texture_view.source_rect.height));
    glUniform4f(presenter.dst_rect_location,
                static_cast<float>(crop_texture_view.destination_rect.x),
                static_cast<float>(crop_texture_view.destination_rect.y),
                static_cast<float>(crop_texture_view.destination_rect.width),
                static_cast<float>(crop_texture_view.destination_rect.height));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, crop_texture_view.source_texture_id);
    glBindVertexArray(presenter.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(previous_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, previous_array_buffer);
    glUseProgram(previous_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previous_texture0);
    glActiveTexture(previous_active_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2],
               previous_viewport[3]);
    glClearColor(previous_clear_color[0],
                 previous_clear_color[1],
                 previous_clear_color[2],
                 previous_clear_color[3]);
    return true;
}

bool renderRotatedCropTexture(CropPreviewWindowState& state,
                              float angle_degrees) {
    if (state.crop_texture == 0 || state.last_width == 0 || state.last_height == 0) {
        return false;
    }

    const unsigned int output_side =
        static_cast<unsigned int>(std::min(state.last_width, state.last_height));
    if (output_side == 0) {
        return false;
    }

    static RotatedCropPresenter presenter;
    ensureRotatedCropPresenter(&presenter);
    ensureRotatedCropTexture(state, output_side);

    const float radians = angle_degrees * (3.14159265f / 180.0f);
    const float c = std::cos(radians);
    const float s = std::sin(radians);

    GLint previous_framebuffer = 0;
    GLint previous_program = 0;
    GLint previous_vertex_array = 0;
    GLint previous_array_buffer = 0;
    GLint previous_active_texture = 0;
    GLint previous_texture0 = 0;
    GLint previous_viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture0);

    glBindFramebuffer(GL_FRAMEBUFFER, presenter.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           state.rotated_crop_texture,
                           0);
    glViewport(0, 0, static_cast<GLsizei>(output_side),
               static_cast<GLsizei>(output_side));
    glUseProgram(presenter.program);
    glUniform1i(presenter.src_texture_location, 0);
    glUniform2f(presenter.src_size_location,
                static_cast<float>(state.last_width),
                static_cast<float>(state.last_height));
    glUniform2f(presenter.dst_size_location,
                static_cast<float>(output_side),
                static_cast<float>(output_side));
    glUniform2f(presenter.rotation_location, c, s);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state.crop_texture);
    glBindVertexArray(presenter.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(previous_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, previous_array_buffer);
    glUseProgram(previous_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previous_texture0);
    glActiveTexture(previous_active_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2],
               previous_viewport[3]);
    return true;
}

bool renderRotatedCropTextureFromSource(CropPreviewWindowState& state,
                                        const CropTextureView& crop_texture_view,
                                        float angle_degrees) {
    if (!crop_texture_view.valid()) {
        return false;
    }

    const unsigned int output_side = static_cast<unsigned int>(
        std::min(crop_texture_view.output_width, crop_texture_view.output_height));
    if (output_side == 0) {
        return false;
    }

    static SourceRotatedCropPresenter presenter;
    ensureSourceRotatedCropPresenter(&presenter);
    ensureRotatedCropTexture(state, output_side);

    const float radians = angle_degrees * (3.14159265f / 180.0f);
    const float c = std::cos(radians);
    const float s = std::sin(radians);

    GLint previous_framebuffer = 0;
    GLint previous_program = 0;
    GLint previous_vertex_array = 0;
    GLint previous_array_buffer = 0;
    GLint previous_active_texture = 0;
    GLint previous_texture0 = 0;
    GLint previous_viewport[4] = {0, 0, 0, 0};
    GLfloat previous_clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previous_clear_color);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture0);

    glBindFramebuffer(GL_FRAMEBUFFER, presenter.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           state.rotated_crop_texture,
                           0);
    glViewport(0, 0, static_cast<GLsizei>(output_side),
               static_cast<GLsizei>(output_side));
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(presenter.program);
    glUniform1i(presenter.src_texture_location, 0);
    glUniform2f(presenter.src_size_location,
                static_cast<float>(crop_texture_view.source_texture_width),
                static_cast<float>(crop_texture_view.source_texture_height));
    glUniform2f(presenter.crop_size_location,
                static_cast<float>(crop_texture_view.output_width),
                static_cast<float>(crop_texture_view.output_height));
    glUniform2f(presenter.dst_size_location,
                static_cast<float>(output_side),
                static_cast<float>(output_side));
    glUniform4f(presenter.src_rect_location,
                static_cast<float>(crop_texture_view.source_rect.x),
                static_cast<float>(crop_texture_view.source_rect.y),
                static_cast<float>(crop_texture_view.source_rect.width),
                static_cast<float>(crop_texture_view.source_rect.height));
    glUniform4f(presenter.dst_rect_location,
                static_cast<float>(crop_texture_view.destination_rect.x),
                static_cast<float>(crop_texture_view.destination_rect.y),
                static_cast<float>(crop_texture_view.destination_rect.width),
                static_cast<float>(crop_texture_view.destination_rect.height));
    glUniform2f(presenter.rotation_location, c, s);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, crop_texture_view.source_texture_id);
    glBindVertexArray(presenter.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(previous_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, previous_array_buffer);
    glUseProgram(previous_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previous_texture0);
    glActiveTexture(previous_active_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2],
               previous_viewport[3]);
    glClearColor(previous_clear_color[0],
                 previous_clear_color[1],
                 previous_clear_color[2],
                 previous_clear_color[3]);
    return true;
}

const char* cropImageOriginLabel(CropImageView::Origin origin) {
    switch (origin) {
    case CropImageView::Origin::LiveFrame:
        return "live frame crop";
    case CropImageView::Origin::PersistedZarr:
        return "persisted zarr crop";
    case CropImageView::Origin::Unknown:
    default:
        return "crop source unresolved";
    }
}

void clearCropPreviewState(CropPreviewWindowState& state) {
    state.last_roi_index = -1;
    state.last_crop_rect = {};
    state.displayed_crop_roi_index = -1;
    state.displayed_crop_source_frame = -1;
    state.displayed_crop_source_label.clear();
    state.crop_kp_positions.clear();
    state.crop_kp_labels.clear();
    state.crop_kp_edges.clear();
    state.rotated_kp_positions.clear();
    state.rotated_kp_labels.clear();
    state.rotated_kp_edges.clear();
    state.rotated_valid = false;
    state.stored_heading_valid = false;
    state.arrow_origin_valid = false;
    resetCropKeypointEditorState(state.editor_state);
}

ResolvedCropPreviewSelection resolveCropPreviewSelection(
    const CropPreviewWindowContext& context) {
    ResolvedCropPreviewSelection resolved;

    const auto& movement_frames = context.zarr_loader.getMovementFrameIndices();
    const auto& detection_indices =
        context.zarr_loader.getMovementDetectionIndices();

    if (context.selected_crop_spec.has_value() &&
        context.selected_crop_spec->valid &&
        context.selected_frame == context.current_frame_num) {
        resolved.crop_spec = context.selected_crop_spec;
        resolved.crop_roi_source = "selected edited bbox";
        if (context.selected_detection_index >= 0) {
            auto selection =
                context.refined_keypoint_repo.resolveFrameDetectionSelection(
                    static_cast<size_t>(context.current_frame_num),
                    static_cast<size_t>(context.selected_detection_index),
                    false);
            if (selection.valid) {
                resolved.crop_roi_index = selection.roi_index;
                resolved.selected_keypoint_selection = selection;
            }
        }
        return resolved;
    }

    if (context.selected_frame == context.current_frame_num &&
        context.selected_box >= 0) {
        auto selection =
            context.refined_keypoint_repo.resolveFrameDetectionSelection(
                static_cast<size_t>(context.current_frame_num),
                static_cast<size_t>(context.selected_box),
                false);
        if (selection.valid) {
            resolved.crop_roi_index = selection.roi_index;
            resolved.crop_roi_source =
                selection.editable ? "selected refined keypoint detection"
                                   : "selected keypoint detection";
            resolved.selected_keypoint_selection = selection;
            return resolved;
        }
        auto mask_metadata =
            context.zarr_loader.getMaskRoiMetadataForFrameDetection(
                static_cast<size_t>(context.current_frame_num),
                static_cast<size_t>(context.selected_box),
                false);
        if (mask_metadata.valid && mask_metadata.has_crop_metadata) {
            resolved.crop_roi_index = mask_metadata.roi_index;
            resolved.crop_roi_source = "selected mask ROI";
            return resolved;
        }
    }

    if (!movement_frames.empty() &&
        movement_frames.size() == detection_indices.size()) {
        auto it = std::lower_bound(movement_frames.begin(),
                                   movement_frames.end(),
                                   context.current_frame_num);
        if (it != movement_frames.end() && *it == context.current_frame_num) {
            const size_t idx =
                static_cast<size_t>(std::distance(movement_frames.begin(), it));
            if (idx < detection_indices.size()) {
                resolved.crop_roi_index = detection_indices[idx];
                resolved.crop_roi_source = "movement ROI";
                return resolved;
            }
        }
    }

    auto frame_detections =
        context.zarr_loader.getRawDetections(
            static_cast<size_t>(context.current_frame_num), false, false);
    const size_t detection_count = frame_detections.boxes.size();
    for (size_t detection_idx = 0; detection_idx < detection_count; ++detection_idx) {
        auto selection =
            context.refined_keypoint_repo.resolveFrameDetectionSelection(
                static_cast<size_t>(context.current_frame_num), detection_idx, false);
        if (selection.roi_metadata.valid && selection.roi_metadata.has_crop_metadata) {
            resolved.crop_roi_index = selection.roi_index;
            resolved.crop_roi_source =
                selection.editable ? "frame refined keypoint ROI"
                                   : "frame keypoint ROI";
            if (selection.valid) {
                resolved.selected_keypoint_selection = selection;
            }
            return resolved;
        }
        auto mask_metadata =
            context.zarr_loader.getMaskRoiMetadataForFrameDetection(
                static_cast<size_t>(context.current_frame_num), detection_idx, false);
        if (mask_metadata.valid && mask_metadata.has_crop_metadata) {
            resolved.crop_roi_index = mask_metadata.roi_index;
            resolved.crop_roi_source = "frame mask ROI";
            return resolved;
        }
    }

    const auto& crop_frames = context.crop_image_provider.getCropFrameIndices();
    auto crop_it =
        std::find(crop_frames.begin(), crop_frames.end(), context.current_frame_num);
    if (crop_it != crop_frames.end()) {
        resolved.crop_roi_index =
            static_cast<int32_t>(std::distance(crop_frames.begin(), crop_it));
        resolved.crop_roi_source = "crop frame fallback";
    }

    return resolved;
}

void buildRotatedCropPreview(const CropPreviewWindowContext& context,
                             CropPreviewWindowState& state,
                             size_t crop_width,
                             size_t crop_height,
                             const std::optional<CropSpec>& crop_spec,
                             const CropTextureView* crop_texture_view,
                             int32_t crop_roi_index,
                             const std::optional<RefinedKeypointSelection>&
                                 selected_keypoint_selection,
                             CropPreviewPerfMetrics* perf) {
    state.rotated_valid = false;
    state.stored_heading_valid = false;
    state.crop_kp_positions.clear();
    state.crop_kp_labels.clear();
    state.crop_kp_edges.clear();
    state.rotated_kp_positions.clear();
    state.rotated_kp_labels.clear();
    state.rotated_kp_edges.clear();
    state.arrow_origin_valid = false;

    const auto& keypoint_descriptor = context.keypoint_repository.descriptor();
    if (keypoint_descriptor.run_name.empty()) {
        return;
    }

    const auto detection_start = std::chrono::steady_clock::now();
    const auto keypoint_frame = context.keypoint_repository.resolveCameraFrame(
        context.current_frame_num, context.zarr_loader.getImageWidth(),
        context.zarr_loader.getImageHeight());
    if (perf != nullptr) {
        perf->get_raw_detections_ms +=
            durationMs(std::chrono::steady_clock::now() - detection_start);
    }
    if (keypoint_frame.status !=
        crimson::zarr::KeypointOverlayStatus::Mapped) {
        return;
    }
    size_t matched = SIZE_MAX;
    std::optional<RefinedKeypointSelection> matched_keypoint_selection;
    if (selected_keypoint_selection.has_value() &&
        (selected_keypoint_selection->roi_index == crop_roi_index ||
         crop_spec.has_value())) {
        const auto found = std::find_if(
            keypoint_frame.detections.begin(),
            keypoint_frame.detections.end(), [&](const auto& detection) {
                return detection.detection_index ==
                       static_cast<int64_t>(
                           selected_keypoint_selection->detection_index);
            });
        if (found != keypoint_frame.detections.end()) {
            matched = static_cast<size_t>(std::distance(
                keypoint_frame.detections.begin(), found));
            matched_keypoint_selection = selected_keypoint_selection;
        }
    }
    if (matched == SIZE_MAX) {
        for (size_t di = 0; di < keypoint_frame.detections.size(); ++di) {
            const int64_t detection_index =
                keypoint_frame.detections[di].detection_index;
            if (detection_index < 0) {
                continue;
            }
            auto candidate =
                context.refined_keypoint_repo.resolveFrameDetectionSelection(
                    static_cast<size_t>(context.current_frame_num),
                    static_cast<size_t>(detection_index), false);
            if (candidate.valid && candidate.roi_index == crop_roi_index) {
                matched = di;
                matched_keypoint_selection = std::move(candidate);
                break;
            }
        }
    }

    if (matched == SIZE_MAX ||
        !keypoint_frame.detections[matched].heading_valid ||
        !keypoint_frame.detections[matched].heading_degrees.has_value()) {
        return;
    }

    state.stored_heading_deg = static_cast<float>(
        *keypoint_frame.detections[matched].heading_degrees);
    state.stored_heading_valid = true;

    const float angle = -state.stored_heading_deg;
    const int width = static_cast<int>(crop_width);
    const int height = static_cast<int>(crop_height);
    const int crop_side = std::min(width, height);
    const auto rotated_render_start = std::chrono::steady_clock::now();
    const bool rotated_rendered =
        crop_side > 0 &&
        ((crop_texture_view != nullptr &&
          renderRotatedCropTextureFromSource(state, *crop_texture_view, angle)) ||
         renderRotatedCropTexture(state, angle));
    if (perf != nullptr) {
        perf->render_rotated_texture_ms +=
            durationMs(std::chrono::steady_clock::now() - rotated_render_start);
    }
    if (!rotated_rendered) {
        return;
    }
    state.rotated_valid = true;
    if (perf != nullptr) {
        perf->rotated_valid_after = true;
    }

    const auto transform_start = std::chrono::steady_clock::now();
    state.rotated_kp_edges = keypoint_descriptor.skeleton_edges;
    state.crop_kp_edges = keypoint_descriptor.skeleton_edges;
    if (matched >= keypoint_frame.detections.size()) {
        if (perf != nullptr) {
            perf->keypoint_transform_ms +=
                durationMs(std::chrono::steady_clock::now() - transform_start);
        }
        return;
    }

    const auto& keypoints = keypoint_frame.detections[matched].keypoints;
    float offset_x = NAN;
    float offset_y = NAN;
    if (crop_spec.has_value() && crop_spec->valid) {
        offset_x = crop_spec->offset_x;
        offset_y = crop_spec->offset_y;
    } else if (matched_keypoint_selection.has_value() &&
               matched_keypoint_selection->roi_metadata.has_crop_metadata) {
        offset_x = matched_keypoint_selection->roi_metadata.offset_x;
        offset_y = matched_keypoint_selection->roi_metadata.offset_y;
    }
    if (!std::isfinite(offset_x) || !std::isfinite(offset_y)) {
        if (perf != nullptr) {
            perf->keypoint_transform_ms +=
                durationMs(std::chrono::steady_clock::now() - transform_start);
        }
        return;
    }

    const float radians = angle * (3.14159265f / 180.0f);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float crop_center_x = width * 0.5f;
    const float crop_center_y = height * 0.5f;
    const float output_center = crop_side * 0.5f;
    const KeypointHeadingComputationSpec& heading_spec =
        context.zarr_loader.getHeadingComputationSpec();

    for (size_t ki = 0; ki < keypoints.size(); ++ki) {
        if (!std::isfinite(keypoints[ki].x) ||
            !std::isfinite(keypoints[ki].y)) {
            state.crop_kp_positions.push_back({NAN, NAN});
            state.rotated_kp_positions.push_back({NAN, NAN});
        } else {
            const float px = static_cast<float>(keypoints[ki].x) - offset_x;
            const float py = static_cast<float>(keypoints[ki].y) - offset_y;
            state.crop_kp_positions.push_back({px, py});
            const float dx = px - crop_center_x;
            const float dy = py - crop_center_y;
            const float rx = c * dx + s * dy + output_center;
            const float ry = -s * dx + c * dy + output_center;
            state.rotated_kp_positions.push_back({rx, ry});
        }

        const std::string label =
            ki < keypoint_descriptor.keypoint_labels.size()
                ? keypoint_descriptor.keypoint_labels[ki]
                : "";
        state.crop_kp_labels.push_back(label);
        state.rotated_kp_labels.push_back(label);
    }

    const auto crop_positions_d =
        convertKeypointPositionsToDouble(state.crop_kp_positions);
    const auto rotated_positions_d =
        convertKeypointPositionsToDouble(state.rotated_kp_positions);
    std::array<double, 2> crop_origin{};
    std::array<double, 2> rotated_origin{};
    const bool crop_origin_valid = evaluateKeypointHeadingOrigin(
        heading_spec, crop_positions_d, crop_origin);
    const bool rotated_origin_valid = evaluateKeypointHeadingOrigin(
        heading_spec, rotated_positions_d, rotated_origin);
    if (crop_origin_valid) {
        state.arrow_origin_crop = {static_cast<float>(crop_origin[0]),
                                   static_cast<float>(crop_origin[1])};
    }
    if (rotated_origin_valid) {
        state.arrow_origin_rotated = {static_cast<float>(rotated_origin[0]),
                                      static_cast<float>(rotated_origin[1])};
    }
    state.arrow_origin_valid = crop_origin_valid && rotated_origin_valid;
    if (perf != nullptr) {
        perf->keypoint_transform_ms +=
            durationMs(std::chrono::steady_clock::now() - transform_start);
    }
}

bool refreshCropPreview(const CropPreviewWindowContext& context,
                        CropPreviewWindowState& state,
                        int32_t crop_roi_index,
                        const std::optional<CropSpec>& crop_spec,
                        const std::string& crop_roi_source,
                        const std::optional<RefinedKeypointSelection>&
                            selected_keypoint_selection,
                        CropPreviewPerfMetrics* perf) {
    const auto refresh_start = std::chrono::steady_clock::now();
    auto finish = [&](bool ok) -> bool {
        if (perf != nullptr) {
            perf->refresh_ms +=
                durationMs(std::chrono::steady_clock::now() - refresh_start);
            perf->rotated_valid_after =
                perf->rotated_valid_after || state.rotated_valid;
        }
        return ok;
    };

    const CropRect requested_crop_rect =
        crop_spec.has_value() ? crop_spec->toPixelRect() : CropRect{};
    const bool frame_changed =
        context.current_frame_num != state.last_crop_preview_source_frame;
    const bool roi_changed = crop_roi_index != state.last_roi_index;
    const bool crop_rect_changed =
        requested_crop_rect.valid() &&
        (requested_crop_rect.x != state.last_crop_rect.x ||
         requested_crop_rect.y != state.last_crop_rect.y ||
         requested_crop_rect.width != state.last_crop_rect.width ||
         requested_crop_rect.height != state.last_crop_rect.height);
    const bool rotated_requested = state.preview_ui_state.show_rotated_crop;
    const bool need_rotated_refresh =
        rotated_requested &&
        (!state.rotated_valid || frame_changed || roi_changed || crop_rect_changed);
    if (perf != nullptr) {
        perf->attempted = true;
        perf->current_frame_num = context.current_frame_num;
        perf->crop_roi_index = crop_roi_index;
        perf->frame_mod32 =
            context.current_frame_num >= 0 ? context.current_frame_num % 32 : -1;
        perf->frame_mod64 =
            context.current_frame_num >= 0 ? context.current_frame_num % 64 : -1;
        perf->frame_changed = frame_changed;
        perf->roi_changed = roi_changed;
        perf->crop_rect_changed = crop_rect_changed;
        perf->rotated_requested = rotated_requested;
        perf->rotated_refresh_needed = need_rotated_refresh;
        perf->crop_source = "none";
    }
    if (!frame_changed && !roi_changed && !crop_rect_changed && !need_rotated_refresh) {
        const bool ok = state.crop_texture != 0 &&
                        state.last_width > 0 && state.last_height > 0;
        if (perf != nullptr) {
            perf->cache_hit = ok;
            perf->refreshed = false;
            if (ok && state.displayed_crop_source_label.find("live frame texture") !=
                          std::string::npos) {
                perf->crop_source = "live_texture_cached";
            } else if (ok) {
                perf->crop_source = "cached";
            }
        }
        return finish(ok);
    }

    CropTextureView crop_texture_view;
    bool has_texture_crop = false;
    const auto texture_lookup_start = std::chrono::steady_clock::now();
    if (crop_spec.has_value()) {
        has_texture_crop =
            context.crop_image_provider.getCropTextureForSpec(*crop_spec,
                                                              crop_texture_view);
        if (!has_texture_crop && crop_roi_index >= 0) {
            has_texture_crop = context.crop_image_provider.getCropTextureForIndex(
                crop_roi_index, crop_texture_view);
        }
    } else {
        has_texture_crop = context.crop_image_provider.getCropTextureForIndex(
            crop_roi_index, crop_texture_view);
    }
    if (perf != nullptr) {
        perf->provider_texture_lookup_ms +=
            durationMs(std::chrono::steady_clock::now() - texture_lookup_start);
    }
    if (has_texture_crop) {
        if (perf != nullptr) {
            perf->crop_source = cropOriginSourceLabel(crop_texture_view.origin, true);
        }
        const bool needs_render =
            state.crop_texture == 0 || crop_roi_index != state.last_roi_index ||
            static_cast<size_t>(crop_texture_view.output_width) != state.last_width ||
            static_cast<size_t>(crop_texture_view.output_height) !=
                state.last_height ||
            frame_changed || crop_rect_changed;
        if (needs_render) {
            const auto render_start = std::chrono::steady_clock::now();
            if (!renderCropTexture(state, crop_texture_view)) {
                return finish(false);
            }
            if (perf != nullptr) {
                perf->render_crop_texture_ms += durationMs(
                    std::chrono::steady_clock::now() - render_start);
            }
        }

        if (perf != nullptr) {
            perf->refreshed = true;
        }
        state.last_roi_index = crop_roi_index;
        state.last_crop_rect = requested_crop_rect;
        state.last_width = static_cast<size_t>(crop_texture_view.output_width);
        state.last_height = static_cast<size_t>(crop_texture_view.output_height);
        state.last_channels = 4;
        state.displayed_crop_roi_index = crop_roi_index;
        state.displayed_crop_source_frame = context.current_frame_num;
        state.displayed_crop_source_label =
            crop_roi_source + " | live frame texture crop";
        state.last_crop_preview_source_frame = context.current_frame_num;

        if (rotated_requested) {
            buildRotatedCropPreview(context,
                                    state,
                                    state.last_width,
                                    state.last_height,
                                    crop_spec,
                                    &crop_texture_view,
                                    crop_roi_index,
                                    selected_keypoint_selection,
                                    perf);
        } else {
            state.rotated_valid = false;
        }
        return finish(state.crop_texture != 0 &&
                      state.last_width > 0 && state.last_height > 0);
    }

    CropImageView crop_view;
    bool has_image_crop = false;
    const auto image_lookup_start = std::chrono::steady_clock::now();
    if (crop_spec.has_value()) {
        has_image_crop =
            context.crop_image_provider.getCropImageForSpec(*crop_spec, crop_view);
        if (!has_image_crop && crop_roi_index >= 0) {
            has_image_crop = context.crop_image_provider.getCropImageForIndex(
                crop_roi_index, crop_view);
        }
    } else {
        has_image_crop =
            context.crop_image_provider.getCropImageForIndex(crop_roi_index,
                                                             crop_view);
    }
    if (perf != nullptr) {
        perf->provider_image_lookup_ms +=
            durationMs(std::chrono::steady_clock::now() - image_lookup_start);
    }
    if (!has_image_crop) {
        return finish(false);
    }
    if (perf != nullptr) {
        perf->crop_source = cropOriginSourceLabel(crop_view.origin, false);
    }

    bool needs_upload = crop_roi_index != state.last_roi_index ||
                        crop_view.width != state.last_width ||
                        crop_view.height != state.last_height ||
                        crop_view.channels != state.last_channels ||
                        frame_changed || crop_rect_changed;
    if (state.crop_texture == 0) {
        create_texture(&state.crop_texture);
        needs_upload = true;
    }

    if (needs_upload) {
        const auto upload_start = std::chrono::steady_clock::now();
        const size_t pixel_count = crop_view.width * crop_view.height;
        state.crop_rgba_buffer.resize(pixel_count * 4);
        const uint8_t* src = crop_view.data;
        uint8_t* dst = state.crop_rgba_buffer.data();
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
                const uint8_t v = src[p];
                dst[4 * p + 0] = v;
                dst[4 * p + 1] = v;
                dst[4 * p + 2] = v;
                dst[4 * p + 3] = 255;
            }
        }

        upload_texture(&state.crop_texture,
                       state.crop_rgba_buffer.data(),
                       static_cast<unsigned int>(crop_view.width),
                       static_cast<unsigned int>(crop_view.height));
        if (perf != nullptr) {
            perf->image_upload_ms +=
                durationMs(std::chrono::steady_clock::now() - upload_start);
            perf->refreshed = true;
        }
        state.last_roi_index = crop_roi_index;
        state.last_crop_rect = requested_crop_rect;
        state.last_width = crop_view.width;
        state.last_height = crop_view.height;
        state.last_channels = crop_view.channels;
        state.displayed_crop_roi_index = crop_roi_index;
        state.displayed_crop_source_frame = context.current_frame_num;
        state.displayed_crop_source_label =
            crop_roi_source + " | " + cropImageOriginLabel(crop_view.origin);
        state.last_crop_preview_source_frame = context.current_frame_num;

        if (rotated_requested) {
            buildRotatedCropPreview(context,
                                    state,
                                    crop_view.width,
                                    crop_view.height,
                                    crop_spec,
                                    nullptr,
                                    crop_roi_index,
                                    selected_keypoint_selection,
                                    perf);
        } else {
            state.rotated_valid = false;
        }
    } else if (need_rotated_refresh) {
        if (perf != nullptr) {
            perf->refreshed = true;
        }
        buildRotatedCropPreview(context,
                                state,
                                crop_view.width,
                                crop_view.height,
                                crop_spec,
                                nullptr,
                                crop_roi_index,
                                selected_keypoint_selection,
                                perf);
    }

    return finish(state.crop_texture != 0 &&
                  state.last_width > 0 && state.last_height > 0);
}

}  // namespace

CropPreviewWindowResult drawCropPreviewWindow(const CropPreviewWindowContext& context,
                                             CropPreviewWindowState& state) {
    CropPreviewWindowResult result;
    const auto total_start = std::chrono::steady_clock::now();
    auto finish = [&]() -> CropPreviewWindowResult {
        const auto finish_start = std::chrono::steady_clock::now();
        result.perf.total_window_ms +=
            durationMs(finish_start - total_start);
        result.perf.finish_ms +=
            durationMs(std::chrono::steady_clock::now() - finish_start);
        return result;
    };
    auto endWindow = [&]() {
        const auto end_start = std::chrono::steady_clock::now();
        ImGui::End();
        result.perf.imgui_end_ms +=
            durationMs(std::chrono::steady_clock::now() - end_start);
    };

    const auto window_setup_start = std::chrono::steady_clock::now();
    ImGui::SetNextWindowSize(ImVec2(300.0f, 300.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(120.0f, 120.0f),
                                        ImVec2(420.0f, 700.0f));
    result.perf.window_setup_ms +=
        durationMs(std::chrono::steady_clock::now() - window_setup_start);
    const auto begin_start = std::chrono::steady_clock::now();
    const bool window_open =
        ImGui::Begin("Advanced Crop Preview###Crop Preview",
                     context.show_window);
    result.perf.imgui_begin_ms +=
        durationMs(std::chrono::steady_clock::now() - begin_start);
    if (!window_open) {
        if (context.show_window != nullptr && !*context.show_window) {
            clearCropPreviewState(state);
        }
        endWindow();
        return finish();
    }

    const auto resolve_start = std::chrono::steady_clock::now();
    const auto resolved = resolveCropPreviewSelection(context);
    result.perf.resolve_selection_ms +=
        durationMs(std::chrono::steady_clock::now() - resolve_start);
    result.selected_keypoint_selection = resolved.selected_keypoint_selection;
    if (!resolved.crop_spec.has_value() && resolved.crop_roi_index < 0) {
        ImGui::TextUnformatted("No crop available for current frame.");
        clearCropPreviewState(state);
        endWindow();
        return finish();
    }

    if (!refreshCropPreview(context,
                            state,
                            resolved.crop_roi_index,
                            resolved.crop_spec,
                            resolved.crop_roi_source,
                            resolved.selected_keypoint_selection,
                            &result.perf)) {
        ImGui::TextUnformatted("No crop available for current frame.");
        clearCropPreviewState(state);
        endWindow();
        return finish();
    }

    const auto context_build_start = std::chrono::steady_clock::now();
    CropKeypointEditorContext editor_context;
    editor_context.selection = resolved.selected_keypoint_selection.has_value()
                                   ? &*resolved.selected_keypoint_selection
                                   : nullptr;
    editor_context.displayed_crop_roi_index = state.displayed_crop_roi_index;
    editor_context.displayed_crop_source_frame = state.displayed_crop_source_frame;
    editor_context.displayed_crop_source_label = &state.displayed_crop_source_label;
    editor_context.play_video = context.play_video;
    editor_context.rotated_valid = state.rotated_valid;
    editor_context.stored_heading_valid = state.stored_heading_valid;
    editor_context.stored_heading_deg = state.stored_heading_deg;
    editor_context.source_positions = &state.crop_kp_positions;
    editor_context.labels = &state.crop_kp_labels;
    editor_context.edges = &state.crop_kp_edges;
    editor_context.heading_spec = &context.zarr_loader.getHeadingComputationSpec();
    editor_context.base_arrow_origin = state.arrow_origin_crop;
    editor_context.base_arrow_origin_valid = state.arrow_origin_valid;
    editor_context.status_message = &state.local_status_message;

    CropKeypointPreviewPanelContext preview_context;
    preview_context.crop_texture_id = state.crop_texture;
    preview_context.crop_width = state.last_width;
    preview_context.crop_height = state.last_height;
    preview_context.displayed_crop_roi_index = state.displayed_crop_roi_index;
    preview_context.displayed_crop_source_frame = state.displayed_crop_source_frame;
    preview_context.current_frame_num = context.current_frame_num;
    preview_context.displayed_crop_source_label = &state.displayed_crop_source_label;
    preview_context.play_video = context.play_video;
    preview_context.editor_context = editor_context;
    preview_context.rotated.valid = state.rotated_valid;
    preview_context.rotated.texture_id = state.rotated_crop_texture;
    preview_context.rotated.width = state.rotated_width;
    preview_context.rotated.height = state.rotated_height;
    preview_context.rotated.positions = &state.rotated_kp_positions;
    preview_context.rotated.labels = &state.rotated_kp_labels;
    preview_context.rotated.edges = &state.rotated_kp_edges;
    preview_context.rotated.arrow_origin = state.arrow_origin_rotated;
    preview_context.rotated.arrow_origin_valid = state.arrow_origin_valid;
    result.perf.context_build_ms +=
        durationMs(std::chrono::steady_clock::now() - context_build_start);

    const auto editor_start = std::chrono::steady_clock::now();
    const auto preview_result = drawCropKeypointPreviewPanel(
        preview_context, state.preview_ui_state, state.editor_state);
    result.perf.editor_panel_ms +=
        durationMs(std::chrono::steady_clock::now() - editor_start);
    const auto post_panel_start = std::chrono::steady_clock::now();
    result.perf.rotated_requested =
        result.perf.rotated_requested ||
        state.preview_ui_state.show_rotated_crop;
    result.perf.rotated_valid_after =
        result.perf.rotated_valid_after || state.rotated_valid;
    result.editor_action = preview_result.editor_action;
    result.perf.post_panel_ms +=
        durationMs(std::chrono::steady_clock::now() - post_panel_start);

    endWindow();
    return finish();
}
