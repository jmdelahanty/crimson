#include "gui/canonical_timeline_bout_shading.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using crimson::gui::CanonicalTimelineProductState;
using crimson::gui::CanonicalTimelineSessionState;
using crimson::gui::CanonicalTimelineSnapshot;
using crimson::timeline::AnalysisSeriesTimelineStatus;
using crimson::timeline::SwimBoutInterval;
using crimson::timeline::SwimBoutTimelineStatus;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                   \
      return false;                                                            \
    }                                                                          \
  } while (false)

CanonicalTimelineSnapshot fixture() {
  CanonicalTimelineSnapshot snapshot;
  snapshot.state = CanonicalTimelineSessionState::Ready;
  snapshot.requested_frame = 15;

  auto& motion = snapshot.motion;
  motion.state = CanonicalTimelineProductState::Ready;
  motion.source_identity = "motion-filtered";
  motion.descriptor.default_source = motion.source_identity;
  motion.descriptor.frame_count = 100;
  crimson::timeline::AnalysisSeriesSourceDescriptor source;
  source.key = motion.source_identity;
  source.run_name = "motion_run";
  source.track_id = "id_0";
  source.variant = "filtered";
  motion.descriptor.sources.push_back(source);
  auto motion_window =
      std::make_shared<crimson::timeline::AnalysisSeriesTimelineWindow>();
  motion_window->status = AnalysisSeriesTimelineStatus::Mapped;
  motion_window->request.source_key = motion.source_identity;
  motion_window->request.first_frame = 10;
  motion_window->request.last_frame = 20;
  motion_window->request.anchor_frame = 10;
  motion.window = motion_window;

  auto& bouts = snapshot.swim_bouts;
  bouts.state = CanonicalTimelineProductState::Ready;
  bouts.source_identity = "bouts-filtered";
  bouts.descriptor.default_candidate = bouts.source_identity;
  bouts.descriptor.frame_count = 100;
  crimson::timeline::SwimBoutCandidateDescriptor candidate;
  candidate.key = bouts.source_identity;
  candidate.run_name = "bouts_run";
  candidate.source_track_kinematics_run = source.run_name;
  candidate.track_id = 0;
  candidate.speed_level = "speed_filtered";
  bouts.descriptor.candidates.push_back(candidate);
  auto bout_window =
      std::make_shared<crimson::timeline::SwimBoutTimelineWindow>();
  bout_window->status = SwimBoutTimelineStatus::Mapped;
  bout_window->request.candidate_key = bouts.source_identity;
  bout_window->request.first_frame = 8;
  bout_window->request.last_frame = 22;
  bout_window->request.anchor_frame = 10;
  bouts.window = bout_window;
  return snapshot;
}

bool close(double actual, double expected) {
  return std::isfinite(actual) && std::abs(actual - expected) < 1e-9;
}

bool contains(const std::vector<AnalysisTimelinePlotBand>& bands, bool core,
              double first, double last) {
  for (const auto& band : bands) {
    if (band.core == core && close(band.start_seconds, first) &&
        close(band.end_seconds, last)) {
      return true;
    }
  }
  return false;
}

bool TestInclusiveEndsClippingAndCore() {
  auto snapshot = fixture();
  auto bouts = std::make_shared<crimson::timeline::SwimBoutTimelineWindow>(
      *snapshot.swim_bouts.window);
  bouts->intervals = {{0, 8, 12, 9, 11, false},
                      {1, 15, 15, 15, 15, false},
                      {2, 19, 24, 20, 22, false},
                      {3, 1, 7, -1, -1, false},
                      {4, 23, 30, -1, -1, false}};
  snapshot.swim_bouts.window = bouts;
  const auto bands = crimson::gui::canonicalTimelineBoutShadingBands(
      snapshot, 15, 10.0);
  CHECK(bands.size() == 6);
  CHECK(contains(bands, false, 1.0, 1.3));
  CHECK(contains(bands, true, 1.0, 1.2));
  CHECK(contains(bands, false, 1.5, 1.6));
  CHECK(contains(bands, true, 1.5, 1.6));
  CHECK(contains(bands, false, 1.9, 2.1));
  CHECK(contains(bands, true, 2.0, 2.1));
  return true;
}

bool TestPublishedTimeMappingAndCoverage() {
  auto snapshot = fixture();
  auto motion =
      std::make_shared<crimson::timeline::AnalysisSeriesTimelineWindow>(
          *snapshot.motion.window);
  motion->mapping_frames = {12, 15, 20};
  motion->mapping_times_seconds = {5.0, 6.0, 8.0};
  snapshot.motion.window = motion;
  auto bouts = std::make_shared<crimson::timeline::SwimBoutTimelineWindow>(
      *snapshot.swim_bouts.window);
  bouts->intervals = {{0, 8, 13, 11, 13, false},
                      {1, 18, 24, 19, 23, false},
                      {2, 10, 11, -1, -1, false},
                      {3, 21, 22, -1, -1, false}};
  snapshot.swim_bouts.window = bouts;
  const auto bands = crimson::gui::canonicalTimelineBoutShadingBands(
      snapshot, 15, 10.0);
  CHECK(bands.size() == 4);
  CHECK(contains(bands, false, 5.0, 5.0 + 2.0 / 3.0));
  CHECK(contains(bands, true, 5.0, 5.0 + 2.0 / 3.0));
  CHECK(contains(bands, false, 7.2, 8.4));
  CHECK(contains(bands, true, 7.6, 8.4));
  return true;
}

bool TestInvalidIntervalsAndMapping() {
  auto snapshot = fixture();
  auto bouts = std::make_shared<crimson::timeline::SwimBoutTimelineWindow>(
      *snapshot.swim_bouts.window);
  bouts->intervals = {{0, -1, 12, -1, -1, false},
                      {1, 16, 15, -1, -1, false},
                      {2, 100, 110, -1, -1, false},
                      {3, 12, 14, 20, 22, false},
                      {4, 15, 16, -1, -1, false}};
  snapshot.swim_bouts.window = bouts;
  auto bands = crimson::gui::canonicalTimelineBoutShadingBands(
      snapshot, 15, 10.0);
  CHECK(bands.size() == 2);
  CHECK(contains(bands, false, 1.2, 1.5));
  CHECK(contains(bands, false, 1.5, 1.7));

  auto motion =
      std::make_shared<crimson::timeline::AnalysisSeriesTimelineWindow>(
          *snapshot.motion.window);
  snapshot.motion.window = motion;
  motion->mapping_frames = {10, 20};
  motion->mapping_times_seconds = {1.0};
  CHECK(crimson::gui::canonicalTimelineBoutShadingBands(snapshot, 15, 10.0)
            .empty());
  motion->mapping_times_seconds = {2.0, 1.0};
  CHECK(crimson::gui::canonicalTimelineBoutShadingBands(snapshot, 15, 10.0)
            .empty());
  motion->mapping_times_seconds = {1.0, std::numeric_limits<double>::quiet_NaN()};
  CHECK(crimson::gui::canonicalTimelineBoutShadingBands(snapshot, 15, 10.0)
            .empty());
  motion->mapping_frames = {10, 10};
  motion->mapping_times_seconds = {1.0, 2.0};
  CHECK(crimson::gui::canonicalTimelineBoutShadingBands(snapshot, 15, 10.0)
            .empty());
  motion->mapping_frames.clear();
  motion->mapping_times_seconds.clear();
  CHECK(crimson::gui::canonicalTimelineBoutShadingBands(snapshot, 15, 0.0)
            .empty());
  CHECK(crimson::gui::canonicalTimelineBoutShadingBands(
            snapshot, 15, std::numeric_limits<double>::quiet_NaN())
            .empty());
  return true;
}

bool TestStateAndIdentityGuards() {
  auto snapshot = fixture();
  auto bouts = std::make_shared<crimson::timeline::SwimBoutTimelineWindow>(
      *snapshot.swim_bouts.window);
  bouts->intervals = {{0, 12, 16, 13, 15, false}};
  snapshot.swim_bouts.window = bouts;
  const auto valid = [&] {
    return crimson::gui::canonicalTimelineBoutShadingBands(snapshot, 15, 10.0);
  };
  CHECK(valid().size() == 2);
  for (int64_t current = 16; current <= 19; ++current) {
    snapshot.requested_frame = current;
    CHECK(crimson::gui::canonicalTimelineBoutShadingBands(
              snapshot, current, 10.0).size() == 2);
  }
  snapshot.requested_frame = 15;
  snapshot.state = CanonicalTimelineSessionState::Opening;
  CHECK(valid().empty());
  snapshot.state = CanonicalTimelineSessionState::Ready;
  snapshot.motion.state = CanonicalTimelineProductState::Pending;
  CHECK(valid().empty());
  snapshot.motion.state = CanonicalTimelineProductState::Ready;
  snapshot.motion.source_identity = "stale-source";
  CHECK(valid().empty());
  snapshot.motion.source_identity = snapshot.motion.descriptor.default_source;
  snapshot.swim_bouts.state = CanonicalTimelineProductState::Error;
  CHECK(valid().empty());
  snapshot.swim_bouts.state = CanonicalTimelineProductState::Empty;
  CHECK(valid().empty());
  snapshot.swim_bouts.state = CanonicalTimelineProductState::Ready;
  snapshot.swim_bouts.source_identity = "stale-candidate";
  CHECK(valid().empty());
  snapshot.swim_bouts.source_identity =
      snapshot.swim_bouts.descriptor.default_candidate;
  bouts->request.candidate_key = "other-candidate";
  CHECK(valid().empty());
  bouts->request.candidate_key = snapshot.swim_bouts.source_identity;
  auto motion =
      std::make_shared<crimson::timeline::AnalysisSeriesTimelineWindow>(
          *snapshot.motion.window);
  snapshot.motion.window = motion;
  motion->request.source_key = "other-source";
  CHECK(valid().empty());
  motion->request.source_key = snapshot.motion.source_identity;
  snapshot.swim_bouts.descriptor.candidates.front().source_track_kinematics_run =
      "other-run";
  CHECK(valid().empty());
  snapshot.swim_bouts.descriptor.candidates.front().source_track_kinematics_run =
      "motion_run";
  snapshot.swim_bouts.descriptor.candidates.front().track_id = 1;
  CHECK(valid().empty());
  snapshot.swim_bouts.descriptor.candidates.front().track_id = 0;
  snapshot.swim_bouts.descriptor.candidates.front().speed_level = "speed_raw";
  CHECK(valid().empty());
  snapshot.swim_bouts.descriptor.candidates.front().speed_level =
      "speed_filtered";
  snapshot.requested_frame = 40;
  CHECK(valid().empty());
  snapshot.requested_frame = 15;
  CHECK(crimson::gui::canonicalTimelineBoutShadingBands(snapshot, 40, 10.0)
            .empty());
  motion->request.first_frame = 30;
  motion->request.last_frame = 40;
  CHECK(valid().empty());
  return true;
}

}  // namespace

int main() {
  if (!TestInclusiveEndsClippingAndCore() ||
      !TestPublishedTimeMappingAndCoverage() ||
      !TestInvalidIntervalsAndMapping() || !TestStateAndIdentityGuards()) {
    return 1;
  }
  std::cout << "canonical_timeline_bout_shading_tests: PASS\n";
  return 0;
}
