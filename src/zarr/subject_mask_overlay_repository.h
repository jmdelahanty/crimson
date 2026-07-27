#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

enum class SubjectMaskStorage : uint8_t {
  Dense,
  Bitpacked,
  Rle,
};

struct SubjectMaskOverlayPoint {
  double x = 0.0;
  double y = 0.0;
};

struct SubjectMaskOverlayDescriptor {
  std::string source_group;
  std::string run_name;
  std::string source_crop_run;
  std::string label_schema_id;
  SubjectMaskStorage storage = SubjectMaskStorage::Dense;
  std::vector<std::string> component_labels;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
  size_t mask_width = 0;
  size_t mask_height = 0;
  size_t storage_chunk_rows = 0;
};

struct SubjectMaskOverlayRepositoryMetrics {
  bool lazy_mapping = false;
  double open_total_ms = 0.0;
  double catalog_ms = 0.0;
  double mapping_read_ms = 0.0;
  double frame_indices_ms = 0.0;
  double detection_indices_ms = 0.0;
  double source_crop_row_ids_ms = 0.0;
  double crop_frame_indices_ms = 0.0;
  double crop_coordinates_ms = 0.0;
  double crop_detection_indices_ms = 0.0;
  double storage_open_ms = 0.0;
  double contour_open_ms = 0.0;
  double metadata_index_ms = 0.0;
  double frame_index_initialize_ms = 0.0;
  double maximum_mapping_page_read_ms = 0.0;
  uint64_t metadata_decoded_bytes = 0;
  uint64_t subject_mapping_bytes = 0;
  uint64_t crop_mapping_bytes = 0;
  uint64_t metadata_retained_bytes = 0;
  uint64_t frame_index_rows_read = 0;
  uint64_t frame_index_source_bytes = 0;
  uint64_t frame_index_retained_bytes = 0;
  uint64_t fallback_frame_index_builds = 0;
  uint64_t fallback_frame_index_rows = 0;
  uint64_t mapping_page_reads = 0;
  uint64_t mapping_page_cache_hits = 0;
  uint64_t mapping_page_evictions = 0;
  uint64_t mapping_page_source_bytes = 0;
  uint64_t cached_mapping_bytes = 0;
  uint64_t peak_cached_mapping_bytes = 0;
  uint64_t mapping_initialize_failures = 0;
  uint64_t demand_chunk_loads = 0;
  uint64_t prefetched_chunk_loads = 0;
  uint64_t chunk_cache_hits = 0;
  uint64_t prefetch_requests = 0;
  uint64_t chunk_evictions = 0;
  uint64_t chunk_load_failures = 0;
  uint64_t chunk_source_bytes_read = 0;
  uint64_t chunk_retained_bytes_produced = 0;
  uint64_t cached_payload_bytes = 0;
  uint64_t peak_cached_payload_bytes = 0;
  uint64_t evicted_payload_bytes = 0;
  size_t cached_chunks = 0;
  size_t peak_cached_chunks = 0;
  double chunk_read_ms = 0.0;
  double chunk_convert_ms = 0.0;
  double contour_load_ms = 0.0;
  double maximum_chunk_load_ms = 0.0;
  double maximum_chunk_read_ms = 0.0;
  double maximum_chunk_convert_ms = 0.0;
  double maximum_contour_load_ms = 0.0;
};

struct SubjectMaskOverlayComponent {
  std::string label;
  size_t channel_index = 0;
  bool present = false;
  size_t mask_width = 0;
  size_t mask_height = 0;
  std::shared_ptr<const std::vector<uint8_t>> mask;
  std::vector<SubjectMaskOverlayPoint> contour;
};

struct SubjectMaskOverlayRow {
  int64_t camera_frame = -1;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
  std::vector<SubjectMaskOverlayComponent> components;
};

struct SubjectMaskOverlayDetection {
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
  std::vector<SubjectMaskOverlayComponent> components;
};

enum class SubjectMaskOverlayStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidDimensions,
  ReadFailed,
};

struct SubjectMaskOverlayResolution {
  SubjectMaskOverlayStatus status = SubjectMaskOverlayStatus::Missing;
  int64_t camera_frame = -1;
  std::vector<SubjectMaskOverlayDetection> detections;
  std::string error;
};

class SubjectMaskOverlayRepository {
public:
  virtual ~SubjectMaskOverlayRepository() = default;

  virtual const SubjectMaskOverlayDescriptor &descriptor() const = 0;
  virtual SubjectMaskOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const = 0;

  virtual SubjectMaskOverlayRepositoryMetrics metrics() const { return {}; }
};

std::unique_ptr<SubjectMaskOverlayRepository>
MakeSubjectMaskOverlayRepository(SubjectMaskOverlayDescriptor descriptor,
                                 std::vector<SubjectMaskOverlayRow> rows);

} // namespace crimson::zarr
