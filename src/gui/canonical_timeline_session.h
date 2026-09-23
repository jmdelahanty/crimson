#pragma once

#include "analysis_series_timeline.h"
#include "analysis_series_timeline_buffer.h"
#include "data_access_scheduler.h"
#include "eye_angle_timeline.h"
#include "eye_angle_timeline_buffer.h"
#include "swim_bout_timeline.h"
#include "swim_bout_timeline_buffer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crimson::gui {

enum class CanonicalTimelineSessionState : uint8_t {
  Closed,
  Opening,
  Ready,
  Failed,
};

enum class CanonicalTimelineProductState : uint8_t {
  Unavailable,
  Opening,
  Pending,
  Ready,
  Empty,
  Error,
};

struct CanonicalTimelineOpenRequest {
  std::string archive_path;
  size_t expected_frame_count = 0;
  double frames_per_second = 0.0;
  std::string eye_angle_run;
  std::string motion_scope = "offline";
  std::string motion_run;
  int64_t motion_track_id = -1;
  std::string swim_bout_run;
  size_t page_span_frames = 4096;
  size_t page_step_frames = 2048;
  size_t max_points_per_trace = 1200;
  size_t cache_pages = 3;
};

template <typename Descriptor, typename Window>
struct CanonicalTimelineProductSnapshot {
  CanonicalTimelineProductState state =
      CanonicalTimelineProductState::Unavailable;
  Descriptor descriptor;
  std::shared_ptr<const Window> window;
  std::string source_identity;
  std::string error;
};

struct CanonicalTimelineSnapshot {
  uint64_t generation = 0;
  int64_t requested_frame = -1;
  CanonicalTimelineSessionState state = CanonicalTimelineSessionState::Closed;
  CanonicalTimelineProductSnapshot<timeline::EyeAngleTimelineDescriptor,
                                   timeline::EyeAngleTimelineWindow>
      eye_angles;
  CanonicalTimelineProductSnapshot<timeline::AnalysisSeriesTimelineDescriptor,
                                   timeline::AnalysisSeriesTimelineWindow>
      motion;
  CanonicalTimelineProductSnapshot<timeline::SwimBoutTimelineDescriptor,
                                   timeline::SwimBoutTimelineWindow>
      swim_bouts;
};

struct CanonicalTimelineSessionMetrics {
  CanonicalTimelineSessionState state = CanonicalTimelineSessionState::Closed;
  uint64_t generation = 0;
  uint64_t open_attempts = 0;
  uint64_t successful_opens = 0;
  uint64_t failed_opens = 0;
  uint64_t cancelled_opens = 0;
  uint64_t frame_requests = 0;
  double last_open_ms = 0.0;
  std::string archive_path;
  std::string eye_angle_run;
  std::string motion_run;
  std::string swim_bout_run;
  std::string last_error;
  std::string eye_angle_error;
  std::string motion_error;
  std::string swim_bout_error;
  EyeAngleTimelineBufferMetrics eye_angles;
  AnalysisSeriesTimelineBufferMetrics motion;
  SwimBoutTimelineBufferMetrics swim_bouts;
};

class CanonicalTimelineSession {
 public:
  explicit CanonicalTimelineSession(
      std::shared_ptr<data::DataAccessScheduler> scheduler);
  ~CanonicalTimelineSession();

  CanonicalTimelineSession(const CanonicalTimelineSession&) = delete;
  CanonicalTimelineSession& operator=(const CanonicalTimelineSession&) = delete;

  bool beginOpen(CanonicalTimelineOpenRequest request,
                 std::string* error = nullptr);
  void close();
  CanonicalTimelineSessionState state() const;
  bool waitUntilOpen(std::chrono::milliseconds timeout) const;

  bool requestFrame(int64_t frame, bool discontinuity = false,
                    std::string* error = nullptr);
  CanonicalTimelineSnapshot snapshot(int64_t frame) const;
  CanonicalTimelineSessionMetrics metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* canonicalTimelineSessionStateName(CanonicalTimelineSessionState);
const char* canonicalTimelineProductStateName(CanonicalTimelineProductState);

}  // namespace crimson::gui
