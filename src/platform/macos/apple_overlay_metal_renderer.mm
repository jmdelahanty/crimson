#include "apple_overlay_metal_renderer.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct TargetSize {
    float width = 0.0f;
    float height = 0.0f;
};

struct MaskVertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct VectorColor {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 0.0f;
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

struct MaskVertexOutput {
    float4 position [[position]];
    float2 texcoord;
};

vertex MaskVertexOutput crimsonMaskVertex(
    uint vertex_id [[vertex_id]],
    device const packed_float4* vertices [[buffer(0)]],
    constant float2& target_size [[buffer(1)]]) {
    const float4 mask_vertex = float4(vertices[vertex_id]);
    MaskVertexOutput output;
    output.position = float4(
        mask_vertex.x / target_size.x * 2.0 - 1.0,
        1.0 - mask_vertex.y / target_size.y * 2.0,
        0.0,
        1.0);
    output.texcoord = mask_vertex.zw;
    return output;
}

fragment float4 crimsonMaskFragment(
    MaskVertexOutput input [[stage_in]],
    texture2d<float> mask [[texture(0)]],
    constant float4& color [[buffer(0)]]) {
    constexpr sampler mask_sampler(coord::normalized,
                                   address::clamp_to_edge,
                                   filter::nearest);
    const float coverage = mask.sample(mask_sampler, input.texcoord).r;
    return float4(color.rgb, color.a * coverage);
}
)METAL";
}

}  // namespace

struct AppleOverlayMetalRenderer::Impl {
    struct CachedMaskTexture {
        id<MTLTexture> texture = nil;
        uint64_t last_used = 0;
    };

    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> vector_pipeline = nil;
    id<MTLRenderPipelineState> mask_pipeline = nil;
    std::unordered_map<std::string, CachedMaskTexture> mask_textures;
    uint64_t use_counter = 0;

    void reset() {
        mask_textures.clear();
        mask_pipeline = nil;
        vector_pipeline = nil;
        device = nil;
        use_counter = 0;
    }
};

namespace {

bool encodeVectorTriangles(id<MTLDevice> device,
                           id<MTLRenderPipelineState> pipeline,
                           id<MTLRenderCommandEncoder> encoder,
                           const TargetSize& target,
                           const std::vector<float>& positions,
                           const std::vector<VectorColor>& colors,
                           std::string* error) {
    if (positions.empty() && colors.empty()) {
        return true;
    }
    if (positions.size() != colors.size() * 2) {
        assignError(error, "Metal overlay vector mesh is inconsistent");
        return false;
    }
    id<MTLBuffer> position_buffer = [device
        newBufferWithBytes:positions.data()
                   length:positions.size() * sizeof(float)
                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> color_buffer = [device
        newBufferWithBytes:colors.data()
                   length:colors.size() * sizeof(VectorColor)
                  options:MTLResourceStorageModeShared];
    if (position_buffer == nil || color_buffer == nil) {
        assignError(error, "Metal overlay vertex buffer allocation failed");
        return false;
    }
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:position_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:color_buffer offset:0 atIndex:1];
    [encoder setVertexBytes:&target length:sizeof(target) atIndex:2];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:colors.size()];
    return true;
}

}  // namespace

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
    auto configureBlend = [](MTLRenderPipelineColorAttachmentDescriptor*
                                 attachment) {
        attachment.blendingEnabled = YES;
        attachment.rgbBlendOperation = MTLBlendOperationAdd;
        attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        attachment.destinationRGBBlendFactor =
            MTLBlendFactorOneMinusSourceAlpha;
        attachment.alphaBlendOperation = MTLBlendOperationAdd;
        attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
        attachment.destinationAlphaBlendFactor =
            MTLBlendFactorOneMinusSourceAlpha;
    };

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
    configureBlend(attachment);
    NSError* pipeline_error = nil;
    impl_->vector_pipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&pipeline_error];
    if (impl_->vector_pipeline == nil) {
        assignError(error,
                    errorText(pipeline_error,
                              "Metal overlay pipeline creation failed"));
        reset();
        return false;
    }

    MTLRenderPipelineDescriptor* mask_descriptor =
        [MTLRenderPipelineDescriptor new];
    mask_descriptor.vertexFunction =
        [library newFunctionWithName:@"crimsonMaskVertex"];
    mask_descriptor.fragmentFunction =
        [library newFunctionWithName:@"crimsonMaskFragment"];
    mask_descriptor.colorAttachments[0].pixelFormat =
        static_cast<MTLPixelFormat>(drawable_pixel_format);
    configureBlend(mask_descriptor.colorAttachments[0]);
    pipeline_error = nil;
    impl_->mask_pipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:mask_descriptor
                                                      error:&pipeline_error];
    if (impl_->mask_pipeline == nil) {
        assignError(error,
                    errorText(pipeline_error,
                              "Metal mask overlay pipeline creation failed"));
        reset();
        return false;
    }
    return true;
}

void AppleOverlayMetalRenderer::reset() { impl_->reset(); }

bool AppleOverlayMetalRenderer::isInitialized() const {
    return impl_->device != nil && impl_->vector_pipeline != nil &&
           impl_->mask_pipeline != nil;
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

    [encoder setViewport:MTLViewport{
                             0.0, 0.0, static_cast<double>(drawable_width),
                             static_cast<double>(drawable_height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{
                                clip_x, clip_y, clip_right - clip_x,
                                clip_bottom - clip_y}];
    const TargetSize target{static_cast<float>(drawable_width),
                            static_cast<float>(drawable_height)};

    auto encodeRaster = [&](const crimson::overlay::RasterMask& raster)
        -> bool {
        if (!raster.source_rect.valid() || raster.width == 0 ||
            raster.height == 0 || !raster.alpha ||
            raster.width >
                std::numeric_limits<size_t>::max() / raster.height ||
            raster.alpha->size() != raster.width * raster.height ||
            !raster.color.valid()) {
            return true;
        }
        const auto visible = crimson::overlay::intersectRects(
            raster.source_rect, transform.visible_source);
        if (!visible) {
            return true;
        }
        const auto display = transform.sourceToDisplay(*visible);
        if (!display) {
            return true;
        }
        const float u0 = static_cast<float>(
            (visible->x - raster.source_rect.x) / raster.source_rect.width);
        const float v0 = static_cast<float>(
            (visible->y - raster.source_rect.y) / raster.source_rect.height);
        const float u1 = static_cast<float>(
            (visible->x + visible->width - raster.source_rect.x) /
            raster.source_rect.width);
        const float v1 = static_cast<float>(
            (visible->y + visible->height - raster.source_rect.y) /
            raster.source_rect.height);
        const float x0 = static_cast<float>(display->x);
        const float y0 = static_cast<float>(display->y);
        const float x1 = static_cast<float>(display->x + display->width);
        const float y1 = static_cast<float>(display->y + display->height);
        const std::array<MaskVertex, 6> vertices = {
            MaskVertex{x0, y0, u0, v0}, MaskVertex{x1, y0, u1, v0},
            MaskVertex{x0, y1, u0, v1}, MaskVertex{x0, y1, u0, v1},
            MaskVertex{x1, y0, u1, v0}, MaskVertex{x1, y1, u1, v1}};

        const std::string key = raster.cache_key + ":" +
            std::to_string(raster.width) + "x" +
            std::to_string(raster.height);
        auto found = impl_->mask_textures.find(key);
        if (found == impl_->mask_textures.end()) {
            MTLTextureDescriptor* texture_descriptor =
                [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                 width:raster.width
                                                height:raster.height
                                             mipmapped:NO];
            texture_descriptor.storageMode = MTLStorageModeShared;
            texture_descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture =
                [impl_->device newTextureWithDescriptor:texture_descriptor];
            if (texture == nil) {
                assignError(error, "Metal mask texture allocation failed");
                return false;
            }
            [texture replaceRegion:MTLRegionMake2D(0, 0, raster.width,
                                                   raster.height)
                       mipmapLevel:0
                         withBytes:raster.alpha->data()
                       bytesPerRow:raster.width];
            found = impl_->mask_textures
                        .emplace(key, Impl::CachedMaskTexture{texture, 0})
                        .first;
        }
        found->second.last_used = ++impl_->use_counter;
        [encoder setRenderPipelineState:impl_->mask_pipeline];
        [encoder setVertexBytes:vertices.data()
                         length:sizeof(vertices)
                        atIndex:0];
        [encoder setVertexBytes:&target length:sizeof(target) atIndex:1];
        [encoder setFragmentTexture:found->second.texture atIndex:0];
        [encoder setFragmentBytes:&raster.color
                           length:sizeof(raster.color)
                          atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:vertices.size()];
        return true;
    };

    auto encodeMesh = [&](crimson::overlay::CameraOverlayLayer layer)
        -> bool {
        const crimson::overlay::ScreenMesh mesh =
            crimson::overlay::tessellateReadOnlyOverlaySceneLayer(
                scene, transform, layer);
        if (mesh.triangle_vertices.empty()) {
            return true;
        }
        std::vector<float> positions;
        std::vector<VectorColor> colors;
        positions.reserve(mesh.triangle_vertices.size() * 2);
        colors.reserve(mesh.triangle_vertices.size());
        for (const auto& vertex : mesh.triangle_vertices) {
            positions.push_back(vertex.x);
            positions.push_back(vertex.y);
            colors.push_back({vertex.color.red, vertex.color.green,
                              vertex.color.blue, vertex.color.alpha});
        }
        return encodeVectorTriangles(impl_->device, impl_->vector_pipeline,
                                     encoder, target, positions, colors,
                                     error);
    };

    for (const auto layer : crimson::overlay::kCameraOverlayLayerOrder) {
        for (const auto& raster : scene.raster_masks) {
            if (raster.layer == layer && !encodeRaster(raster)) {
                return false;
            }
        }
        if (!encodeMesh(layer)) {
            return false;
        }
    }

    constexpr size_t kMaximumCachedMaskTextures = 64;
    while (impl_->mask_textures.size() > kMaximumCachedMaskTextures) {
        auto oldest = impl_->mask_textures.end();
        uint64_t oldest_use = std::numeric_limits<uint64_t>::max();
        for (auto candidate = impl_->mask_textures.begin();
             candidate != impl_->mask_textures.end(); ++candidate) {
            if (candidate->second.last_used < oldest_use) {
                oldest = candidate;
                oldest_use = candidate->second.last_used;
            }
        }
        if (oldest == impl_->mask_textures.end()) {
            break;
        }
        impl_->mask_textures.erase(oldest);
    }
    return true;
}

bool AppleOverlayMetalRenderer::encodePolar(
    const crimson::polar::ChaserDistancePolarScene& scene,
    crimson::polar::ChaserDistancePolarScenePoint display_origin,
    double scale_x,
    double scale_y,
    uintptr_t metal_render_encoder,
    uint32_t drawable_width,
    uint32_t drawable_height,
    std::string* error) {
    if (!isInitialized() || drawable_width == 0 || drawable_height == 0 ||
        !std::isfinite(display_origin.x) ||
        !std::isfinite(display_origin.y) || !std::isfinite(scale_x) ||
        !std::isfinite(scale_y) || scale_x <= 0.0 || scale_y <= 0.0) {
        assignError(error, "Metal polar renderer received invalid state");
        return false;
    }
    if (!scene.ready()) {
        return true;
    }
    id<MTLRenderCommandEncoder> encoder =
        (__bridge id<MTLRenderCommandEncoder>)reinterpret_cast<void*>(
            metal_render_encoder);
    if (encoder == nil) {
        assignError(error, "Metal polar render encoder is null");
        return false;
    }

    const double viewport_right =
        display_origin.x + scene.viewport.width_px * scale_x;
    const double viewport_bottom =
        display_origin.y + scene.viewport.height_px * scale_y;
    const double clipped_left = std::max(0.0, display_origin.x);
    const double clipped_top = std::max(0.0, display_origin.y);
    const double clipped_right = std::min(
        static_cast<double>(drawable_width), viewport_right);
    const double clipped_bottom = std::min(
        static_cast<double>(drawable_height), viewport_bottom);
    if (!(clipped_right > clipped_left && clipped_bottom > clipped_top)) {
        return true;
    }
    const NSUInteger clip_x =
        static_cast<NSUInteger>(std::floor(clipped_left));
    const NSUInteger clip_y =
        static_cast<NSUInteger>(std::floor(clipped_top));
    const NSUInteger clip_right =
        static_cast<NSUInteger>(std::ceil(clipped_right));
    const NSUInteger clip_bottom =
        static_cast<NSUInteger>(std::ceil(clipped_bottom));

    const auto mesh = crimson::polar::tessellateChaserDistancePolarScene(
        scene, display_origin, scale_x, scale_y);
    if (mesh.triangle_vertices.empty()) {
        return true;
    }
    std::vector<float> positions;
    std::vector<VectorColor> colors;
    positions.reserve(mesh.triangle_vertices.size() * 2);
    colors.reserve(mesh.triangle_vertices.size());
    for (const auto& vertex : mesh.triangle_vertices) {
        positions.push_back(vertex.x);
        positions.push_back(vertex.y);
        colors.push_back(
            {static_cast<float>(vertex.color.red),
             static_cast<float>(vertex.color.green),
             static_cast<float>(vertex.color.blue),
             static_cast<float>(vertex.color.alpha)});
    }

    [encoder setViewport:MTLViewport{
                             0.0, 0.0, static_cast<double>(drawable_width),
                             static_cast<double>(drawable_height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{
                                clip_x, clip_y, clip_right - clip_x,
                                clip_bottom - clip_y}];
    const TargetSize target{static_cast<float>(drawable_width),
                            static_cast<float>(drawable_height)};
    return encodeVectorTriangles(impl_->device, impl_->vector_pipeline,
                                 encoder, target, positions, colors, error);
}

bool AppleOverlayMetalRenderer::encodeStimulusCameraOverlay(
    const crimson::stimulus::StimulusCameraOverlayScene& scene,
    crimson::stimulus::StimulusCameraOverlayPoint display_origin,
    double scale_x,
    double scale_y,
    uintptr_t metal_render_encoder,
    uint32_t drawable_width,
    uint32_t drawable_height,
    std::string* error) {
    if (!isInitialized() || drawable_width == 0 || drawable_height == 0 ||
        !std::isfinite(display_origin.x) ||
        !std::isfinite(display_origin.y) || !std::isfinite(scale_x) ||
        !std::isfinite(scale_y) || scale_x <= 0.0 || scale_y <= 0.0) {
        assignError(error, "Metal stimulus overlay renderer received invalid state");
        return false;
    }
    if (!scene.ready()) {
        return true;
    }
    id<MTLRenderCommandEncoder> encoder =
        (__bridge id<MTLRenderCommandEncoder>)reinterpret_cast<void*>(
            metal_render_encoder);
    if (encoder == nil) {
        assignError(error, "Metal stimulus overlay render encoder is null");
        return false;
    }

    const double viewport_right =
        display_origin.x + scene.viewport.width_px * scale_x;
    const double viewport_bottom =
        display_origin.y + scene.viewport.height_px * scale_y;
    const double clipped_left = std::max(0.0, display_origin.x);
    const double clipped_top = std::max(0.0, display_origin.y);
    const double clipped_right =
        std::min(static_cast<double>(drawable_width), viewport_right);
    const double clipped_bottom =
        std::min(static_cast<double>(drawable_height), viewport_bottom);
    if (!(clipped_right > clipped_left && clipped_bottom > clipped_top)) {
        return true;
    }
    const NSUInteger clip_x =
        static_cast<NSUInteger>(std::floor(clipped_left));
    const NSUInteger clip_y =
        static_cast<NSUInteger>(std::floor(clipped_top));
    const NSUInteger clip_right_px =
        static_cast<NSUInteger>(std::ceil(clipped_right));
    const NSUInteger clip_bottom_px =
        static_cast<NSUInteger>(std::ceil(clipped_bottom));

    const auto mesh =
        crimson::stimulus::tessellateStimulusCameraOverlayScene(
            scene, display_origin, scale_x, scale_y);
    if (mesh.triangle_vertices.empty()) {
        return true;
    }
    std::vector<float> positions;
    std::vector<VectorColor> colors;
    positions.reserve(mesh.triangle_vertices.size() * 2);
    colors.reserve(mesh.triangle_vertices.size());
    for (const auto& vertex : mesh.triangle_vertices) {
        positions.push_back(vertex.x);
        positions.push_back(vertex.y);
        colors.push_back(
            {static_cast<float>(vertex.color.red),
             static_cast<float>(vertex.color.green),
             static_cast<float>(vertex.color.blue),
             static_cast<float>(vertex.color.alpha)});
    }

    [encoder setViewport:MTLViewport{
                             0.0, 0.0, static_cast<double>(drawable_width),
                             static_cast<double>(drawable_height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{
                                clip_x, clip_y, clip_right_px - clip_x,
                                clip_bottom_px - clip_y}];
    const TargetSize target{static_cast<float>(drawable_width),
                            static_cast<float>(drawable_height)};
    return encodeVectorTriangles(impl_->device, impl_->vector_pipeline,
                                 encoder, target, positions, colors, error);
}
