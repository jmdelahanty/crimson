#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::timeline {

enum class EyeAngleTraceRole : uint8_t {
  Other,
  Left,
  Right,
  Vergence,
};

struct EyeAngleTimelineFieldDescriptor {
  std::string requested_name;
  std::string source_name;
  std::string display_name;
  std::string units = "deg";
  EyeAngleTraceRole role = EyeAngleTraceRole::Other;
  bool fallback = false;
};

struct EyeAngleTimelineRepresentation {
  std::string key;
  std::string display_name;
  std::string role;
  std::string coordinate_frame;
  std::string units = "deg";
  std::vector<EyeAngleTimelineFieldDescriptor> fields;
};

struct EyeAngleTimelineDescriptor {
  std::string source_group;
  std::string run_name;
  std::string schema_id;
  int schema_version = 0;
  std::string method;
  std::string method_version;
  std::string layout;
  std::string default_representation;
  size_t roi_row_count = 0;
  size_t frame_count = 0;
  std::vector<EyeAngleTimelineRepresentation> representations;
};

struct EyeAngleTimelineRequest {
  std::string representation_key;
  int64_t first_frame = 0;
  int64_t last_frame = -1;
  int64_t anchor_frame = 0;
  size_t max_points_per_trace = 1200;
  double fallback_frames_per_second = 0.0;
};

enum class EyeAngleTimelineStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidRequest,
  ReadFailed,
};

struct EyeAngleTimelineTrace {
  EyeAngleTimelineFieldDescriptor field;
  std::vector<int64_t> frames;
  std::vector<double> times_seconds;
  std::vector<double> values;
};

struct EyeAngleTimelineWindow {
  EyeAngleTimelineStatus status = EyeAngleTimelineStatus::Missing;
  EyeAngleTimelineRequest request;
  size_t source_row_count = 0;
  size_t published_point_count = 0;
  std::vector<EyeAngleTimelineTrace> traces;
  std::string error;

  bool ready() const {
    return status == EyeAngleTimelineStatus::Mapped && !traces.empty();
  }

  bool coversFrame(int64_t frame,
                   const std::string& representation_key = {}) const {
    return ready() && frame >= request.first_frame &&
           frame <= request.last_frame &&
           (representation_key.empty() ||
            request.representation_key == representation_key);
  }
};

struct EyeAngleTimelineFieldSeries {
  std::string source_name;
  std::vector<double> frame_values;
};

struct EyeAngleTimelinePageBounds {
  int64_t first_frame = 0;
  int64_t last_frame = -1;

  bool valid() const { return first_frame >= 0 && last_frame >= first_frame; }
};

struct EyeAngleTimelineRepositoryMetrics {
  uint64_t preload_candidate_bytes = 0;
  uint64_t preloaded_retained_bytes = 0;
  uint64_t preloaded_window_resolves = 0;
  uint64_t paged_window_resolves = 0;
  double preload_ms = 0.0;
  bool frame_series_preloaded = false;
};

class EyeAngleTimelineRepository {
 public:
  virtual ~EyeAngleTimelineRepository() = default;
  virtual const EyeAngleTimelineDescriptor& descriptor() const = 0;
  virtual EyeAngleTimelineWindow resolveWindow(
      const EyeAngleTimelineRequest& request) const = 0;
  virtual EyeAngleTimelineRepositoryMetrics metrics() const { return {}; }
};

EyeAngleTraceRole eyeAngleTraceRoleForField(const std::string& field_name);

const EyeAngleTimelineRepresentation* findEyeAngleTimelineRepresentation(
    const EyeAngleTimelineDescriptor& descriptor, const std::string& key);

std::string defaultEyeAngleTimelineRepresentation(
    const EyeAngleTimelineDescriptor& descriptor);

EyeAngleTimelinePageBounds eyeAngleTimelinePageBounds(
    int64_t frame, size_t frame_count, size_t page_span_frames = 4096,
    size_t page_step_frames = 2048);

double eyeAngleTimelineTimeForFrame(const EyeAngleTimelineWindow& window,
                                    int64_t frame,
                                    double fallback_frames_per_second);

int64_t eyeAngleTimelineNearestFrame(const EyeAngleTimelineWindow& window,
                                     double time_seconds,
                                     double fallback_frames_per_second);

EyeAngleTimelineWindow buildEyeAngleTimelineWindow(
    const EyeAngleTimelineDescriptor& descriptor,
    const EyeAngleTimelineRequest& request, int64_t series_first_frame,
    const std::vector<double>& frame_times,
    const std::vector<EyeAngleTimelineFieldSeries>& fields);

std::unique_ptr<EyeAngleTimelineRepository> MakeEyeAngleTimelineRepository(
    EyeAngleTimelineDescriptor descriptor, std::vector<double> frame_times,
    std::vector<EyeAngleTimelineFieldSeries> fields);

}  // namespace crimson::timeline
