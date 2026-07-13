#include "platform/macos/apple_stimulus_playback_session.h"
#include "platform/macos/apple_video_metal_renderer.h"
#include "platform/macos/apple_video_playback_buffer.h"
#include "stimulus_presentation_coordinator.h"
#include "zarr/stimulus_repository.h"

#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

struct TestFailure {
  std::string message;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure{std::string("CHECK failed: ") + #condition +         \
                        " at " + __FILE__ + ":" +                          \
                        std::to_string(__LINE__)};                             \
    }                                                                          \
  } while (false)

using Pixel = std::array<uint8_t, 4>;

struct CompositePixels {
  Pixel camera{};
  Pixel gap{};
  Pixel stimulus{};
};

std::unique_ptr<crimson::zarr::StimulusRepository> MakeRepository(
    const std::string& video_path) {
  crimson::zarr::StimulusAlignmentData alignment;
  alignment.run_name = "stimulus_composite_fixture";
  alignment.source_video_path = video_path;
  alignment.resolved_source_video_path = video_path;
  alignment.camera_to_stimulus_frame_corrected =
      {-1, 0, 0, 2, 3, -1, 5, 6, 7, 8, 9, 11};
  alignment.camera_frame_original =
      {1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  alignment.camera_stimulus_frame_interpolated =
      {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  alignment.alignment_available = true;
  alignment.direct_corrected_available = true;
  return crimson::zarr::MakeStimulusRepository(std::move(alignment));
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

CompositePixels RenderComposite(
    AppleVideoMetalRenderer& renderer,
    const AppleDecodedVideoFrame& camera_frame,
    const std::optional<AppleAlignedStimulusFrame>& stimulus_frame) {
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
  CHECK(target != nil);

  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
  id<MTLCommandBuffer> command = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:pass];
  CHECK(command != nil);
  CHECK(encoder != nil);

  std::string error;
  CHECK(renderer.encode(
      camera_frame,
      reinterpret_cast<uintptr_t>((__bridge void*)command),
      reinterpret_cast<uintptr_t>((__bridge void*)encoder),
      {0.0, 0.0, 20.0, 20.0}, &error));
  if (stimulus_frame) {
    CHECK(renderer.encode(
        stimulus_frame->decoded_frame,
        reinterpret_cast<uintptr_t>((__bridge void*)command),
        reinterpret_cast<uintptr_t>((__bridge void*)encoder),
        {28.0, 0.0, 20.0, 20.0}, &error));
  }
  [encoder endEncoding];
  [command commit];
  [command waitUntilCompleted];
  CHECK(command.status == MTLCommandBufferStatusCompleted);

  return {ReadPixel(target, 10, 10), ReadPixel(target, 24, 10),
          ReadPixel(target, 38, 10)};
}

struct CompositeHarness {
  AppleVideoPlaybackBuffer camera;
  AppleStimulusPlaybackSession stimulus;
  crimson::playback::StimulusPresentationCoordinator presentation;
  AppleVideoMetalRenderer renderer;
  std::optional<AppleAlignedStimulusFrame> current_stimulus;
  std::string error;

  CompositePixels present(int32_t camera_frame, bool discontinuity) {
    CHECK(camera.requestSeek(camera_frame, &error));
    CHECK(camera.waitForFrame(camera_frame, std::chrono::seconds(5)));
    const auto decoded_camera = camera.frameForTarget(camera_frame, true);
    CHECK(decoded_camera.has_value());
    CHECK(decoded_camera->metadata.frame_number == camera_frame);

    CHECK(stimulus.requestCameraFrame(camera_frame, discontinuity, &error));
    const auto resolution = stimulus.resolveCameraFrame(camera_frame);
    if (resolution.status == crimson::zarr::StimulusMappingStatus::Mapped) {
      CHECK(stimulus.waitForCameraFrame(camera_frame, std::chrono::seconds(5),
                                        &error));
    }
    auto aligned = stimulus.frameForCameraFrame(camera_frame);
    const auto decision = presentation.update(
        camera_frame, resolution,
        aligned ? aligned->decoded_frame.metadata.frame_number
                : std::optional<int32_t>{});
    CHECK(decision.commit_composite);
    if (decision.render_current) {
      if (aligned) {
        current_stimulus = std::move(aligned);
      }
      CHECK(current_stimulus.has_value());
      CHECK(resolution.stimulus_frame.has_value());
      CHECK(current_stimulus->decoded_frame.metadata.frame_number ==
            *resolution.stimulus_frame);
    } else {
      current_stimulus.reset();
    }
    return RenderComposite(renderer, *decoded_camera, current_stimulus);
  }
};

void RunTest(const std::string& video_path) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device != nil);
    CompositeHarness harness;
    CHECK(harness.renderer.initialize(
        reinterpret_cast<uintptr_t>((__bridge void*)device),
        static_cast<uint64_t>(MTLPixelFormatBGRA8Unorm), &harness.error));
    CHECK(harness.camera.open(video_path, "camera-main", 4, &harness.error));
    CHECK(harness.stimulus.open(MakeRepository(video_path), 4,
                                &harness.error));

    const auto first = harness.present(1, true);
    CHECK(Brightness(first.camera) > 10);
    CHECK(Brightness(first.stimulus) > 5);
    CHECK(Brightness(first.gap) < 3);

    const auto repeated = harness.present(2, false);
    CHECK(std::abs(Brightness(repeated.stimulus) -
                   Brightness(first.stimulus)) < 4);
    CHECK(Brightness(repeated.camera) > Brightness(first.camera));
    CHECK(Brightness(repeated.gap) < 3);

    const auto missing = harness.present(5, false);
    CHECK(Brightness(missing.camera) > 10);
    CHECK(Brightness(missing.stimulus) < 3);
    CHECK(Brightness(missing.gap) < 3);

    const auto forward_jump = harness.present(11, true);
    CHECK(Brightness(forward_jump.camera) > Brightness(repeated.camera));
    CHECK(Brightness(forward_jump.stimulus) > Brightness(repeated.stimulus));

    const auto backward = harness.present(3, true);
    CHECK(Brightness(backward.camera) < Brightness(forward_jump.camera));
    CHECK(Brightness(backward.stimulus) < Brightness(forward_jump.stimulus));

    const auto final = harness.present(11, true);
    CHECK(Brightness(final.camera) > Brightness(backward.camera));
    CHECK(Brightness(final.stimulus) > Brightness(backward.stimulus));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto presentation_metrics = harness.presentation.metrics();
    const auto stimulus_metrics = harness.stimulus.metrics();
    CHECK(presentation_metrics.camera_presentations == 6);
    CHECK(presentation_metrics.missing_presentations == 1);
    CHECK(presentation_metrics.held_presentations >= 1);
    CHECK(presentation_metrics.mismatched_mapping_frames == 0);
    CHECK(presentation_metrics.mismatched_decoded_frames == 0);
    CHECK(presentation_metrics.max_abs_camera_skew_frames == 0);
    CHECK(stimulus_metrics.failed_requests == 0);
    CHECK(stimulus_metrics.decoder.peak_buffered_frames <= 4);
    CHECK(stimulus_metrics.last_target_stimulus_frame == 11);
    CHECK(stimulus_metrics.decoder.last_decoded_frame == 11);

    harness.current_stimulus.reset();
    harness.stimulus.close();
    harness.camera.close();
    harness.renderer.reset();
    std::cout << "[AppleStimulusCompositeMetal] PASS presentations="
              << presentation_metrics.camera_presentations
              << " holds=" << presentation_metrics.held_presentations
              << " clears=" << presentation_metrics.cleared_presentations
              << " camera_skew="
              << presentation_metrics.max_abs_camera_skew_frames << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: apple_stimulus_composite_metal_tests VIDEO_FIXTURE\n";
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
  return 0;
}
