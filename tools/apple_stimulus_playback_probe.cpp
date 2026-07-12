#include "platform/macos/apple_stimulus_playback_session.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_stimulus_repository.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>

namespace {

const char* StatusName(crimson::zarr::StimulusMappingStatus status) {
  switch (status) {
    case crimson::zarr::StimulusMappingStatus::Mapped:
      return "mapped";
    case crimson::zarr::StimulusMappingStatus::Missing:
      return "missing";
    case crimson::zarr::StimulusMappingStatus::OutOfRange:
      return "out_of_range";
  }
  return "unknown";
}

bool ParseFrame(const std::string& text, int32_t* frame) {
  try {
    size_t consumed = 0;
    const long value = std::stol(text, &consumed);
    if (consumed != text.size() || value < 0 || value > INT32_MAX) {
      return false;
    }
    *frame = static_cast<int32_t>(value);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <analysis.zarr> [--run RUN] [camera_frame ...]\n";
    return 1;
  }

  std::string requested_run;
  std::set<int32_t> frames;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--run") {
      if (++index >= argc) {
        std::cerr << "--run requires a value\n";
        return 1;
      }
      requested_run = argv[index];
      continue;
    }
    int32_t frame = -1;
    if (!ParseFrame(argument, &frame)) {
      std::cerr << "Invalid camera frame: " << argument << '\n';
      return 1;
    }
    frames.insert(frame);
  }

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << error << '\n';
    return 2;
  }
  auto repository = crimson::zarr::OpenStimulusRepository(
      archive, requested_run, &error);
  if (!repository) {
    std::cerr << error << '\n';
    return 3;
  }

  if (frames.empty()) {
    if (const auto first = repository->firstCameraFrameWithStimulus()) {
      frames.insert(*first);
      frames.insert(*first + 1);
    }
    if (repository->cameraFrameCount() > 0) {
      frames.insert(
          static_cast<int32_t>(repository->cameraFrameCount() / 2));
      frames.insert(
          static_cast<int32_t>(repository->cameraFrameCount() - 1));
    }
  }

  const std::string run_name = repository->runName();
  const std::string video_path = repository->resolvedSourceVideoPath();
  AppleStimulusPlaybackSession session;
  if (!session.open(std::move(repository), 6, &error)) {
    std::cerr << error << '\n';
    return 4;
  }

  std::cout << "archive=" << archive->rootPath().string() << '\n';
  std::cout << "run=" << run_name << '\n';
  std::cout << "video=" << video_path << '\n';
  std::cout << "video_frames=" << session.info().frame_count
            << " video_fps=" << session.info().nominal_frame_rate << '\n';

  for (int32_t camera_frame : frames) {
    const auto resolution = session.resolveCameraFrame(camera_frame);
    std::cout << "camera=" << camera_frame
              << " status=" << StatusName(resolution.status);
    if (resolution.stimulus_frame) {
      std::cout << " stimulus=" << *resolution.stimulus_frame;
    }
    if (resolution.status != crimson::zarr::StimulusMappingStatus::Mapped) {
      std::cout << '\n';
      continue;
    }

    const auto started = std::chrono::steady_clock::now();
    if (!session.requestCameraFrame(camera_frame, true, &error) ||
        !session.waitForCameraFrame(camera_frame, std::chrono::seconds(30),
                                    &error)) {
      std::cerr << "\ncamera " << camera_frame << " decode failed: " << error
                << '\n';
      return 5;
    }
    const auto decoded = session.frameForCameraFrame(camera_frame);
    if (!decoded || decoded->decoded_frame.metadata.frame_number !=
                        *resolution.stimulus_frame) {
      std::cerr << "\ncamera " << camera_frame
                << " decoded the wrong stimulus identity\n";
      return 6;
    }
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    std::cout << " decoded=" << decoded->decoded_frame.metadata.frame_number
              << " pts=" << decoded->decoded_frame.metadata.frame_pts
              << " seek_ms=" << elapsed_ms << '\n';
  }

  const auto metrics = session.metrics();
  std::cout << "[AppleStimulusPlaybackProbe] PASS requests="
            << metrics.camera_requests << " seeks=" << metrics.seek_requests
            << " decoded=" << metrics.decoder.decoded_frames
            << " peak_buffered=" << metrics.decoder.peak_buffered_frames
            << '\n';
  return 0;
}
