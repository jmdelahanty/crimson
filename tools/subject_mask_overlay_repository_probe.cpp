#include "zarr/archive_context.h"
#include "zarr/tensorstore_subject_mask_overlay_repository.h"

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
  auto repository = crimson::zarr::OpenSubjectMaskOverlayRepository(
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
  size_t present_components = 0;
  size_t foreground_pixels = 0;
  size_t contour_points = 0;
  for (const auto &detection : resolution.detections) {
    for (const auto &component : detection.components) {
      present_components += component.present ? 1 : 0;
      if (component.mask) {
        for (const uint8_t value : *component.mask) {
          foreground_pixels += value != 0 ? 1 : 0;
        }
      }
      contour_points += component.contour.size();
    }
  }
  const auto &descriptor = repository->descriptor();
  const char *storage =
      descriptor.storage == crimson::zarr::SubjectMaskStorage::Dense ? "dense"
      : descriptor.storage == crimson::zarr::SubjectMaskStorage::Bitpacked
          ? "bitpacked"
          : "rle";
  std::cout << "group=" << descriptor.source_group
            << " run=" << descriptor.run_name << " storage=" << storage
            << " crop_run=" << descriptor.source_crop_run
            << " rows=" << descriptor.row_count
            << " camera_frames=" << descriptor.camera_frame_count
            << " components=" << descriptor.component_labels.size()
            << " mask=" << descriptor.mask_width << 'x'
            << descriptor.mask_height << " frame=" << frame
            << " status=" << static_cast<int>(resolution.status)
            << " detections=" << resolution.detections.size()
            << " present_components=" << present_components
            << " foreground_pixels=" << foreground_pixels
            << " contour_points=" << contour_points
            << " resolve_ms=" << elapsed_ms << '\n';
  return resolution.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped
             ? 0
             : 1;
}
