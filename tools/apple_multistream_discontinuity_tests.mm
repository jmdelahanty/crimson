#include "crop_presentation_coordinator.h"
#include "platform/macos/apple_acquisition_crop_playback_session.h"
#include "platform/macos/apple_stimulus_playback_session.h"
#include "platform/macos/apple_video_metal_renderer.h"
#include "platform/macos/apple_video_playback_buffer.h"
#include "stimulus_presentation_coordinator.h"
#include "zarr/acquisition_crop_repository.h"
#include "zarr/analysis_crop_geometry_repository.h"
#include "zarr/stimulus_repository.h"

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

std::unique_ptr<crimson::zarr::StimulusRepository> MakeStimulusRepository(
    const std::string& video_path) {
  crimson::zarr::StimulusAlignmentData alignment;
  alignment.run_name = "multistream_fixture";
  alignment.source_video_path = video_path;
  alignment.resolved_source_video_path = video_path;
  alignment.camera_to_stimulus_frame_corrected =
      {0, 1, 2, 3, 4, -1, 6, 7, 8, 9, 10, 11};
  alignment.camera_frame_original.assign(12, 1);
  alignment.camera_stimulus_frame_interpolated.assign(12, 0);
  alignment.alignment_available = true;
  alignment.direct_corrected_available = true;
  return crimson::zarr::MakeStimulusRepository(std::move(alignment));
}

std::unique_ptr<crimson::zarr::AcquisitionCropRepository> MakeCropRepository(
    const std::string& video_path) {
  crimson::zarr::AcquisitionCropStreamDescriptor descriptor;
  descriptor.schema_id = "palette.acquisition_video_streams.v1";
  descriptor.schema_version = 1;
  descriptor.stream_id = "multistream_crop";
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
    row.blank_frame = frame == 6;
    row.has_detection = !row.blank_frame;
    if (!row.blank_frame) {
      row.full_frame_crop = {0.0, 0.0, 32.0, 48.0};
      row.full_frame_detection =
          crimson::crop::CropRect{8.0, 12.0, 12.0, 16.0};
    }
    rows.push_back(std::move(row));
  }
  return crimson::zarr::MakeAcquisitionCropRepository(
      std::move(descriptor), std::move(rows));
}

std::unique_ptr<crimson::zarr::AnalysisCropGeometryRepository>
MakeAnalysisGeometryRepository() {
  crimson::zarr::AnalysisCropGeometryDescriptor descriptor;
  descriptor.run_name = "multistream_analysis_geometry";
  descriptor.output_width = 32;
  descriptor.output_height = 48;
  std::vector<crimson::zarr::AnalysisCropGeometryRow> rows;
  for (int64_t frame = 0; frame < 12; ++frame) {
    if (frame == 6) {
      continue;
    }
    crimson::zarr::AnalysisCropGeometryRow row;
    row.camera_frame = frame;
    row.roi_index = frame;
    row.offset_x = 0.0;
    row.offset_y = 0.0;
    row.normalized_detection_cxcywh =
        std::array<double, 4>{0.25, 0.5, 0.25, 0.5};
    rows.push_back(std::move(row));
  }
  return crimson::zarr::MakeAnalysisCropGeometryRepository(
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

struct RenderResult {
  int camera_brightness = 0;
  int crop_brightness = 0;
  int stimulus_brightness = 0;
};

RenderResult RenderComposite(
    AppleVideoMetalRenderer& renderer,
    const AppleDecodedVideoFrame& camera,
    const AppleAlignedStimulusFrame* stimulus,
    const AppleAlignedAcquisitionCropFrame* acquisition,
    const crimson::crop::CropSourceSelection* crop_selection) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue = [device newCommandQueue];
  CHECK(device != nil);
  CHECK(queue != nil);
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:72
                                  height:24
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
  if (crop_selection != nullptr && crop_selection->selected()) {
    if (crop_selection->source ==
        crimson::crop::CropSourceKind::AcquisitionVideo) {
      CHECK(acquisition != nullptr);
      CHECK(renderer.encode(
          acquisition->decoded_frame,
          reinterpret_cast<uintptr_t>((__bridge void*)command),
          reinterpret_cast<uintptr_t>((__bridge void*)encoder),
          {26.0, 0.0, 20.0, 20.0}, &error));
    } else {
      CHECK(crop_selection->geometry.has_value());
      const auto& geometry = *crop_selection->geometry;
      CHECK(renderer.encodeRegion(
          camera, reinterpret_cast<uintptr_t>((__bridge void*)command),
          reinterpret_cast<uintptr_t>((__bridge void*)encoder),
          {26.0, 0.0, 20.0, 20.0},
          {geometry.full_frame_crop.x / geometry.source_width,
           geometry.full_frame_crop.y / geometry.source_height,
           geometry.full_frame_crop.width / geometry.source_width,
           geometry.full_frame_crop.height / geometry.source_height},
          &error));
    }
  }
  if (stimulus != nullptr) {
    CHECK(renderer.encode(
        stimulus->decoded_frame,
        reinterpret_cast<uintptr_t>((__bridge void*)command),
        reinterpret_cast<uintptr_t>((__bridge void*)encoder),
        {52.0, 0.0, 20.0, 20.0}, &error));
  }
  [encoder endEncoding];
  [command commit];
  [command waitUntilCompleted];
  CHECK(command.status == MTLCommandBufferStatusCompleted);
  return {Brightness(ReadPixel(target, 10, 10)),
          Brightness(ReadPixel(target, 36, 10)),
          Brightness(ReadPixel(target, 62, 10))};
}

struct Harness {
  AppleVideoPlaybackBuffer camera;
  AppleStimulusPlaybackSession stimulus;
  AppleAcquisitionCropPlaybackSession crop;
  std::unique_ptr<crimson::zarr::AnalysisCropGeometryRepository>
      analysis_geometry;
  crimson::playback::StimulusPresentationCoordinator stimulus_presentation;
  crimson::crop::CropPresentationCoordinator crop_presentation;
  AppleVideoMetalRenderer renderer;
  std::string error;

  RenderResult settle(
      int64_t camera_frame,
      crimson::crop::CropSourcePreference preference,
      bool discontinuity) {
    CHECK(camera.requestSeek(camera_frame, &error));
    CHECK(camera.waitForFrame(camera_frame, std::chrono::seconds(5)));
    auto decoded_camera = camera.frameForTarget(camera_frame, true);
    CHECK(decoded_camera.has_value());

    CHECK(stimulus.requestCameraFrame(static_cast<int32_t>(camera_frame),
                                      discontinuity, &error));
    const auto stimulus_resolution =
        stimulus.resolveCameraFrame(static_cast<int32_t>(camera_frame));
    std::optional<AppleAlignedStimulusFrame> aligned_stimulus;
    if (stimulus_resolution.status ==
        crimson::zarr::StimulusMappingStatus::Mapped) {
      CHECK(stimulus.waitForCameraFrame(static_cast<int32_t>(camera_frame),
                                        std::chrono::seconds(5), &error));
      aligned_stimulus =
          stimulus.frameForCameraFrame(static_cast<int32_t>(camera_frame));
      CHECK(aligned_stimulus.has_value());
    }
    const auto stimulus_decision = stimulus_presentation.update(
        static_cast<int32_t>(camera_frame), stimulus_resolution,
        aligned_stimulus
            ? std::optional<int32_t>(
                  aligned_stimulus->decoded_frame.metadata.frame_number)
            : std::nullopt);
    CHECK(stimulus_decision.commit_composite);

    std::optional<AppleAlignedAcquisitionCropFrame> aligned_crop;
    if (preference ==
        crimson::crop::CropSourcePreference::PreferAcquisitionVideo) {
      CHECK(crop.requestCameraFrame(camera_frame, discontinuity, &error));
      CHECK(crop.waitForCameraFrame(camera_frame, std::chrono::seconds(5),
                                    &error));
      aligned_crop = crop.frameForCameraFrame(camera_frame);
      CHECK(aligned_crop.has_value());
    }
    crimson::crop::CropFrameSourceState state;
    state.camera_frame = camera_frame;
    state.exact_full_frame = camera_frame;
    state.live_geometry = analysis_geometry
                              ->resolveCameraFrame(
                                  camera_frame, camera.info().width,
                                  camera.info().height)
                              .geometry;
    state.acquisition = crop.repository()->acquisitionFrameState(
        camera_frame,
        aligned_crop ? std::optional<int64_t>(
                           aligned_crop->decoded_frame.metadata.frame_number)
                     : std::nullopt,
        camera.info().width, camera.info().height);
    auto capabilities = crop.repository()->sourceCapabilities();
    capabilities.live_geometry = true;
    const auto selection = crimson::crop::SelectCropSource(
        capabilities, state, preference,
        crimson::crop::CropFallbackPolicy::WaitForPreferred, 12);
    const std::optional<int64_t> surface_camera_frame =
        selection.selected()
            ? std::optional<int64_t>(camera_frame)
            : std::nullopt;
    const auto crop_decision = crop_presentation.update(
        camera_frame, selection, surface_camera_frame);
    CHECK(crop_decision.commit_crop);
    CHECK(crop_decision.action != crimson::crop::CropPresentationAction::Wait);

    return RenderComposite(
        renderer, *decoded_camera,
        stimulus_decision.render_current && aligned_stimulus
            ? &*aligned_stimulus
            : nullptr,
        crop_decision.render_current && aligned_crop ? &*aligned_crop : nullptr,
        crop_decision.render_current ? &selection : nullptr);
  }
};

void RunTest(const std::string& video_path) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  CHECK(device != nil);
  Harness harness;
  harness.analysis_geometry = MakeAnalysisGeometryRepository();
  CHECK(harness.analysis_geometry != nullptr);
  CHECK(harness.renderer.initialize(
      reinterpret_cast<uintptr_t>((__bridge void*)device),
      static_cast<uint64_t>(MTLPixelFormatBGRA8Unorm), &harness.error));
  CHECK(harness.camera.open(video_path, "camera-main", 4, &harness.error));
  CHECK(harness.stimulus.open(MakeStimulusRepository(video_path), 4,
                              &harness.error));
  CHECK(harness.crop.open(MakeCropRepository(video_path), 64, 48, 4,
                          &harness.error));

  for (const int64_t frame : {1, 2, 8, 3}) {
    const auto rendered = harness.settle(
        frame, crimson::crop::CropSourcePreference::PreferAcquisitionVideo,
        true);
    CHECK(rendered.camera_brightness > 5);
    CHECK(rendered.crop_brightness > 5);
    CHECK(rendered.stimulus_brightness > 5);
  }

  const auto missing_stimulus = harness.settle(
      5, crimson::crop::CropSourcePreference::PreferAcquisitionVideo, true);
  CHECK(missing_stimulus.camera_brightness > 5);
  CHECK(missing_stimulus.crop_brightness > 5);
  CHECK(missing_stimulus.stimulus_brightness < 3);

  const auto blank_acquisition = harness.settle(
      6, crimson::crop::CropSourcePreference::PreferAcquisitionVideo, true);
  CHECK(blank_acquisition.camera_brightness > 5);
  CHECK(blank_acquisition.crop_brightness > 5);
  CHECK(blank_acquisition.stimulus_brightness > 5);

  const auto missing_geometry = harness.settle(
      6, crimson::crop::CropSourcePreference::PreferLiveGeometry, true);
  CHECK(missing_geometry.camera_brightness > 5);
  CHECK(missing_geometry.crop_brightness < 3);
  CHECK(missing_geometry.stimulus_brightness > 5);

  const auto final_geometry = harness.settle(
      11, crimson::crop::CropSourcePreference::PreferLiveGeometry, true);
  CHECK(final_geometry.camera_brightness > 5);
  CHECK(final_geometry.crop_brightness > 5);
  CHECK(final_geometry.stimulus_brightness > 5);

  const auto stimulus_metrics = harness.stimulus_presentation.metrics();
  const auto crop_metrics = harness.crop_presentation.metrics();
  CHECK(stimulus_metrics.missing_presentations == 1);
  CHECK(stimulus_metrics.mismatched_mapping_frames == 0);
  CHECK(stimulus_metrics.mismatched_decoded_frames == 0);
  CHECK(stimulus_metrics.max_abs_camera_skew_frames == 0);
  CHECK(crop_metrics.cleared_presentations == 1);
  CHECK(crop_metrics.mismatched_selection_frames == 0);
  CHECK(crop_metrics.mismatched_surface_frames == 0);
  CHECK(crop_metrics.max_abs_camera_skew_frames == 0);
  CHECK(harness.camera.metrics().peak_buffered_frames <= 4);
  CHECK(harness.stimulus.metrics().decoder.peak_buffered_frames <= 4);
  CHECK(harness.crop.metrics().decoder.peak_buffered_frames <= 4);

  harness.crop.close();
  harness.stimulus.close();
  harness.camera.close();
  harness.renderer.reset();
  std::cout << "[AppleMultistreamDiscontinuity] PASS settlements=8 "
               "missing_stimulus=1 missing_geometry=1 camera_skew=0\n";
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 2) {
      std::cerr << "usage: apple_multistream_discontinuity_tests VIDEO_FIXTURE\n";
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
