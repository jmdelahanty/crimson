#include "zarr/archive_context.h"
#include "zarr/canonical_overlay_selection.h"
#include "zarr/tensorstore_bound_subject_shape_overlay_repository.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: " << argv[0]
              << " ARCHIVE.zarr CAMERA_FRAME [EYE_RUN]\n";
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
  crimson::zarr::CanonicalOverlaySelectionRequest selection_request;
  if (argc == 4) {
    selection_request.eye_run = argv[3];
  }
  auto selection = crimson::zarr::SelectCanonicalOverlaySources(
      archive, selection_request, &error);
  if (!selection || !selection->shape.valid || !selection->mask.valid) {
    std::cerr << (error.empty() ? "Bound shape/mask selection is invalid"
                                : error)
              << '\n';
    return 1;
  }
  crimson::zarr::BoundSubjectShapeOverlayOpenRequest open_request;
  open_request.archive = archive;
  open_request.selection = *selection;
  crimson::zarr::SubjectShapeOverlayOpenMetrics open_metrics;
  auto repository = crimson::zarr::OpenBoundSubjectShapeOverlayRepository(
      open_request, &error, &open_metrics);
  if (!repository) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto resolution = repository->resolveCameraFrame(
      frame, static_cast<int>(selection->source_width),
      static_cast<int>(selection->source_height));
  const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count();
  size_t centerline_points = 0;
  size_t bspline_points = 0;
  size_t valid_keys = 0;
  size_t headings = 0;
  for (const auto &detection : resolution.detections) {
    centerline_points += detection.geometry.centerline.size();
    bspline_points += detection.geometry.bspline_sample.size();
    valid_keys += detection.instance_key_valid ? 1 : 0;
    headings += detection.geometry.heading_degrees.has_value() ? 1 : 0;
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
            << " valid_instance_keys=" << valid_keys
            << " body_frame_headings=" << headings
            << " centerline_points=" << centerline_points
            << " bspline_points=" << bspline_points
            << " exact_handle_opens=" << open_metrics.exact_handle_opens
            << " offset_read_calls=" << open_metrics.offset_read_calls
            << " retained_offset_bytes="
            << open_metrics.retained_offset_bytes
            << " max_observations_per_frame="
            << open_metrics.maximum_observations_per_frame
            << " max_decoded_frame_bytes="
            << open_metrics.maximum_decoded_frame_bytes
            << " resolve_ms=" << elapsed_ms << '\n';
  return resolution.status == crimson::zarr::SubjectShapeOverlayStatus::Mapped
             ? 0
             : 1;
}
