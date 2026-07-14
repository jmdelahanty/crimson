#include "eye_angle_timeline.h"
#include "eye_angle_timeline_buffer.h"

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
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__         \
                << ": " #condition << '\n';                                  \
      return false;                                                            \
    }                                                                          \
  } while (false)

crimson::timeline::EyeAngleTimelineDescriptor makeDescriptor(
    size_t frame_count = 120) {
  using namespace crimson::timeline;
  EyeAngleTimelineDescriptor descriptor;
  descriptor.source_group = "analysis/eye_angle_runs";
  descriptor.run_name = "fixture";
  descriptor.schema_id = "analysis.eye_angle_runs";
  descriptor.schema_version = 5;
  descriptor.method = "fixture";
  descriptor.layout = "compact_dense_v2";
  descriptor.default_representation = "eye_frame";
  descriptor.frame_count = frame_count;
  EyeAngleTimelineRepresentation representation;
  representation.key = "eye_frame";
  representation.display_name = "Bianco/Engert eye-frame angles";
  representation.units = "deg";
  representation.fields = {
      {"left_eye_angle_deg_smoothed", "left_eye_angle_deg",
       "Left", "deg", EyeAngleTraceRole::Left, true},
      {"right_eye_angle_deg_smoothed", "right_eye_angle_deg_smoothed",
       "Right", "deg", EyeAngleTraceRole::Right, false},
      {"vergence_eye_angle_deg_smoothed", "vergence_eye_angle_deg",
       "Vergence", "deg", EyeAngleTraceRole::Vergence, true},
  };
  descriptor.representations.push_back(std::move(representation));
  return descriptor;
}

std::vector<double> values(size_t count, double offset) {
  std::vector<double> result(count);
  for (size_t frame = 0; frame < count; ++frame) {
    result[frame] = offset + std::sin(static_cast<double>(frame) * 0.1);
  }
  return result;
}

std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository>
makeRepository(size_t frame_count = 120) {
  using namespace crimson::timeline;
  std::vector<double> times(frame_count);
  for (size_t frame = 0; frame < frame_count; ++frame) {
    times[frame] = static_cast<double>(frame) * 0.011;
  }
  auto left = values(frame_count, 10.0);
  auto right = values(frame_count, -10.0);
  auto vergence = values(frame_count, 20.0);
  left[37] = 90.0;
  right[72] = -85.0;
  return MakeEyeAngleTimelineRepository(
      makeDescriptor(frame_count), std::move(times),
      {{"left_eye_angle_deg", std::move(left)},
       {"right_eye_angle_deg_smoothed", std::move(right)},
       {"vergence_eye_angle_deg", std::move(vergence)}});
}

bool containsValue(const crimson::timeline::EyeAngleTimelineTrace& trace,
                   double value) {
  return std::find(trace.values.begin(), trace.values.end(), value) !=
         trace.values.end();
}

bool testContractAndDecimation() {
  using namespace crimson::timeline;
  auto repository = makeRepository();
  CHECK(repository != nullptr);
  CHECK(defaultEyeAngleTimelineRepresentation(repository->descriptor()) ==
        "eye_frame");
  CHECK(eyeAngleTraceRoleForField("mean_eye_vergence_gaze_deg") ==
        EyeAngleTraceRole::Vergence);
  CHECK(eyeAngleTraceRoleForField("left_gaze_signed_deg") ==
        EyeAngleTraceRole::Left);

  EyeAngleTimelineRequest request;
  request.representation_key = "eye_frame";
  request.first_frame = 10;
  request.last_frame = 109;
  request.anchor_frame = 60;
  request.max_points_per_trace = 11;
  request.fallback_frames_per_second = 100.0;
  const auto window = repository->resolveWindow(request);
  CHECK(window.ready());
  CHECK(window.source_row_count == 100);
  CHECK(window.traces.size() == 3);
  for (const auto& trace : window.traces) {
    CHECK(trace.values.size() >= 3);
    CHECK(trace.values.size() <= 11);
    CHECK(trace.frames.size() == trace.values.size());
    CHECK(trace.times_seconds.size() == trace.values.size());
  }
  CHECK(containsValue(window.traces[0], 90.0));
  CHECK(containsValue(window.traces[1], -85.0));
  CHECK(window.traces[0].field.fallback);
  CHECK(std::fabs(eyeAngleTimelineTimeForFrame(window, 44, 100.0) -
                  44.0 * 0.011) < 1e-9);
  CHECK(eyeAngleTimelineNearestFrame(window, 44.0 * 0.011, 100.0) == 44);

  request.representation_key = "unavailable";
  CHECK(repository->resolveWindow(request).status ==
        EyeAngleTimelineStatus::InvalidRequest);
  request.representation_key = "eye_frame";
  request.last_frame = 120;
  CHECK(repository->resolveWindow(request).status ==
        EyeAngleTimelineStatus::OutOfRange);
  return true;
}

bool testPageBounds() {
  using crimson::timeline::eyeAngleTimelinePageBounds;
  const auto first = eyeAngleTimelinePageBounds(0, 10000, 100, 50);
  CHECK(first.first_frame == 0);
  CHECK(first.last_frame == 99);
  const auto middle = eyeAngleTimelinePageBounds(100, 10000, 100, 50);
  CHECK(middle.first_frame == 75);
  CHECK(middle.last_frame == 174);
  const auto final = eyeAngleTimelinePageBounds(9999, 10000, 100, 50);
  CHECK(final.first_frame == 9900);
  CHECK(final.last_frame == 9999);
  CHECK(!eyeAngleTimelinePageBounds(-1, 10000).valid());
  return true;
}

bool testMissingValues() {
  using namespace crimson::timeline;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto repository = MakeEyeAngleTimelineRepository(
      makeDescriptor(8), {},
      {{"left_eye_angle_deg", std::vector<double>(8, nan)},
       {"right_eye_angle_deg_smoothed", std::vector<double>(8, nan)},
       {"vergence_eye_angle_deg", std::vector<double>(8, nan)}});
  EyeAngleTimelineRequest request{"eye_frame", 0, 7, 3, 8, 100.0};
  const auto window = repository->resolveWindow(request);
  CHECK(window.status == EyeAngleTimelineStatus::Missing);
  CHECK(window.traces.empty());
  return true;
}

bool testBoundedAsyncBuffer() {
  EyeAngleTimelineBuffer buffer;
  std::string error;
  CHECK(buffer.open(makeRepository(), 40, 20, 9, 2, &error));
  for (int64_t frame : {5, 45, 85}) {
    CHECK(buffer.requestFrame(frame, "eye_frame", 100.0, frame == 5, &error));
    CHECK(buffer.waitForFrame(frame, "eye_frame", 100.0,
                              std::chrono::seconds(2)));
    const auto window = buffer.window(frame, "eye_frame");
    CHECK(window != nullptr);
    CHECK(window->ready());
  }
  const auto metrics = buffer.metrics();
  CHECK(metrics.resolved_windows == 3);
  CHECK(metrics.peak_cached_windows <= 2);
  CHECK(metrics.peak_pending_windows <= 1);
  CHECK(metrics.source_rows_read == 120);
  CHECK(buffer.window(5, "eye_frame") == nullptr);
  CHECK(buffer.requestFrame(85, "eye_frame", 100.0, false, &error));
  CHECK(buffer.metrics().cache_hits >= 1);
  buffer.close();
  return true;
}

}  // namespace

int main() {
  if (!testContractAndDecimation() || !testPageBounds() ||
      !testMissingValues() || !testBoundedAsyncBuffer()) {
    return 1;
  }
  std::cout << "eye_angle_timeline_tests: PASS\n";
  return 0;
}
