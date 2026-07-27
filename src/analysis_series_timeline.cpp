#include "analysis_series_timeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crimson::timeline {
namespace {

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

bool hasCanonicalMapping(const AnalysisSeriesTimelineWindow &window) {
  return window.mapping_frames.size() >= 2 &&
         window.mapping_frames.size() == window.mapping_times_seconds.size() &&
         std::is_sorted(window.mapping_frames.begin(),
                        window.mapping_frames.end()) &&
         std::is_sorted(window.mapping_times_seconds.begin(),
                        window.mapping_times_seconds.end());
}

class VectorRepository final : public AnalysisSeriesTimelineRepository {
public:
  VectorRepository(AnalysisSeriesTimelineDescriptor descriptor,
                   std::vector<AnalysisSeriesTimelineSourceData> sources)
      : descriptor_(std::move(descriptor)) {
    for (auto &source : sources) {
      sources_[source.source_key] = std::move(source);
    }
  }

  const AnalysisSeriesTimelineDescriptor &descriptor() const override {
    return descriptor_;
  }

  AnalysisSeriesTimelineWindow
  resolveWindow(const AnalysisSeriesTimelineRequest &request) const override {
    const auto found = sources_.find(request.source_key);
    if (found == sources_.end()) {
      AnalysisSeriesTimelineWindow failure;
      failure.request = request;
      failure.status = AnalysisSeriesTimelineStatus::InvalidRequest;
      failure.error = "Analysis-series source is unavailable";
      return failure;
    }
    return buildAnalysisSeriesTimelineWindow(
        descriptor_, request, found->second.frames, found->second.times_seconds,
        found->second.fields);
  }

private:
  AnalysisSeriesTimelineDescriptor descriptor_;
  std::unordered_map<std::string, AnalysisSeriesTimelineSourceData> sources_;
};

} // namespace

const AnalysisSeriesSourceDescriptor *
findAnalysisSeriesSource(const AnalysisSeriesTimelineDescriptor &descriptor,
                         const std::string &source_key) {
  const auto found = std::find_if(
      descriptor.sources.begin(), descriptor.sources.end(),
      [&](const auto &source) { return source.key == source_key; });
  return found == descriptor.sources.end() ? nullptr : &*found;
}

std::string defaultAnalysisSeriesSource(
    const AnalysisSeriesTimelineDescriptor &descriptor) {
  if (findAnalysisSeriesSource(descriptor, descriptor.default_source)) {
    return descriptor.default_source;
  }
  return descriptor.sources.empty() ? std::string{}
                                    : descriptor.sources.front().key;
}

AnalysisSeriesTimelinePageBounds
analysisSeriesTimelinePageBounds(int64_t frame, size_t frame_count,
                                 size_t page_span_frames,
                                 size_t page_step_frames) {
  AnalysisSeriesTimelinePageBounds bounds;
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

AnalysisSeriesTimelineWindow buildAnalysisSeriesTimelineWindow(
    const AnalysisSeriesTimelineDescriptor &descriptor,
    const AnalysisSeriesTimelineRequest &request,
    const std::vector<int64_t> &frames,
    const std::vector<double> &times_seconds,
    const std::vector<AnalysisSeriesTimelineFieldSeries> &fields) {
  AnalysisSeriesTimelineWindow window;
  window.request = request;
  const auto *source = findAnalysisSeriesSource(descriptor, request.source_key);
  if (source == nullptr || request.first_frame < 0 ||
      request.last_frame < request.first_frame ||
      request.max_points_per_trace < 3 ||
      !std::isfinite(request.fallback_frames_per_second) ||
      request.fallback_frames_per_second <= 0.0) {
    window.status = AnalysisSeriesTimelineStatus::InvalidRequest;
    window.error = "Analysis-series timeline request is invalid";
    return window;
  }
  if (descriptor.frame_count == 0 ||
      request.first_frame >= static_cast<int64_t>(descriptor.frame_count) ||
      request.last_frame >= static_cast<int64_t>(descriptor.frame_count)) {
    window.status = AnalysisSeriesTimelineStatus::OutOfRange;
    window.error =
        "Analysis-series timeline request is outside the frame range";
    return window;
  }
  if (!std::is_sorted(frames.begin(), frames.end())) {
    window.status = AnalysisSeriesTimelineStatus::ReadFailed;
    window.error = "Analysis-series camera frames are not sorted";
    return window;
  }

  const auto first =
      std::lower_bound(frames.begin(), frames.end(), request.first_frame);
  const auto last = std::upper_bound(first, frames.end(), request.last_frame);
  const size_t first_index = static_cast<size_t>(first - frames.begin());
  const size_t last_index = static_cast<size_t>(last - frames.begin());
  window.source_row_count = last_index - first_index;
  if (window.source_row_count == 0) {
    window.status = AnalysisSeriesTimelineStatus::Missing;
    window.error = "No analysis-series rows map to this frame window";
    return window;
  }

  std::vector<int64_t> selected_frames(first, last);
  std::vector<double> selected_times(window.source_row_count);
  for (size_t offset = 0; offset < window.source_row_count; ++offset) {
    const size_t index = first_index + offset;
    selected_times[offset] =
        index < times_seconds.size() && std::isfinite(times_seconds[index])
            ? times_seconds[index]
            : static_cast<double>(selected_frames[offset]) /
                  request.fallback_frames_per_second;
  }
  window.mapping_frames = selected_frames;
  window.mapping_times_seconds = selected_times;

  std::unordered_map<std::string, const std::vector<double> *> mapped_fields;
  for (const auto &field : fields) {
    mapped_fields[field.key] = &field.values;
  }
  for (const auto &trace_descriptor : source->traces) {
    const auto found = mapped_fields.find(trace_descriptor.key);
    if (found == mapped_fields.end() || found->second->size() < last_index) {
      continue;
    }
    std::vector<double> selected_values(window.source_row_count);
    std::copy(found->second->begin() + static_cast<std::ptrdiff_t>(first_index),
              found->second->begin() + static_cast<std::ptrdiff_t>(last_index),
              selected_values.begin());
    const auto indices =
        decimatedIndices(selected_frames, selected_times, selected_values,
                         request.anchor_frame, request.max_points_per_trace);
    if (indices.size() < 2) {
      continue;
    }
    AnalysisSeriesTimelineTrace trace;
    trace.descriptor = trace_descriptor;
    trace.frames.reserve(indices.size());
    trace.times_seconds.reserve(indices.size());
    trace.values.reserve(indices.size());
    for (size_t index : indices) {
      trace.frames.push_back(selected_frames[index]);
      trace.times_seconds.push_back(selected_times[index]);
      trace.values.push_back(selected_values[index]);
    }
    window.published_point_count += trace.values.size();
    window.traces.push_back(std::move(trace));
  }
  window.status = window.traces.empty() ? AnalysisSeriesTimelineStatus::Missing
                                        : AnalysisSeriesTimelineStatus::Mapped;
  if (window.traces.empty()) {
    window.error = "No finite analysis-series traces are available";
  }
  return window;
}

double
analysisSeriesTimelineTimeForFrame(const AnalysisSeriesTimelineWindow &window,
                                   int64_t frame,
                                   double fallback_frames_per_second) {
  if (!hasCanonicalMapping(window)) {
    return fallback_frames_per_second > 0.0
               ? static_cast<double>(frame) / fallback_frames_per_second
               : 0.0;
  }
  const auto upper = std::lower_bound(window.mapping_frames.begin(),
                                      window.mapping_frames.end(), frame);
  if (upper == window.mapping_frames.begin()) {
    return window.mapping_times_seconds.front();
  }
  if (upper == window.mapping_frames.end()) {
    return window.mapping_times_seconds.back();
  }
  const size_t right =
      static_cast<size_t>(upper - window.mapping_frames.begin());
  if (window.mapping_frames[right] == frame) {
    return window.mapping_times_seconds[right];
  }
  const size_t left = right - 1;
  const double fraction =
      static_cast<double>(frame - window.mapping_frames[left]) /
      static_cast<double>(window.mapping_frames[right] -
                          window.mapping_frames[left]);
  return window.mapping_times_seconds[left] +
         fraction * (window.mapping_times_seconds[right] -
                     window.mapping_times_seconds[left]);
}

int64_t
analysisSeriesTimelineNearestFrame(const AnalysisSeriesTimelineWindow &window,
                                   double time_seconds,
                                   double fallback_frames_per_second) {
  if (!hasCanonicalMapping(window)) {
    return fallback_frames_per_second > 0.0 && std::isfinite(time_seconds)
               ? static_cast<int64_t>(
                     std::llround(time_seconds * fallback_frames_per_second))
               : -1;
  }
  const auto upper =
      std::lower_bound(window.mapping_times_seconds.begin(),
                       window.mapping_times_seconds.end(), time_seconds);
  if (upper == window.mapping_times_seconds.begin()) {
    return window.mapping_frames.front();
  }
  if (upper == window.mapping_times_seconds.end()) {
    return window.mapping_frames.back();
  }
  const size_t right =
      static_cast<size_t>(upper - window.mapping_times_seconds.begin());
  const size_t left = right - 1;
  const double span =
      window.mapping_times_seconds[right] - window.mapping_times_seconds[left];
  if (!std::isfinite(span) || span <= 0.0) {
    return window.mapping_frames[left];
  }
  const double fraction = std::clamp(
      (time_seconds - window.mapping_times_seconds[left]) / span, 0.0, 1.0);
  return static_cast<int64_t>(std::llround(
      static_cast<double>(window.mapping_frames[left]) +
      fraction * static_cast<double>(window.mapping_frames[right] -
                                     window.mapping_frames[left])));
}

std::unique_ptr<AnalysisSeriesTimelineRepository>
MakeAnalysisSeriesTimelineRepository(
    AnalysisSeriesTimelineDescriptor descriptor,
    std::vector<AnalysisSeriesTimelineSourceData> sources) {
  return std::make_unique<VectorRepository>(std::move(descriptor),
                                            std::move(sources));
}

} // namespace crimson::timeline
