#include "swim_bout_timeline.h"

#include "analysis_series_timeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace crimson::timeline {
namespace {

std::string normalizeSpeedLevel(std::string value) {
  if (value.rfind("speed_", 0) == 0) {
    value.erase(0, 6);
  }
  return value;
}

int32_t parseTrackId(std::string value) {
  if (value.rfind("id_", 0) == 0) {
    value.erase(0, 3);
  }
  try {
    size_t consumed = 0;
    const int64_t parsed = std::stoll(value, &consumed);
    if (consumed != value.size() || parsed < 0 ||
        parsed > std::numeric_limits<int32_t>::max()) {
      return -1;
    }
    return static_cast<int32_t>(parsed);
  } catch (...) {
    return -1;
  }
}

bool speedLevelMatches(const std::string &candidate,
                       const std::string &selected) {
  const std::string normalized_candidate = normalizeSpeedLevel(candidate);
  const std::string normalized_selected = normalizeSpeedLevel(selected);
  return normalized_selected.empty() ||
         normalized_candidate == normalized_selected;
}

bool speedSourceMatches(const std::string &source,
                        const std::string &selected) {
  const std::string normalized_source = normalizeSpeedLevel(source);
  const std::string normalized_selected = normalizeSpeedLevel(selected);
  return !normalized_source.empty() && !normalized_selected.empty() &&
         normalized_source == normalized_selected;
}

bool pathReferencesSpeedLevel(const std::string &path,
                              const std::string &selected) {
  const std::string normalized_selected = normalizeSpeedLevel(selected);
  std::string normalized_path = path;
  std::transform(normalized_path.begin(), normalized_path.end(),
                 normalized_path.begin(), [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  std::string normalized_token = normalized_selected;
  std::transform(normalized_token.begin(), normalized_token.end(),
                 normalized_token.begin(), [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return !normalized_path.empty() && !normalized_token.empty() &&
         (normalized_path.find("speed_" + normalized_token) !=
              std::string::npos ||
          normalized_path.find("/" + normalized_token + "/") !=
              std::string::npos);
}

std::vector<size_t> decimatedIndices(const std::vector<int64_t> &frames,
                                     const std::vector<double> &times,
                                     const std::vector<double> &values,
                                     int64_t anchor_frame,
                                     size_t maximum_points) {
  std::vector<size_t> finite;
  const size_t count = std::min({frames.size(), times.size(), values.size()});
  finite.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    if (frames[index] >= 0 && std::isfinite(times[index]) &&
        std::isfinite(values[index])) {
      finite.push_back(index);
    }
  }
  if (finite.size() <= maximum_points) {
    return finite;
  }
  if (maximum_points < 3) {
    return {};
  }

  std::vector<size_t> selected = {finite.front(), finite.back()};
  const auto anchor = std::lower_bound(
      finite.begin(), finite.end(), anchor_frame,
      [&](size_t index, int64_t frame) { return frames[index] < frame; });
  selected.push_back(anchor == finite.end() ? finite.back() : *anchor);
  const size_t bucket_count = maximum_points > selected.size()
                                  ? (maximum_points - selected.size()) / 2
                                  : 0;
  const size_t interior_count = finite.size() - 2;
  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    const size_t begin = 1 + interior_count * bucket / bucket_count;
    const size_t end = 1 + interior_count * (bucket + 1) / bucket_count;
    if (end <= begin) {
      continue;
    }
    size_t minimum = finite[begin];
    size_t maximum = finite[begin];
    for (size_t position = begin + 1; position < end; ++position) {
      const size_t index = finite[position];
      if (values[index] < values[minimum]) {
        minimum = index;
      }
      if (values[index] > values[maximum]) {
        maximum = index;
      }
    }
    selected.push_back(minimum);
    selected.push_back(maximum);
  }
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  return selected;
}

class VectorRepository final : public SwimBoutTimelineRepository {
public:
  VectorRepository(SwimBoutTimelineDescriptor descriptor,
                   std::vector<SwimBoutCandidateData> candidates)
      : descriptor_(std::move(descriptor)) {
    for (auto &candidate : candidates) {
      candidates_[candidate.candidate_key] = std::move(candidate);
    }
  }

  const SwimBoutTimelineDescriptor &descriptor() const override {
    return descriptor_;
  }

  SwimBoutTimelineWindow
  resolveWindow(const SwimBoutTimelineRequest &request) const override {
    const auto found = candidates_.find(request.candidate_key);
    if (found == candidates_.end()) {
      SwimBoutTimelineWindow result;
      result.request = request;
      result.status = SwimBoutTimelineStatus::InvalidRequest;
      result.error = "Swim-bout candidate is unavailable";
      return result;
    }
    const auto &candidate = found->second;
    return buildSwimBoutTimelineWindow(
        descriptor_, request, candidate.intervals, candidate.detector_frames,
        candidate.detector_times_seconds, candidate.detector_values);
  }

private:
  SwimBoutTimelineDescriptor descriptor_;
  std::unordered_map<std::string, SwimBoutCandidateData> candidates_;
};

} // namespace

const SwimBoutCandidateDescriptor *
findSwimBoutCandidate(const SwimBoutTimelineDescriptor &descriptor,
                      const std::string &candidate_key) {
  const auto found = std::find_if(
      descriptor.candidates.begin(), descriptor.candidates.end(),
      [&](const auto &candidate) { return candidate.key == candidate_key; });
  return found == descriptor.candidates.end() ? nullptr : &*found;
}

bool swimBoutCandidateCompatible(
    const SwimBoutCandidateDescriptor &candidate,
    const AnalysisSeriesSourceDescriptor &motion_source) {
  const int32_t selected_track = parseTrackId(motion_source.track_id);
  if (!candidate.source_track_kinematics_run.empty() &&
      candidate.source_track_kinematics_run != motion_source.run_name) {
    return false;
  }
  if (candidate.source_track_kinematics_run.empty() &&
      candidate.run_name.find(motion_source.run_name) == std::string::npos) {
    return false;
  }
  if (candidate.track_id >= 0 && selected_track >= 0 &&
      candidate.track_id != selected_track) {
    return false;
  }
  if (motion_source.variant.empty() ||
      speedLevelMatches(candidate.speed_level, motion_source.variant) ||
      speedSourceMatches(candidate.detection_signal_source_level,
                         motion_source.variant) ||
      speedSourceMatches(candidate.movement_metric_source_level,
                         motion_source.variant) ||
      speedSourceMatches(candidate.path_distance_source_level,
                         motion_source.variant)) {
    return true;
  }
  return pathReferencesSpeedLevel(candidate.detection_signal_source_path,
                                  motion_source.variant);
}

std::vector<const SwimBoutCandidateDescriptor *> compatibleSwimBoutCandidates(
    const SwimBoutTimelineDescriptor &descriptor,
    const AnalysisSeriesSourceDescriptor &motion_source) {
  std::vector<const SwimBoutCandidateDescriptor *> result;
  for (const auto &candidate : descriptor.candidates) {
    if (swimBoutCandidateCompatible(candidate, motion_source)) {
      result.push_back(&candidate);
    }
  }
  return result;
}

std::string
defaultSwimBoutCandidate(const SwimBoutTimelineDescriptor &descriptor,
                         const AnalysisSeriesSourceDescriptor &motion_source,
                         const std::string &preferred_candidate) {
  const auto compatible =
      compatibleSwimBoutCandidates(descriptor, motion_source);
  if (compatible.empty()) {
    return {};
  }
  const auto preferred = std::find_if(
      compatible.begin(), compatible.end(), [&](const auto *candidate) {
        return candidate->key == preferred_candidate;
      });
  if (preferred != compatible.end()) {
    return (*preferred)->key;
  }
  const SwimBoutCandidateDescriptor *chosen = compatible.front();
  for (const auto *candidate : compatible) {
    if (candidate->latest_run && candidate->default_level) {
      return candidate->key;
    }
    if (chosen->latest_run && !chosen->default_level) {
      continue;
    }
    if (candidate->latest_run || candidate->default_level) {
      chosen = candidate;
    }
  }
  return chosen->key;
}

std::string
swimBoutCandidateLabel(const SwimBoutCandidateDescriptor &candidate) {
  std::ostringstream label;
  label << candidate.run_name << " / " << candidate.speed_level << " ("
        << (candidate.detection_method.empty() ? "method unknown"
                                               : candidate.detection_method)
        << ", " << candidate.bout_count << " bouts";
  if (candidate.compact_layout) {
    if (!candidate.signal_role.empty()) {
      label << ", role " << candidate.signal_role;
    }
    label << ", candidate " << candidate.candidate_id << " signal "
          << candidate.signal_id;
  }
  if (std::isfinite(candidate.threshold_mm)) {
    label << ", threshold " << std::fixed << std::setprecision(3)
          << candidate.threshold_mm;
  }
  if (std::isfinite(candidate.exponential_tau_s)) {
    label << ", tau " << std::fixed << std::setprecision(3)
          << candidate.exponential_tau_s << 's';
  }
  if (candidate.latest_run) {
    label << ", latest";
  }
  if (candidate.default_level) {
    label << ", default";
  }
  label << ')';
  return label.str();
}

std::string makeSwimBoutCandidateKey(const std::string &run_name,
                                     int32_t candidate_id, int32_t signal_id,
                                     const std::string &speed_level) {
  return run_name + "/candidate_" + std::to_string(candidate_id) + "/signal_" +
         std::to_string(signal_id) + "/" + speed_level;
}

SwimBoutTimelinePageBounds swimBoutTimelinePageBounds(int64_t frame,
                                                      size_t frame_count,
                                                      size_t page_span_frames,
                                                      size_t page_step_frames) {
  SwimBoutTimelinePageBounds bounds;
  if (frame < 0 || frame_count == 0 || page_span_frames == 0 ||
      page_step_frames == 0 || page_step_frames > page_span_frames ||
      frame >= static_cast<int64_t>(frame_count)) {
    bounds.last_frame = -1;
    return bounds;
  }
  const int64_t count = static_cast<int64_t>(std::min<size_t>(
      frame_count, static_cast<size_t>(std::numeric_limits<int64_t>::max())));
  if (frame_count <= page_span_frames) {
    return {0, count - 1};
  }
  const int64_t step = static_cast<int64_t>(page_step_frames);
  const int64_t span = static_cast<int64_t>(page_span_frames);
  const int64_t base = frame / step * step;
  int64_t first = std::max<int64_t>(0, base - (span - step) / 2);
  if (first + span > count) {
    first = count - span;
  }
  return {first, first + span - 1};
}

SwimBoutTimelineWindow
buildSwimBoutTimelineWindow(const SwimBoutTimelineDescriptor &descriptor,
                            const SwimBoutTimelineRequest &request,
                            const std::vector<SwimBoutInterval> &intervals,
                            const std::vector<int64_t> &detector_frames,
                            const std::vector<double> &detector_times_seconds,
                            const std::vector<double> &detector_values) {
  SwimBoutTimelineWindow window;
  window.request = request;
  if (findSwimBoutCandidate(descriptor, request.candidate_key) == nullptr ||
      request.first_frame < 0 || request.last_frame < request.first_frame ||
      request.max_detector_points < 3 ||
      !std::isfinite(request.fallback_frames_per_second) ||
      request.fallback_frames_per_second <= 0.0) {
    window.status = SwimBoutTimelineStatus::InvalidRequest;
    window.error = "Swim-bout timeline request is invalid";
    return window;
  }
  if (descriptor.frame_count == 0 ||
      request.first_frame >= static_cast<int64_t>(descriptor.frame_count) ||
      request.last_frame >= static_cast<int64_t>(descriptor.frame_count)) {
    window.status = SwimBoutTimelineStatus::OutOfRange;
    window.error = "Swim-bout timeline request is outside the frame range";
    return window;
  }

  window.candidate_interval_count = intervals.size();
  for (const auto &interval : intervals) {
    if (interval.start_frame < 0 || interval.end_frame < interval.start_frame) {
      window.status = SwimBoutTimelineStatus::ReadFailed;
      window.error = "Swim-bout interval boundaries are invalid";
      return window;
    }
    const bool partial_core =
        (interval.core_start_frame >= 0) != (interval.core_end_frame >= 0);
    if (partial_core ||
        (interval.core_start_frame >= 0 && !interval.hasCore())) {
      window.status = SwimBoutTimelineStatus::ReadFailed;
      window.error = "Swim-bout core boundaries are invalid";
      return window;
    }
    if (interval.end_frame >= request.first_frame &&
        interval.start_frame <= request.last_frame) {
      window.intervals.push_back(interval);
    }
  }

  if (request.include_detector_trace) {
    if (detector_frames.size() != detector_values.size() ||
        (!detector_times_seconds.empty() &&
         detector_times_seconds.size() != detector_values.size()) ||
        !std::is_sorted(detector_frames.begin(), detector_frames.end())) {
      window.status = SwimBoutTimelineStatus::ReadFailed;
      window.error = "Swim-bout detector trace arrays are inconsistent";
      return window;
    }
    const auto first = std::lower_bound(
        detector_frames.begin(), detector_frames.end(), request.first_frame);
    const auto last =
        std::upper_bound(first, detector_frames.end(), request.last_frame);
    const size_t first_index =
        static_cast<size_t>(first - detector_frames.begin());
    const size_t last_index =
        static_cast<size_t>(last - detector_frames.begin());
    window.source_detector_row_count = last_index - first_index;
    std::vector<int64_t> selected_frames(first, last);
    std::vector<double> selected_times(selected_frames.size());
    std::vector<double> selected_values(selected_frames.size());
    for (size_t index = 0; index < selected_frames.size(); ++index) {
      const size_t source = first_index + index;
      selected_times[index] =
          detector_times_seconds.empty()
              ? static_cast<double>(selected_frames[index]) /
                    request.fallback_frames_per_second
              : detector_times_seconds[source];
      selected_values[index] = detector_values[source];
    }
    const auto selected =
        decimatedIndices(selected_frames, selected_times, selected_values,
                         request.anchor_frame, request.max_detector_points);
    window.detector_frames.reserve(selected.size());
    window.detector_times_seconds.reserve(selected.size());
    window.detector_values.reserve(selected.size());
    for (size_t index : selected) {
      window.detector_frames.push_back(selected_frames[index]);
      window.detector_times_seconds.push_back(selected_times[index]);
      window.detector_values.push_back(selected_values[index]);
    }
  }

  window.status = window.intervals.empty() && window.detector_values.empty()
                      ? SwimBoutTimelineStatus::Missing
                      : SwimBoutTimelineStatus::Mapped;
  if (window.status == SwimBoutTimelineStatus::Missing) {
    window.error = "No swim-bout data maps to this frame window";
  }
  return window;
}

std::unique_ptr<SwimBoutTimelineRepository>
MakeSwimBoutTimelineRepository(SwimBoutTimelineDescriptor descriptor,
                               std::vector<SwimBoutCandidateData> candidates) {
  return std::make_unique<VectorRepository>(std::move(descriptor),
                                            std::move(candidates));
}

} // namespace crimson::timeline
