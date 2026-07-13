#include "apple_overlay_metal_renderer.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

struct TargetSize {
    float width = 0.0f;
    float height = 0.0f;
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

const char* shaderSource() {
    return R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float4 color;
};

vertex VertexOutput crimsonOverlayVertex(
    uint vertex_id [[vertex_id]],
    device const packed_float2* positions [[buffer(0)]],
    device const float4* colors [[buffer(1)]],
    constant float2& target_size [[buffer(2)]]) {
    const float2 pixel = float2(positions[vertex_id]);
    VertexOutput output;
    output.position = float4(
        pixel.x / target_size.x * 2.0 - 1.0,
        1.0 - pixel.y / target_size.y * 2.0,
        0.0,
        1.0);
    output.color = colors[vertex_id];
    return output;
}

fragment float4 crimsonOverlayFragment(VertexOutput input [[stage_in]]) {
    return input.color;
}
)METAL";
}

}  // namespace

struct AppleOverlayMetalRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> pipeline = nil;

    void reset() {
        pipeline = nil;
        device = nil;
    }
};

AppleOverlayMetalRenderer::AppleOverlayMetalRenderer()
    : impl_(std::make_unique<Impl>()) {}

AppleOverlayMetalRenderer::~AppleOverlayMetalRenderer() { reset(); }

bool AppleOverlayMetalRenderer::initialize(uintptr_t metal_device,
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
        assignError(
            error,
            errorText(library_error, "Metal overlay shader compilation failed"));
        reset();
        return false;
    }
    MTLRenderPipelineDescriptor* descriptor =
        [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction =
        [library newFunctionWithName:@"crimsonOverlayVertex"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"crimsonOverlayFragment"];
    MTLRenderPipelineColorAttachmentDescriptor* attachment =
        descriptor.colorAttachments[0];
    attachment.pixelFormat =
        static_cast<MTLPixelFormat>(drawable_pixel_format);
    attachment.blendingEnabled = YES;
    attachment.rgbBlendOperation = MTLBlendOperationAdd;
    attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    attachment.destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    attachment.alphaBlendOperation = MTLBlendOperationAdd;
    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    attachment.destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    NSError* pipeline_error = nil;
    impl_->pipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&pipeline_error];
    if (impl_->pipeline == nil) {
        assignError(error,
                    errorText(pipeline_error,
                              "Metal overlay pipeline creation failed"));
        reset();
        return false;
    }
    return true;
}

void AppleOverlayMetalRenderer::reset() { impl_->reset(); }

bool AppleOverlayMetalRenderer::isInitialized() const {
    return impl_->device != nil && impl_->pipeline != nil;
}

bool AppleOverlayMetalRenderer::encode(
    const crimson::overlay::ReadOnlyOverlayScene& scene,
    const crimson::overlay::SourceViewportTransform& transform,
    uintptr_t metal_render_encoder,
    uint32_t drawable_width,
    uint32_t drawable_height,
    std::string* error) {
    if (!isInitialized() || drawable_width == 0 || drawable_height == 0 ||
        !transform.valid()) {
        assignError(error, "Metal overlay renderer received invalid state");
        return false;
    }
    if (!scene.ready()) {
        return true;
    }
    id<MTLRenderCommandEncoder> encoder =
        (__bridge id<MTLRenderCommandEncoder>)reinterpret_cast<void*>(
            metal_render_encoder);
    if (encoder == nil) {
        assignError(error, "Metal overlay render encoder is null");
        return false;
    }

    const crimson::overlay::ScreenMesh mesh =
        crimson::overlay::tessellateReadOnlyOverlayScene(scene, transform);
    if (mesh.triangle_vertices.empty()) {
        return true;
    }
    const auto clipped = crimson::overlay::intersectRects(
        transform.display,
        {0.0, 0.0, static_cast<double>(drawable_width),
         static_cast<double>(drawable_height)});
    if (!clipped) {
        return true;
    }
    const NSUInteger clip_x = static_cast<NSUInteger>(
        std::max(0.0, std::floor(clipped->x)));
    const NSUInteger clip_y = static_cast<NSUInteger>(
        std::max(0.0, std::floor(clipped->y)));
    const NSUInteger clip_right = static_cast<NSUInteger>(std::min(
        static_cast<double>(drawable_width),
        std::ceil(clipped->x + clipped->width)));
    const NSUInteger clip_bottom = static_cast<NSUInteger>(std::min(
        static_cast<double>(drawable_height),
        std::ceil(clipped->y + clipped->height)));
    if (clip_right <= clip_x || clip_bottom <= clip_y) {
        return true;
    }

    std::vector<float> positions;
    std::vector<crimson::overlay::Color> colors;
    positions.reserve(mesh.triangle_vertices.size() * 2);
    colors.reserve(mesh.triangle_vertices.size());
    for (const auto& vertex : mesh.triangle_vertices) {
        positions.push_back(vertex.x);
        positions.push_back(vertex.y);
        colors.push_back(vertex.color);
    }
    id<MTLBuffer> position_buffer = [impl_->device
        newBufferWithBytes:positions.data()
                   length:positions.size() * sizeof(float)
                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> color_buffer = [impl_->device
        newBufferWithBytes:colors.data()
                   length:colors.size() * sizeof(crimson::overlay::Color)
                  options:MTLResourceStorageModeShared];
    if (position_buffer == nil || color_buffer == nil) {
        assignError(error, "Metal overlay vertex buffer allocation failed");
        return false;
    }

    [encoder setViewport:MTLViewport{
                             0.0, 0.0, static_cast<double>(drawable_width),
                             static_cast<double>(drawable_height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{
                                clip_x, clip_y, clip_right - clip_x,
                                clip_bottom - clip_y}];
    [encoder setRenderPipelineState:impl_->pipeline];
    [encoder setVertexBuffer:position_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:color_buffer offset:0 atIndex:1];
    const TargetSize target{static_cast<float>(drawable_width),
                            static_cast<float>(drawable_height)};
    [encoder setVertexBytes:&target length:sizeof(target) atIndex:2];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:mesh.triangle_vertices.size()];
    return true;
}
