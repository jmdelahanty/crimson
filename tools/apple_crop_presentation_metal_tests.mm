#include "crop_presentation_coordinator.h"
#include "platform/macos/apple_acquisition_crop_playback_session.h"
#include "platform/macos/apple_video_metal_renderer.h"
#include "platform/macos/apple_video_playback_buffer.h"
#include "zarr/acquisition_crop_repository.h"

#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct TestFailure {
  std::string message;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure{std::string("CHECK failed: ") + #condition +          \
                        " at " + __FILE__ + ":" +                            \
                        std::to_string(__LINE__)};                              \
    }                                                                          \
  } while (false)

using Pixel = std::array<uint8_t, 4>;

std::unique_ptr<crimson::zarr::AcquisitionCropRepository> MakeRepository(
    const std::string& video_path) {
  crimson::zarr::AcquisitionCropStreamDescriptor descriptor;
  descriptor.schema_id = "palette.acquisition_video_streams.v1";
  descriptor.schema_version = 1;
  descriptor.stream_id = "crop_metal_fixture";
  descriptor.output_width = 64;
  descriptor.output_height = 48;
  descriptor.frame_count = 12;
  descriptor.frame_rate = 10.0;
  descriptor.resolved_video_path = video_path;

  std::vector<crimson::zarr::AcquisitionCropFrameRow> rows;
  for (int64_t frame = 0; frame < 12; ++frame) {
    crimson::zarr::AcquisitionCropFrameRow row;
    row.recording_frame_id = frame + 1;
    row.local_frame_id = frame;
    row.camera_frame_id = frame;
    row.has_detection = true;
    row.full_frame_crop = {0.0, 0.0, 32.0, 48.0};
    row.full_frame_detection = crimson::crop::CropRect{
        8.0, 12.0, 12.0, 16.0};
    rows.push_back(std::move(row));
  }
  return crimson::zarr::MakeAcquisitionCropRepository(
      std::move(descriptor), std::move(rows));
}

Pixel ReadPixel(id<MTLTexture> texture, NSUInteger x, NSUInteger y) {
  Pixel pixel{};
  [texture getBytes:pixel.data()
        bytesPerRow:pixel.size()
         fromRegion:MTLRegionMake2D(x, y, 1, 1)
        mipmapLevel:0];
  return pixel;
}

int Brightness(const Pixel& pixel) {
  return (static_cast<int>(pixel[0]) + static_cast<int>(pixel[1]) +
          static_cast<int>(pixel[2])) /
         3;
}

struct RenderedPixels {
  Pixel camera{};
  Pixel gap{};
  Pixel crop{};
};

RenderedPixels Render(AppleVideoMetalRenderer& renderer,
                      const AppleDecodedVideoFrame& camera,
                      const AppleAlignedAcquisitionCropFrame* acquisition,
                      const crimson::crop::CropSourceSelection& selection) {
  constexpr NSUInteger width = 48;
  constexpr NSUInteger height = 24;
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue = [device newCommandQueue];
  CHECK(device != nil);
  CHECK(queue != nil);
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
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
  id<MTLCommandBuffer> command = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:pass];
  std::string error;
  CHECK(renderer.encode(
      camera, reinterpret_cast<uintptr_t>((__bridge void*)command),
      reinterpret_cast<uintptr_t>((__bridge void*)encoder),
      {0.0, 0.0, 20.0, 20.0}, &error));
  if (selection.source == crimson::crop::CropSourceKind::AcquisitionVideo) {
    CHECK(acquisition != nullptr);
    CHECK(renderer.encode(
        acquisition->decoded_frame,
        reinterpret_cast<uintptr_t>((__bridge void*)command),
        reinterpret_cast<uintptr_t>((__bridge void*)encoder),
        {28.0, 0.0, 20.0, 20.0}, &error));
  } else {
    CHECK(selection.geometry.has_value());
    const auto& geometry = *selection.geometry;
    CHECK(renderer.encodeRegion(
        camera, reinterpret_cast<uintptr_t>((__bridge void*)command),
        reinterpret_cast<uintptr_t>((__bridge void*)encoder),
        {28.0, 0.0, 20.0, 20.0},
        {geometry.full_frame_crop.x / geometry.source_width,
         geometry.full_frame_crop.y / geometry.source_height,
         geometry.full_frame_crop.width / geometry.source_width,
         geometry.full_frame_crop.height / geometry.source_height},
        &error));
  }
  [encoder endEncoding];
  [command commit];
  [command waitUntilCompleted];
  CHECK(command.status == MTLCommandBufferStatusCompleted);
  return {ReadPixel(target, 10, 10), ReadPixel(target, 24, 10),
          ReadPixel(target, 38, 10)};
}

struct Harness {
  AppleVideoPlaybackBuffer camera;
  AppleAcquisitionCropPlaybackSession crop;
  crimson::crop::CropPresentationCoordinator presentation;
  AppleVideoMetalRenderer renderer;
  std::string error;

  RenderedPixels present(
      int64_t camera_frame,
      crimson::crop::CropSourcePreference preference,
      bool discontinuity) {
    CHECK(camera.requestSeek(camera_frame, &error));
    CHECK(camera.waitForFrame(camera_frame, std::chrono::seconds(5)));
    auto decoded_camera = camera.frameForTarget(camera_frame, true);
    CHECK(decoded_camera.has_value());
    CHECK(crop.requestCameraFrame(camera_frame, discontinuity, &error));
    CHECK(crop.waitForCameraFrame(camera_frame, std::chrono::seconds(5),
                                  &error));
    auto aligned = crop.frameForCameraFrame(camera_frame);
    CHECK(aligned.has_value());

    crimson::crop::CropFrameSourceState state;
    state.camera_frame = camera_frame;
    state.exact_full_frame = camera_frame;
    state.live_geometry = crop.repository()->liveGeometry(
        camera_frame, camera.info().width, camera.info().height);
    state.acquisition = aligned->source_state;
    const auto selection = crimson::crop::SelectCropSource(
        crop.repository()->sourceCapabilities(), state, preference,
        crimson::crop::CropFallbackPolicy::WaitForPreferred,
        static_cast<int64_t>(crop.repository()->cameraFrameCount()));
    CHECK(selection.selected());
    const int64_t surface_camera_frame =
        selection.source == crimson::crop::CropSourceKind::LiveGeometry
            ? camera_frame
            : aligned->resolution.camera_frame;
    const auto decision = presentation.update(
        camera_frame, selection, surface_camera_frame);
    CHECK(decision.commit_crop);
    CHECK(decision.render_current);
    return Render(renderer, *decoded_camera,
                  selection.source ==
                          crimson::crop::CropSourceKind::AcquisitionVideo
                      ? &*aligned
                      : nullptr,
                  selection);
  }
};

void RunTest(const std::string& video_path) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  CHECK(device != nil);
  Harness harness;
  CHECK(harness.renderer.initialize(
      reinterpret_cast<uintptr_t>((__bridge void*)device),
      static_cast<uint64_t>(MTLPixelFormatBGRA8Unorm), &harness.error));
  CHECK(harness.camera.open(video_path, "camera-main", 4, &harness.error));
  CHECK(harness.crop.open(MakeRepository(video_path), 64, 48, 4,
                          &harness.error));

  const auto acquisition = harness.present(
      2, crimson::crop::CropSourcePreference::PreferAcquisitionVideo, true);
  CHECK(Brightness(acquisition.camera) > 5);
  CHECK(Brightness(acquisition.crop) > 5);
  CHECK(Brightness(acquisition.gap) < 3);

  crimson::crop::CropSourceSelection awaiting;
  awaiting.status =
      crimson::crop::CropSourceSelectionStatus::AwaitingExactFrame;
  awaiting.source = crimson::crop::CropSourceKind::AcquisitionVideo;
  awaiting.camera_frame = 3;
  awaiting.source_frame_index = 3;
  const auto deferred = harness.presentation.update(3, awaiting, std::nullopt);
  CHECK(deferred.action == crimson::crop::CropPresentationAction::Wait);
  CHECK(!deferred.commit_crop);
  CHECK(harness.presentation.metrics().presented_crop_camera_frame == 2);

  const auto geometry = harness.present(
      7, crimson::crop::CropSourcePreference::PreferLiveGeometry, true);
  CHECK(Brightness(geometry.camera) > Brightness(acquisition.camera));
  CHECK(Brightness(geometry.crop) > Brightness(acquisition.crop));
  CHECK(Brightness(geometry.gap) < 3);

  const auto backward = harness.present(
      3, crimson::crop::CropSourcePreference::PreferAcquisitionVideo, true);
  CHECK(Brightness(backward.camera) < Brightness(geometry.camera));
  CHECK(Brightness(backward.crop) < Brightness(geometry.crop));

  const auto presentation_metrics = harness.presentation.metrics();
  const auto playback_metrics = harness.crop.metrics();
  CHECK(presentation_metrics.exact_presentations == 3);
  CHECK(presentation_metrics.deferred_presentations == 1);
  CHECK(presentation_metrics.mismatched_selection_frames == 0);
  CHECK(presentation_metrics.mismatched_surface_frames == 0);
  CHECK(presentation_metrics.max_abs_camera_skew_frames == 0);
  CHECK(playback_metrics.failed_requests == 0);
  CHECK(playback_metrics.decoder.peak_buffered_frames <= 4);
  harness.crop.close();
  harness.camera.close();
  harness.renderer.reset();
  std::cout << "[AppleCropPresentationMetal] PASS exact="
            << presentation_metrics.exact_presentations
            << " peak_buffer=" << playback_metrics.decoder.peak_buffered_frames
            << " camera_skew="
            << presentation_metrics.max_abs_camera_skew_frames << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 2) {
      std::cerr << "usage: apple_crop_presentation_metal_tests VIDEO_FIXTURE\n";
      return 2;
    }
    try {
      RunTest(argv[1]);
    } catch (const TestFailure& failure) {
      std::cerr << failure.message << std::endl;
      return 1;
    } catch (const std::exception& exception) {
      std::cerr << "unexpected exception: " << exception.what() << std::endl;
      return 1;
    }
  }
  return 0;
}
