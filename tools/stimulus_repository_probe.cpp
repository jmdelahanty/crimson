#include "zarr/archive_context.h"
#include "zarr/stimulus_repository.h"
#include "zarr/tensorstore_stimulus_repository.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
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

const char* SourceName(crimson::zarr::StimulusMappingSource source) {
  switch (source) {
    case crimson::zarr::StimulusMappingSource::CorrectedDirect:
      return "corrected_direct";
    case crimson::zarr::StimulusMappingSource::CorrectedMetadata:
      return "corrected_metadata";
    case crimson::zarr::StimulusMappingSource::LegacyMetadata:
      return "legacy_metadata";
    case crimson::zarr::StimulusMappingSource::None:
      return "none";
  }
  return "unknown";
}

bool ParseFrame(const std::string& text, int32_t* frame) {
  try {
    size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size() ||
        value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
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

  const size_t frame_count = repository->cameraFrameCount();
  if (frames.empty()) {
    if (auto first = repository->firstCameraFrameWithStimulus()) {
      frames.insert(std::max<int32_t>(0, *first - 1));
      frames.insert(*first);
      frames.insert(*first + 1);
    }
    if (frame_count > 0) {
      frames.insert(static_cast<int32_t>(frame_count / 2));
      frames.insert(static_cast<int32_t>(frame_count - 1));
      frames.insert(static_cast<int32_t>(frame_count));
    }
  }

  std::cout << "archive=" << archive->rootPath().string() << '\n';
  std::cout << "run=" << repository->runName() << '\n';
  std::cout << "source_video=" << repository->sourceVideoPath() << '\n';
  std::cout << "resolved_source_video="
            << repository->resolvedSourceVideoPath() << '\n';
  std::cout << "camera_frame_offset=" << repository->cameraFrameOffset()
            << '\n';
  std::cout << "camera_frame_count=" << frame_count << '\n';
  std::cout << "has_corrected_mapping="
            << (repository->hasCorrectedMapping() ? "true" : "false") << '\n';

  for (int32_t frame : frames) {
    const auto result = repository->resolveCameraFrame(frame);
    std::cout << "frame=" << frame << " status=" << StatusName(result.status)
              << " source=" << SourceName(result.source);
    if (result.metadata_index) {
      std::cout << " metadata=" << *result.metadata_index;
    }
    if (result.stimulus_frame) {
      std::cout << " stimulus=" << *result.stimulus_frame;
    }
    std::cout << " interpolated="
              << (result.interpolated ? "true" : "false") << '\n';
  }
  return 0;
}
