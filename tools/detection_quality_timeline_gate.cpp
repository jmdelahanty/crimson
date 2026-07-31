#include "zarr/archive_context.h"
#include "zarr/tensorstore_detection_quality_timeline_repository.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char **argv) {
  if (argc < 4 || argc > 5) {
    std::fprintf(stderr,
                 "Usage: %s ARCHIVE canonical|refined RUN [allow-ineligible]\n",
                 argv[0]);
    return 2;
  }
  const std::string surface = argv[2];
  if (surface != "canonical" && surface != "refined") {
    std::fprintf(stderr, "Surface must be canonical or refined\n");
    return 2;
  }

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::fprintf(stderr, "Archive open failed: %s\n", error.c_str());
    return 1;
  }
  crimson::zarr::DetectionQualityTimelineOpenRequest request;
  request.surface_kind =
      surface == "refined"
          ? crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1
          : crimson::zarr::DetectionSurfaceKind::CanonicalRawV1;
  request.run_name = argv[3];
  request.allow_selector_ineligible_refined_run = argc == 5;
  crimson::zarr::DetectionQualityTimelineOpenMetrics open_metrics;
  const auto started = std::chrono::steady_clock::now();
  auto repository = crimson::zarr::OpenDetectionQualityTimelineRepository(
      archive, request, &error, &open_metrics);
  if (!repository) {
    std::fprintf(stderr, "Repository open failed: %s\n", error.c_str());
    return 1;
  }
  const auto &descriptor = repository->descriptor();
  const size_t expected_offset_reads = surface == "refined" ? 2 : 1;
  if (!descriptor.ready() ||
      descriptor.offset_read_calls != expected_offset_reads ||
      descriptor.frame_count == 0) {
    std::fprintf(stderr, "Repository descriptor contract failed\n");
    return 1;
  }
  const int64_t last_frame =
      static_cast<int64_t>(std::min<size_t>(descriptor.frame_count - 1, 8191));
  const auto window = repository->resolveWindow(0, last_frame);
  if (!window.ready() ||
      window.frames.size() != static_cast<size_t>(last_frame + 1)) {
    std::fprintf(stderr, "Timeline window failed: %s\n", window.error.c_str());
    return 1;
  }
  const auto repository_metrics = repository->metrics();
  const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
  std::printf(
      "[DetectionQualityGate] PASS surface=%s run=%s frames=%zu "
      "source_rows=%zu instance_rows=%zu offset_reads=%zu "
      "retained_offset_bytes=%zu open_ms=%.1f offset_ms=%.1f "
      "window_frames=%zu window_source_rows=%zu window_instance_rows=%zu "
      "decoded_bytes=%llu peak_concurrent_fields=%zu total_ms=%.1f\n",
      surface.c_str(), descriptor.run_name.c_str(), descriptor.frame_count,
      descriptor.source_row_count, descriptor.instance_row_count,
      open_metrics.offset_read_calls, open_metrics.retained_offset_bytes,
      open_metrics.total_ms, open_metrics.offset_read_ms, window.frames.size(),
      window.source_rows_read, window.instance_rows_read,
      static_cast<unsigned long long>(repository_metrics.decoded_bytes),
      repository_metrics.peak_concurrent_field_reads, elapsed_ms);
  return 0;
}
