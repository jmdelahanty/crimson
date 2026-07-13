#include "platform/macos/apple_acquisition_crop_playback_session.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_acquisition_crop_repository.h"

#include <CoreVideo/CoreVideo.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

bool ParseInt64(const std::string& text, int64_t* value) {
  try {
    size_t consumed = 0;
    const long long parsed = std::stoll(text, &consumed);
    if (consumed != text.size()) {
      return false;
    }
    *value = static_cast<int64_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool ParseFullSize(const std::string& text, int* width, int* height) {
  const size_t separator = text.find('x');
  if (separator == std::string::npos) {
    return false;
  }
  int64_t parsed_width = 0;
  int64_t parsed_height = 0;
  if (!ParseInt64(text.substr(0, separator), &parsed_width) ||
      !ParseInt64(text.substr(separator + 1), &parsed_height) ||
      parsed_width <= 0 || parsed_height <= 0 ||
      parsed_width > std::numeric_limits<int>::max() ||
      parsed_height > std::numeric_limits<int>::max()) {
    return false;
  }
  *width = static_cast<int>(parsed_width);
  *height = static_cast<int>(parsed_height);
  return true;
}

const char* ActionName(AppleAcquisitionCropDecodeAction action) {
  switch (action) {
    case AppleAcquisitionCropDecodeAction::Clear:
      return "clear";
    case AppleAcquisitionCropDecodeAction::Hold:
      return "hold";
    case AppleAcquisitionCropDecodeAction::Follow:
      return "follow";
    case AppleAcquisitionCropDecodeAction::Seek:
      return "seek";
  }
  return "unknown";
}

std::optional<double> MeanLuma(const AppleDecodedVideoFrame& frame) {
  auto pixel_buffer = reinterpret_cast<CVPixelBufferRef>(
      frame.surface ? frame.surface->nativeHandle() : 0);
  if (pixel_buffer == nullptr || CVPixelBufferGetPlaneCount(pixel_buffer) < 1 ||
      CVPixelBufferLockBaseAddress(pixel_buffer,
                                   kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return std::nullopt;
  }
  const auto* pixels = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  const size_t width = CVPixelBufferGetWidthOfPlane(pixel_buffer, 0);
  const size_t height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 0);
  if (pixels == nullptr || width == 0 || height == 0) {
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    return std::nullopt;
  }
  uint64_t sum = 0;
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      sum += pixels[y * stride + x];
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  return static_cast<double>(sum) / static_cast<double>(width * height);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || std::string(argv[2]) != "--full-size") {
    std::cerr << "Usage: " << argv[0]
              << " <analysis.zarr> --full-size WIDTHxHEIGHT [frame ...]\n";
    return 1;
  }
  int full_width = 0;
  int full_height = 0;
  if (!ParseFullSize(argv[3], &full_width, &full_height)) {
    std::cerr << "Invalid full-frame dimensions: " << argv[3] << '\n';
    return 1;
  }
  std::vector<int64_t> requested_frames;
  for (int index = 4; index < argc; ++index) {
    int64_t frame = -1;
    if (!ParseInt64(argv[index], &frame) || frame < 0) {
      std::cerr << "Invalid camera frame: " << argv[index] << '\n';
      return 1;
    }
    requested_frames.push_back(frame);
  }

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << error << '\n';
    return 2;
  }
  auto repository =
      crimson::zarr::OpenAcquisitionCropRepository(archive, &error);
  if (!repository) {
    std::cerr << error << '\n';
    return 3;
  }
  const auto descriptor = repository->descriptor();
  if (requested_frames.empty()) {
    requested_frames = {0, descriptor.frame_count / 2,
                        descriptor.frame_count - 1,
                        descriptor.frame_count / 2};
    for (int64_t frame = 0; frame < descriptor.frame_count; ++frame) {
      const auto resolution = repository->resolveCameraFrame(frame);
      if (resolution.row && resolution.row->blank_frame) {
        requested_frames.push_back(frame);
        break;
      }
    }
  }

  AppleAcquisitionCropPlaybackSession session;
  constexpr size_t kBufferCapacity = 6;
  if (!session.open(std::move(repository), full_width, full_height,
                    kBufferCapacity, &error)) {
    std::cerr << error << '\n';
    return 4;
  }

  std::cout << "archive=" << archive->rootPath().string() << '\n';
  std::cout << "video=" << descriptor.resolved_video_path.string() << '\n';
  std::cout << "video_dimensions=" << session.info().width << 'x'
            << session.info().height << " fps="
            << session.info().nominal_frame_rate << " frames="
            << session.info().frame_count << '\n';

  for (int64_t camera_frame : requested_frames) {
    const auto resolution = session.resolveCameraFrame(camera_frame);
    if (resolution.status !=
            crimson::zarr::AcquisitionCropMappingStatus::Mapped ||
        !resolution.video_frame) {
      std::cerr << "camera " << camera_frame << " is not mapped\n";
      return 5;
    }
    const auto started = std::chrono::steady_clock::now();
    if (!session.requestCameraFrame(camera_frame, true, &error) ||
        !session.waitForCameraFrame(camera_frame, std::chrono::seconds(60),
                                    &error)) {
      std::cerr << "camera " << camera_frame << " decode failed: " << error
                << '\n';
      return 6;
    }
    const auto aligned = session.frameForCameraFrame(camera_frame);
    if (!aligned || aligned->decoded_frame.metadata.frame_number !=
                        *resolution.video_frame ||
        !aligned->selection.selected()) {
      std::cerr << "camera " << camera_frame
                << " returned a mismatched crop frame\n";
      return 7;
    }
    std::optional<double> blank_luma;
    if (aligned->selection.blank_frame) {
      blank_luma = MeanLuma(aligned->decoded_frame);
      if (!blank_luma || *blank_luma > 24.0) {
        std::cerr << "camera " << camera_frame
                  << " is marked blank but its decoded luma is not black\n";
        return 8;
      }
    }
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    const auto current_metrics = session.metrics();
    std::cout << "camera=" << camera_frame
              << " video=" << aligned->decoded_frame.metadata.frame_number
              << " recording="
              << aligned->resolution.row->recording_frame_id
              << " blank=" << (aligned->selection.blank_frame ? 1 : 0)
              << (blank_luma ? " blank_luma=" + std::to_string(*blank_luma)
                             : "")
              << " action=" << ActionName(current_metrics.last_action)
              << " elapsed_ms=" << elapsed_ms << '\n';
  }

  const auto metrics = session.metrics();
  if (metrics.failed_requests != 0 ||
      metrics.decoder.buffered_frames > kBufferCapacity ||
      metrics.decoder.peak_buffered_frames > kBufferCapacity ||
      !metrics.decoder.last_error.empty()) {
    std::cerr << "acquisition crop playback metrics are invalid\n";
    return 9;
  }
  std::cout << "[AppleAcquisitionCropPlaybackProbe] PASS requests="
            << metrics.camera_requests << " seeks=" << metrics.seek_requests
            << " decoded=" << metrics.decoder.decoded_frames
            << " peak_buffered=" << metrics.decoder.peak_buffered_frames
            << '\n';
  return 0;
}
