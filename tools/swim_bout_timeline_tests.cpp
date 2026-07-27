#include "analysis_series_timeline.h"
#include "swim_bout_timeline.h"
#include "swim_bout_timeline_buffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
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

crimson::timeline::AnalysisSeriesSourceDescriptor
motionSource(std::string variant = "filtered",
             std::string run = "track_fixture", std::string track = "id_0") {
  crimson::timeline::AnalysisSeriesSourceDescriptor source;
  source.key = variant;
  source.run_name = std::move(run);
  source.track_id = std::move(track);
  source.variant = std::move(variant);
  return source;
}

crimson::timeline::SwimBoutCandidateDescriptor
candidate(std::string key, std::string level, bool latest, bool is_default) {
  crimson::timeline::SwimBoutCandidateDescriptor result;
  result.key = std::move(key);
  result.run_name = latest ? "latest_bouts" : "older_bouts";
  result.speed_level = std::move(level);
  result.layout = "compact_tabular_v2";
  result.source_track_kinematics_run = "track_fixture";
  result.track_id = 0;
  result.candidate_id = 0;
  result.signal_id = result.speed_level == "speed_exponential" ? 4 : 0;
  result.signal_role = "detector";
  result.detection_method = "peak_event";
  result.detection_signal_source_level =
      result.speed_level == "speed_exponential" ? "speed_filtered"
                                                : result.speed_level;
  result.detector_trace_label = "Detector response";
  result.detector_trace_units = "mm/s";
  result.bout_count = 3;
  result.detector_sample_count = 200;
  result.compact_layout = true;
  result.latest_run = latest;
  result.default_level = is_default;
  result.has_detector_trace = true;
  return result;
}

crimson::timeline::SwimBoutTimelineDescriptor descriptor() {
  crimson::timeline::SwimBoutTimelineDescriptor result;
  result.frame_count = 240;
  result.default_candidate = "latest-exp";
  result.candidates = {
      candidate("older-filtered", "speed_filtered", false, true),
      candidate("latest-raw", "speed_raw", true, false),
      candidate("latest-exp", "speed_exponential", true, true),
  };
  return result;
}

std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository> repository() {
  using crimson::timeline::SwimBoutCandidateData;
  using crimson::timeline::SwimBoutInterval;
  std::vector<int64_t> frames(200);
  std::vector<double> values(200);
  for (size_t index = 0; index < frames.size(); ++index) {
    frames[index] = static_cast<int64_t>(index);
    values[index] = std::sin(static_cast<double>(index) * 0.1);
  }
  values[80] = 50.0;
  std::vector<SwimBoutInterval> intervals = {
      {0, 20, 40, 25, 35, false},
      {1, 70, 90, 75, 85, false},
      {2, 150, 180, -1, -1, true},
  };
  std::vector<SwimBoutCandidateData> data;
  for (const auto &item : descriptor().candidates) {
    data.push_back({item.key, intervals, frames, {}, values});
  }
  return crimson::timeline::MakeSwimBoutTimelineRepository(descriptor(),
                                                           std::move(data));
}

struct BlockingRepositoryState {
  std::mutex mutex;
  std::condition_variable condition;
  bool first_request_entered = false;
  bool release_first_request = false;
};

class BlockingRepository final
    : public crimson::timeline::SwimBoutTimelineRepository {
public:
  BlockingRepository(
      std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository> delegate,
      std::shared_ptr<BlockingRepositoryState> state)
      : descriptor_(delegate->descriptor()), delegate_(std::move(delegate)),
        state_(std::move(state)) {}

  const crimson::timeline::SwimBoutTimelineDescriptor &
  descriptor() const override {
    return descriptor_;
  }

  crimson::timeline::SwimBoutTimelineWindow
  resolveWindow(const crimson::timeline::SwimBoutTimelineRequest &request)
      const override {
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      if (!state_->first_request_entered) {
        state_->first_request_entered = true;
        state_->condition.notify_all();
        state_->condition.wait(lock,
                               [&] { return state_->release_first_request; });
      }
    }
    return delegate_->resolveWindow(request);
  }

private:
  crimson::timeline::SwimBoutTimelineDescriptor descriptor_;
  std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository> delegate_;
  std::shared_ptr<BlockingRepositoryState> state_;
};

bool testCompatibilityAndDefaults() {
  using namespace crimson::timeline;
  const auto model = descriptor();
  const auto filtered = motionSource();
  const auto compatible = compatibleSwimBoutCandidates(model, filtered);
  CHECK(compatible.size() == 2);
  CHECK(compatible.front()->key == "older-filtered");
  CHECK(compatible.back()->key == "latest-exp");
  CHECK(defaultSwimBoutCandidate(model, filtered) == "latest-exp");
  CHECK(defaultSwimBoutCandidate(model, filtered, "older-filtered") ==
        "older-filtered");
  CHECK(compatibleSwimBoutCandidates(model, motionSource("filtered", "other"))
            .empty());
  CHECK(compatibleSwimBoutCandidates(
            model, motionSource("filtered", "track_fixture", "id_1"))
            .empty());
  auto path_candidate =
      candidate("path-filtered", "speed_exponential", true, false);
  path_candidate.detection_signal_source_level.clear();
  path_candidate.detection_signal_source_path =
      "Analysis/Track/Signals/SPEED_FILTERED/values";
  CHECK(swimBoutCandidateCompatible(path_candidate, filtered));
  CHECK(swimBoutCandidateLabel(*compatible.back()).find("3 bouts") !=
        std::string::npos);
  CHECK(makeSwimBoutCandidateKey("run", 2, 7, "speed_raw") ==
        "run/candidate_2/signal_7/speed_raw");
  return true;
}

bool testWindowIntervalsAndDetector() {
  using namespace crimson::timeline;
  auto data = repository();
  SwimBoutTimelineRequest request;
  request.candidate_key = "latest-exp";
  request.first_frame = 30;
  request.last_frame = 160;
  request.anchor_frame = 80;
  request.max_detector_points = 9;
  request.fallback_frames_per_second = 100.0;
  auto window = data->resolveWindow(request);
  CHECK(window.ready());
  CHECK(window.intervals.size() == 3);
  CHECK(window.intervals.front().start_frame == 20);
  CHECK(window.intervals.front().hasCore());
  CHECK(!window.intervals.back().hasCore());
  CHECK(window.source_detector_row_count == 131);
  CHECK(window.detector_values.size() >= 3);
  CHECK(window.detector_values.size() <= 9);
  CHECK(std::find(window.detector_values.begin(), window.detector_values.end(),
                  50.0) != window.detector_values.end());

  request.include_detector_trace = false;
  window = data->resolveWindow(request);
  CHECK(window.ready());
  CHECK(window.detector_values.empty());
  request.first_frame = 200;
  request.last_frame = 220;
  CHECK(data->resolveWindow(request).status == SwimBoutTimelineStatus::Missing);
  request.last_frame = 240;
  CHECK(data->resolveWindow(request).status ==
        SwimBoutTimelineStatus::OutOfRange);
  request.candidate_key = "missing";
  CHECK(data->resolveWindow(request).status ==
        SwimBoutTimelineStatus::InvalidRequest);

  auto broken_descriptor = descriptor();
  auto broken = MakeSwimBoutTimelineRepository(
      std::move(broken_descriptor),
      {{"latest-exp", {{0, 10, 20, 9, 15, false}}, {}, {}, {}}});
  request = {"latest-exp", 0, 30, 10, 20, 100.0, false};
  CHECK(broken->resolveWindow(request).status ==
        SwimBoutTimelineStatus::ReadFailed);
  return true;
}

bool testPageBoundsAndAsyncCache() {
  using crimson::timeline::swimBoutTimelinePageBounds;
  CHECK(swimBoutTimelinePageBounds(0, 10000, 100, 50).first_frame == 0);
  const auto middle = swimBoutTimelinePageBounds(100, 10000, 100, 50);
  CHECK(middle.first_frame == 75);
  CHECK(middle.last_frame == 174);
  CHECK(!swimBoutTimelinePageBounds(-1, 10000).valid());

  SwimBoutTimelineBuffer buffer;
  std::string error;
  CHECK(buffer.open(repository(), 80, 40, 15, 2, &error));
  for (int64_t frame : {20, 80, 160}) {
    CHECK(buffer.requestFrame(frame, "latest-exp", 100.0, true, frame == 20,
                              &error));
    CHECK(buffer.waitForFrame(frame, "latest-exp", 100.0, true,
                              std::chrono::seconds(2)));
    const auto window = buffer.window(frame, "latest-exp", true);
    CHECK(window != nullptr);
    CHECK(window->ready());
  }
  CHECK(buffer.requestFrame(160, "latest-exp", 100.0, false, true, &error));
  CHECK(buffer.waitForFrame(160, "latest-exp", 100.0, false,
                            std::chrono::seconds(2)));
  CHECK(buffer.window(160, "latest-exp", false)->detector_values.empty());
  const auto metrics = buffer.metrics();
  CHECK(metrics.resolved_windows == 4);
  CHECK(metrics.peak_cached_windows <= 2);
  CHECK(metrics.peak_pending_windows <= 1);
  CHECK(buffer.window(20, "latest-exp", true) == nullptr);
  CHECK(buffer.requestFrame(160, "latest-exp", 100.0, false, false, &error));
  CHECK(buffer.metrics().cache_hits >= 1);
  buffer.close();
  return true;
}

bool testStaleGenerationIsDiscarded() {
  auto state = std::make_shared<BlockingRepositoryState>();
  SwimBoutTimelineBuffer buffer;
  std::string error;
  CHECK(buffer.open(std::make_unique<BlockingRepository>(repository(), state),
                    80, 40, 15, 2, &error));
  CHECK(buffer.requestFrame(20, "latest-exp", 100.0, true, false, &error));
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    CHECK(state->condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return state->first_request_entered;
    }));
  }
  CHECK(buffer.requestFrame(160, "latest-exp", 100.0, true, true, &error));
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->release_first_request = true;
    state->condition.notify_all();
  }
  CHECK(buffer.waitForFrame(160, "latest-exp", 100.0, true,
                            std::chrono::seconds(2)));
  CHECK(buffer.window(20, "latest-exp", true) == nullptr);
  CHECK(buffer.window(160, "latest-exp", true)->ready());
  CHECK(buffer.metrics().discarded_results == 1);
  buffer.close();
  return true;
}

} // namespace

int main() {
  if (!testCompatibilityAndDefaults() || !testWindowIntervalsAndDetector() ||
      !testPageBoundsAndAsyncCache() || !testStaleGenerationIsDiscarded()) {
    return 1;
  }
  std::cout << "swim_bout_timeline_tests: PASS\n";
  return 0;
}
