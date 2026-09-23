#include "eye_angle_timeline.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_eye_angle_timeline_repository.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

bool parseFrame(const char* value, int64_t* frame) {
  const std::string text = value == nullptr ? "" : value;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *frame);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() &&
         *frame >= 0;
}

const char* roleName(crimson::timeline::EyeAngleTraceRole role) {
  switch (role) {
    case crimson::timeline::EyeAngleTraceRole::Left:
      return "left";
    case crimson::timeline::EyeAngleTraceRole::Right:
      return "right";
    case crimson::timeline::EyeAngleTraceRole::Vergence:
      return "vergence";
    case crimson::timeline::EyeAngleTraceRole::Other:
      return "other";
  }
  return "other";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: eye_angle_timeline_repository_probe ZARR [FRAME] "
                 "[REPRESENTATION]\n";
    return 2;
  }
  int64_t frame = 100;
  if (argc >= 3 && !parseFrame(argv[2], &frame)) {
    std::cerr << "FRAME must be a non-negative integer\n";
    return 2;
  }
  std::string error;
  const auto start = std::chrono::steady_clock::now();
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << "Archive open failed: " << error << '\n';
    return 1;
  }
  auto repository =
      crimson::zarr::OpenEyeAngleTimelineRepository(archive, {}, &error);
  if (!repository) {
    std::cerr << "Timeline open failed: " << error << '\n';
    return 1;
  }
  const auto& descriptor = repository->descriptor();
  const std::string representation =
      argc >= 4 ? argv[3]
                : crimson::timeline::defaultEyeAngleTimelineRepresentation(
                      descriptor);
  if (descriptor.frame_count == 0 ||
      frame >= static_cast<int64_t>(descriptor.frame_count)) {
    std::cerr << "FRAME is outside the timeline range\n";
    return 2;
  }
  crimson::timeline::EyeAngleTimelineRequest request;
  request.representation_key = representation;
  request.first_frame = std::max<int64_t>(0, frame - 128);
  request.last_frame = std::min<int64_t>(
      static_cast<int64_t>(descriptor.frame_count) - 1, frame + 127);
  request.anchor_frame = frame;
  request.max_points_per_trace = 128;
  request.fallback_frames_per_second = 100.0;
  const auto read_start = std::chrono::steady_clock::now();
  const auto window = repository->resolveWindow(request);
  const double read_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - read_start)
                             .count();
  const double open_ms = std::chrono::duration<double, std::milli>(
                             read_start - start)
                             .count();
  std::cout << "run=" << descriptor.run_name
            << " schema=" << descriptor.schema_id << ':'
            << descriptor.schema_version << " layout=" << descriptor.layout
            << " roi_rows=" << descriptor.roi_row_count
            << " frames=" << descriptor.frame_count
            << " default=" << descriptor.default_representation
            << " representations=" << descriptor.representations.size()
            << " open_ms=" << open_ms << '\n';
  if (!window.ready()) {
    std::cerr << "Timeline read failed: " << window.error << '\n';
    return 1;
  }
  std::cout << "window=" << request.first_frame << ':' << request.last_frame
            << " representation=" << representation
            << " source_rows=" << window.source_row_count
            << " traces=" << window.traces.size()
            << " points=" << window.published_point_count
            << " read_ms=" << read_ms << '\n';
  for (const auto& trace : window.traces) {
    std::cout << "  role=" << roleName(trace.field.role)
              << " requested=" << trace.field.requested_name
              << " source=" << trace.field.source_name
              << " fallback=" << (trace.field.fallback ? 1 : 0)
              << " points=" << trace.values.size() << '\n';
  }
  return 0;
}
