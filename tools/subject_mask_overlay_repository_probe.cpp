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
  const auto archive_open_start = std::chrono::steady_clock::now();
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  const double archive_open_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() -
                                     archive_open_start)
                                     .count();
  if (!archive) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto repository_open_start = std::chrono::steady_clock::now();
  auto repository = crimson::zarr::OpenSubjectMaskOverlayRepository(
      archive, argc == 4 ? argv[3] : std::string{}, &error);
  const double repository_open_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() -
                                        repository_open_start)
                                        .count();
  if (!repository) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto resolution = repository->resolveCameraFrame(frame, 4512, 4512);
  const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count();
  const auto warm_start = std::chrono::steady_clock::now();
  const auto warm_resolution =
      repository->resolveCameraFrame(frame, 4512, 4512);
  const double warm_elapsed_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() -
                                     warm_start)
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
  const auto metrics = repository->metrics();
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
            << descriptor.mask_height
            << " chunk_rows=" << descriptor.storage_chunk_rows
            << " lazy_mapping=" << (metrics.lazy_mapping ? 1 : 0)
            << " cache_pool_bytes=" << archive->cachePoolBytes()
            << " archive_open_ms=" << archive_open_ms
            << " repository_open_ms=" << repository_open_ms
            << " catalog_ms=" << metrics.catalog_ms
            << " mapping_ms=" << metrics.mapping_read_ms
            << " frame_indices_ms=" << metrics.frame_indices_ms
            << " detection_indices_ms=" << metrics.detection_indices_ms
            << " source_crop_row_ids_ms=" << metrics.source_crop_row_ids_ms
            << " crop_frame_indices_ms=" << metrics.crop_frame_indices_ms
            << " crop_coordinates_ms=" << metrics.crop_coordinates_ms
            << " crop_detection_indices_ms="
            << metrics.crop_detection_indices_ms
            << " storage_ms=" << metrics.storage_open_ms
            << " contour_open_ms=" << metrics.contour_open_ms
            << " index_ms=" << metrics.metadata_index_ms
            << " metadata_decoded_bytes=" << metrics.metadata_decoded_bytes
            << " subject_mapping_bytes=" << metrics.subject_mapping_bytes
            << " crop_mapping_bytes=" << metrics.crop_mapping_bytes
            << " metadata_retained_bytes=" << metrics.metadata_retained_bytes
            << " frame_index_initialize_ms="
            << metrics.frame_index_initialize_ms
            << " frame_index_rows=" << metrics.frame_index_rows_read
            << " frame_index_source_bytes="
            << metrics.frame_index_source_bytes
            << " frame_index_retained_bytes="
            << metrics.frame_index_retained_bytes
            << " fallback_frame_index_builds="
            << metrics.fallback_frame_index_builds
            << " fallback_frame_index_rows="
            << metrics.fallback_frame_index_rows
            << " mapping_page_reads=" << metrics.mapping_page_reads
            << " mapping_page_hits=" << metrics.mapping_page_cache_hits
            << " mapping_page_evictions=" << metrics.mapping_page_evictions
            << " mapping_page_source_bytes="
            << metrics.mapping_page_source_bytes
            << " cached_mapping_bytes=" << metrics.cached_mapping_bytes
            << " peak_mapping_bytes=" << metrics.peak_cached_mapping_bytes
            << " mapping_initialize_failures="
            << metrics.mapping_initialize_failures
            << " max_mapping_page_ms="
            << metrics.maximum_mapping_page_read_ms
            << " frame=" << frame
            << " status=" << static_cast<int>(resolution.status)
            << " detections=" << resolution.detections.size()
            << " present_components=" << present_components
            << " foreground_pixels=" << foreground_pixels
            << " contour_points=" << contour_points
            << " resolve_ms=" << elapsed_ms
            << " warm_status=" << static_cast<int>(warm_resolution.status)
            << " warm_resolve_ms=" << warm_elapsed_ms
            << " demand_chunks=" << metrics.demand_chunk_loads
            << " prefetched_chunks=" << metrics.prefetched_chunk_loads
            << " chunk_cache_hits=" << metrics.chunk_cache_hits
            << " prefetch_requests=" << metrics.prefetch_requests
            << " cached_chunks=" << metrics.cached_chunks
            << " peak_chunks=" << metrics.peak_cached_chunks
            << " source_bytes=" << metrics.chunk_source_bytes_read
            << " retained_chunk_bytes="
            << metrics.chunk_retained_bytes_produced
            << " cached_chunk_bytes=" << metrics.cached_payload_bytes
            << " peak_chunk_bytes=" << metrics.peak_cached_payload_bytes
            << " evicted_chunk_bytes=" << metrics.evicted_payload_bytes
            << " chunk_read_ms=" << metrics.chunk_read_ms
            << " chunk_convert_ms=" << metrics.chunk_convert_ms
            << " contour_load_ms=" << metrics.contour_load_ms
            << " max_chunk_ms=" << metrics.maximum_chunk_load_ms
            << " max_read_ms=" << metrics.maximum_chunk_read_ms
            << " max_convert_ms=" << metrics.maximum_chunk_convert_ms
            << " max_contour_ms=" << metrics.maximum_contour_load_ms << '\n';
  return resolution.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped
             ? 0
             : 1;
}
