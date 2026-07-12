#include "platform/macos/apple_stimulus_playback_session.h"
#include "zarr/stimulus_repository.h"

#import <CoreVideo/CoreVideo.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
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

std::unique_ptr<crimson::zarr::StimulusRepository> MakeRepository(
    const std::string& video_path) {
  crimson::zarr::StimulusAlignmentData alignment;
  alignment.run_name = "stimulus_playback_fixture";
  alignment.source_video_path = video_path;
  alignment.resolved_source_video_path = video_path;
  alignment.camera_frame_offset = 1;
  alignment.camera_to_metadata_index = {-1, 0, 1, 2, 3, -1, 4, 5, 6, 6};
  alignment.camera_to_metadata_index_corrected =
      {-1, 0, 1, 2, 3, 4, -1, 5, 6, -1};
  alignment.camera_to_stimulus_frame_corrected =
      {-1, 0, 2, -1, 4, 4, -1, 8, 11, 99};
  alignment.frame_metadata_stimulus_frames = {0, 1, 3, 5, 7, 9, 10};
  alignment.frame_metadata_stimulus_frames_corrected = {0, 2, 3, 4, 6, 8, 10};
  alignment.camera_frame_original = {1, 1, 1, 0, 1, 1, 1, 1, 1, 1};
  alignment.camera_stimulus_frame_interpolated =
      {0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
  alignment.alignment_available = true;
  alignment.direct_corrected_available = true;
  alignment.legacy_metadata_available = true;
  alignment.corrected_metadata_available = true;
  return crimson::zarr::MakeStimulusRepository(std::move(alignment));
}

double MeanLuma(const AppleDecodedVideoFrame& frame) {
  auto pixel_buffer = reinterpret_cast<CVPixelBufferRef>(
      frame.surface->nativeHandle());
  CHECK(pixel_buffer != nullptr);
  CHECK(CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) ==
        kCVReturnSuccess);
  const auto* pixels = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  const size_t width = CVPixelBufferGetWidthOfPlane(pixel_buffer, 0);
  const size_t height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 0);
  uint64_t sum = 0;
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      sum += pixels[y * stride + x];
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  return static_cast<double>(sum) / static_cast<double>(width * height);
}

void RequestAndCheck(AppleStimulusPlaybackSession& session,
                     int32_t camera_frame,
                     int32_t stimulus_frame,
                     bool discontinuity,
                     std::string* error) {
  CHECK(session.requestCameraFrame(camera_frame, discontinuity, error));
  CHECK(session.waitForCameraFrame(camera_frame, std::chrono::seconds(5),
                                   error));
  auto aligned = session.frameForCameraFrame(camera_frame);
  CHECK(aligned.has_value());
  CHECK(aligned->resolution.stimulus_frame == stimulus_frame);
  CHECK(aligned->decoded_frame.metadata.stream_id == "stimulus");
  CHECK(aligned->decoded_frame.metadata.frame_number == stimulus_frame);
  CHECK(aligned->decoded_frame.metadata.local_frame_number == stimulus_frame);
  CHECK(std::fabs(MeanLuma(aligned->decoded_frame) -
                  (24.0 + stimulus_frame * 12.0)) < 8.0);
}

void RunTest(const std::string& video_path) {
  std::string error;
  AppleStimulusPlaybackSession session;
  CHECK(session.open(MakeRepository(video_path), 4, &error));
  CHECK(session.isOpen());
  CHECK(session.info().frame_count == 12);
  CHECK(session.repository()->hasCorrectedMapping());

  RequestAndCheck(session, 1, 0, true, &error);
  RequestAndCheck(session, 2, 2, false, &error);
  RequestAndCheck(session, 3, 3, false, &error);
  RequestAndCheck(session, 4, 4, false, &error);
  RequestAndCheck(session, 5, 4, false, &error);
  RequestAndCheck(session, 6, 7, false, &error);

  CHECK(session.requestCameraFrame(0, false, &error));
  CHECK(!session.frameForCameraFrame(0));
  CHECK(session.resolveCameraFrame(0).status ==
        crimson::zarr::StimulusMappingStatus::Missing);

  RequestAndCheck(session, 7, 8, false, &error);
  RequestAndCheck(session, 3, 3, true, &error);
  RequestAndCheck(session, 8, 11, true, &error);

  CHECK(!session.requestCameraFrame(9, true, &error));
  CHECK(error == "mapped stimulus frame is outside the source video range");
  CHECK(!session.frameForCameraFrame(9));

  CHECK(session.requestCameraFrame(10, false, &error));
  CHECK(!session.frameForCameraFrame(10));
  CHECK(session.resolveCameraFrame(10).status ==
        crimson::zarr::StimulusMappingStatus::OutOfRange);

  session.suspend();
  RequestAndCheck(session, 4, 4, true, &error);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const auto settled_metrics = session.metrics();
  CHECK(settled_metrics.decoder.buffered_frames <= 4);
  CHECK(settled_metrics.decoder.peak_buffered_frames <= 4);
  CHECK(settled_metrics.decoder.last_decoded_frame <= 7);
  CHECK(session.frameForCameraFrame(4).has_value());

  CHECK(settled_metrics.camera_requests == 13);
  CHECK(settled_metrics.mapped_requests == 11);
  CHECK(settled_metrics.missing_requests == 1);
  CHECK(settled_metrics.out_of_range_requests == 1);
  CHECK(settled_metrics.failed_requests == 1);
  CHECK(settled_metrics.seek_requests > 0);
  CHECK(settled_metrics.decoder.last_error.empty());

  std::cout << "[AppleStimulusPlayback] PASS camera_requests="
            << settled_metrics.camera_requests
            << " seeks=" << settled_metrics.seek_requests
            << " follows=" << settled_metrics.follow_requests
            << " holds=" << settled_metrics.hold_requests
            << " decoded=" << settled_metrics.decoder.decoded_frames << '\n';
  session.close();
  CHECK(!session.isOpen());
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 2) {
      std::cerr << "usage: apple_stimulus_playback_tests VIDEO_FIXTURE\n";
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
