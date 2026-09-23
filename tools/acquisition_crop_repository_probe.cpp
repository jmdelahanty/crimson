#include "zarr/archive_context.h"
#include "zarr/tensorstore_acquisition_crop_repository.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>

namespace {

bool ParseInt64(const std::string& text, int64_t* value) {
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, *value);
  return result.ec == std::errc() && result.ptr == end;
}

bool ParseSize(const std::string& text, int* width, int* height) {
  const size_t separator = text.find('x');
  if (separator == std::string::npos) {
    return false;
  }
  int64_t parsed_width = 0;
  int64_t parsed_height = 0;
  if (!ParseInt64(text.substr(0, separator), &parsed_width) ||
      !ParseInt64(text.substr(separator + 1), &parsed_height) ||
      parsed_width <= 0 || parsed_width > std::numeric_limits<int>::max() ||
      parsed_height <= 0 || parsed_height > std::numeric_limits<int>::max()) {
    return false;
  }
  *width = static_cast<int>(parsed_width);
  *height = static_cast<int>(parsed_height);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <analysis.zarr> [--full-size WIDTHxHEIGHT]"
                 " [--video-frame-count COUNT] [camera_frame ...]\n";
    return 1;
  }

  int full_width = 0;
  int full_height = 0;
  std::optional<int64_t> video_frame_count;
  std::set<int64_t> requested_frames;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--full-size") {
      if (++index >= argc ||
          !ParseSize(argv[index], &full_width, &full_height)) {
        std::cerr << "--full-size requires WIDTHxHEIGHT\n";
        return 1;
      }
      continue;
    }
    if (argument == "--video-frame-count") {
      int64_t value = 0;
      if (++index >= argc || !ParseInt64(argv[index], &value) || value < 0) {
        std::cerr << "--video-frame-count requires a nonnegative integer\n";
        return 1;
      }
      video_frame_count = value;
      continue;
    }
    int64_t frame = 0;
    if (!ParseInt64(argument, &frame)) {
      std::cerr << "Invalid camera frame: " << argument << '\n';
      return 1;
    }
    requested_frames.insert(frame);
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

  const auto& descriptor = repository->descriptor();
  std::cout << "archive=" << archive->rootPath().string() << '\n';
  std::cout << "recording_root=" << archive->recordingRootPath().string()
            << '\n';
  std::cout << "schema=" << descriptor.schema_id
            << " version=" << descriptor.schema_version << '\n';
  std::cout << "stream_id=" << descriptor.stream_id
            << " camera_id=" << descriptor.camera_id << '\n';
  std::cout << "video=" << descriptor.resolved_video_path.string() << '\n';
  std::cout << "metadata=" << descriptor.resolved_metadata_path.string()
            << '\n';
  std::cout << "dimensions=" << descriptor.output_width << 'x'
            << descriptor.output_height << " fps=" << descriptor.frame_rate
            << " frame_count=" << descriptor.frame_count << '\n';
  std::cout << "keyframes=" << descriptor.keyframe_count
            << " first=" << descriptor.first_keyframe
            << " last=" << descriptor.last_keyframe << '\n';

  if (video_frame_count && *video_frame_count != descriptor.frame_count) {
    std::cerr << "video frame count mismatch: actual=" << *video_frame_count
              << " contract=" << descriptor.frame_count << '\n';
    return 4;
  }

  int64_t mapped = 0;
  int64_t blanks = 0;
  int64_t detections = 0;
  int64_t invalid_geometry = 0;
  for (int64_t frame = 0; frame < descriptor.frame_count; ++frame) {
    const auto resolution = repository->resolveCameraFrame(frame);
    if (resolution.status !=
            crimson::zarr::AcquisitionCropMappingStatus::Mapped ||
        resolution.video_frame != frame || resolution.metadata_row != frame ||
        !resolution.row ||
        resolution.row->recording_frame_id != frame + 1) {
      std::cerr << "identity mismatch at camera frame " << frame << '\n';
      return 5;
    }
    ++mapped;
    blanks += resolution.row->blank_frame ? 1 : 0;
    detections += resolution.row->has_detection ? 1 : 0;
    if (full_width > 0) {
      const auto geometry =
          repository->liveGeometry(frame, full_width, full_height);
      if (!geometry || !geometry->valid()) {
        ++invalid_geometry;
      }
    }
  }
  if (invalid_geometry != 0) {
    std::cerr << "invalid full-frame geometry rows=" << invalid_geometry
              << '\n';
    return 6;
  }

  if (requested_frames.empty()) {
    requested_frames.insert(0);
    requested_frames.insert(descriptor.frame_count / 2);
    requested_frames.insert(descriptor.frame_count - 1);
    requested_frames.insert(descriptor.frame_count);
  }
  for (int64_t frame : requested_frames) {
    const auto resolution = repository->resolveCameraFrame(frame);
    std::cout << "frame=" << frame << " status="
              << (resolution.status ==
                          crimson::zarr::AcquisitionCropMappingStatus::Mapped
                      ? "mapped"
                      : "out_of_range");
    if (resolution.video_frame) {
      std::cout << " video=" << *resolution.video_frame;
    }
    if (resolution.row) {
      std::cout << " recording=" << resolution.row->recording_frame_id
                << " local=" << resolution.row->local_frame_id
                << " camera_counter=" << resolution.row->camera_frame_id
                << " blank=" << (resolution.row->blank_frame ? 1 : 0);
    }
    std::cout << '\n';
  }

  std::cout << "mapped=" << mapped << " detections=" << detections
            << " blanks=" << blanks
            << " invalid_geometry=" << invalid_geometry << '\n';
  std::cout << "acquisition_crop_repository_probe: PASS\n";
  return 0;
}
