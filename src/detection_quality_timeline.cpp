#include "detection_quality_timeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace crimson::timeline {
namespace {

using Clock = std::chrono::steady_clock;

void assignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

double median(std::vector<double> values) {
  if (values.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  const double upper = values[middle];
  if (values.size() % 2 != 0) {
    return upper;
  }
  const double lower =
      *std::max_element(values.begin(), values.begin() + middle);
  return 0.5 * (lower + upper);
}

bool validOffsets(const std::vector<int64_t> &offsets, size_t frame_count,
                  size_t row_count) {
  return offsets.size() == frame_count + 1 && !offsets.empty() &&
         offsets.front() == 0 && offsets.back() >= 0 &&
         static_cast<size_t>(offsets.back()) == row_count &&
         std::is_sorted(offsets.begin(), offsets.end());
}

class VectorRepository final : public DetectionQualityTimelineRepository {
public:
  VectorRepository(DetectionQualityTimelineDescriptor descriptor,
                   DetectionQualityColumnData columns)
      : descriptor_(std::move(descriptor)), columns_(std::move(columns)) {}

  const DetectionQualityTimelineDescriptor &descriptor() const override {
    return descriptor_;
  }

  DetectionQualityTimelineWindow
  resolveWindow(int64_t first_camera_frame,
                int64_t last_camera_frame) const override {
    const auto started = Clock::now();
    DetectionQualityTimelineWindow result;
    if (first_camera_frame < 0 || last_camera_frame < first_camera_frame ||
        static_cast<size_t>(last_camera_frame) >= descriptor_.frame_count) {
      result.status = DetectionQualityTimelineStatus::OutOfRange;
    } else {
      const size_t first = static_cast<size_t>(first_camera_frame);
      const size_t last = static_cast<size_t>(last_camera_frame);
      const size_t source_first =
          static_cast<size_t>(columns_.source_frame_row_offsets[first]);
      const size_t source_last =
          static_cast<size_t>(columns_.source_frame_row_offsets[last + 1]);
      const size_t instance_first =
          static_cast<size_t>(columns_.instance_frame_row_offsets[first]);
      const size_t instance_last =
          static_cast<size_t>(columns_.instance_frame_row_offsets[last + 1]);
      std::vector<float> scores(columns_.source_scores.begin() + source_first,
                                columns_.source_scores.begin() + source_last);
      std::vector<uint8_t> decisions(
          columns_.source_decision_codes.begin() + source_first,
          columns_.source_decision_codes.begin() + source_last);
      std::vector<uint16_t> reasons(
          columns_.source_reason_codes.begin() + source_first,
          columns_.source_reason_codes.begin() + source_last);
      std::vector<uint8_t> kinds(
          columns_.instance_source_kind_codes.begin() + instance_first,
          columns_.instance_source_kind_codes.begin() + instance_last);
      result = buildDetectionQualityTimelineWindow(
          descriptor_, first_camera_frame, last_camera_frame,
          columns_.source_frame_row_offsets, source_first, scores, decisions,
          reasons, columns_.instance_frame_row_offsets, instance_first, kinds);
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.range_reads;
    metrics_.maximum_range_read_ms =
        std::max(metrics_.maximum_range_read_ms, elapsed);
    if (result.ready()) {
      metrics_.source_rows_read += result.source_rows_read;
      metrics_.instance_rows_read += result.instance_rows_read;
      metrics_.decoded_bytes += result.decoded_bytes;
    } else if (result.status == DetectionQualityTimelineStatus::ReadFailed) {
      ++metrics_.failed_reads;
      metrics_.last_error = result.error;
    }
    return result;
  }

  DetectionQualityTimelineOverview
  resolveOverview(size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
                  const std::function<bool()> &cancelled) const override {
    (void)maximum_decoded_bytes;
    const auto started = Clock::now();
    DetectionQualityOverviewAccumulator accumulator(descriptor_.frame_count,
                                                    maximum_points_per_trace,
                                                    descriptor_.source_audit);
    DetectionQualityTimelineOverview result;
    std::string error;
    for (size_t frame = 0; frame < descriptor_.frame_count; ++frame) {
      if (cancelled && cancelled()) {
        result.error = "Detection-quality overview was cancelled";
        return result;
      }
      const size_t source_first =
          static_cast<size_t>(columns_.source_frame_row_offsets[frame]);
      const size_t source_last =
          static_cast<size_t>(columns_.source_frame_row_offsets[frame + 1]);
      const size_t instance_first =
          static_cast<size_t>(columns_.instance_frame_row_offsets[frame]);
      const size_t instance_last =
          static_cast<size_t>(columns_.instance_frame_row_offsets[frame + 1]);
      const float *scores = source_last > source_first
                                ? columns_.source_scores.data() + source_first
                                : nullptr;
      const uint8_t *decisions =
          descriptor_.source_audit && source_last > source_first
              ? columns_.source_decision_codes.data() + source_first
              : nullptr;
      const uint8_t *kinds =
          descriptor_.source_audit && instance_last > instance_first
              ? columns_.instance_source_kind_codes.data() + instance_first
              : nullptr;
      if (!accumulator.addFrame(
              static_cast<int64_t>(frame), scores, decisions,
              source_last - source_first, kinds,
              descriptor_.source_audit ? instance_last - instance_first : 0,
              &error)) {
        result.error = std::move(error);
        break;
      }
    }
    if (result.error.empty()) {
      result = accumulator.finish();
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.overview_reads;
    metrics_.maximum_overview_read_ms =
        std::max(metrics_.maximum_overview_read_ms, elapsed);
    if (result.ready()) {
      metrics_.overview_source_rows_read += result.source_rows_read;
      metrics_.overview_instance_rows_read += result.instance_rows_read;
      metrics_.overview_decoded_bytes += result.decoded_bytes;
    } else if (!(cancelled && cancelled())) {
      ++metrics_.failed_overview_reads;
      metrics_.last_error = result.error;
    }
    return result;
  }

  DetectionQualityTimelineRepositoryMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
  }

private:
  DetectionQualityTimelineDescriptor descriptor_;
  DetectionQualityColumnData columns_;
  mutable std::mutex metrics_mutex_;
  mutable DetectionQualityTimelineRepositoryMetrics metrics_;
};

} // namespace

DetectionQualityOverviewAccumulator::DetectionQualityOverviewAccumulator(
    size_t frame_count, size_t maximum_points_per_trace, bool source_audit)
    : frame_count_(frame_count), source_audit_(source_audit) {
  if (frame_count_ == 0 || maximum_points_per_trace < 2) {
    return;
  }
  bin_count_ =
      std::min(frame_count_, std::max<size_t>(1, maximum_points_per_trace / 2));
  source_confidence_.resize(bin_count_);
  accepted_confidence_.resize(bin_count_);
  source_count_.resize(bin_count_);
  accepted_count_.resize(bin_count_);
  filtered_count_.resize(bin_count_);
  duplicate_count_.resize(bin_count_);
  manual_clear_count_.resize(bin_count_);
  manual_count_.resize(bin_count_);
}

bool DetectionQualityOverviewAccumulator::addFrame(
    int64_t camera_frame, const float *source_scores,
    const uint8_t *source_decisions, size_t source_count,
    const uint8_t *instance_source_kinds, size_t instance_count,
    std::string *error) {
  if (bin_count_ == 0 || camera_frame < 0 ||
      static_cast<size_t>(camera_frame) >= frame_count_ ||
      (source_count > 0 && source_scores == nullptr) ||
      (source_audit_ && source_count > 0 && source_decisions == nullptr) ||
      (source_audit_ && instance_count > 0 &&
       instance_source_kinds == nullptr)) {
    assignError(error, "Detection-quality overview input is invalid");
    return false;
  }
  const size_t bin_width =
      frame_count_ / bin_count_ + (frame_count_ % bin_count_ != 0);
  const size_t bin =
      std::min(bin_count_ - 1, static_cast<size_t>(camera_frame) / bin_width);
  const auto update = [camera_frame](Extremum *destination, double value) {
    if (!destination->has_value || value < destination->minimum) {
      destination->minimum = value;
      destination->minimum_frame = camera_frame;
    }
    if (!destination->has_value || value > destination->maximum) {
      destination->maximum = value;
      destination->maximum_frame = camera_frame;
    }
    destination->has_value = true;
  };
  size_t accepted = 0;
  size_t filtered = 0;
  size_t duplicate = 0;
  size_t manual_clear = 0;
  for (size_t row = 0; row < source_count; ++row) {
    const float score = source_scores[row];
    if (!std::isfinite(score) || score < 0.0f || score > 1.0f) {
      assignError(error,
                  "Detection-quality overview score is invalid at frame " +
                      std::to_string(camera_frame));
      return false;
    }
    update(&source_confidence_[bin], score);
    const uint8_t decision = source_audit_ ? source_decisions[row] : 0;
    switch (decision) {
    case 0:
      ++accepted;
      update(&accepted_confidence_[bin], score);
      break;
    case 1:
      ++filtered;
      break;
    case 2:
      ++duplicate;
      break;
    case 3:
      ++manual_clear;
      break;
    default:
      assignError(error,
                  "Detection-quality overview decision is invalid at frame " +
                      std::to_string(camera_frame));
      return false;
    }
  }
  size_t manual = 0;
  if (source_audit_) {
    for (size_t row = 0; row < instance_count; ++row) {
      if (instance_source_kinds[row] == 3) {
        ++manual;
      } else if (instance_source_kinds[row] != 1) {
        assignError(
            error,
            "Detection-quality overview source kind is invalid at frame " +
                std::to_string(camera_frame));
        return false;
      }
    }
  }
  update(&source_count_[bin], static_cast<double>(source_count));
  update(&accepted_count_[bin], static_cast<double>(accepted));
  update(&filtered_count_[bin], static_cast<double>(filtered));
  update(&duplicate_count_[bin], static_cast<double>(duplicate));
  update(&manual_clear_count_[bin], static_cast<double>(manual_clear));
  update(&manual_count_[bin], static_cast<double>(manual));
  source_rows_read_ += source_count;
  instance_rows_read_ += source_audit_ ? instance_count : 0;
  return true;
}

DetectionQualityTimelineOverview DetectionQualityOverviewAccumulator::finish() {
  DetectionQualityTimelineOverview result;
  if (bin_count_ == 0) {
    result.error = "Detection-quality overview configuration is invalid";
    return result;
  }
  result.frame_count = frame_count_;
  result.source_rows_read = source_rows_read_;
  result.instance_rows_read = instance_rows_read_;
  result.decoded_bytes = source_rows_read_ * sizeof(float);
  if (source_audit_) {
    result.decoded_bytes += source_rows_read_ * sizeof(uint8_t) +
                            instance_rows_read_ * sizeof(uint8_t);
  }
  const auto append = [](const Extremum &source,
                         DetectionQualityOverviewTrace *destination) {
    if (!source.has_value) {
      return;
    }
    const auto append_value = [&](int64_t frame, double value) {
      destination->camera_frames.push_back(frame);
      destination->values.push_back(value);
    };
    if (source.minimum_frame < source.maximum_frame) {
      append_value(source.minimum_frame, source.minimum);
      append_value(source.maximum_frame, source.maximum);
    } else if (source.maximum_frame < source.minimum_frame) {
      append_value(source.maximum_frame, source.maximum);
      append_value(source.minimum_frame, source.minimum);
    } else {
      append_value(source.minimum_frame, source.minimum);
      if (source.maximum != source.minimum) {
        append_value(source.maximum_frame, source.maximum);
      }
    }
  };
  for (size_t bin = 0; bin < bin_count_; ++bin) {
    append(source_confidence_[bin], &result.source_confidence);
    append(accepted_confidence_[bin], &result.accepted_confidence);
    append(source_count_[bin], &result.source_count);
    append(accepted_count_[bin], &result.accepted_count);
    append(filtered_count_[bin], &result.filtered_count);
    append(duplicate_count_[bin], &result.duplicate_count);
    append(manual_clear_count_[bin], &result.manual_clear_count);
    append(manual_count_[bin], &result.manual_count);
  }
  result.status = DetectionQualityTimelineStatus::Ready;
  return result;
}

DetectionQualityTimelineOverview
DetectionQualityTimelineRepository::resolveOverview(
    size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
    const std::function<bool()> &cancelled) const {
  (void)maximum_points_per_trace;
  (void)maximum_decoded_bytes;
  (void)cancelled;
  DetectionQualityTimelineOverview result;
  result.error = "Detection-quality overview is not supported";
  return result;
}

DetectionQualityTimelineWindow buildDetectionQualityTimelineWindow(
    const DetectionQualityTimelineDescriptor &descriptor,
    int64_t first_camera_frame, int64_t last_camera_frame,
    const std::vector<int64_t> &source_offsets, size_t first_source_row,
    const std::vector<float> &source_scores,
    const std::vector<uint8_t> &source_decisions,
    const std::vector<uint16_t> &source_reasons,
    const std::vector<int64_t> &instance_offsets, size_t first_instance_row,
    const std::vector<uint8_t> &instance_kinds) {
  DetectionQualityTimelineWindow window;
  window.first_camera_frame = first_camera_frame;
  window.last_camera_frame = last_camera_frame;
  if (!descriptor.ready() || first_camera_frame < 0 ||
      last_camera_frame < first_camera_frame ||
      static_cast<size_t>(last_camera_frame) >= descriptor.frame_count ||
      source_offsets.size() != descriptor.frame_count + 1 ||
      instance_offsets.size() != descriptor.frame_count + 1) {
    window.status = DetectionQualityTimelineStatus::OutOfRange;
    window.error = "Detection-quality frame range is invalid";
    return window;
  }
  const size_t first_frame = static_cast<size_t>(first_camera_frame);
  const size_t last_frame = static_cast<size_t>(last_camera_frame);
  const size_t expected_source_first =
      static_cast<size_t>(source_offsets[first_frame]);
  const size_t expected_source_last =
      static_cast<size_t>(source_offsets[last_frame + 1]);
  const size_t expected_instance_first =
      static_cast<size_t>(instance_offsets[first_frame]);
  const size_t expected_instance_last =
      static_cast<size_t>(instance_offsets[last_frame + 1]);
  const size_t source_count = expected_source_last - expected_source_first;
  const size_t instance_count =
      expected_instance_last - expected_instance_first;
  if (first_source_row != expected_source_first ||
      first_instance_row != expected_instance_first ||
      source_scores.size() != source_count ||
      source_decisions.size() != source_count ||
      source_reasons.size() != source_count ||
      instance_kinds.size() != instance_count) {
    window.status = DetectionQualityTimelineStatus::ReadFailed;
    window.error = "Detection-quality row ranges are inconsistent";
    return window;
  }

  std::unordered_map<uint16_t, size_t> reason_indices;
  for (size_t index = 0; index < descriptor.source_reason_codes.size();
       ++index) {
    reason_indices[descriptor.source_reason_codes[index].code] = index;
  }
  window.frames.reserve(last_frame - first_frame + 1);
  for (size_t frame = first_frame; frame <= last_frame; ++frame) {
    DetectionQualityFrame summary;
    summary.camera_frame = static_cast<int64_t>(frame);
    summary.reason_counts.resize(descriptor.source_reason_codes.size());
    const size_t source_begin =
        static_cast<size_t>(source_offsets[frame]) - first_source_row;
    const size_t source_end =
        static_cast<size_t>(source_offsets[frame + 1]) - first_source_row;
    const size_t instance_begin =
        static_cast<size_t>(instance_offsets[frame]) - first_instance_row;
    const size_t instance_end =
        static_cast<size_t>(instance_offsets[frame + 1]) - first_instance_row;
    summary.source_count = static_cast<uint32_t>(source_end - source_begin);
    std::vector<double> scores;
    std::vector<double> accepted_scores;
    scores.reserve(source_end - source_begin);
    accepted_scores.reserve(source_end - source_begin);
    for (size_t row = source_begin; row < source_end; ++row) {
      const float score = source_scores[row];
      const uint8_t decision = source_decisions[row];
      const uint16_t reason = source_reasons[row];
      if (!std::isfinite(score) || score < 0.0f || score > 1.0f ||
          decision > 3 || reason_indices.find(reason) == reason_indices.end()) {
        window.status = DetectionQualityTimelineStatus::ReadFailed;
        window.error = "Detection-quality source value contract failed";
        window.frames.clear();
        return window;
      }
      scores.push_back(score);
      switch (decision) {
      case 0:
        ++summary.accepted_count;
        accepted_scores.push_back(score);
        break;
      case 1:
        ++summary.filtered_count;
        break;
      case 2:
        ++summary.duplicate_count;
        break;
      case 3:
        ++summary.manual_clear_count;
        break;
      }
      ++summary.reason_counts[reason_indices.at(reason)];
    }
    for (size_t row = instance_begin; row < instance_end; ++row) {
      const uint8_t kind = instance_kinds[row];
      if (kind == 3) {
        ++summary.manual_count;
      } else if (kind != 1) {
        window.status = DetectionQualityTimelineStatus::ReadFailed;
        window.error = "Detection-quality instance source kind is invalid";
        window.frames.clear();
        return window;
      }
    }
    if (!scores.empty()) {
      const auto extrema = std::minmax_element(scores.begin(), scores.end());
      summary.score_min = *extrema.first;
      summary.score_max = *extrema.second;
      summary.score_median = median(scores);
    }
    summary.accepted_score_median = median(std::move(accepted_scores));
    window.frames.push_back(std::move(summary));
  }
  window.source_rows_read = source_count;
  window.instance_rows_read = descriptor.source_audit ? instance_count : 0;
  window.decoded_bytes = source_count * sizeof(float);
  if (descriptor.source_audit) {
    window.decoded_bytes +=
        source_count * (sizeof(uint8_t) + sizeof(uint16_t)) +
        instance_count * sizeof(uint8_t);
  }
  window.status = DetectionQualityTimelineStatus::Ready;
  return window;
}

std::unique_ptr<DetectionQualityTimelineRepository>
MakeDetectionQualityTimelineRepository(
    DetectionQualityTimelineDescriptor descriptor,
    DetectionQualityColumnData columns, std::string *error) {
  if (!descriptor.ready() ||
      !validOffsets(columns.source_frame_row_offsets, descriptor.frame_count,
                    descriptor.source_row_count) ||
      !validOffsets(columns.instance_frame_row_offsets, descriptor.frame_count,
                    descriptor.instance_row_count) ||
      columns.source_scores.size() != descriptor.source_row_count ||
      columns.source_decision_codes.size() != descriptor.source_row_count ||
      columns.source_reason_codes.size() != descriptor.source_row_count ||
      columns.instance_source_kind_codes.size() !=
          descriptor.instance_row_count) {
    assignError(error, "Detection-quality column contract is invalid");
    return nullptr;
  }
  if (error) {
    error->clear();
  }
  return std::make_unique<VectorRepository>(std::move(descriptor),
                                            std::move(columns));
}

const char *
detectionQualityTimelineStatusName(DetectionQualityTimelineStatus status) {
  switch (status) {
  case DetectionQualityTimelineStatus::Ready:
    return "ready";
  case DetectionQualityTimelineStatus::OutOfRange:
    return "out of range";
  case DetectionQualityTimelineStatus::ReadFailed:
    return "read failed";
  }
  return "unknown";
}

} // namespace crimson::timeline
