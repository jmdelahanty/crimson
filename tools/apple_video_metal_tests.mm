#include "platform/macos/apple_video_metal_renderer.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

struct TestFailure {
    std::string message;
};

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw TestFailure{std::string("CHECK failed: ") + #condition +    \
                              " at " + __FILE__ + ":" +                      \
                              std::to_string(__LINE__)};                       \
        }                                                                      \
    } while (false)

class TestPixelBufferSurface final : public FrameSurface {
  public:
    TestPixelBufferSurface(CVPixelBufferRef buffer,
                           const FrameSurfaceDescriptor& descriptor)
        : buffer_(CVPixelBufferRetain(buffer)), descriptor_(descriptor) {}

    ~TestPixelBufferSurface() override { CVPixelBufferRelease(buffer_); }

    const FrameSurfaceDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    uintptr_t nativeHandle(size_t plane_index) const noexcept override {
        return plane_index < descriptor_.plane_count
                   ? reinterpret_cast<uintptr_t>(buffer_)
                   : 0;
    }

  private:
    CVPixelBufferRef buffer_ = nullptr;
    FrameSurfaceDescriptor descriptor_;
};

AppleDecodedVideoFrame makeFrame(bool full_range, int matrix, uint8_t y,
                                 uint8_t cb, uint8_t cr) {
    constexpr int width = 16;
    constexpr int height = 16;
    const OSType format =
        full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                   : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    NSDictionary* attributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(format),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferRef buffer = nullptr;
    CHECK(CVPixelBufferCreate(kCFAllocatorDefault, width, height, format,
                              (__bridge CFDictionaryRef)attributes,
                              &buffer) == kCVReturnSuccess);
    CHECK(buffer != nullptr);
    CVPixelBufferLockBaseAddress(buffer, 0);
    const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    std::memset(CVPixelBufferGetBaseAddressOfPlane(buffer, 0), y,
                y_stride * height);
    uint8_t* uv = static_cast<uint8_t*>(
        CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    for (int row = 0; row < height / 2; ++row) {
        for (int column = 0; column < width / 2; ++column) {
            uv[row * uv_stride + column * 2] = cb;
            uv[row * uv_stride + column * 2 + 1] = cr;
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);

    AppleDecodedVideoFrame frame;
    frame.metadata.stream_id = "metal-test";
    frame.metadata.frame_number = 0;
    frame.metadata.local_frame_number = 0;
    frame.metadata.frame_pts = 0;
    frame.metadata.time_base = {1, 100};
    frame.metadata.width = width;
    frame.metadata.height = height;
    frame.metadata.pixel_format = FramePixelFormat::NV12;
    frame.metadata.plane_count = 2;
    size_t offset = 0;
    for (size_t plane = 0; plane < 2; ++plane) {
        FramePlaneLayout& layout = frame.metadata.planes[plane];
        layout.offset_bytes = offset;
        layout.row_stride_bytes = static_cast<int>(
            CVPixelBufferGetBytesPerRowOfPlane(buffer, plane));
        layout.width_pixels = static_cast<int>(
            CVPixelBufferGetWidthOfPlane(buffer, plane));
        layout.height_pixels = static_cast<int>(
            CVPixelBufferGetHeightOfPlane(buffer, plane));
        layout.bytes_per_element = plane == 0 ? 1 : 2;
        offset += static_cast<size_t>(layout.row_stride_bytes) *
                  static_cast<size_t>(layout.height_pixels);
    }
    frame.metadata.pitch_bytes = frame.metadata.planes[0].row_stride_bytes;
    frame.metadata.frame_bytes = offset;
    frame.metadata.color_matrix = matrix;
    frame.metadata.color_range =
        full_range ? ColorRange_JPEG : ColorRange_MPEG;
    frame.metadata.surface_backend = FrameSurfaceBackend::AppleVideoToolbox;
    frame.metadata.ownership = FrameSurfaceOwnership::ReferenceCounted;
    frame.metadata.lifetime = FrameSurfaceLifetime::ReferenceCounted;
    frame.surface = std::make_shared<TestPixelBufferSurface>(
        buffer, frameSurfaceDescriptorFromMetadata(frame.metadata));
    CVPixelBufferRelease(buffer);
    return frame;
}

AppleDecodedVideoFrame makeBgraSplitFrame() {
    constexpr int width = 16;
    constexpr int height = 16;
    NSDictionary* attributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferRef buffer = nullptr;
    CHECK(CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                              kCVPixelFormatType_32BGRA,
                              (__bridge CFDictionaryRef)attributes,
                              &buffer) == kCVReturnSuccess);
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto* pixels = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            uint8_t* pixel = pixels + row * stride + column * 4;
            if (column < width / 2) {
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 255;
            } else {
                pixel[0] = 0;
                pixel[1] = 255;
                pixel[2] = 0;
            }
            pixel[3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);

    AppleDecodedVideoFrame frame;
    frame.metadata.stream_id = "metal-bgra-test";
    frame.metadata.frame_number = 0;
    frame.metadata.local_frame_number = 0;
    frame.metadata.width = width;
    frame.metadata.height = height;
    frame.metadata.pixel_format = FramePixelFormat::BGRA8;
    frame.metadata.plane_count = 1;
    frame.metadata.planes[0].row_stride_bytes = static_cast<int>(stride);
    frame.metadata.planes[0].width_pixels = width;
    frame.metadata.planes[0].height_pixels = height;
    frame.metadata.planes[0].bytes_per_element = 4;
    frame.metadata.pitch_bytes = static_cast<int>(stride);
    frame.metadata.frame_bytes = stride * height;
    frame.metadata.surface_backend = FrameSurfaceBackend::AppleVideoToolbox;
    frame.metadata.ownership = FrameSurfaceOwnership::ReferenceCounted;
    frame.metadata.lifetime = FrameSurfaceLifetime::ReferenceCounted;
    frame.surface = std::make_shared<TestPixelBufferSurface>(
        buffer, frameSurfaceDescriptorFromMetadata(frame.metadata));
    CVPixelBufferRelease(buffer);
    return frame;
}

std::array<uint8_t, 4> render(AppleVideoMetalRenderer& renderer,
                              AppleDecodedVideoFrame frame,
                              bool* retained_until_completion = nullptr,
                              const AppleMetalVideoSourceRegion* region =
                                  nullptr) {
    constexpr NSUInteger width = 16;
    constexpr NSUInteger height = 16;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:width
                                    height:height
                                 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [command renderCommandEncoderWithDescriptor:pass];
    std::string error;
    std::weak_ptr<const FrameSurface> weak = frame.surface;
    const AppleMetalVideoViewport viewport{
        0.0, 0.0, static_cast<double>(width), static_cast<double>(height)};
    CHECK(region ? renderer.encodeRegion(
                       frame,
                       reinterpret_cast<uintptr_t>((__bridge void*)command),
                       reinterpret_cast<uintptr_t>((__bridge void*)encoder),
                       viewport, *region, &error)
                 : renderer.encode(
                       frame,
                       reinterpret_cast<uintptr_t>((__bridge void*)command),
                       reinterpret_cast<uintptr_t>((__bridge void*)encoder),
                       viewport, &error));
    frame.surface.reset();
    if (retained_until_completion != nullptr) {
        *retained_until_completion = !weak.expired();
    }
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted);
    std::array<uint8_t, width * height * 4> pixels{};
    [target getBytes:pixels.data()
          bytesPerRow:width * 4
           fromRegion:MTLRegionMake2D(0, 0, width, height)
          mipmapLevel:0];
    const size_t center = ((height / 2) * width + width / 2) * 4;
    return {pixels[center], pixels[center + 1], pixels[center + 2],
            pixels[center + 3]};
}

void checkNear(uint8_t actual, int expected, int tolerance) {
    CHECK(std::abs(static_cast<int>(actual) - expected) <= tolerance);
}

void runTests() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device != nil);
    AppleVideoMetalRenderer renderer;
    std::string error;
    CHECK(renderer.initialize(
        reinterpret_cast<uintptr_t>((__bridge void*)device),
        static_cast<uint64_t>(MTLPixelFormatBGRA8Unorm), &error));

    bool retained = false;
    const auto limited_black =
        render(renderer,
               makeFrame(false, ColorSpaceStandard_BT709, 16, 128, 128),
               &retained);
    CHECK(retained);
    checkNear(limited_black[0], 0, 2);
    checkNear(limited_black[1], 0, 2);
    checkNear(limited_black[2], 0, 2);

    const auto limited_white = render(
        renderer, makeFrame(false, ColorSpaceStandard_BT709, 235, 128, 128));
    checkNear(limited_white[0], 255, 2);
    checkNear(limited_white[1], 255, 2);
    checkNear(limited_white[2], 255, 2);

    const auto full_mid = render(
        renderer, makeFrame(true, ColorSpaceStandard_BT709, 128, 128, 128));
    checkNear(full_mid[0], 128, 2);
    checkNear(full_mid[1], 128, 2);
    checkNear(full_mid[2], 128, 2);

    const auto bt601 = render(
        renderer, makeFrame(true, ColorSpaceStandard_BT601, 100, 90, 200));
    const auto bt709 = render(
        renderer, makeFrame(true, ColorSpaceStandard_BT709, 100, 90, 200));
    checkNear(bt601[0], 34, 2);
    checkNear(bt601[1], 61, 2);
    checkNear(bt601[2], 202, 2);
    checkNear(bt709[0], 30, 2);
    checkNear(bt709[1], 73, 2);
    checkNear(bt709[2], 214, 2);

    const auto bt2020 = render(
        renderer, makeFrame(true, ColorSpaceStandard_BT2020, 100, 90, 200));
    checkNear(bt2020[0], 29, 2);
    checkNear(bt2020[1], 65, 2);
    checkNear(bt2020[2], 207, 2);

    const AppleMetalVideoSourceRegion left_half{0.0, 0.0, 0.5, 1.0};
    const auto cropped_left =
        render(renderer, makeBgraSplitFrame(), nullptr, &left_half);
    checkNear(cropped_left[0], 0, 2);
    checkNear(cropped_left[1], 0, 2);
    checkNear(cropped_left[2], 255, 2);

    const AppleMetalVideoSourceRegion right_half{0.5, 0.0, 0.5, 1.0};
    const auto cropped_right =
        render(renderer, makeBgraSplitFrame(), nullptr, &right_half);
    checkNear(cropped_right[0], 0, 2);
    checkNear(cropped_right[1], 255, 2);
    checkNear(cropped_right[2], 0, 2);
}

}  // namespace

int main() {
    @autoreleasepool {
        try {
            runTests();
        } catch (const TestFailure& failure) {
            std::cerr << failure.message << std::endl;
            return 1;
        }
        std::cout << "apple_video_metal_tests: PASS" << std::endl;
        return 0;
    }
}
