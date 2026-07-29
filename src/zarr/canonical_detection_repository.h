#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

enum class DetectionSurfaceKind : uint8_t {
  CanonicalRawV1,
  RefinedSnapshotV1,
};

struct CanonicalDetectionDescriptor {
  DetectionSurfaceKind surface_kind = DetectionSurfaceKind::CanonicalRawV1;
  std::string source_group;
  std::string run_name;
  std::string instance_group;
  std::string run_manifest_digest;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
  size_t source_width = 0;
  size_t source_height = 0;
  size_t retained_offset_bytes = 0;
  size_t offset_read_calls = 0;
  bool consolidated_metadata = false;
  bool stable_identity = false;
  bool source_audit_lazy = false;
  bool authority_approved = false;
  bool coordinate_catalog_validated = false;

  bool ready() const {
    return !run_name.empty() && camera_frame_count > 0 &&
           offset_read_calls == 1;
  }
};

struct CanonicalDetection {
  int64_t row_index = -1;
  uint64_t instance_key = 0;
  int64_t refined_row_id = -1;
  int64_t source_detect_row_index = -1;
  uint8_t source_kind_code = 0;
  bool score_valid = true;
  bool manual_edit = false;
  std::array<float, 4> normalized_cxcywh{};
  float score = 0.0f;
  int32_t class_id = 0;
};

struct CanonicalDetectionFrame {
  int64_t camera_frame = -1;
  std::vector<CanonicalDetection> detections;
};

enum class CanonicalDetectionPageStatus : uint8_t {
  Ready,
  OutOfRange,
  ReadFailed,
};

struct CanonicalDetectionPage {
  CanonicalDetectionPageStatus status =
      CanonicalDetectionPageStatus::ReadFailed;
  int64_t first_camera_frame = -1;
  int64_t last_camera_frame = -1;
  std::vector<CanonicalDetectionFrame> frames;
  uint64_t decoded_bytes = 0;
  std::string error;
};

struct CanonicalDetectionUiRows {
  CanonicalDetectionPageStatus status =
      CanonicalDetectionPageStatus::ReadFailed;
  size_t first_row = 0;
  size_t last_row_exclusive = 0;
  std::vector<int32_t> frame_indices;
  std::vector<float> bbox_norm_coords;
  std::vector<float> scores;
  std::vector<int32_t> class_ids;
  std::vector<uint64_t> instance_keys;
  std::vector<int64_t> refined_row_ids;
  std::vector<int64_t> source_detect_row_indices;
  std::vector<uint8_t> source_kind_codes;
  std::vector<uint8_t> score_valid;
  std::vector<uint8_t> manual_edit_flags;
  uint64_t decoded_bytes = 0;
  std::string error;

  bool ready() const { return status == CanonicalDetectionPageStatus::Ready; }
};

struct CanonicalDetectionUiResidencyChunk {
  int64_t first_camera_frame = -1;
  int64_t last_camera_frame = -1;
  size_t first_row = 0;
  size_t last_row_exclusive = 0;

  bool valid() const {
    return first_camera_frame >= 0 && last_camera_frame >= first_camera_frame &&
           last_row_exclusive >= first_row;
  }
};

struct CanonicalDetectionResidentUiColumns {
  std::vector<float> bbox_norm_coords;
  std::vector<float> scores;
  std::vector<int32_t> class_ids;
  std::vector<uint64_t> instance_keys;
  std::vector<int64_t> refined_row_ids;
  std::vector<int64_t> source_detect_row_indices;
  std::vector<uint8_t> source_kind_codes;
  std::vector<uint8_t> score_valid;
  std::vector<uint8_t> manual_edit_flags;

  uint64_t retainedBytes() const {
    return bbox_norm_coords.size() * sizeof(float) +
           scores.size() * sizeof(float) + class_ids.size() * sizeof(int32_t) +
           instance_keys.size() * sizeof(uint64_t) +
           refined_row_ids.size() * sizeof(int64_t) +
           source_detect_row_indices.size() * sizeof(int64_t) +
           source_kind_codes.size() * sizeof(uint8_t) +
           score_valid.size() * sizeof(uint8_t) +
           manual_edit_flags.size() * sizeof(uint8_t);
  }

  bool valid(size_t row_count, bool stable_identity = false) const {
    const bool identity_valid =
        !stable_identity || (instance_keys.size() == row_count &&
                             refined_row_ids.size() == row_count &&
                             source_detect_row_indices.size() == row_count &&
                             source_kind_codes.size() == row_count &&
                             score_valid.size() == row_count &&
                             manual_edit_flags.size() == row_count);
    return row_count <= SIZE_MAX / 4 && identity_valid &&
           bbox_norm_coords.size() == row_count * 4 &&
           scores.size() == row_count && class_ids.size() == row_count;
  }
};

struct CanonicalDetectionRepositoryMetrics {
  uint64_t range_reads = 0;
  uint64_t paged_range_reads = 0;
  uint64_t resident_range_reads = 0;
  uint64_t resolved_frames = 0;
  uint64_t resolved_rows = 0;
  uint64_t failed_reads = 0;
  uint64_t ui_field_reads = 0;
  uint64_t residency_chunk_reads = 0;
  uint64_t residency_rows_read = 0;
  uint64_t residency_decoded_bytes = 0;
  uint64_t resident_publications = 0;
  uint64_t resident_retained_bytes = 0;
  size_t peak_concurrent_ui_field_reads = 0;
  double maximum_range_read_ms = 0.0;
  double maximum_residency_chunk_read_ms = 0.0;
  std::string last_error;
};

class CanonicalDetectionRepository {
public:
  virtual ~CanonicalDetectionRepository() = default;

  virtual const CanonicalDetectionDescriptor &descriptor() const = 0;
  virtual CanonicalDetectionPage
  resolveCameraFrameRange(int64_t first_camera_frame,
                          int64_t last_camera_frame) const = 0;
  virtual uint64_t decodedUiColumnBytes() const = 0;
  virtual std::vector<CanonicalDetectionUiResidencyChunk>
  planUiResidency(uint64_t maximum_chunk_decoded_bytes) const = 0;
  virtual CanonicalDetectionUiRows
  readUiRowsForResidency(size_t first_row, size_t last_row_exclusive) const = 0;
  virtual bool publishResidentUiColumns(
      std::shared_ptr<const CanonicalDetectionResidentUiColumns> columns,
      std::string *error = nullptr) = 0;
  virtual bool residentUiColumnsReady() const = 0;
  virtual uint64_t residentUiColumnBytes() const = 0;
  virtual CanonicalDetectionRepositoryMetrics metrics() const = 0;
};

} // namespace crimson::zarr
