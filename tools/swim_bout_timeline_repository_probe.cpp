#include "swim_bout_timeline.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_swim_bout_timeline_repository.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc < 2 || argc > 5) {
    std::cerr << "Usage: " << argv[0]
              << " ARCHIVE [FRAME_COUNT] [RUN] [FRAME]\n";
    return 2;
  }
  const size_t frame_count =
      argc >= 3 ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 10)) : 0;
  const std::string run = argc >= 4 ? argv[3] : std::string{};
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << "Archive open failed: " << error << '\n';
    return 1;
  }
  auto repository = crimson::zarr::OpenSwimBoutTimelineRepository(
      archive, frame_count, run, &error);
  if (!repository) {
    std::cerr << "Swim-bout timeline open failed: " << error << '\n';
    return 1;
  }
  const auto &descriptor = repository->descriptor();
  std::cout << "candidates=" << descriptor.candidates.size()
            << " frame_count=" << descriptor.frame_count
            << " default=" << descriptor.default_candidate << '\n';
  for (const auto &candidate : descriptor.candidates) {
    std::cout << "candidate=" << candidate.key << " run=" << candidate.run_name
              << " level=" << candidate.speed_level
              << " candidate_id=" << candidate.candidate_id
              << " signal_id=" << candidate.signal_id
              << " bouts=" << candidate.bout_count
              << " detector_samples=" << candidate.detector_sample_count
              << " latest=" << candidate.latest_run
              << " default_level=" << candidate.default_level
              << " source_run=" << candidate.source_track_kinematics_run
              << " track_id=" << candidate.track_id << '\n';
  }
  const int64_t requested_frame =
      argc >= 5 ? std::strtoll(argv[4], nullptr, 10)
                : static_cast<int64_t>(descriptor.frame_count / 2);
  const auto bounds = crimson::timeline::swimBoutTimelinePageBounds(
      requested_frame, descriptor.frame_count);
  crimson::timeline::SwimBoutTimelineRequest request;
  request.candidate_key = descriptor.default_candidate;
  request.first_frame = bounds.first_frame;
  request.last_frame = bounds.last_frame;
  request.anchor_frame = requested_frame;
  request.max_detector_points = 1200;
  request.fallback_frames_per_second = 100.0;
  request.include_detector_trace = true;
  const auto window = repository->resolveWindow(request);
  if (window.status == crimson::timeline::SwimBoutTimelineStatus::ReadFailed ||
      window.status ==
          crimson::timeline::SwimBoutTimelineStatus::InvalidRequest) {
    std::cerr << "Window resolve failed: " << window.error << '\n';
    return 1;
  }
  std::cout << "window=" << request.first_frame << ':' << request.last_frame
            << " intervals=" << window.intervals.size()
            << " detector_rows=" << window.source_detector_row_count
            << " detector_points=" << window.detector_values.size()
            << " status=" << static_cast<int>(window.status) << '\n';
  return 0;
}
