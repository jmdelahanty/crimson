#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::timeline {

enum class AnalysisSeriesKind : uint8_t {
  Motion,
  TailKinematics,
};

enum class AnalysisSeriesTraceRole : uint8_t {
  Other,
  PrimarySpeed,
  SecondarySpeed,
  HeadingRaw,
  HeadingSmoothed,
  PositionX,
  PositionY,
  TailTipAngle,
  MaxAbsTailAngle,
  TailTipLateralDeflection,
  MaxAbsTailCurvature,
};

struct AnalysisSeriesTraceDescriptor {
  std::string key;
  std::string display_name;
  std::string units;
  std::string row_key;
  AnalysisSeriesTraceRole role = AnalysisSeriesTraceRole::Other;
  bool default_visible = false;
};

struct AnalysisSeriesSourceDescriptor {
  std::string key;
  std::string display_name;
  std::string source_group;
  std::string run_name;
  std::string category;
  std::string track_id;
  std::string variant;
  size_t sample_count = 0;
  std::vector<AnalysisSeriesTraceDescriptor> traces;
};

struct AnalysisSeriesTimelineDescriptor {
  AnalysisSeriesKind kind = AnalysisSeriesKind::Motion;
  std::string title;
  std::string default_source;
  size_t frame_count = 0;
  std::vector<AnalysisSeriesSourceDescriptor> sources;
};

struct AnalysisSeriesTimelineRequest {
  std::string source_key;
  int64_t first_frame = 0;
  int64_t last_frame = -1;
  int64_t anchor_frame = 0;
  size_t max_points_per_trace = 1200;
  double fallback_frames_per_second = 0.0;
};

enum class AnalysisSeriesTimelineStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidRequest,
  ReadFailed,
};

struct AnalysisSeriesTimelineTrace {
  AnalysisSeriesTraceDescriptor descriptor;
  std::vector<int64_t> frames;
  std::vector<double> times_seconds;
  std::vector<double> values;
};

struct AnalysisSeriesTimelineWindow {
  AnalysisSeriesTimelineStatus status = AnalysisSeriesTimelineStatus::Missing;
  AnalysisSeriesTimelineRequest request;
  size_t source_row_count = 0;
  size_t published_point_count = 0;
  std::vector<int64_t> mapping_frames;
  std::vector<double> mapping_times_seconds;
  std::vector<AnalysisSeriesTimelineTrace> traces;
  std::string error;

  bool ready() const {
    return status == AnalysisSeriesTimelineStatus::Mapped && !traces.empty();
  }
};

struct AnalysisSeriesTimelineFieldSeries {
  std::string key;
  std::vector<double> values;
};

struct AnalysisSeriesTimelineSourceData {
  std::string source_key;
  std::vector<int64_t> frames;
  std::vector<double> times_seconds;
  std::vector<AnalysisSeriesTimelineFieldSeries> fields;
};

struct AnalysisSeriesTimelinePageBounds {
  int64_t first_frame = 0;
  int64_t last_frame = -1;

  bool valid() const { return first_frame >= 0 && last_frame >= first_frame; }
};

struct AnalysisSeriesTimelineRepositoryMetrics {
  uint64_t frame_index_block_reads = 0;
  uint64_t frame_index_cache_hits = 0;
  uint64_t frame_index_cache_evictions = 0;
  uint64_t frame_index_source_bytes = 0;
  uint64_t cached_frame_index_bytes = 0;
  uint64_t peak_cached_frame_index_bytes = 0;
  double maximum_frame_index_read_ms = 0.0;
  uint64_t preload_candidate_bytes = 0;
  uint64_t preloaded_retained_bytes = 0;
  uint64_t preloaded_window_resolves = 0;
  uint64_t paged_window_resolves = 0;
  double preload_ms = 0.0;
  bool default_source_preloaded = false;
};

class AnalysisSeriesTimelineRepository {
public:
  virtual ~AnalysisSeriesTimelineRepository() = default;
  virtual const AnalysisSeriesTimelineDescriptor &descriptor() const = 0;
  virtual AnalysisSeriesTimelineWindow
  resolveWindow(const AnalysisSeriesTimelineRequest &request) const = 0;
  virtual AnalysisSeriesTimelineRepositoryMetrics metrics() const {
    return {};
  }
};

const AnalysisSeriesSourceDescriptor *
findAnalysisSeriesSource(const AnalysisSeriesTimelineDescriptor &descriptor,
                         const std::string &source_key);

std::string
defaultAnalysisSeriesSource(const AnalysisSeriesTimelineDescriptor &descriptor);

AnalysisSeriesTimelinePageBounds
analysisSeriesTimelinePageBounds(int64_t frame, size_t frame_count,
                                 size_t page_span_frames = 4096,
                                 size_t page_step_frames = 2048);

double
analysisSeriesTimelineTimeForFrame(const AnalysisSeriesTimelineWindow &window,
                                   int64_t frame,
                                   double fallback_frames_per_second);

int64_t
analysisSeriesTimelineNearestFrame(const AnalysisSeriesTimelineWindow &window,
                                   double time_seconds,
                                   double fallback_frames_per_second);

AnalysisSeriesTimelineWindow buildAnalysisSeriesTimelineWindow(
    const AnalysisSeriesTimelineDescriptor &descriptor,
    const AnalysisSeriesTimelineRequest &request,
    const std::vector<int64_t> &frames,
    const std::vector<double> &times_seconds,
    const std::vector<AnalysisSeriesTimelineFieldSeries> &fields);

std::unique_ptr<AnalysisSeriesTimelineRepository>
MakeAnalysisSeriesTimelineRepository(
    AnalysisSeriesTimelineDescriptor descriptor,
    std::vector<AnalysisSeriesTimelineSourceData> sources);

} // namespace crimson::timeline
