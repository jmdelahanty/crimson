#ifndef RED_RENDER
#define RED_RENDER
#include "gx_helper.h"
#include "decoder.h"
#include <cuda_runtime_api.h>
#include <cstdlib>
#include <vector>


struct PBO_CUDA {
    GLuint pbo;
    unsigned char* cuda_buffer;
    cudaGraphicsResource_t cuda_resource;
    size_t cuda_pbo_storage_buffer_size;
};

struct CameraResources {
    u32 image_width = 0;
    u32 image_height = 0;
    GLuint image_texture = 0;
    PBO_CUDA pbo_cuda = {};
    GLuint playback_staging_texture = 0;
    PBO_CUDA playback_staging_pbo = {};
    std::vector<PBO_CUDA> display_buffer_pbos;
    PictureBuffer *display_buffer = nullptr;
    SeekInfo seek_context = {false, false, 0, false, 0, 0};
    int last_uploaded_frame = -1;
    bool texture_has_valid_frame = false;
    int applied_preview_sampling_mode = -1;
    int playback_staging_frame = -1;
    bool playback_staging_valid = false;
    int playback_staging_preview_sampling_mode = -1;
    int display_texture_width = 0;
    int display_texture_height = 0;
    std::vector<unsigned char> playback_preview_rgba_cpu;
};

struct render_scene
{
    u32 num_cams = 0;
    std::vector<CameraResources> cameras;
    u32 size_of_buffer = 0;
    bool use_cpu_buffer = false;
};

inline void checkCudaStatus(cudaError_t status, const char* message) {
    if (status != cudaSuccess) {
        fprintf(stderr, "%s: %s\n", message, cudaGetErrorString(status));
        std::exit(EXIT_FAILURE);
    }
}

inline void render_initialize_target(gx_context *context, int cuda_device_index)
{
    GLFWwindow *render_target = gx_glfw_init_render_target(3, 3, context->width, context->height, "Red", context->glsl_version);
    gx_init(context, render_target);
    checkCudaStatus(cudaGLSetGLDevice(cuda_device_index), "cudaGLSetGLDevice failed");
    checkCudaStatus(cudaSetDevice(cuda_device_index), "cudaSetDevice failed");
    gx_imgui_init(context, std::filesystem::path());
}

inline void render_initialize_target(gx_context *context,
                                     int cuda_device_index,
                                     const std::filesystem::path& argv0_path)
{
    GLFWwindow *render_target = gx_glfw_init_render_target(3, 3, context->width, context->height, "Red", context->glsl_version);
    gx_init(context, render_target);
    checkCudaStatus(cudaGLSetGLDevice(cuda_device_index), "cudaGLSetGLDevice failed");
    checkCudaStatus(cudaSetDevice(cuda_device_index), "cudaSetDevice failed");
    gx_imgui_init(context, argv0_path);
}

static void render_allocate_scene_memory(render_scene *scene, u32 size_of_buffer)
{
    int num_cams = scene->num_cams;
    scene->cameras.resize(num_cams);
    scene->size_of_buffer = size_of_buffer;

    for (u32 j = 0; j < num_cams; j++)
    {
        scene->cameras[j].seek_context.use_seek = false;
        scene->cameras[j].seek_context.seek_frame = 0;
        scene->cameras[j].seek_context.seek_done = false;
        scene->cameras[j].seek_context.seek_accurate = false;
        scene->cameras[j].seek_context.seek_id = 0;
        scene->cameras[j].seek_context.settled_seek_id = 0;
        scene->cameras[j].last_uploaded_frame = -1;
        scene->cameras[j].texture_has_valid_frame = false;
        scene->cameras[j].applied_preview_sampling_mode = -1;
        scene->cameras[j].playback_staging_frame = -1;
        scene->cameras[j].playback_staging_valid = false;
        scene->cameras[j].playback_staging_preview_sampling_mode = -1;
        scene->cameras[j].display_texture_width =
            static_cast<int>(scene->cameras[j].image_width);
        scene->cameras[j].display_texture_height =
            static_cast<int>(scene->cameras[j].image_height);
        scene->cameras[j].playback_preview_rgba_cpu.clear();
        scene->cameras[j].display_buffer_pbos.clear();
    }

    for (u32 j = 0; j < num_cams; j++)
    {
        scene->cameras[j].display_buffer = (PictureBuffer *)malloc(size_of_buffer * sizeof(PictureBuffer));
    }

    for (u32 j = 0; j < num_cams; j++) {
        create_pbo(&scene->cameras[j].pbo_cuda.pbo, scene->cameras[j].image_width, scene->cameras[j].image_height);
        register_pbo_to_cuda(&scene->cameras[j].pbo_cuda.pbo, &scene->cameras[j].pbo_cuda.cuda_resource);
        map_cuda_resource(&scene->cameras[j].pbo_cuda.cuda_resource);
        cuda_pointer_from_resource(&scene->cameras[j].pbo_cuda.cuda_buffer, &scene->cameras[j].pbo_cuda.cuda_pbo_storage_buffer_size, &scene->cameras[j].pbo_cuda.cuda_resource);

        create_pbo(&scene->cameras[j].playback_staging_pbo.pbo,
                   scene->cameras[j].image_width,
                   scene->cameras[j].image_height);
        register_pbo_to_cuda(&scene->cameras[j].playback_staging_pbo.pbo,
                             &scene->cameras[j].playback_staging_pbo.cuda_resource);
        map_cuda_resource(&scene->cameras[j].playback_staging_pbo.cuda_resource);
        cuda_pointer_from_resource(
            &scene->cameras[j].playback_staging_pbo.cuda_buffer,
            &scene->cameras[j].playback_staging_pbo.cuda_pbo_storage_buffer_size,
            &scene->cameras[j].playback_staging_pbo.cuda_resource);
    }


    // allocate buffer on cpu
    for (u32 j = 0; j < num_cams; j++)
    {
        const int rgba_pitch =
            static_cast<int>(scene->cameras[j].image_width) * 4;
        const size_t rgba_frame_bytes =
            static_cast<size_t>(rgba_pitch) *
            static_cast<size_t>(scene->cameras[j].image_height);
        const int nv12_pitch =
            static_cast<int>((scene->cameras[j].image_width + 1) & ~1u);
        const int nv12_chroma_height =
            static_cast<int>((scene->cameras[j].image_height + 1) / 2);
        const size_t nv12_frame_bytes =
            static_cast<size_t>(nv12_pitch) *
            static_cast<size_t>(scene->cameras[j].image_height +
                                nv12_chroma_height);
        for (u32 i = 0; i < size_of_buffer; i++)
        {
            if (scene->use_cpu_buffer) {
                scene->cameras[j].display_buffer[i].frame =
                    static_cast<unsigned char *>(malloc(rgba_frame_bytes));
                decoder_clear_buffer_with_constant_image(
                    scene->cameras[j].display_buffer[i].frame,
                    scene->cameras[j].image_width,
                    scene->cameras[j].image_height);
                scene->cameras[j].display_buffer[i].pitch_bytes = rgba_pitch;
                scene->cameras[j].display_buffer[i].frame_bytes =
                    rgba_frame_bytes;
                scene->cameras[j].display_buffer[i].format =
                    PictureBufferFormat::RGBA32;
            } else {
                // GPU-buffer mode stores compact NV12 slots and converts only
                // the selected display frame to RGBA at presentation time.
                unsigned char *slot_buffer = nullptr;
                checkCudaStatus(
                    cudaMalloc(reinterpret_cast<void **>(&slot_buffer),
                               nv12_frame_bytes),
                    "cudaMalloc failed for camera NV12 display buffer");
                checkCudaStatus(cudaMemset(slot_buffer, 0, nv12_frame_bytes),
                                "cudaMemset failed for camera NV12 display buffer");
                scene->cameras[j].display_buffer[i].frame = slot_buffer;
                scene->cameras[j].display_buffer[i].pitch_bytes = nv12_pitch;
                scene->cameras[j].display_buffer[i].frame_bytes =
                    nv12_frame_bytes;
                scene->cameras[j].display_buffer[i].format =
                    PictureBufferFormat::NV12;
            }
            scene->cameras[j].display_buffer[i].frame_number = -1;
            scene->cameras[j].display_buffer[i].available_to_write = true;
            scene->cameras[j].display_buffer[i].color_matrix =
                ColorSpaceStandard_BT709;
        }
    }


    for (u32 j = 0; j < num_cams; j++)
    {
        glGenTextures(1, &scene->cameras[j].image_texture);
        glBindTexture(GL_TEXTURE_2D, scene->cameras[j].image_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, scene->cameras[j].image_width, scene->cameras[j].image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        // Setup filtering parameters for display
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // This is required on WebGL for non power-of-two textures
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // Same

        glGenTextures(1, &scene->cameras[j].playback_staging_texture);
        glBindTexture(GL_TEXTURE_2D, scene->cameras[j].playback_staging_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, scene->cameras[j].image_width, scene->cameras[j].image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

}

static void render_resize_camera_texture(CameraResources* camera,
                                         int texture_width,
                                         int texture_height) {
    if (camera == nullptr || texture_width <= 0 || texture_height <= 0) {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, camera->image_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture_width, texture_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    camera->display_texture_width = texture_width;
    camera->display_texture_height = texture_height;
}

#endif
