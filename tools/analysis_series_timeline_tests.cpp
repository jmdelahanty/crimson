#include "analysis_series_timeline.h"
#include "analysis_series_timeline_buffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

crimson::timeline::AnalysisSeriesTimelineDescriptor
makeDescriptor(size_t frame_count = 240) {
  using namespace crimson::timeline;
  AnalysisSeriesTimelineDescriptor descriptor;
  descriptor.kind = AnalysisSeriesKind::Motion;
  descriptor.title = "Motion";
  descriptor.default_source = "filtered";
  descriptor.frame_count = frame_count;
  descriptor.sources = {
      {"filtered",
       "Filtered (track 0)",
       "analysis/track_kinematics_runs/offline",
       "fixture",
       "track_kinematics/offline",
       "id_0",
       "filtered",
       120,
       {{"speed_filtered_mm", "Filtered Speed", "mm/s", "speed",
         AnalysisSeriesTraceRole::PrimarySpeed, true},
        {"speed_raw_mm", "Raw Speed", "mm/s", "speed",
         AnalysisSeriesTraceRole::SecondarySpeed, false},
        {"heading_smoothed_degrees", "Smoothed Heading", "deg", "heading",
         AnalysisSeriesTraceRole::HeadingSmoothed, true}}},
      {"raw",
       "Raw (track 0)",
       "analysis/track_kinematics_runs/offline",
       "fixture",
       "track_kinematics/offline",
       "id_0",
       "raw",
       120,
       {{"speed_raw_mm", "Raw Speed", "mm/s", "speed",
         AnalysisSeriesTraceRole::PrimarySpeed, true}}},
  };
  return descriptor;
}

std::vector<int64_t> sparseFrames(size_t count) {
  std::vector<int64_t> frames;
  frames.reserve(count);
  for (size_t row = 0; row < count; ++row) {
    const int64_t frame = static_cast<int64_t>(row * 2);
    if (frame != 74 && frame != 76) {
      frames.push_back(frame);
    }
  }
  return frames;
}

std::vector<double> seriesValues(const std::vector<int64_t> &frames,
                                 double offset) {
  std::vector<double> result(frames.size());
  for (size_t row = 0; row < frames.size(); ++row) {
    result[row] = offset + std::sin(static_cast<double>(frames[row]) * 0.1);
  }
  return result;
}

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
makeRepository(size_t frame_count = 240) {
  using namespace crimson::timeline;
  const auto frames = sparseFrames(120);
  std::vector<double> times(frames.size());
  for (size_t row = 0; row < frames.size(); ++row) {
    times[row] = static_cast<double>(frames[row]) * 0.0125;
  }
  auto filtered = seriesValues(frames, 5.0);
  auto raw = seriesValues(frames, 7.0);
  auto heading = seriesValues(frames, 20.0);
  const auto spike = std::find(frames.begin(), frames.end(), 120);
  filtered[static_cast<size_t>(spike - frames.begin())] = 95.0;
  return MakeAnalysisSeriesTimelineRepository(
      makeDescriptor(frame_count),
      {{"filtered",
        frames,
        times,
        {{"speed_filtered_mm", filtered},
         {"speed_raw_mm", raw},
         {"heading_smoothed_degrees", heading}}},
       {"raw", frames, times, {{"speed_raw_mm", raw}}}});
}

bool containsValue(const crimson::timeline::AnalysisSeriesTimelineTrace &trace,
                   double value) {
  return std::find(trace.values.begin(), trace.values.end(), value) !=
         trace.values.end();
}

bool testSparseContractAndDecimation() {
  using namespace crimson::timeline;
  auto repository = makeRepository();
  CHECK(repository != nullptr);
  CHECK(defaultAnalysisSeriesSource(repository->descriptor()) == "filtered");
  CHECK(findAnalysisSeriesSource(repository->descriptor(), "raw") != nullptr);

  AnalysisSeriesTimelineRequest request;
  request.source_key = "filtered";
  request.first_frame = 40;
  request.last_frame = 180;
  request.anchor_frame = 100;
  request.max_points_per_trace = 11;
  request.fallback_frames_per_second = 80.0;
  const auto window = repository->resolveWindow(request);
  CHECK(window.ready());
  CHECK(window.traces.size() == 3);
  CHECK(window.source_row_count == 69);
  CHECK(window.mapping_frames.size() == window.source_row_count);
  CHECK(window.mapping_times_seconds.size() == window.source_row_count);
  for (const auto &trace : window.traces) {
    CHECK(trace.values.size() >= 3);
    CHECK(trace.values.size() <= 11);
    CHECK(trace.frames.size() == trace.values.size());
    CHECK(trace.times_seconds.size() == trace.values.size());
    CHECK(trace.frames.front() >= 40);
    CHECK(trace.frames.back() <= 180);
  }
  CHECK(containsValue(window.traces.front(), 95.0));
  CHECK(std::fabs(analysisSeriesTimelineTimeForFrame(window, 100, 80.0) -
                  1.25) < 1e-9);
  CHECK(analysisSeriesTimelineNearestFrame(window, 1.25, 80.0) == 100);
  CHECK(std::fabs(analysisSeriesTimelineTimeForFrame(window, 75, 80.0) -
                  0.9375) < 1e-9);
  CHECK(analysisSeriesTimelineNearestFrame(window, 0.9375, 80.0) == 75);

  request.source_key = "missing";
  CHECK(repository->resolveWindow(request).status ==
        AnalysisSeriesTimelineStatus::InvalidRequest);
  request.source_key = "filtered";
  request.last_frame = 240;
  CHECK(repository->resolveWindow(request).status ==
        AnalysisSeriesTimelineStatus::OutOfRange);
  return true;
}

bool testMissingAndFallbackTime() {
  using namespace crimson::timeline;
  auto descriptor = makeDescriptor(20);
  descriptor.sources.resize(1);
  descriptor.sources.front().sample_count = 3;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto repository = MakeAnalysisSeriesTimelineRepository(
      std::move(descriptor),
      {{"filtered",
        {2, 8, 14},
        {nan, nan, nan},
        {{"speed_filtered_mm", {nan, nan, nan}},
         {"speed_raw_mm", {nan, nan, nan}},
         {"heading_smoothed_degrees", {nan, nan, nan}}}}});
  AnalysisSeriesTimelineRequest request{"filtered", 0, 19, 8, 10, 50.0};
  const auto window = repository->resolveWindow(request);
  CHECK(window.status == AnalysisSeriesTimelineStatus::Missing);
  CHECK(window.source_row_count == 3);

  repository = MakeAnalysisSeriesTimelineRepository(
      makeDescriptor(20), {{"filtered",
                            {2, 8, 14},
                            {nan, nan, nan},
                            {{"speed_filtered_mm", {1.0, 2.0, 3.0}},
                             {"speed_raw_mm", {2.0, 3.0, 4.0}},
                             {"heading_smoothed_degrees", {3.0, 4.0, 5.0}}}}});
  const auto fallback = repository->resolveWindow(request);
  CHECK(fallback.ready());
  CHECK(std::fabs(fallback.traces.front().times_seconds[1] - 0.16) < 1e-9);
  return true;
}

bool testPageBoundsAndAsyncCache() {
  using crimson::timeline::analysisSeriesTimelinePageBounds;
  CHECK(analysisSeriesTimelinePageBounds(0, 10000, 100, 50).first_frame == 0);
  const auto middle = analysisSeriesTimelinePageBounds(100, 10000, 100, 50);
  CHECK(middle.first_frame == 75);
  CHECK(middle.last_frame == 174);
  CHECK(!analysisSeriesTimelinePageBounds(-1, 10000).valid());

  AnalysisSeriesTimelineBuffer buffer;
  std::string error;
  CHECK(buffer.open(makeRepository(), 80, 40, 15, 2, &error));
  for (int64_t frame : {10, 90, 170}) {
    CHECK(buffer.requestFrame(frame, "filtered", 80.0, frame == 10, &error));
    CHECK(
        buffer.waitForFrame(frame, "filtered", 80.0, std::chrono::seconds(2)));
    const auto window = buffer.window(frame, "filtered");
    CHECK(window != nullptr);
    CHECK(window->ready());
  }
  CHECK(buffer.requestFrame(170, "raw", 80.0, true, &error));
  CHECK(buffer.waitForFrame(170, "raw", 80.0, std::chrono::seconds(2)));
  CHECK(buffer.window(170, "raw")->traces.size() == 1);
  const auto metrics = buffer.metrics();
  CHECK(metrics.resolved_windows == 4);
  CHECK(metrics.peak_cached_windows <= 2);
  CHECK(metrics.peak_pending_windows <= 1);
  CHECK(buffer.window(10, "filtered") == nullptr);
  CHECK(buffer.requestFrame(170, "raw", 80.0, false, &error));
  CHECK(buffer.metrics().cache_hits >= 1);
  buffer.close();
  return true;
}

} // namespace

int main() {
  if (!testSparseContractAndDecimation() || !testMissingAndFallbackTime() ||
      !testPageBoundsAndAsyncCache()) {
    return 1;
  }
  std::cout << "analysis_series_timeline_tests: PASS\n";
  return 0;
}
