#include "zarr/archive_context.h"
#include "zarr/tensorstore_keypoint_overlay_repository.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: keypoint_overlay_repository_probe ZARR [FRAME]\n";
    return 2;
  }
  const int64_t frame = argc == 3 ? std::strtoll(argv[2], nullptr, 10) : 1024;
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << "archive error: " << error << '\n';
    return 1;
  }
  auto repository = crimson::zarr::OpenKeypointOverlayRepository(
      archive, {}, &error);
  if (!repository) {
    std::cerr << "repository error: " << error << '\n';
    return 1;
  }
  const auto& descriptor = repository->descriptor();
  const auto resolution = repository->resolveCameraFrame(frame, 4512, 4512);
  size_t finite_keypoints = 0;
  size_t headings = 0;
  for (const auto& detection : resolution.detections) {
    headings += detection.heading_valid ? 1 : 0;
    finite_keypoints += static_cast<size_t>(std::count_if(
        detection.keypoints.begin(), detection.keypoints.end(),
        [](const auto& point) {
          return std::isfinite(point.x) && std::isfinite(point.y);
        }));
  }
  std::cout << "group=" << descriptor.source_group
            << " run=" << descriptor.run_name
            << " refined=" << descriptor.refined
            << " crop_run=" << descriptor.source_crop_run
            << " rows=" << descriptor.row_count
            << " camera_frames=" << descriptor.camera_frame_count
            << " labels=" << descriptor.keypoint_labels.size()
            << " edges=" << descriptor.skeleton_edges.size()
            << " frame=" << frame
            << " detections=" << resolution.detections.size()
            << " finite_keypoints=" << finite_keypoints
            << " valid_headings=" << headings << '\n';
  return resolution.status == crimson::zarr::KeypointOverlayStatus::Mapped
             ? 0
             : 1;
}
