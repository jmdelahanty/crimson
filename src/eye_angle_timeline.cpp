#include "eye_angle_timeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crimson::timeline {
namespace {

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   if (character >= 'A' && character <= 'Z') {
                     return static_cast<char>(character - 'A' + 'a');
                   }
                   return static_cast<char>(character);
                 });
  return value;
}

std::vector<size_t> decimatedIndices(const std::vector<double>& times,
                                     const std::vector<double>& values,
                                     int64_t first_frame,
                                     int64_t anchor_frame,
                                     size_t maximum_points) {
  std::vector<size_t> finite;
  finite.reserve(values.size());
  for (size_t index = 0; index < values.size() && index < times.size(); ++index) {
    if (std::isfinite(times[index]) && std::isfinite(values[index])) {
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
  const int64_t clamped_anchor =
      std::clamp<int64_t>(anchor_frame - first_frame, 0,
                          static_cast<int64_t>(values.size() - 1));
  const auto anchor = std::lower_bound(
      finite.begin(), finite.end(), static_cast<size_t>(clamped_anchor));
  if (anchor != finite.end()) {
    selected.push_back(*anchor);
  } else {
    selected.push_back(finite.back());
  }

  const size_t reserved = selected.size();
  const size_t bucket_count =
      maximum_points > reserved ? (maximum_points - reserved) / 2 : 0;
  const size_t interior_begin = 1;
  const size_t interior_count = finite.size() - 2;
  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    const size_t begin =
        interior_begin + interior_count * bucket / bucket_count;
    const size_t end =
        interior_begin + interior_count * (bucket + 1) / bucket_count;
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

EyeAngleTimelineWindow buildWindow(
    const EyeAngleTimelineDescriptor& descriptor,
    const EyeAngleTimelineRequest& request,
    int64_t series_first_frame,
    const std::vector<double>& frame_times,
    const std::unordered_map<std::string, std::vector<double>>& fields) {
  EyeAngleTimelineWindow window;
  window.request = request;
  const auto* representation =
      findEyeAngleTimelineRepresentation(descriptor, request.representation_key);
  if (representation == nullptr || request.last_frame < request.first_frame ||
      request.first_frame < 0 || request.max_points_per_trace < 3 ||
      !std::isfinite(request.fallback_frames_per_second) ||
      request.fallback_frames_per_second <= 0.0) {
    window.status = EyeAngleTimelineStatus::InvalidRequest;
    window.error = "Eye-angle timeline request is invalid";
    return window;
  }
  if (descriptor.frame_count == 0 ||
      request.first_frame >= static_cast<int64_t>(descriptor.frame_count) ||
      request.last_frame >= static_cast<int64_t>(descriptor.frame_count)) {
    window.status = EyeAngleTimelineStatus::OutOfRange;
    window.error = "Eye-angle timeline request is outside the frame range";
    return window;
  }

  const size_t first = static_cast<size_t>(request.first_frame);
  const size_t last = static_cast<size_t>(request.last_frame);
  window.source_row_count = last - first + 1;
  if (series_first_frame < 0 || request.first_frame < series_first_frame) {
    window.status = EyeAngleTimelineStatus::ReadFailed;
    window.error = "Eye-angle timeline series origin is invalid";
    return window;
  }
  const size_t series_first =
      static_cast<size_t>(request.first_frame - series_first_frame);
  const size_t series_last = series_first + window.source_row_count - 1;
  std::vector<double> times(window.source_row_count);
  for (size_t offset = 0; offset < times.size(); ++offset) {
    const size_t frame = first + offset;
    const size_t series_index = series_first + offset;
    times[offset] = series_index < frame_times.size() &&
                            std::isfinite(frame_times[series_index])
                        ? frame_times[series_index]
                        : static_cast<double>(frame) /
                              request.fallback_frames_per_second;
  }

  for (const auto& field : representation->fields) {
    const auto found = fields.find(field.source_name);
    if (found == fields.end() || found->second.size() <= series_last) {
      continue;
    }
    std::vector<double> values(window.source_row_count);
    std::copy(found->second.begin() +
                  static_cast<std::ptrdiff_t>(series_first),
              found->second.begin() +
                  static_cast<std::ptrdiff_t>(series_last + 1),
              values.begin());
    const auto indices = decimatedIndices(
        times, values, request.first_frame, request.anchor_frame,
        request.max_points_per_trace);
    if (indices.size() < 2) {
      continue;
    }
    EyeAngleTimelineTrace trace;
    trace.field = field;
    trace.frames.reserve(indices.size());
    trace.times_seconds.reserve(indices.size());
    trace.values.reserve(indices.size());
    for (size_t index : indices) {
      trace.frames.push_back(request.first_frame + static_cast<int64_t>(index));
      trace.times_seconds.push_back(times[index]);
      trace.values.push_back(values[index]);
    }
    window.published_point_count += trace.values.size();
    window.traces.push_back(std::move(trace));
  }
  window.status = window.traces.empty() ? EyeAngleTimelineStatus::Missing
                                        : EyeAngleTimelineStatus::Mapped;
  if (window.traces.empty()) {
    window.error = "No finite eye-angle timeline traces are available";
  }
  return window;
}

class VectorRepository final : public EyeAngleTimelineRepository {
 public:
  VectorRepository(EyeAngleTimelineDescriptor descriptor,
                   std::vector<double> frame_times,
                   std::vector<EyeAngleTimelineFieldSeries> fields)
      : descriptor_(std::move(descriptor)),
        frame_times_(std::move(frame_times)) {
    for (auto& field : fields) {
      fields_[field.source_name] = std::move(field.frame_values);
    }
  }

  const EyeAngleTimelineDescriptor& descriptor() const override {
    return descriptor_;
  }

  EyeAngleTimelineWindow resolveWindow(
      const EyeAngleTimelineRequest& request) const override {
    return buildWindow(descriptor_, request, 0, frame_times_, fields_);
  }

 private:
  EyeAngleTimelineDescriptor descriptor_;
  std::vector<double> frame_times_;
  std::unordered_map<std::string, std::vector<double>> fields_;
};

const EyeAngleTimelineTrace* mappingTrace(
    const EyeAngleTimelineWindow& window) {
  const auto found = std::find_if(
      window.traces.begin(), window.traces.end(), [](const auto& trace) {
        return trace.frames.size() >= 2 &&
               trace.frames.size() == trace.times_seconds.size();
      });
  return found == window.traces.end() ? nullptr : &*found;
}

}  // namespace

EyeAngleTraceRole eyeAngleTraceRoleForField(const std::string& field_name) {
  const std::string lower = lowerAscii(field_name);
  if (lower.find("vergence") != std::string::npos) {
    return EyeAngleTraceRole::Vergence;
  }
  if (lower.find("left") != std::string::npos) {
    return EyeAngleTraceRole::Left;
  }
  if (lower.find("right") != std::string::npos) {
    return EyeAngleTraceRole::Right;
  }
  return EyeAngleTraceRole::Other;
}

const EyeAngleTimelineRepresentation* findEyeAngleTimelineRepresentation(
    const EyeAngleTimelineDescriptor& descriptor, const std::string& key) {
  const auto found = std::find_if(
      descriptor.representations.begin(), descriptor.representations.end(),
      [&](const auto& representation) { return representation.key == key; });
  return found == descriptor.representations.end() ? nullptr : &*found;
}

std::string defaultEyeAngleTimelineRepresentation(
    const EyeAngleTimelineDescriptor& descriptor) {
  if (findEyeAngleTimelineRepresentation(descriptor,
                                         descriptor.default_representation)) {
    return descriptor.default_representation;
  }
  return descriptor.representations.empty()
             ? std::string{}
             : descriptor.representations.front().key;
}

EyeAngleTimelinePageBounds eyeAngleTimelinePageBounds(
    int64_t frame, size_t frame_count, size_t page_span_frames,
    size_t page_step_frames) {
  EyeAngleTimelinePageBounds bounds;
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

double eyeAngleTimelineTimeForFrame(const EyeAngleTimelineWindow& window,
                                    int64_t frame,
                                    double fallback_frames_per_second) {
  const auto* trace = mappingTrace(window);
  if (trace == nullptr) {
    return fallback_frames_per_second > 0.0
               ? static_cast<double>(frame) / fallback_frames_per_second
               : 0.0;
  }
  const auto upper = std::lower_bound(trace->frames.begin(), trace->frames.end(),
                                      frame);
  if (upper == trace->frames.begin()) {
    return trace->times_seconds.front();
  }
  if (upper == trace->frames.end()) {
    return trace->times_seconds.back();
  }
  const size_t right = static_cast<size_t>(upper - trace->frames.begin());
  if (trace->frames[right] == frame) {
    return trace->times_seconds[right];
  }
  const size_t left = right - 1;
  const double fraction = static_cast<double>(frame - trace->frames[left]) /
                          static_cast<double>(trace->frames[right] -
                                              trace->frames[left]);
  return trace->times_seconds[left] +
         fraction * (trace->times_seconds[right] - trace->times_seconds[left]);
}

int64_t eyeAngleTimelineNearestFrame(const EyeAngleTimelineWindow& window,
                                     double time_seconds,
                                     double fallback_frames_per_second) {
  const auto* trace = mappingTrace(window);
  if (trace == nullptr) {
    return fallback_frames_per_second > 0.0 && std::isfinite(time_seconds)
               ? static_cast<int64_t>(
                     std::llround(time_seconds * fallback_frames_per_second))
               : -1;
  }
  const auto upper = std::lower_bound(trace->times_seconds.begin(),
                                      trace->times_seconds.end(), time_seconds);
  if (upper == trace->times_seconds.begin()) {
    return trace->frames.front();
  }
  if (upper == trace->times_seconds.end()) {
    return trace->frames.back();
  }
  const size_t right =
      static_cast<size_t>(upper - trace->times_seconds.begin());
  const size_t left = right - 1;
  const double time_span =
      trace->times_seconds[right] - trace->times_seconds[left];
  if (!std::isfinite(time_span) || time_span <= 0.0) {
    return trace->frames[left];
  }
  const double fraction =
      std::clamp((time_seconds - trace->times_seconds[left]) / time_span, 0.0,
                 1.0);
  return static_cast<int64_t>(std::llround(
      static_cast<double>(trace->frames[left]) +
      fraction * static_cast<double>(trace->frames[right] -
                                     trace->frames[left])));
}

EyeAngleTimelineWindow buildEyeAngleTimelineWindow(
    const EyeAngleTimelineDescriptor& descriptor,
    const EyeAngleTimelineRequest& request, int64_t series_first_frame,
    const std::vector<double>& frame_times,
    const std::vector<EyeAngleTimelineFieldSeries>& fields) {
  std::unordered_map<std::string, std::vector<double>> mapped;
  for (const auto& field : fields) {
    mapped[field.source_name] = field.frame_values;
  }
  return buildWindow(descriptor, request, series_first_frame, frame_times,
                     mapped);
}

std::unique_ptr<EyeAngleTimelineRepository> MakeEyeAngleTimelineRepository(
    EyeAngleTimelineDescriptor descriptor, std::vector<double> frame_times,
    std::vector<EyeAngleTimelineFieldSeries> fields) {
  return std::make_unique<VectorRepository>(
      std::move(descriptor), std::move(frame_times), std::move(fields));
}

}  // namespace crimson::timeline
