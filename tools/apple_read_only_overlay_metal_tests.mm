#include "platform/macos/apple_overlay_metal_renderer.h"
#include "tests/fixtures/read_only_overlay_scene_fixture.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

RenderedImage renderPolar(
    id<MTLDevice> device,
    id<MTLCommandQueue> queue,
    AppleOverlayMetalRenderer& renderer,
    const crimson::polar::ChaserDistancePolarScene& scene,
    crimson::polar::ChaserDistancePolarScenePoint display_origin,
    double scale_x = 1.0,
    double scale_y = 1.0) {
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
    CHECK(renderer.encodePolar(
        scene, display_origin, scale_x, scale_y,
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

RenderedImage renderStimulusCameraOverlay(
    id<MTLDevice> device,
    id<MTLCommandQueue> queue,
    AppleOverlayMetalRenderer& renderer,
    const crimson::stimulus::StimulusCameraOverlayScene& scene,
    crimson::stimulus::StimulusCameraOverlayPoint display_origin,
    double scale_x = 1.0,
    double scale_y = 1.0) {
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
    CHECK(renderer.encodeStimulusCameraOverlay(
        scene, display_origin, scale_x, scale_y,
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

using BinaryMask = std::vector<uint8_t>;

BinaryMask changedPixelMask(const RenderedImage& image) {
    BinaryMask result(image.pixels.size(), 0);
    std::transform(image.pixels.begin(), image.pixels.end(), result.begin(),
                   [](const Pixel& pixel) { return !isClear(pixel); });
    return result;
}

BinaryMask rectangleMask(const crimson::overlay::Rect& rectangle) {
    BinaryMask result(static_cast<size_t>(kWidth) * kHeight, 0);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const double center_x = static_cast<double>(x) + 0.5;
            const double center_y = static_cast<double>(y) + 0.5;
            result[static_cast<size_t>(y) * kWidth + x] =
                center_x >= rectangle.x &&
                center_x < rectangle.x + rectangle.width &&
                center_y >= rectangle.y &&
                center_y < rectangle.y + rectangle.height;
        }
    }
    return result;
}

double maskIou(const BinaryMask& reference, const BinaryMask& actual) {
    CHECK(reference.size() == actual.size());
    size_t intersection = 0;
    size_t union_count = 0;
    for (size_t index = 0; index < reference.size(); ++index) {
        intersection += reference[index] != 0 && actual[index] != 0;
        union_count += reference[index] != 0 || actual[index] != 0;
    }
    return union_count == 0
               ? 1.0
               : static_cast<double>(intersection) /
                     static_cast<double>(union_count);
}

bool maskNear(const BinaryMask& mask, int x, int y, int radius) {
    for (int offset_y = -radius; offset_y <= radius; ++offset_y) {
        for (int offset_x = -radius; offset_x <= radius; ++offset_x) {
            if (offset_x * offset_x + offset_y * offset_y > radius * radius) {
                continue;
            }
            const int candidate_x = x + offset_x;
            const int candidate_y = y + offset_y;
            if (candidate_x >= 0 && candidate_x < static_cast<int>(kWidth) &&
                candidate_y >= 0 && candidate_y < static_cast<int>(kHeight) &&
                mask[static_cast<size_t>(candidate_y) * kWidth +
                     static_cast<size_t>(candidate_x)] != 0) {
                return true;
            }
        }
    }
    return false;
}

double horizontalReferenceCoverage(const BinaryMask& actual, int x0, int x1,
                                   int y, int radius) {
    size_t covered = 0;
    const size_t count = static_cast<size_t>(x1 - x0 + 1);
    for (int x = x0; x <= x1; ++x) {
        covered += maskNear(actual, x, y, radius);
    }
    return static_cast<double>(covered) / static_cast<double>(count);
}

bool allMaskPixelsNearHorizontalReference(const BinaryMask& actual, int x0,
                                          int x1, int y, int radius) {
    const int radius_squared = radius * radius;
    for (int actual_y = 0; actual_y < static_cast<int>(kHeight); ++actual_y) {
        for (int actual_x = 0; actual_x < static_cast<int>(kWidth); ++actual_x) {
            if (actual[static_cast<size_t>(actual_y) * kWidth +
                       static_cast<size_t>(actual_x)] == 0) {
                continue;
            }
            const int nearest_x = std::clamp(actual_x, x0, x1);
            const int delta_x = actual_x - nearest_x;
            const int delta_y = actual_y - y;
            if (delta_x * delta_x + delta_y * delta_y > radius_squared) {
                return false;
            }
        }
    }
    return true;
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

    crimson::polar::ChaserDistancePolarDescriptor polar_descriptor;
    polar_descriptor.availability =
        crimson::polar::ChaserDistancePolarAvailability::Ready;
    polar_descriptor.provenance.run_name = "run";
    polar_descriptor.provenance.component_name = "component";
    polar_descriptor.provenance.run_selection =
        crimson::polar::ChaserDistancePolarSelectionProvenance::LatestComplete;
    polar_descriptor.provenance.component_selection =
        crimson::polar::ChaserDistancePolarSelectionProvenance::LatestCompleted;
    polar_descriptor.row_count = 20;
    polar_descriptor.chaser_count = 2;
    polar_descriptor.coordinate_frame =
        std::string(crimson::polar::kArenaRelativeCanvasPixelFrame);
    polar_descriptor.angle_convention =
        std::string(crimson::polar::kPositiveAnatomicalLeftAngleConvention);
    polar_descriptor.dataset_global_max_distance_mm = 100.0;
    polar_descriptor = crimson::polar::normalizeChaserDistancePolarDescriptor(
        std::move(polar_descriptor));
    crimson::polar::ChaserDistancePolarPoint polar_front;
    polar_front.chaser_index = 0;
    polar_front.distance_mm = 105.0;
    polar_front.bearing_degrees = 0.0;
    polar_front.valid = true;
    polar_front.color = crimson::polar::resolveChaserDistancePolarColor(
        0, std::nullopt, std::nullopt);
    crimson::polar::ChaserDistancePolarPoint polar_left;
    polar_left.chaser_index = 1;
    polar_left.distance_mm = 105.0;
    polar_left.bearing_degrees = 90.0;
    polar_left.valid = true;
    polar_left.color = crimson::polar::resolveChaserDistancePolarColor(
        1, std::nullopt, std::nullopt);
    const auto polar_sample =
        crimson::polar::makeChaserDistancePolarFrameSample(
            polar_descriptor, 7, 7,
            {std::move(polar_front), std::move(polar_left)});
    crimson::polar::ChaserDistancePolarSceneControls polar_controls;
    polar_controls.show_readout = false;
    const auto polar_scene = crimson::polar::buildChaserDistancePolarScene(
        polar_sample, {300.0, 200.0}, polar_controls);
    CHECK(polar_scene.ready());
    const crimson::polar::ChaserDistancePolarScenePoint polar_origin{30.0,
                                                                     10.0};
    const RenderedImage polar_image =
        renderPolar(device, queue, renderer, polar_scene, polar_origin);
    CHECK(changedPixelCount(polar_image) > 10000);
    checkOutsideIsClear(polar_image, {polar_origin.x, polar_origin.y,
                                     polar_scene.viewport.width_px,
                                     polar_scene.viewport.height_px});
    const auto polar_marker = std::find_if(
        polar_scene.primitives.begin(), polar_scene.primitives.end(),
        [](const auto& primitive) {
            return primitive.type ==
                       crimson::polar::ChaserDistancePolarScenePrimitiveType::
                           Marker &&
                   primitive.chaser_index == 0;
        });
    CHECK(polar_marker != polar_scene.primitives.end());
    const Pixel marker_pixel = polar_image.at(
        static_cast<uint32_t>(
            std::lround(polar_origin.x + polar_marker->first.x)),
        static_cast<uint32_t>(
            std::lround(polar_origin.y + polar_marker->first.y)));
    CHECK(marker_pixel[0] < 40);
    CHECK(marker_pixel[1] < 45);
    CHECK(marker_pixel[2] > 240);

    auto withheld_polar_scene = polar_scene;
    withheld_polar_scene.status =
        crimson::polar::ChaserDistancePolarSceneStatus::NonExactFrame;
    CHECK(changedPixelCount(renderPolar(device, queue, renderer,
                                        withheld_polar_scene, polar_origin)) ==
          0);
    CHECK(changedPixelCount(renderPolar(device, queue, renderer, polar_scene,
                                        {-500.0, -500.0})) == 0);

    crimson::timeline::StimulusContextTimelineSnapshot stimulus_snapshot;
    stimulus_snapshot.descriptor.frame_count = 20;
    crimson::timeline::StimulusContextEvent stimulus_event;
    stimulus_event.source_event_index = 0;
    stimulus_event.camera_frame = 7;
    stimulus_event.event_type_id = 3;
    stimulus_event.label = "Trial start - approach";
    stimulus_snapshot.events.push_back(std::move(stimulus_event));
    crimson::timeline::StimulusContextStep stimulus_step;
    stimulus_step.step_index = 2;
    stimulus_step.stimulus_mode = "MOVING_GRATING";
    stimulus_step.kind = crimson::timeline::StimulusStepKind::MovingGrating;
    stimulus_step.start_camera_frame = 5;
    stimulus_step.end_camera_frame = 12;
    stimulus_step.moving_grating.present = true;
    stimulus_step.moving_grating.grating_direction_camera_deg = 0.0;
    stimulus_snapshot.steps.push_back(std::move(stimulus_step));
    const auto stimulus_frame =
        crimson::stimulus::resolveStimulusCameraOverlayFrame(
            &stimulus_snapshot, 7);
    const auto stimulus_scene =
        crimson::stimulus::buildStimulusCameraOverlayScene(
            stimulus_frame, {300.0, 200.0}, {135.0, 14.0});
    CHECK(stimulus_scene.ready());
    const crimson::stimulus::StimulusCameraOverlayPoint stimulus_origin{
        30.0, 10.0};
    const RenderedImage stimulus_image = renderStimulusCameraOverlay(
        device, queue, renderer, stimulus_scene, stimulus_origin);
    CHECK(changedPixelCount(stimulus_image) > 12000);
    checkOutsideIsClear(stimulus_image,
                        {stimulus_origin.x, stimulus_origin.y,
                         stimulus_scene.viewport.width_px,
                         stimulus_scene.viewport.height_px});
    const Pixel event_panel_pixel = stimulus_image.at(50, 35);
    CHECK(!isClear(event_panel_pixel));
    const Pixel arrow_pixel = stimulus_image.at(228, 71);
    CHECK(arrow_pixel[0] < 100);
    CHECK(arrow_pixel[1] > 180);
    CHECK(arrow_pixel[2] > 220);

    auto withheld_stimulus_scene = stimulus_scene;
    withheld_stimulus_scene.status =
        crimson::stimulus::StimulusCameraOverlaySceneStatus::NonExactFrame;
    CHECK(changedPixelCount(renderStimulusCameraOverlay(
              device, queue, renderer, withheld_stimulus_scene,
              stimulus_origin)) == 0);
    CHECK(changedPixelCount(renderStimulusCameraOverlay(
              device, queue, renderer, stimulus_scene,
              {-500.0, -500.0})) == 0);

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
    mask_input.show_subject_mask_contours = false;
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
    mask_input.subject_masks.push_back(std::move(body));
    const ReadOnlyOverlayScene mask_scene =
        buildReadOnlyOverlayScene(mask_input);
    CHECK(mask_scene.ready());
    CHECK(mask_scene.rasterCount(CameraOverlayLayer::SubjectMasks) == 1);
    CHECK(mask_scene.count(CameraOverlayLayer::SubjectMasks) == 0);
    const SourceViewportTransform mask_full{{0.0, 0.0, 100.0, 100.0},
                                            {40.0, 20.0, 180.0, 180.0}};
    const RenderedImage masked =
        render(device, queue, renderer, mask_scene, mask_full);
    checkOutsideIsClear(masked, mask_full.display);
    CHECK(changedPixelCount(masked) > 1200);
    const auto expected_mask_bounds =
        mask_full.sourceToDisplay(
            crimson::overlay::Rect{30.0, 30.0, 20.0, 20.0});
    CHECK(expected_mask_bounds.has_value());
    const double mask_iou = maskIou(rectangleMask(*expected_mask_bounds),
                                    changedPixelMask(masked));
    if (mask_iou < kPhase5ParityThresholds.mask_iou) {
        throw TestFailure{"Mask IoU " + std::to_string(mask_iou) +
                          " is below " +
                          std::to_string(kPhase5ParityThresholds.mask_iou)};
    }
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

    ReadOnlyOverlayScene vector_scene;
    vector_scene.status = ReadOnlyOverlayBuildStatus::Ready;
    vector_scene.identity = {0, 8, 0, 8};
    vector_scene.source_width = 100.0;
    vector_scene.source_height = 100.0;
    Primitive reference_line;
    reference_line.type = PrimitiveType::Polyline;
    reference_line.layer = CameraOverlayLayer::Keypoints;
    reference_line.points = {{10.0, 50.0}, {90.0, 50.0}};
    reference_line.stroke = {0.2f, 0.4f, 0.8f, 1.0f};
    reference_line.stroke_width_px = 1.0;
    vector_scene.primitives.push_back(std::move(reference_line));
    const RenderedImage vector_image =
        render(device, queue, renderer, vector_scene, mask_full);
    const BinaryMask vector_pixels = changedPixelMask(vector_image);
    const auto line_start = mask_full.sourceToDisplay(
        crimson::overlay::Point{10.0, 50.0});
    const auto line_end = mask_full.sourceToDisplay(
        crimson::overlay::Point{90.0, 50.0});
    CHECK(line_start.has_value());
    CHECK(line_end.has_value());
    const int reference_x0 =
        static_cast<int>(std::ceil(line_start->x - 0.5));
    const int reference_x1 =
        static_cast<int>(std::floor(line_end->x - 0.5));
    const int reference_y = static_cast<int>(std::lround(line_start->y));
    const double vector_coverage = horizontalReferenceCoverage(
        vector_pixels, reference_x0, reference_x1, reference_y,
        static_cast<int>(kPhase5ParityThresholds.vector_coverage_radius_px));
    const bool vector_has_no_outliers = allMaskPixelsNearHorizontalReference(
        vector_pixels, reference_x0, reference_x1, reference_y,
        static_cast<int>(kPhase5ParityThresholds.vector_max_outlier_px));
    if (vector_coverage < kPhase5ParityThresholds.vector_coverage ||
        !vector_has_no_outliers) {
        throw TestFailure{
            "Vector coverage=" + std::to_string(vector_coverage) +
            " no_outliers=" +
            std::string(vector_has_no_outliers ? "true" : "false") +
            " reference=" + std::to_string(reference_x0) + ".." +
            std::to_string(reference_x1) + "@" +
            std::to_string(reference_y)};
    }
    const uint32_t color_x =
        static_cast<uint32_t>((reference_x0 + reference_x1) / 2);
    Pixel vector_color = vector_image.at(color_x,
                                         static_cast<uint32_t>(reference_y));
    if (isClear(vector_color) && reference_y > 0) {
        vector_color = vector_image.at(
            color_x, static_cast<uint32_t>(reference_y - 1));
    }
    if (isClear(vector_color) &&
        reference_y + 1 < static_cast<int>(kHeight)) {
        vector_color = vector_image.at(
            color_x, static_cast<uint32_t>(reference_y + 1));
    }
    CHECK(!isClear(vector_color));
    CHECK(nearChannel(vector_color[0], 204,
                      kPhase5ParityThresholds.raster_channel_delta));
    CHECK(nearChannel(vector_color[1], 102,
                      kPhase5ParityThresholds.raster_channel_delta));
    CHECK(nearChannel(vector_color[2], 51,
                      kPhase5ParityThresholds.raster_channel_delta));

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

    ReadOnlyOverlayInput eye_input;
    eye_input.identity = {0, 11, 0, 11};
    eye_input.source_width = 100.0;
    eye_input.source_height = 100.0;
    eye_input.show_boxes = false;
    eye_input.show_headings = false;
    eye_input.show_keypoints = false;
    EyeGeometryInput eye_geometry;
    eye_geometry.eye_row = 29;
    eye_geometry.detection_index = 0;
    eye_geometry.source_crop_row_id = 43;
    eye_geometry.source_rect = {20.0, 20.0, 60.0, 60.0};
    eye_geometry.coordinate_width = 10.0;
    eye_geometry.coordinate_height = 10.0;
    eye_geometry.frame_valid = true;
    eye_geometry.body_frame_valid = true;
    eye_geometry.body_forward_axis = {1.0, 0.0};
    eye_geometry.body_left_axis = {0.0, 1.0};
    eye_geometry.vergence_valid = true;
    eye_geometry.vergence_degrees = 8.0;
    for (size_t eye = 0; eye < eye_geometry.eyes.size(); ++eye) {
        auto& values = eye_geometry.eyes[eye];
        values.valid = true;
        const double x = eye == 0 ? 4.0 : 6.0;
        values.major_axis = {true, {x - 1.5, 5.0}, {x + 1.5, 5.0}};
        values.minor_axis = {true, {x, 4.0}, {x, 6.0}};
        values.gaze_valid = true;
        values.gaze = eye == 0 ? crimson::overlay::Point{1.0, 0.15}
                               : crimson::overlay::Point{1.0, -0.15};
        values.signed_angle_valid = true;
        values.signed_angle_degrees = eye == 0 ? 12.0 : -10.0;
        values.eye_frame_angle_valid = true;
        values.eye_frame_angle_degrees = eye == 0 ? 9.0 : -7.0;
    }
    eye_input.eye_geometry.push_back(std::move(eye_geometry));
    const ReadOnlyOverlayScene eye_scene =
        buildReadOnlyOverlayScene(eye_input);
    CHECK(eye_scene.ready());
    CHECK(eye_scene.count(PrimitiveType::Polygon) >= 2);
    CHECK(eye_scene.textCount(CameraOverlayLayer::SubjectMasks) == 3);
    const SourceViewportTransform eye_full{{0.0, 0.0, 100.0, 100.0},
                                           {40.0, 20.0, 180.0, 180.0}};
    const ScreenMesh eye_mesh =
        tessellateReadOnlyOverlayScene(eye_scene, eye_full);
    CHECK(eye_mesh.primitive_count == eye_scene.primitives.size());
    CHECK(eye_mesh.triangleCount() > 100);
    const RenderedImage eyes =
        render(device, queue, renderer, eye_scene, eye_full);
    checkOutsideIsClear(eyes, eye_full.display);
    CHECK(changedPixelCount(eyes) > 1500);
    CHECK(!isClear(eyes.at(130, 110)));

    --eye_input.identity.overlay_frame;
    const ReadOnlyOverlayScene stale_eye =
        buildReadOnlyOverlayScene(eye_input);
    CHECK(!stale_eye.ready());
    const RenderedImage withheld_eye =
        render(device, queue, renderer, stale_eye, eye_full);
    CHECK(changedPixelCount(withheld_eye) == 0);

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
