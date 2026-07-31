#pragma once

#include "zarr/canonical_detection_repository.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace crimson::timeline {

struct DetectionReasonDescriptor {
  uint16_t code = 0;
  std::string label;
};

struct DetectionQualityTimelineDescriptor {
  zarr::DetectionSurfaceKind surface_kind =
      zarr::DetectionSurfaceKind::CanonicalRawV1;
  std::string source_group;
  std::string run_name;
  std::string run_manifest_digest;
  std::string model_artifact_sha256;
  std::string producer_id;
  std::string producer_version;
  size_t frame_count = 0;
  size_t source_row_count = 0;
  size_t instance_row_count = 0;
  size_t retained_offset_bytes = 0;
  size_t offset_read_calls = 0;
  bool source_audit = false;
  bool numeric_cutoffs_declared = false;
  std::vector<DetectionReasonDescriptor> source_reason_codes;

  bool ready() const {
    return !run_name.empty() && frame_count > 0 && offset_read_calls > 0;
  }
};

enum class DetectionQualityTimelineStatus : uint8_t {
  Ready,
  OutOfRange,
  ReadFailed,
};

struct DetectionQualityFrame {
  int64_t camera_frame = -1;
  uint32_t source_count = 0;
  uint32_t accepted_count = 0;
  uint32_t filtered_count = 0;
  uint32_t duplicate_count = 0;
  uint32_t manual_clear_count = 0;
  uint32_t manual_count = 0;
  double score_min = std::numeric_limits<double>::quiet_NaN();
  double score_median = std::numeric_limits<double>::quiet_NaN();
  double score_max = std::numeric_limits<double>::quiet_NaN();
  double accepted_score_median = std::numeric_limits<double>::quiet_NaN();
  std::vector<uint32_t> reason_counts;
};

struct DetectionQualityTimelineWindow {
  DetectionQualityTimelineStatus status =
      DetectionQualityTimelineStatus::ReadFailed;
  int64_t first_camera_frame = -1;
  int64_t last_camera_frame = -1;
  size_t source_rows_read = 0;
  size_t instance_rows_read = 0;
  uint64_t decoded_bytes = 0;
  std::vector<DetectionQualityFrame> frames;
  std::string error;

  bool ready() const { return status == DetectionQualityTimelineStatus::Ready; }
};

struct DetectionQualityOverviewTrace {
  std::vector<int64_t> camera_frames;
  std::vector<double> values;
};

struct DetectionQualityTimelineOverview {
  DetectionQualityTimelineStatus status =
      DetectionQualityTimelineStatus::ReadFailed;
  size_t frame_count = 0;
  size_t source_rows_read = 0;
  size_t instance_rows_read = 0;
  uint64_t decoded_bytes = 0;
  DetectionQualityOverviewTrace source_confidence;
  DetectionQualityOverviewTrace accepted_confidence;
  DetectionQualityOverviewTrace source_count;
  DetectionQualityOverviewTrace accepted_count;
  DetectionQualityOverviewTrace filtered_count;
  DetectionQualityOverviewTrace duplicate_count;
  DetectionQualityOverviewTrace manual_clear_count;
  DetectionQualityOverviewTrace manual_count;
  std::string error;

  bool ready() const { return status == DetectionQualityTimelineStatus::Ready; }
};

class DetectionQualityOverviewAccumulator {
public:
  DetectionQualityOverviewAccumulator(size_t frame_count,
                                      size_t maximum_points_per_trace,
                                      bool source_audit);

  bool addFrame(int64_t camera_frame, const float *source_scores,
                const uint8_t *source_decisions, size_t source_count,
                const uint8_t *instance_source_kinds, size_t instance_count,
                std::string *error = nullptr);
  DetectionQualityTimelineOverview finish();

private:
  struct Extremum {
    bool has_value = false;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    int64_t minimum_frame = -1;
    int64_t maximum_frame = -1;
  };
  size_t frame_count_ = 0;
  size_t bin_count_ = 0;
  size_t source_rows_read_ = 0;
  size_t instance_rows_read_ = 0;
  bool source_audit_ = false;
  std::vector<Extremum> source_confidence_;
  std::vector<Extremum> accepted_confidence_;
  std::vector<Extremum> source_count_;
  std::vector<Extremum> accepted_count_;
  std::vector<Extremum> filtered_count_;
  std::vector<Extremum> duplicate_count_;
  std::vector<Extremum> manual_clear_count_;
  std::vector<Extremum> manual_count_;
};

struct DetectionQualityTimelineRepositoryMetrics {
  uint64_t range_reads = 0;
  uint64_t source_rows_read = 0;
  uint64_t instance_rows_read = 0;
  uint64_t decoded_bytes = 0;
  uint64_t failed_reads = 0;
  uint64_t overview_reads = 0;
  uint64_t overview_source_rows_read = 0;
  uint64_t overview_instance_rows_read = 0;
  uint64_t overview_decoded_bytes = 0;
  uint64_t failed_overview_reads = 0;
  size_t peak_concurrent_field_reads = 0;
  double maximum_range_read_ms = 0.0;
  double maximum_overview_read_ms = 0.0;
  std::string last_error;
};

class DetectionQualityTimelineRepository {
public:
  virtual ~DetectionQualityTimelineRepository() = default;
  virtual const DetectionQualityTimelineDescriptor &descriptor() const = 0;
  virtual DetectionQualityTimelineWindow
  resolveWindow(int64_t first_camera_frame,
                int64_t last_camera_frame) const = 0;
  virtual DetectionQualityTimelineOverview
  resolveOverview(size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
                  const std::function<bool()> &cancelled = {}) const;
  virtual DetectionQualityTimelineRepositoryMetrics metrics() const {
    return {};
  }
};

struct DetectionQualityColumnData {
  std::vector<int64_t> source_frame_row_offsets;
  std::vector<float> source_scores;
  std::vector<uint8_t> source_decision_codes;
  std::vector<uint16_t> source_reason_codes;
  std::vector<int64_t> instance_frame_row_offsets;
  std::vector<uint8_t> instance_source_kind_codes;
};

DetectionQualityTimelineWindow buildDetectionQualityTimelineWindow(
    const DetectionQualityTimelineDescriptor &descriptor,
    int64_t first_camera_frame, int64_t last_camera_frame,
    const std::vector<int64_t> &source_frame_row_offsets,
    size_t first_source_row, const std::vector<float> &source_scores,
    const std::vector<uint8_t> &source_decision_codes,
    const std::vector<uint16_t> &source_reason_codes,
    const std::vector<int64_t> &instance_frame_row_offsets,
    size_t first_instance_row,
    const std::vector<uint8_t> &instance_source_kind_codes);

std::unique_ptr<DetectionQualityTimelineRepository>
MakeDetectionQualityTimelineRepository(
    DetectionQualityTimelineDescriptor descriptor,
    DetectionQualityColumnData columns, std::string *error = nullptr);

const char *
detectionQualityTimelineStatusName(DetectionQualityTimelineStatus status);

} // namespace crimson::timeline
