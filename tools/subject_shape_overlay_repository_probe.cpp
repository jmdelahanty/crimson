#include "zarr/archive_context.h"
#include "zarr/tensorstore_subject_shape_overlay_repository.h"

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
  auto repository = crimson::zarr::OpenSubjectShapeOverlayRepository(
      archive, argc == 4 ? argv[3] : std::string{}, &error);
  if (!repository) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto resolution = repository->resolveCameraFrame(frame, 4512, 4512);
  const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count();
  size_t centerline_points = 0;
  size_t bspline_points = 0;
  for (const auto &detection : resolution.detections) {
    centerline_points += detection.geometry.centerline.size();
    bspline_points += detection.geometry.bspline_sample.size();
  }
  const auto &descriptor = repository->descriptor();
  std::cout << "group=" << descriptor.source_group
            << " run=" << descriptor.run_name
            << " refined_masks="
            << descriptor.source_refined_subject_masks_run
            << " crop_run=" << descriptor.source_crop_run
            << " rows=" << descriptor.row_count
            << " camera_frames=" << descriptor.camera_frame_count
            << " coordinates=" << descriptor.coordinate_width << 'x'
            << descriptor.coordinate_height << " frame=" << frame
            << " status=" << static_cast<int>(resolution.status)
            << " detections=" << resolution.detections.size()
            << " centerline_points=" << centerline_points
            << " bspline_points=" << bspline_points
            << " resolve_ms=" << elapsed_ms << '\n';
  return resolution.status == crimson::zarr::SubjectShapeOverlayStatus::Mapped
             ? 0
             : 1;
}
