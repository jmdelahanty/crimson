#pragma once

#include "gui/analysis_timeline_trace_plot.h"
#include "gui/canonical_timeline_session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace crimson::gui {
namespace detail {

inline bool validMotionTimeMapping(
    const timeline::AnalysisSeriesTimelineWindow& motion) {
  const auto& frames = motion.mapping_frames;
  const auto& times = motion.mapping_times_seconds;
  if (frames.empty() && times.empty()) return true;
  if (frames.size() < 2 || frames.size() != times.size()) return false;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (!std::isfinite(times[i]) ||
        (i > 0 && (frames[i] <= frames[i - 1] ||
                   times[i] <= times[i - 1]))) {
      return false;
    }
  }
  return true;
}

inline std::optional<double> mappedFrameTime(
    const timeline::AnalysisSeriesTimelineWindow& motion,
    long double frame, double frames_per_second) {
  const auto& frames = motion.mapping_frames;
  const auto& times = motion.mapping_times_seconds;
  long double time = 0.0L;
  if (frames.empty()) {
    time = frame / static_cast<long double>(frames_per_second);
  } else {
    // Interpolate authoritative source times. The sole extrapolated boundary
    // allowed by the caller is one frame past the last mapped sample.
    auto upper = std::lower_bound(
        frames.begin(), frames.end(), frame,
        [](int64_t sample_frame, long double target) {
          return static_cast<long double>(sample_frame) < target;
        });
    const size_t right =
        upper == frames.begin()
            ? 1
            : std::min(static_cast<size_t>(upper - frames.begin()),
                       frames.size() - 1);
    const size_t left = right - 1;
    const long double fraction =
        (frame - static_cast<long double>(frames[left])) /
        (static_cast<long double>(frames[right]) -
         static_cast<long double>(frames[left]));
    time = static_cast<long double>(times[left]) +
           fraction * (static_cast<long double>(times[right]) -
                       static_cast<long double>(times[left]));
  }
  const double result = static_cast<double>(time);
  if (!std::isfinite(result)) return std::nullopt;
  return result;
}

}  // namespace detail

// Each inclusive frame interval becomes a half-open time band. Frames are
// clipped to both loaded pages and the motion source's mapped-time coverage.
inline std::vector<AnalysisTimelinePlotBand> canonicalTimelineBoutShadingBands(
    const CanonicalTimelineSnapshot& snapshot, int64_t current_frame,
    double frames_per_second) {
  std::vector<AnalysisTimelinePlotBand> bands;
  if (snapshot.state != CanonicalTimelineSessionState::Ready ||
      snapshot.motion.state != CanonicalTimelineProductState::Ready ||
      (snapshot.swim_bouts.state != CanonicalTimelineProductState::Ready &&
       snapshot.swim_bouts.state != CanonicalTimelineProductState::Empty) ||
      !snapshot.motion.window || !snapshot.swim_bouts.window ||
      current_frame < 0 || snapshot.requested_frame != current_frame ||
      !std::isfinite(frames_per_second) || frames_per_second <= 0.0) {
    return bands;
  }
  const auto& motion = *snapshot.motion.window;
  const auto& bouts = *snapshot.swim_bouts.window;
  if (snapshot.swim_bouts.state == CanonicalTimelineProductState::Empty) {
    return bands;
  }
  const auto& motion_request = motion.request;
  const auto& bout_request = bouts.request;
  if (motion.status != timeline::AnalysisSeriesTimelineStatus::Mapped ||
      bouts.status != timeline::SwimBoutTimelineStatus::Mapped ||
      motion_request.first_frame < 0 || bout_request.first_frame < 0 ||
      motion_request.last_frame < motion_request.first_frame ||
      bout_request.last_frame < bout_request.first_frame ||
      current_frame < motion_request.first_frame ||
      current_frame > motion_request.last_frame ||
      current_frame < bout_request.first_frame ||
      current_frame > bout_request.last_frame ||
      snapshot.motion.descriptor.frame_count == 0 ||
      snapshot.motion.descriptor.frame_count !=
          snapshot.swim_bouts.descriptor.frame_count ||
      static_cast<uint64_t>(motion_request.last_frame) >=
          snapshot.motion.descriptor.frame_count ||
      static_cast<uint64_t>(bout_request.last_frame) >=
          snapshot.swim_bouts.descriptor.frame_count ||
      snapshot.motion.source_identity.empty() ||
      snapshot.swim_bouts.source_identity.empty() ||
      snapshot.motion.source_identity !=
          snapshot.motion.descriptor.default_source ||
      snapshot.swim_bouts.source_identity !=
          snapshot.swim_bouts.descriptor.default_candidate ||
      motion_request.source_key != snapshot.motion.source_identity ||
      bout_request.candidate_key != snapshot.swim_bouts.source_identity ||
      !detail::validMotionTimeMapping(motion)) {
    return bands;
  }
  const auto* source = timeline::findAnalysisSeriesSource(
      snapshot.motion.descriptor, motion_request.source_key);
  const auto* candidate = timeline::findSwimBoutCandidate(
      snapshot.swim_bouts.descriptor, bout_request.candidate_key);
  if (!source || !candidate ||
      !timeline::swimBoutCandidateCompatible(*candidate, *source)) {
    return bands;
  }

  int64_t first = std::max(motion_request.first_frame,
                           bout_request.first_frame);
  int64_t last = std::min(motion_request.last_frame,
                          bout_request.last_frame);
  if (!motion.mapping_frames.empty()) {
    first = std::max(first, motion.mapping_frames.front());
    last = std::min(last, motion.mapping_frames.back());
  }
  if (last < first) return bands;

  auto append = [&](int64_t start_frame, int64_t end_frame, bool core) {
    if (start_frame < 0 || end_frame < start_frame ||
        end_frame < first || start_frame > last) {
      return;
    }
    const int64_t clipped_start = std::max(start_frame, first);
    const int64_t clipped_end = std::min(end_frame, last);
    const auto start = detail::mappedFrameTime(
        motion, static_cast<long double>(clipped_start), frames_per_second);
    const auto end = detail::mappedFrameTime(
        motion, static_cast<long double>(clipped_end) + 1.0L,
        frames_per_second);
    if (!start || !end || *end <= *start) return;
    AnalysisTimelinePlotBand band;
    band.start_seconds = *start;
    band.end_seconds = *end;
    band.color = core ? ImVec4(0.15f, 0.95f, 0.45f, 0.24f)
                      : ImVec4(0.15f, 0.95f, 0.45f, 0.16f);
    band.core = core;
    bands.push_back(band);
  };
  bands.reserve(bouts.intervals.size() * 2);
  for (const auto& interval : bouts.intervals) {
    if (interval.start_frame < 0 ||
        interval.end_frame < interval.start_frame ||
        static_cast<uint64_t>(interval.end_frame) >=
            snapshot.swim_bouts.descriptor.frame_count) {
      continue;
    }
    append(interval.start_frame, interval.end_frame, false);
    if (interval.hasCore()) {
      append(interval.core_start_frame, interval.core_end_frame, true);
    }
  }
  return bands;
}

}  // namespace crimson::gui
