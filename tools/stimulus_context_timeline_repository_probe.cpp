#include "zarr/archive_context.h"
#include "zarr/tensorstore_stimulus_context_timeline_repository.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: stimulus_context_timeline_repository_probe "
                 "<analysis.zarr> [run]\n";
    return 2;
  }
  const auto started = std::chrono::steady_clock::now();
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << error << '\n';
    return 1;
  }
  const std::string run = argc == 3 ? argv[2] : std::string{};
  auto repository = crimson::zarr::OpenStimulusContextTimelineRepository(
      archive, 0, run, &error);
  if (!repository) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto snapshot = repository->snapshot();
  size_t unresolved = 0;
  for (const auto& event : snapshot->events) {
    unresolved += event.camera_frame < 0 ? 1 : 0;
  }
  const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
  std::cout << "run=" << snapshot->descriptor.run_name
            << " events=" << snapshot->descriptor.event_count
            << " steps=" << snapshot->descriptor.step_count
            << " event_types=" << snapshot->descriptor.event_types.size()
            << " frame_count=" << snapshot->descriptor.frame_count
            << " unresolved_camera_frames=" << unresolved
            << " open_ms=" << elapsed_ms << '\n';
  if (!snapshot->events.empty()) {
    const auto& event = snapshot->events.front();
    std::cout << "first_event stimulus_frame=" << event.stimulus_frame
              << " camera_frame=" << event.camera_frame
              << " type=" << event.event_type_id << " label='" << event.label
              << "'\n";
  }
  if (!snapshot->steps.empty()) {
    const auto& step = snapshot->steps.front();
    std::cout << "first_step index=" << step.step_index << " frames="
              << step.start_camera_frame << ':' << step.end_camera_frame
              << " mode=" << step.stimulus_mode << " name='" << step.step_name
              << "'\n";
  }
  return 0;
}
