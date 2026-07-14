#include "zarr/archive_context.h"
#include "zarr/tensorstore_eye_geometry_overlay_repository.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: " << argv[0] << " ARCHIVE.zarr CAMERA_FRAME [RUN]\n";
    return 2;
  }
  int64_t frame = -1;
  try {
    frame = std::stoll(argv[2]);
  } catch (...) {
    std::cerr << "Invalid camera frame\n";
    return 2;
  }
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto open_started = std::chrono::steady_clock::now();
  auto repository = crimson::zarr::OpenEyeGeometryOverlayRepository(
      archive, argc == 4 ? argv[3] : std::string{}, &error);
  const double open_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - open_started)
                             .count();
  if (!repository) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto resolution = repository->resolveCameraFrame(frame, 4512, 4512);
  const double resolve_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
  size_t valid_eyes = 0;
  size_t valid_gaze = 0;
  for (const auto &detection : resolution.detections) {
    for (const auto &eye : detection.eyes) {
      valid_eyes += eye.major_axis.valid && eye.minor_axis.valid ? 1 : 0;
      valid_gaze += eye.gaze_valid ? 1 : 0;
    }
  }
  const auto &descriptor = repository->descriptor();
  std::cout << "group=" << descriptor.source_group
            << " run=" << descriptor.run_name
            << " refined_masks=" << descriptor.source_refined_subject_masks_run
            << " crop_run=" << descriptor.source_crop_run
            << " rows=" << descriptor.row_count
            << " camera_frames=" << descriptor.camera_frame_count
            << " coordinates=" << descriptor.coordinate_width << 'x'
            << descriptor.coordinate_height << " frame=" << frame
            << " status=" << static_cast<int>(resolution.status)
            << " detections=" << resolution.detections.size()
            << " valid_eyes=" << valid_eyes << " valid_gaze=" << valid_gaze
            << " open_ms=" << open_ms << " resolve_ms=" << resolve_ms << '\n';
  return resolution.status == crimson::zarr::EyeGeometryOverlayStatus::Mapped
             ? 0
             : 1;
}
