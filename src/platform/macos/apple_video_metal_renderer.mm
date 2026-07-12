#include "apple_video_metal_renderer.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

struct ColorConversion {
    float y_offset = 0.0f;
    float y_scale = 1.0f;
    float r_cr = 1.5748f;
    float g_cb = -0.187324f;
    float g_cr = -0.468124f;
    float b_cb = 1.8556f;
    float padding[2] = {};
};

void assignError(std::string* destination, const std::string& value) {
    if (destination != nullptr) {
        *destination = value;
    }
}

std::string errorText(NSError* error, const char* fallback) {
    if (error != nil && error.localizedDescription.UTF8String != nullptr) {
        return error.localizedDescription.UTF8String;
    }
    return fallback;
}

ColorConversion conversionFor(const DecodedFrameMetadata& metadata) {
    ColorConversion conversion;
    if (!colorRangeIsFull(metadata.color_range)) {
        conversion.y_offset = 16.0f / 255.0f;
        conversion.y_scale = 255.0f / 219.0f;
    }
    switch (metadata.color_matrix) {
    case ColorSpaceStandard_BT601:
        conversion.r_cr = 1.402f;
        conversion.g_cb = -0.344136f;
        conversion.g_cr = -0.714136f;
        conversion.b_cb = 1.772f;
        break;
    case ColorSpaceStandard_SMPTE240M:
        conversion.r_cr = 1.5756f;
        conversion.g_cb = -0.2253f;
        conversion.g_cr = -0.4768f;
        conversion.b_cb = 1.827f;
        break;
    case ColorSpaceStandard_BT2020:
    case ColorSpaceStandard_BT2020C:
        conversion.r_cr = 1.4746f;
        conversion.g_cb = -0.164553f;
        conversion.g_cr = -0.571353f;
        conversion.b_cb = 1.8814f;
        break;
    case ColorSpaceStandard_BT709:
    default:
        break;
    }
    return conversion;
}

const char* shaderSource() {
    return R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float2 uv;
};

struct ColorConversion {
    float y_offset;
    float y_scale;
    float r_cr;
    float g_cb;
    float g_cr;
    float b_cb;
    float2 padding;
};

vertex VertexOutput crimsonVideoVertex(uint vertex_id [[vertex_id]]) {
    constexpr float2 positions[] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)
    };
    constexpr float2 coordinates[] = {
        float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = coordinates[vertex_id];
    return output;
}

fragment float4 crimsonNv12Fragment(
    VertexOutput input [[stage_in]],
    texture2d<float> luma [[texture(0)]],
    texture2d<float> chroma [[texture(1)]],
    constant ColorConversion& conversion [[buffer(0)]]) {
    constexpr sampler video_sampler(coord::normalized, address::clamp_to_edge,
                                    filter::linear);
    const float y = (luma.sample(video_sampler, input.uv).r -
                     conversion.y_offset) * conversion.y_scale;
    const float2 uv = chroma.sample(video_sampler, input.uv).rg - float2(0.5);
    const float3 rgb = float3(
        y + conversion.r_cr * uv.y,
        y + conversion.g_cb * uv.x + conversion.g_cr * uv.y,
        y + conversion.b_cb * uv.x);
    return float4(clamp(rgb, 0.0, 1.0), 1.0);
}

fragment float4 crimsonBgraFragment(
    VertexOutput input [[stage_in]],
    texture2d<float> color [[texture(0)]]) {
    constexpr sampler video_sampler(coord::normalized, address::clamp_to_edge,
                                    filter::linear);
    return color.sample(video_sampler, input.uv);
}
)METAL";
}

}  // namespace

struct AppleVideoMetalRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> nv12_pipeline = nil;
    id<MTLRenderPipelineState> bgra_pipeline = nil;
    CVMetalTextureCacheRef texture_cache = nullptr;

    void reset() {
        if (texture_cache != nullptr) {
            CFRelease(texture_cache);
            texture_cache = nullptr;
        }
        bgra_pipeline = nil;
        nv12_pipeline = nil;
        device = nil;
    }
};

AppleVideoMetalRenderer::AppleVideoMetalRenderer()
    : impl_(std::make_unique<Impl>()) {}

AppleVideoMetalRenderer::~AppleVideoMetalRenderer() { reset(); }

bool AppleVideoMetalRenderer::initialize(uintptr_t metal_device,
                                         uint64_t drawable_pixel_format,
                                         std::string* error) {
    reset();
    impl_->device = (__bridge id<MTLDevice>)reinterpret_cast<void*>(metal_device);
    if (impl_->device == nil) {
        assignError(error, "Metal device is null");
        return false;
    }
    NSError* library_error = nil;
    NSString* source = [NSString stringWithUTF8String:shaderSource()];
    id<MTLLibrary> library = [impl_->device newLibraryWithSource:source
                                                        options:nil
                                                          error:&library_error];
    if (library == nil) {
        assignError(error,
                    errorText(library_error, "Metal shader compilation failed"));
        reset();
        return false;
    }

    id<MTLFunction> vertex =
        [library newFunctionWithName:@"crimsonVideoVertex"];
    auto make_pipeline =
        [&](NSString* fragment_name) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor* descriptor =
            [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction =
            [library newFunctionWithName:fragment_name];
        descriptor.colorAttachments[0].pixelFormat =
            static_cast<MTLPixelFormat>(drawable_pixel_format);
        NSError* pipeline_error = nil;
        id<MTLRenderPipelineState> pipeline =
            [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                          error:&pipeline_error];
        if (pipeline == nil) {
            assignError(error,
                        errorText(pipeline_error,
                                  "Metal video pipeline creation failed"));
            return nil;
        }
        return pipeline;
    };
    impl_->nv12_pipeline = make_pipeline(@"crimsonNv12Fragment");
    impl_->bgra_pipeline = make_pipeline(@"crimsonBgraFragment");
    if (impl_->nv12_pipeline == nil || impl_->bgra_pipeline == nil) {
        reset();
        return false;
    }
    const CVReturn cache_status = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, impl_->device, nullptr,
        &impl_->texture_cache);
    if (cache_status != kCVReturnSuccess || impl_->texture_cache == nullptr) {
        assignError(error, "CVMetalTextureCache creation failed");
        reset();
        return false;
    }
    return true;
}

void AppleVideoMetalRenderer::reset() { impl_->reset(); }

bool AppleVideoMetalRenderer::isInitialized() const {
    return impl_->device != nil && impl_->texture_cache != nullptr &&
           impl_->nv12_pipeline != nil && impl_->bgra_pipeline != nil;
}

bool AppleVideoMetalRenderer::encode(
    const AppleDecodedVideoFrame& frame, uintptr_t metal_command_buffer,
    uintptr_t metal_render_encoder, const AppleMetalVideoViewport& viewport,
    std::string* error) {
    if (!isInitialized() || !frame || viewport.width <= 0.0 ||
        viewport.height <= 0.0) {
        assignError(error, "Metal video renderer received invalid state");
        return false;
    }
    id<MTLCommandBuffer> command_buffer =
        (__bridge id<MTLCommandBuffer>)reinterpret_cast<void*>(
            metal_command_buffer);
    id<MTLRenderCommandEncoder> encoder =
        (__bridge id<MTLRenderCommandEncoder>)reinterpret_cast<void*>(
            metal_render_encoder);
    CVPixelBufferRef pixel_buffer = reinterpret_cast<CVPixelBufferRef>(
        frame.surface->nativeHandle());
    if (command_buffer == nil || encoder == nil || pixel_buffer == nullptr) {
        assignError(error, "Metal command state or pixel buffer is null");
        return false;
    }

    const NSUInteger x = static_cast<NSUInteger>(std::max(0.0, viewport.x));
    const NSUInteger y = static_cast<NSUInteger>(std::max(0.0, viewport.y));
    const NSUInteger width =
        static_cast<NSUInteger>(std::max(1.0, viewport.width));
    const NSUInteger height =
        static_cast<NSUInteger>(std::max(1.0, viewport.height));
    [encoder setViewport:MTLViewport{static_cast<double>(x),
                                     static_cast<double>(y),
                                     static_cast<double>(width),
                                     static_cast<double>(height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{x, y, width, height}];

    CVMetalTextureRef first_ref = nullptr;
    CVMetalTextureRef second_ref = nullptr;
    bool encoded = false;
    if (frame.metadata.pixel_format == FramePixelFormat::NV12) {
        const CVReturn y_status = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, impl_->texture_cache, pixel_buffer, nullptr,
            MTLPixelFormatR8Unorm,
            CVPixelBufferGetWidthOfPlane(pixel_buffer, 0),
            CVPixelBufferGetHeightOfPlane(pixel_buffer, 0), 0, &first_ref);
        const CVReturn uv_status = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, impl_->texture_cache, pixel_buffer, nullptr,
            MTLPixelFormatRG8Unorm,
            CVPixelBufferGetWidthOfPlane(pixel_buffer, 1),
            CVPixelBufferGetHeightOfPlane(pixel_buffer, 1), 1, &second_ref);
        if (y_status == kCVReturnSuccess && uv_status == kCVReturnSuccess &&
            first_ref != nullptr && second_ref != nullptr) {
            const ColorConversion conversion = conversionFor(frame.metadata);
            [encoder setRenderPipelineState:impl_->nv12_pipeline];
            [encoder setFragmentTexture:CVMetalTextureGetTexture(first_ref)
                                 atIndex:0];
            [encoder setFragmentTexture:CVMetalTextureGetTexture(second_ref)
                                 atIndex:1];
            [encoder setFragmentBytes:&conversion
                               length:sizeof(conversion)
                              atIndex:0];
            encoded = true;
        }
    } else if (frame.metadata.pixel_format == FramePixelFormat::BGRA8) {
        const CVReturn color_status = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, impl_->texture_cache, pixel_buffer, nullptr,
            MTLPixelFormatBGRA8Unorm, CVPixelBufferGetWidth(pixel_buffer),
            CVPixelBufferGetHeight(pixel_buffer), 0, &first_ref);
        if (color_status == kCVReturnSuccess && first_ref != nullptr) {
            [encoder setRenderPipelineState:impl_->bgra_pipeline];
            [encoder setFragmentTexture:CVMetalTextureGetTexture(first_ref)
                                 atIndex:0];
            encoded = true;
        }
    }
    if (encoded) {
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:3];
        std::shared_ptr<const FrameSurface> retained_surface = frame.surface;
        [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
            (void)retained_surface;
        }];
    } else {
        assignError(error, "CVPixelBuffer could not be mapped as a Metal texture");
    }
    if (first_ref != nullptr) {
        CFRelease(first_ref);
    }
    if (second_ref != nullptr) {
        CFRelease(second_ref);
    }
    return encoded;
}
