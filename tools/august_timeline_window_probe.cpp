#include "gui/canonical_timeline_session.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include "timeline_raw_verifier.h"

namespace {
using json = nlohmann::json;
using namespace crimson::gui;
using namespace std::chrono_literals;
void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
template<class Product> json product(const Product& value) {
  return {{"state", canonicalTimelineProductStateName(value.state)},
          {"source", value.source_identity}, {"error", value.error}};
}
template<class Trace> json samples(const Trace& trace) {
  size_t finite = 0;
  for (double v : trace.values) finite += std::isfinite(v);
  return {{"frames", trace.frames}, {"times_seconds", trace.times_seconds},
          {"values", trace.values}, {"finite_count", finite}};
}
template<class Trace> void validateTrace(const Trace& trace, int64_t first, int64_t last) {
  require(trace.frames.size() == trace.values.size() &&
          trace.frames.size() == trace.times_seconds.size(), "Trace column sizes differ");
  require(trace.frames.size() <= 256, "Trace exceeds point bound");
  int64_t previous = -1;
  for (int64_t frame : trace.frames) {
    require(frame >= first && frame <= last && frame > previous,
            "Trace loses explicit, ordered camera-frame identity");
    previous = frame;
  }
}
json snapshotJson(const CanonicalTimelineSnapshot& snapshot) {
  json result = {{"frame", snapshot.requested_frame},
                 {"generation", snapshot.generation},
                 {"eye_angles", product(snapshot.eye_angles)},
                 {"motion", product(snapshot.motion)},
                 {"swim_bouts", product(snapshot.swim_bouts)}};
  require(snapshot.eye_angles.state == CanonicalTimelineProductState::Ready ||
          snapshot.eye_angles.state == CanonicalTimelineProductState::Empty,
          "Eye-angle window failed: " + snapshot.eye_angles.error);
  require(snapshot.motion.state == CanonicalTimelineProductState::Ready ||
          snapshot.motion.state == CanonicalTimelineProductState::Empty,
          "Motion window failed: " + snapshot.motion.error);
  require(snapshot.swim_bouts.state == CanonicalTimelineProductState::Ready ||
          snapshot.swim_bouts.state == CanonicalTimelineProductState::Empty,
          "Swim-bout window failed: " + snapshot.swim_bouts.error);
  const auto& eyes = *snapshot.eye_angles.window;
  result["eye_angles"]["run"] = snapshot.eye_angles.descriptor.run_name;
  result["eye_angles"]["window"] = {eyes.request.first_frame, eyes.request.last_frame};
  result["eye_angles"]["rows_read"] = eyes.source_row_count;
  result["eye_angles"]["traces"] = json::array();
  for (const auto& trace : eyes.traces) {
    validateTrace(trace, eyes.request.first_frame, eyes.request.last_frame);
    auto item = samples(trace);
    item["field"] = trace.field.source_name;
    item["units"] = trace.field.units;
    result["eye_angles"]["traces"].push_back(std::move(item));
  }
  const auto& motion = *snapshot.motion.window;
  result["motion"]["window"] = {motion.request.first_frame, motion.request.last_frame};
  result["motion"]["rows_read"] = motion.source_row_count;
  result["motion"]["traces"] = json::array();
  for (const auto& trace : motion.traces) {
    validateTrace(trace, motion.request.first_frame, motion.request.last_frame);
    auto item = samples(trace);
    item["field"] = trace.descriptor.key;
    item["units"] = trace.descriptor.units;
    result["motion"]["traces"].push_back(std::move(item));
  }
  const auto& bouts = *snapshot.swim_bouts.window;
  result["swim_bouts"]["window"] = {bouts.request.first_frame, bouts.request.last_frame};
  result["swim_bouts"]["detector_rows_read"] = bouts.source_detector_row_count;
  result["swim_bouts"]["detector_frames"] = bouts.detector_frames;
  result["swim_bouts"]["detector_values"] = bouts.detector_values;
  result["swim_bouts"]["intervals"] = json::array();
  const auto* candidate = crimson::timeline::findSwimBoutCandidate(
      snapshot.swim_bouts.descriptor, snapshot.swim_bouts.descriptor.default_candidate);
  require(candidate != nullptr, "Selected bout candidate disappeared");
  result["swim_bouts"]["run"] = candidate->run_name;
  result["swim_bouts"]["signal_id"] = candidate->signal_id;
  result["swim_bouts"]["candidate_id"] = candidate->candidate_id;
  result["swim_bouts"]["retained_interval_index_bytes"] =
      snapshot.swim_bouts.descriptor.retained_interval_index_bytes;
  result["swim_bouts"]["interval_index_budget_bytes"] =
      snapshot.swim_bouts.descriptor.interval_index_budget_bytes;
  require(bouts.detector_frames.size() == bouts.detector_values.size() &&
          bouts.detector_frames.size() <= 256, "Detector window violates point bound");
  for (const auto& interval : bouts.intervals) {
    require(interval.start_frame <= bouts.request.last_frame &&
            interval.end_frame >= bouts.request.first_frame, "Unrelated bout in window");
    result["swim_bouts"]["intervals"].push_back(
        {{"source_index", interval.source_index}, {"start", interval.start_frame},
         {"end", interval.end_frame}, {"core_start", interval.core_start_frame},
         {"core_end", interval.core_end_frame}, {"gap_censored", interval.gap_censored}});
  }
  require(eyes.source_row_count <= 256 && motion.source_row_count <= 256 &&
          bouts.source_detector_row_count <= 256, "Payload read exceeded bounded page");
  return result;
}
} // namespace

int main(int argc, char** argv) {
  json report;
  try {
    require(argc >= 2 && argc <= 10,
            "Usage: august_timeline_window_probe ARCHIVE [FRAME ...] (at most eight frames)");
    auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(32, 4, 1, 1);
    CanonicalTimelineSession session(scheduler);
    CanonicalTimelineOpenRequest request;
    request.archive_path = argv[1];
    request.expected_frame_count = 2937604;
    request.frames_per_second = 30.0;
    request.page_span_frames = 256;
    request.page_step_frames = 128;
    request.max_points_per_trace = 256;
    request.cache_pages = 2;
    report["archive"] = request.archive_path;
    std::string error;
    require(session.beginOpen(request, &error), error);
    require(session.waitUntilOpen(90s), "Timeline open timed out");
    const auto opened = session.metrics();
    report["open"] = {{"state", canonicalTimelineSessionStateName(opened.state)},
                      {"milliseconds", opened.last_open_ms}, {"error", opened.last_error},
                      {"eye_error", opened.eye_angle_error}, {"motion_error", opened.motion_error},
                      {"bout_error", opened.swim_bout_error}};
    require(opened.state == CanonicalTimelineSessionState::Ready, "Timeline session did not open");
    std::vector<int64_t> frames;
    for (int i = 2; i < argc; ++i) frames.push_back(std::stoll(argv[i]));
    if (frames.empty()) frames = {54000, 2592030, 2937603};
    report["windows"] = json::array();
    RawTimelineVerifier raw(request.archive_path);
    for (int64_t frame : frames) {
      require(session.requestFrame(frame, true, &error), error);
      scheduler->waitUntilIdle(); // Caller uses an external deadline for blocked storage.
      const auto snapshot = session.snapshot(frame);
      report["windows"].push_back(snapshotJson(snapshot));
      report["direct_raw_comparisons"] = raw.verify(report["windows"].back());
      const auto submissions = scheduler->metrics().queue.submissions;
      for (int repeat = 0; repeat < 20; ++repeat) session.snapshot(frame);
      require(scheduler->metrics().queue.submissions == submissions,
              "Snapshot performed hidden storage scheduling");
    }
    const auto metrics = session.metrics();
    report["metrics"] = {{"eye_windows", metrics.eye_angles.resolved_windows},
                          {"motion_windows", metrics.motion.resolved_windows},
                          {"bout_windows", metrics.swim_bouts.resolved_windows},
                          {"eye_rows", metrics.eye_angles.source_rows_read},
                          {"motion_rows", metrics.motion.source_rows_read},
                          {"bout_detector_rows", metrics.swim_bouts.source_detector_rows_read},
                          {"eye_peak_cached_pages", metrics.eye_angles.peak_cached_windows},
                          {"motion_peak_cached_pages", metrics.motion.peak_cached_windows},
                          {"bout_peak_cached_pages", metrics.swim_bouts.peak_cached_windows}};
    require(metrics.eye_angles.peak_cached_windows <= 2 &&
            metrics.motion.peak_cached_windows <= 2 &&
            metrics.swim_bouts.peak_cached_windows <= 2, "Cache exceeds page limit");
    session.close();
    require(session.snapshot(frames.back()).state == CanonicalTimelineSessionState::Closed,
            "Session did not close");
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    report["peak_rss_kib"] = usage.ru_maxrss;
    report["pass"] = true;
  } catch (const std::exception& exception) {
    report["pass"] = false;
    report["failure"] = exception.what();
  }
  std::cout << report.dump(2) << '\n';
  return report.value("pass", false) ? 0 : 1;
}
