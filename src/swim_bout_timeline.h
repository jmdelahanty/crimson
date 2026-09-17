#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace crimson::timeline {

struct AnalysisSeriesSourceDescriptor;

struct SwimBoutCandidateDescriptor {
  std::string key;
  std::string display_name;
  std::string source_group;
  std::string run_name;
  std::string speed_level;
  std::string layout;
  std::string source_track_kinematics_run;
  int32_t track_id = -1;
  int32_t candidate_id = -1;
  int32_t signal_id = -1;
  std::string signal_role;
  std::string signal_name;
  std::string detection_method;
  std::string detection_signal_source_level;
  std::string detection_signal_source_path;
  std::string movement_metric_source_level;
  std::string path_distance_source_level;
  std::string detector_trace_label;
  std::string detector_trace_units;
  double threshold_mm = std::numeric_limits<double>::quiet_NaN();
  double exponential_tau_s = std::numeric_limits<double>::quiet_NaN();
  double min_bout_duration_s = std::numeric_limits<double>::quiet_NaN();
  double min_gap_duration_s = std::numeric_limits<double>::quiet_NaN();
  double min_peak_prominence_mm_s = std::numeric_limits<double>::quiet_NaN();
  double peak_width_rel_height = std::numeric_limits<double>::quiet_NaN();
  size_t bout_count = 0;
  size_t detector_sample_count = 0;
  bool compact_layout = false;
  bool latest_run = false;
  bool default_level = false;
  bool has_detector_trace = false;
};

struct SwimBoutTimelineDescriptor {
  size_t frame_count = 0;
  size_t retained_interval_count = 0;
  uint64_t retained_interval_index_bytes = 0;
  uint64_t interval_index_budget_bytes = 0;
  std::string default_candidate;
  std::vector<SwimBoutCandidateDescriptor> candidates;
};

struct SwimBoutInterval {
  size_t source_index = 0;
  int64_t start_frame = -1;
  int64_t end_frame = -1;
  int64_t core_start_frame = -1;
  int64_t core_end_frame = -1;
  bool gap_censored = false;

  bool hasCore() const {
    return core_start_frame >= start_frame &&
           core_end_frame >= core_start_frame && core_end_frame <= end_frame;
  }
};

struct SwimBoutTimelineRequest {
  std::string candidate_key;
  int64_t first_frame = 0;
  int64_t last_frame = -1;
  int64_t anchor_frame = 0;
  size_t max_detector_points = 1200;
  double fallback_frames_per_second = 0.0;
  bool include_detector_trace = true;
};

enum class SwimBoutTimelineStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidRequest,
  ReadFailed,
};

struct SwimBoutTimelineWindow {
  SwimBoutTimelineStatus status = SwimBoutTimelineStatus::Missing;
  SwimBoutTimelineRequest request;
  size_t candidate_interval_count = 0;
  size_t source_detector_row_count = 0;
  std::vector<SwimBoutInterval> intervals;
  std::vector<int64_t> detector_frames;
  std::vector<double> detector_times_seconds;
  std::vector<double> detector_values;
  std::string error;

  bool ready() const {
    return status == SwimBoutTimelineStatus::Mapped &&
           (!intervals.empty() || !detector_values.empty());
  }
};

struct SwimBoutCandidateData {
  std::string candidate_key;
  std::vector<SwimBoutInterval> intervals;
  std::vector<int64_t> detector_frames;
  std::vector<double> detector_times_seconds;
  std::vector<double> detector_values;
};

struct SwimBoutTimelinePageBounds {
  int64_t first_frame = 0;
  int64_t last_frame = -1;

  bool valid() const { return first_frame >= 0 && last_frame >= first_frame; }
};

class SwimBoutTimelineRepository {
public:
  virtual ~SwimBoutTimelineRepository() = default;
  virtual const SwimBoutTimelineDescriptor &descriptor() const = 0;
  virtual SwimBoutTimelineWindow
  resolveWindow(const SwimBoutTimelineRequest &request) const = 0;
};

const SwimBoutCandidateDescriptor *
findSwimBoutCandidate(const SwimBoutTimelineDescriptor &descriptor,
                      const std::string &candidate_key);

bool swimBoutCandidateCompatible(
    const SwimBoutCandidateDescriptor &candidate,
    const AnalysisSeriesSourceDescriptor &motion_source);

std::vector<const SwimBoutCandidateDescriptor *> compatibleSwimBoutCandidates(
    const SwimBoutTimelineDescriptor &descriptor,
    const AnalysisSeriesSourceDescriptor &motion_source);

std::string
defaultSwimBoutCandidate(const SwimBoutTimelineDescriptor &descriptor,
                         const AnalysisSeriesSourceDescriptor &motion_source,
                         const std::string &preferred_candidate = {});

std::string
swimBoutCandidateLabel(const SwimBoutCandidateDescriptor &candidate);

std::string makeSwimBoutCandidateKey(const std::string &run_name,
                                     int32_t candidate_id, int32_t signal_id,
                                     const std::string &speed_level);

SwimBoutTimelinePageBounds
swimBoutTimelinePageBounds(int64_t frame, size_t frame_count,
                           size_t page_span_frames = 4096,
                           size_t page_step_frames = 2048);

SwimBoutTimelineWindow
buildSwimBoutTimelineWindow(const SwimBoutTimelineDescriptor &descriptor,
                            const SwimBoutTimelineRequest &request,
                            const std::vector<SwimBoutInterval> &intervals,
                            const std::vector<int64_t> &detector_frames,
                            const std::vector<double> &detector_times_seconds,
                            const std::vector<double> &detector_values);

std::unique_ptr<SwimBoutTimelineRepository>
MakeSwimBoutTimelineRepository(SwimBoutTimelineDescriptor descriptor,
                               std::vector<SwimBoutCandidateData> candidates);

} // namespace crimson::timeline
