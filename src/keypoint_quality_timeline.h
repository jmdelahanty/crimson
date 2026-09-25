#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace crimson::timeline {

struct KeypointQualityMetricDescriptor {
  std::string id;
  std::string units;
  bool higher_is_worse = false;
};

struct KeypointQualityFlagDescriptor {
  uint16_t mask = 0;
  std::string label;
};

struct KeypointQualityCodeDescriptor {
  uint16_t code = 0;
  std::string label;
};

struct KeypointQualityTimelineDescriptor {
  bool refined = false;
  std::string source_group;
  std::string run_name;
  std::string run_manifest_digest;
  std::string quality_run_name;
  std::string quality_manifest_digest;
  size_t frame_count = 0;
  size_t row_count = 0;
  size_t keypoint_count = 0;
  size_t retained_offset_bytes = 0;
  size_t offset_read_calls = 0;
  std::vector<std::string> keypoint_labels;
  std::vector<KeypointQualityMetricDescriptor> keypoint_metrics;
  std::vector<KeypointQualityMetricDescriptor> pose_metrics;
  std::vector<KeypointQualityFlagDescriptor> keypoint_flags;
  std::vector<KeypointQualityFlagDescriptor> pose_flags;
  std::vector<KeypointQualityCodeDescriptor> review_states;
  std::vector<KeypointQualityCodeDescriptor> reason_codes;

  bool ready() const {
    return !run_name.empty() && !quality_run_name.empty() && frame_count > 0 &&
           keypoint_count > 0 && keypoint_labels.size() == keypoint_count &&
           !keypoint_metrics.empty() && !pose_metrics.empty() &&
           !review_states.empty() && !reason_codes.empty() &&
           offset_read_calls == 1;
  }
};

enum class KeypointQualityTimelineStatus : uint8_t {
  Ready,
  OutOfRange,
  ReadFailed,
};

struct KeypointQualityFrame {
  int64_t camera_frame = -1;
  uint32_t observation_count = 0;
  uint32_t source_success_count = 0;
  uint32_t refined_success_count = 0;
  uint32_t usable_count = 0;
  uint32_t proposed_usable_count = 0;
  uint32_t flip_corrected_count = 0;
  uint32_t edited_keypoint_count = 0;
  double pose_confidence_median = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> keypoint_confidence_medians;
  std::vector<uint32_t> valid_keypoint_counts;
  std::vector<uint32_t> proposed_valid_keypoint_counts;
  std::vector<double> keypoint_metric_medians;
  std::vector<double> pose_metric_medians;
  std::vector<uint32_t> keypoint_flag_counts;
  std::vector<uint32_t> pose_flag_counts;
  std::vector<uint32_t> review_state_counts;
  std::vector<uint32_t> reason_code_counts;
};

struct KeypointQualityTimelineWindow {
  KeypointQualityTimelineStatus status =
      KeypointQualityTimelineStatus::ReadFailed;
  int64_t first_camera_frame = -1;
  int64_t last_camera_frame = -1;
  size_t rows_read = 0;
  uint64_t decoded_bytes = 0;
  std::vector<KeypointQualityFrame> frames;
  std::string error;

  bool ready() const { return status == KeypointQualityTimelineStatus::Ready; }
};

struct KeypointQualityOverviewTrace {
  std::vector<int64_t> camera_frames;
  std::vector<double> values;
};

struct KeypointQualityTimelineOverview {
  KeypointQualityTimelineStatus status =
      KeypointQualityTimelineStatus::ReadFailed;
  size_t frame_count = 0;
  size_t rows_read = 0;
  uint64_t decoded_bytes = 0;
  KeypointQualityOverviewTrace pose_confidence;
  std::vector<KeypointQualityOverviewTrace> keypoint_confidence;
  std::string error;

  bool ready() const { return status == KeypointQualityTimelineStatus::Ready; }
};

class KeypointQualityOverviewAccumulator {
public:
  KeypointQualityOverviewAccumulator(size_t frame_count, size_t keypoint_count,
                                     size_t maximum_points_per_trace);

  bool addFrame(int64_t camera_frame, const float *pose_confidences,
                const uint8_t *pose_valid, const float *keypoint_confidences,
                const uint8_t *keypoint_valid, size_t observation_count,
                std::string *error = nullptr);
  KeypointQualityTimelineOverview finish();

private:
  struct Extremum {
    bool has_value = false;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    int64_t minimum_frame = -1;
    int64_t maximum_frame = -1;
  };
  size_t frame_count_ = 0;
  size_t keypoint_count_ = 0;
  size_t bin_count_ = 0;
  size_t rows_read_ = 0;
  std::vector<Extremum> pose_extrema_;
  std::vector<Extremum> keypoint_extrema_;
};

struct KeypointQualityTimelineRepositoryMetrics {
  uint64_t range_reads = 0;
  uint64_t rows_read = 0;
  uint64_t decoded_bytes = 0;
  uint64_t failed_reads = 0;
  uint64_t overview_reads = 0;
  uint64_t overview_rows_read = 0;
  uint64_t overview_decoded_bytes = 0;
  uint64_t failed_overview_reads = 0;
  size_t peak_concurrent_field_reads = 0;
  double maximum_range_read_ms = 0.0;
  double maximum_overview_read_ms = 0.0;
  std::string last_error;
};

class KeypointQualityTimelineRepository {
public:
  virtual ~KeypointQualityTimelineRepository() = default;
  virtual const KeypointQualityTimelineDescriptor &descriptor() const = 0;
  virtual KeypointQualityTimelineWindow
  resolveWindow(int64_t first_camera_frame,
                int64_t last_camera_frame) const = 0;
  virtual KeypointQualityTimelineOverview
  resolveOverview(size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
                  const std::function<bool()> &cancelled = {}) const;
  virtual KeypointQualityTimelineRepositoryMetrics metrics() const {
    return {};
  }
};

struct KeypointQualityColumnData {
  std::vector<int64_t> frame_row_offsets;
  std::vector<uint64_t> selected_instance_keys;
  std::vector<uint64_t> quality_instance_keys;
  std::vector<float> keypoint_confidences;
  std::vector<uint8_t> keypoint_valid;
  std::vector<float> pose_confidences;
  std::vector<uint8_t> source_success;
  std::vector<uint8_t> refined_success;
  std::vector<uint8_t> keypoint_edit_flags;
  std::vector<uint8_t> flip_corrected;
  std::vector<uint8_t> usable_keypoints;
  std::vector<uint8_t> review_state_codes;
  std::vector<uint16_t> reason_codes;
  std::vector<float> keypoint_metric_values;
  std::vector<uint8_t> keypoint_metric_valid;
  std::vector<float> pose_metric_values;
  std::vector<uint8_t> pose_metric_valid;
  std::vector<uint16_t> keypoint_quality_flags;
  std::vector<uint16_t> pose_quality_flags;
  std::vector<uint8_t> proposed_keypoint_valid;
  std::vector<uint8_t> proposed_pose_usable;
};

KeypointQualityTimelineWindow buildKeypointQualityTimelineWindow(
    const KeypointQualityTimelineDescriptor &descriptor,
    int64_t first_camera_frame, int64_t last_camera_frame,
    const std::vector<int64_t> &frame_row_offsets, size_t first_row,
    const KeypointQualityColumnData &columns);

std::unique_ptr<KeypointQualityTimelineRepository>
MakeKeypointQualityTimelineRepository(
    KeypointQualityTimelineDescriptor descriptor,
    KeypointQualityColumnData columns, std::string *error = nullptr);

const char *
keypointQualityTimelineStatusName(KeypointQualityTimelineStatus status);

} // namespace crimson::timeline
