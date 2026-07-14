#include "platform/macos/apple_overlay_metal_renderer.h"
#include "tests/fixtures/read_only_overlay_scene_fixture.h"

#import <Metal/Metal.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct TestFailure {
    std::string message;
};

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw TestFailure{std::string("CHECK failed: ") + #condition +   \
                              " at " + __FILE__ + ":" +                   \
                              std::to_string(__LINE__)};                       \
        }                                                                      \
    } while (false)

using Pixel = std::array<uint8_t, 4>;

constexpr uint32_t kWidth = 360;
constexpr uint32_t kHeight = 220;
constexpr Pixel kClearPixel{41, 31, 20, 255};

struct RenderedImage {
    std::vector<Pixel> pixels;

    const Pixel& at(uint32_t x, uint32_t y) const {
        return pixels[static_cast<size_t>(y) * kWidth + x];
    }
};

bool nearChannel(uint8_t actual, int expected, int tolerance = 4) {
    return std::abs(static_cast<int>(actual) - expected) <= tolerance;
}

bool isClear(const Pixel& pixel) {
    return nearChannel(pixel[0], kClearPixel[0], 1) &&
           nearChannel(pixel[1], kClearPixel[1], 1) &&
           nearChannel(pixel[2], kClearPixel[2], 1) &&
           pixel[3] == 255;
}

RenderedImage render(id<MTLDevice> device,
                     id<MTLCommandQueue> queue,
                     AppleOverlayMetalRenderer& renderer,
                     const crimson::overlay::ReadOnlyOverlayScene& scene,
                     const crimson::overlay::SourceViewportTransform& transform) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:kWidth
                                    height:kHeight
                                 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
    CHECK(target != nil);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(0.08, 0.12, 0.16, 1.0);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [command renderCommandEncoderWithDescriptor:pass];
    CHECK(command != nil);
    CHECK(encoder != nil);
    std::string error;
    CHECK(renderer.encode(
        scene, transform,
        reinterpret_cast<uintptr_t>((__bridge void*)encoder), kWidth, kHeight,
        &error));
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted);

    RenderedImage result;
    result.pixels.resize(static_cast<size_t>(kWidth) * kHeight);
    [target getBytes:result.pixels.data()
          bytesPerRow:kWidth * sizeof(Pixel)
           fromRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
          mipmapLevel:0];
    return result;
}

size_t changedPixelCount(const RenderedImage& image) {
    size_t changed = 0;
    for (const Pixel& pixel : image.pixels) {
        changed += !isClear(pixel);
    }
    return changed;
}

void checkOutsideIsClear(
    const RenderedImage& image,
    const crimson::overlay::Rect& display) {
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            if (x >= display.x && x < display.x + display.width &&
                y >= display.y && y < display.y + display.height) {
                continue;
            }
            CHECK(isClear(image.at(x, y)));
        }
    }
}

void runTest() {
    using namespace crimson::overlay;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    CHECK(queue != nil);
    AppleOverlayMetalRenderer renderer;
    std::string error;
    if (!renderer.initialize(
            reinterpret_cast<uintptr_t>((__bridge void*)device),
            static_cast<uint64_t>(MTLPixelFormatBGRA8Unorm), &error)) {
        throw TestFailure{"Metal overlay initialization failed: " + error};
    }

    const ReadOnlyOverlayScene scene =
        buildReadOnlyOverlayScene(fixture::makeReadOnlyOverlayInput());
    CHECK(scene.ready());
    const SourceViewportTransform full{{0.0, 0.0, 640.0, 360.0},
                                       {20.0, 20.0, 320.0, 180.0}};
    const RenderedImage image = render(device, queue, renderer, scene, full);
    CHECK(changedPixelCount(image) > 400);
    checkOutsideIsClear(image, full.display);

    const Pixel clean_box = image.at(110, 60);
    CHECK(nearChannel(clean_box[0], 255));
    CHECK(nearChannel(clean_box[1], 153));
    CHECK(nearChannel(clean_box[2], 51));
    const Pixel interpolated_box = image.at(220, 65);
    CHECK(interpolated_box[0] < 15);
    CHECK(interpolated_box[1] >= 155 && interpolated_box[1] <= 175);
    CHECK(interpolated_box[2] >= 225 && interpolated_box[2] <= 240);
    const Pixel heading = image.at(140, 100);
    CHECK(heading[0] < 40);
    CHECK(heading[1] >= 50 && heading[1] <= 75);
    CHECK(heading[2] >= 235);
    const Pixel marker = image.at(100, 100);
    CHECK(marker[0] < 70);
    CHECK(marker[1] >= 195);
    CHECK(marker[2] >= 240);

    const SourceViewportTransform zoom{{80.0, 60.0, 320.0, 180.0},
                                       {20.0, 20.0, 320.0, 180.0}};
    const RenderedImage zoomed = render(device, queue, renderer, scene, zoom);
    CHECK(changedPixelCount(zoomed) > 400);
    checkOutsideIsClear(zoomed, zoom.display);
    CHECK(!isClear(zoomed.at(40, 40)));

    ReadOnlyOverlayInput mask_input;
    mask_input.identity = {0, 7, 0, 7};
    mask_input.source_width = 100.0;
    mask_input.source_height = 100.0;
    mask_input.show_boxes = false;
    mask_input.show_headings = false;
    mask_input.show_keypoints = false;
    auto alpha = std::make_shared<std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{0, 0, 0, 0,
                                       0, 255, 255, 0,
                                       0, 255, 255, 0,
                                       0, 0, 0, 0});
    SubjectMaskComponentInput body;
    body.label = "subject_body";
    body.source_crop_row_id = 17;
    body.source_rect = {20.0, 20.0, 40.0, 40.0};
    body.mask_width = 4;
    body.mask_height = 4;
    body.mask = alpha;
    body.contour = {{20.0, 20.0}, {60.0, 20.0},
                    {60.0, 60.0}, {20.0, 60.0}};
    mask_input.subject_masks.push_back(std::move(body));
    const ReadOnlyOverlayScene mask_scene =
        buildReadOnlyOverlayScene(mask_input);
    CHECK(mask_scene.ready());
    CHECK(mask_scene.rasterCount(CameraOverlayLayer::SubjectMasks) == 1);
    CHECK(mask_scene.count(CameraOverlayLayer::SubjectMasks) == 1);
    const SourceViewportTransform mask_full{{0.0, 0.0, 100.0, 100.0},
                                            {40.0, 20.0, 180.0, 180.0}};
    const RenderedImage masked =
        render(device, queue, renderer, mask_scene, mask_full);
    checkOutsideIsClear(masked, mask_full.display);
    CHECK(changedPixelCount(masked) > 1500);
    const Pixel mask_fill = masked.at(112, 92);
    CHECK(mask_fill[0] > 65);
    CHECK(mask_fill[1] > 50);
    CHECK(isClear(masked.at(80, 60)));

    const SourceViewportTransform mask_zoom{{30.0, 30.0, 20.0, 20.0},
                                            {40.0, 20.0, 180.0, 180.0}};
    const RenderedImage masked_zoom =
        render(device, queue, renderer, mask_scene, mask_zoom);
    checkOutsideIsClear(masked_zoom, mask_zoom.display);
    CHECK(!isClear(masked_zoom.at(130, 110)));

    ReadOnlyOverlayInput shape_input;
    shape_input.identity = {0, 9, 0, 9};
    shape_input.source_width = 100.0;
    shape_input.source_height = 100.0;
    shape_input.show_boxes = false;
    shape_input.show_headings = false;
    shape_input.show_keypoints = false;
    SubjectShapeInput shape;
    shape.shape_row = 17;
    shape.detection_index = 0;
    shape.source_refined_row_id = 51;
    shape.source_crop_row_id = 33;
    shape.source_rect = {20.0, 20.0, 60.0, 60.0};
    shape.coordinate_width = 10.0;
    shape.coordinate_height = 10.0;
    shape.snout_tip_valid = true;
    shape.snout_tip = {5.0, 5.0};
    shape.tail_base_valid = true;
    shape.tail_base = {3.0, 7.0};
    shape.tail_tip = {1.0, 9.0};
    shape.caudal_anchor_valid = true;
    shape.caudal_anchor = {6.0, 6.0};
    shape.centerline_valid = true;
    shape.centerline = {{2.0, 2.0}, {5.0, 5.0}, {8.0, 8.0}};
    shape.bspline_valid = true;
    shape.bspline_sample = {{2.0, 3.0}, {5.0, 6.0}, {8.0, 9.0}};
    shape_input.subject_shapes.push_back(std::move(shape));
    const ReadOnlyOverlayScene shape_scene =
        buildReadOnlyOverlayScene(shape_input);
    CHECK(shape_scene.ready());
    CHECK(shape_scene.count(CameraOverlayLayer::SubjectShape) == 6);
    const SourceViewportTransform shape_full{{0.0, 0.0, 100.0, 100.0},
                                             {40.0, 20.0, 180.0, 180.0}};
    const RenderedImage shaped =
        render(device, queue, renderer, shape_scene, shape_full);
    checkOutsideIsClear(shaped, shape_full.display);
    CHECK(changedPixelCount(shaped) > 150);
    CHECK(!isClear(shaped.at(130, 110)));

    --shape_input.identity.overlay_frame;
    const ReadOnlyOverlayScene stale_shape =
        buildReadOnlyOverlayScene(shape_input);
    CHECK(!stale_shape.ready());
    const RenderedImage withheld_shape =
        render(device, queue, renderer, stale_shape, shape_full);
    CHECK(changedPixelCount(withheld_shape) == 0);

    --mask_input.identity.overlay_frame;
    const ReadOnlyOverlayScene stale_mask =
        buildReadOnlyOverlayScene(mask_input);
    CHECK(!stale_mask.ready());
    const RenderedImage withheld_mask =
        render(device, queue, renderer, stale_mask, mask_full);
    CHECK(changedPixelCount(withheld_mask) == 0);

    auto stale_input = fixture::makeReadOnlyOverlayInput();
    --stale_input.identity.overlay_frame;
    const ReadOnlyOverlayScene stale = buildReadOnlyOverlayScene(stale_input);
    CHECK(!stale.ready());
    const RenderedImage withheld =
        render(device, queue, renderer, stale, full);
    CHECK(changedPixelCount(withheld) == 0);
    renderer.reset();
    std::cout << "apple_read_only_overlay_metal_tests: PASS primitives="
              << scene.primitives.size() << " triangles="
              << tessellateReadOnlyOverlayScene(scene, full).triangleCount()
              << '\n';
}

}  // namespace

int main() {
    @autoreleasepool {
        try {
            runTest();
        } catch (const TestFailure& failure) {
            std::cerr << failure.message << '\n';
            return 1;
        } catch (const std::exception& exception) {
            std::cerr << "unexpected exception: " << exception.what() << '\n';
            return 1;
        }
    }
    return 0;
}
