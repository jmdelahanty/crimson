#include "keypoint_quality_timeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <type_traits>
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
  return 0.5 *
         (upper + *std::max_element(values.begin(), values.begin() + middle));
}

bool validOffsets(const std::vector<int64_t> &offsets, size_t frame_count,
                  size_t row_count) {
  return offsets.size() == frame_count + 1 && !offsets.empty() &&
         offsets.front() == 0 && offsets.back() >= 0 &&
         static_cast<size_t>(offsets.back()) == row_count &&
         std::is_sorted(offsets.begin(), offsets.end());
}

template <typename Descriptor>
std::unordered_map<uint16_t, size_t>
indexCodes(const std::vector<Descriptor> &values) {
  std::unordered_map<uint16_t, size_t> result;
  for (size_t index = 0; index < values.size(); ++index) {
    const uint16_t value = [&] {
      if constexpr (std::is_same_v<Descriptor, KeypointQualityFlagDescriptor>) {
        return values[index].mask;
      } else {
        return values[index].code;
      }
    }();
    result.emplace(value, index);
  }
  return result;
}

bool validColumnSizes(const KeypointQualityTimelineDescriptor &descriptor,
                      size_t rows, const KeypointQualityColumnData &columns) {
  const size_t keypoints = descriptor.keypoint_count;
  const size_t keypoint_metrics = descriptor.keypoint_metrics.size();
  const size_t pose_metrics = descriptor.pose_metrics.size();
  return columns.selected_instance_keys.size() == rows &&
         columns.quality_instance_keys.size() == rows &&
         columns.keypoint_confidences.size() == rows * keypoints &&
         columns.keypoint_valid.size() == rows * keypoints &&
         columns.pose_confidences.size() == rows &&
         columns.source_success.size() == rows &&
         columns.refined_success.size() == rows &&
         columns.keypoint_edit_flags.size() == rows * keypoints &&
         columns.flip_corrected.size() == rows &&
         columns.usable_keypoints.size() == rows &&
         columns.review_state_codes.size() == rows &&
         columns.reason_codes.size() == rows &&
         columns.keypoint_metric_values.size() ==
             rows * keypoints * keypoint_metrics &&
         columns.keypoint_metric_valid.size() ==
             rows * keypoints * keypoint_metrics &&
         columns.pose_metric_values.size() == rows * pose_metrics &&
         columns.pose_metric_valid.size() == rows * pose_metrics &&
         columns.keypoint_quality_flags.size() == rows * keypoints &&
         columns.pose_quality_flags.size() == rows &&
         columns.proposed_keypoint_valid.size() == rows * keypoints &&
         columns.proposed_pose_usable.size() == rows;
}

class VectorRepository final : public KeypointQualityTimelineRepository {
public:
  VectorRepository(KeypointQualityTimelineDescriptor descriptor,
                   KeypointQualityColumnData columns)
      : descriptor_(std::move(descriptor)), columns_(std::move(columns)) {}

  const KeypointQualityTimelineDescriptor &descriptor() const override {
    return descriptor_;
  }

  KeypointQualityTimelineWindow
  resolveWindow(int64_t first_camera_frame,
                int64_t last_camera_frame) const override {
    const auto started = Clock::now();
    KeypointQualityTimelineWindow result;
    if (first_camera_frame < 0 || last_camera_frame < first_camera_frame ||
        static_cast<size_t>(last_camera_frame) >= descriptor_.frame_count) {
      result.status = KeypointQualityTimelineStatus::OutOfRange;
    } else {
      const size_t first = static_cast<size_t>(first_camera_frame);
      const size_t last = static_cast<size_t>(last_camera_frame);
      const size_t first_row =
          static_cast<size_t>(columns_.frame_row_offsets[first]);
      const size_t last_row =
          static_cast<size_t>(columns_.frame_row_offsets[last + 1]);
      const size_t rows = last_row - first_row;
      const size_t keypoints = descriptor_.keypoint_count;
      const size_t keypoint_metrics = descriptor_.keypoint_metrics.size();
      const size_t pose_metrics = descriptor_.pose_metrics.size();
      KeypointQualityColumnData page;
      page.selected_instance_keys.assign(
          columns_.selected_instance_keys.begin() + first_row,
          columns_.selected_instance_keys.begin() + last_row);
      page.quality_instance_keys.assign(
          columns_.quality_instance_keys.begin() + first_row,
          columns_.quality_instance_keys.begin() + last_row);
#define COPY_ROWS(member, width)                                               \
  page.member.assign(columns_.member.begin() + first_row * (width),            \
                     columns_.member.begin() + last_row * (width))
      COPY_ROWS(keypoint_confidences, keypoints);
      COPY_ROWS(keypoint_valid, keypoints);
      COPY_ROWS(pose_confidences, 1);
      COPY_ROWS(source_success, 1);
      COPY_ROWS(refined_success, 1);
      COPY_ROWS(keypoint_edit_flags, keypoints);
      COPY_ROWS(flip_corrected, 1);
      COPY_ROWS(usable_keypoints, 1);
      COPY_ROWS(review_state_codes, 1);
      COPY_ROWS(reason_codes, 1);
      COPY_ROWS(keypoint_metric_values, keypoints * keypoint_metrics);
      COPY_ROWS(keypoint_metric_valid, keypoints * keypoint_metrics);
      COPY_ROWS(pose_metric_values, pose_metrics);
      COPY_ROWS(pose_metric_valid, pose_metrics);
      COPY_ROWS(keypoint_quality_flags, keypoints);
      COPY_ROWS(pose_quality_flags, 1);
      COPY_ROWS(proposed_keypoint_valid, keypoints);
      COPY_ROWS(proposed_pose_usable, 1);
#undef COPY_ROWS
      result = buildKeypointQualityTimelineWindow(
          descriptor_, first_camera_frame, last_camera_frame,
          columns_.frame_row_offsets, first_row, page);
      (void)rows;
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.range_reads;
    metrics_.maximum_range_read_ms =
        std::max(metrics_.maximum_range_read_ms, elapsed);
    if (result.ready()) {
      metrics_.rows_read += result.rows_read;
      metrics_.decoded_bytes += result.decoded_bytes;
    } else if (result.status == KeypointQualityTimelineStatus::ReadFailed) {
      ++metrics_.failed_reads;
      metrics_.last_error = result.error;
    }
    return result;
  }

  KeypointQualityTimelineOverview
  resolveOverview(size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
                  const std::function<bool()> &cancelled) const override {
    (void)maximum_decoded_bytes;
    const auto started = Clock::now();
    KeypointQualityOverviewAccumulator accumulator(descriptor_.frame_count,
                                                   descriptor_.keypoint_count,
                                                   maximum_points_per_trace);
    std::string error;
    for (size_t frame = 0; frame < descriptor_.frame_count; ++frame) {
      if (cancelled && cancelled()) {
        KeypointQualityTimelineOverview result;
        result.error = "Keypoint-quality overview was cancelled";
        return result;
      }
      const size_t first =
          static_cast<size_t>(columns_.frame_row_offsets[frame]);
      const size_t last =
          static_cast<size_t>(columns_.frame_row_offsets[frame + 1]);
      const float *pose =
          last > first ? columns_.pose_confidences.data() + first : nullptr;
      const uint8_t *pose_valid =
          last > first ? columns_.source_success.data() + first : nullptr;
      const float *points = last > first
                                ? columns_.keypoint_confidences.data() +
                                      first * descriptor_.keypoint_count
                                : nullptr;
      const uint8_t *point_valid = last > first
                                       ? columns_.keypoint_valid.data() +
                                             first * descriptor_.keypoint_count
                                       : nullptr;
      if (!accumulator.addFrame(static_cast<int64_t>(frame), pose, pose_valid,
                                points, point_valid, last - first, &error)) {
        KeypointQualityTimelineOverview result;
        result.error = std::move(error);
        return result;
      }
    }
    auto result = accumulator.finish();
    const double elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.overview_reads;
    metrics_.maximum_overview_read_ms =
        std::max(metrics_.maximum_overview_read_ms, elapsed);
    if (result.ready()) {
      metrics_.overview_rows_read += result.rows_read;
      metrics_.overview_decoded_bytes += result.decoded_bytes;
    } else {
      ++metrics_.failed_overview_reads;
      metrics_.last_error = result.error;
    }
    return result;
  }

  KeypointQualityTimelineRepositoryMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
  }

private:
  KeypointQualityTimelineDescriptor descriptor_;
  KeypointQualityColumnData columns_;
  mutable std::mutex metrics_mutex_;
  mutable KeypointQualityTimelineRepositoryMetrics metrics_;
};

} // namespace

KeypointQualityOverviewAccumulator::KeypointQualityOverviewAccumulator(
    size_t frame_count, size_t keypoint_count, size_t maximum_points_per_trace)
    : frame_count_(frame_count), keypoint_count_(keypoint_count) {
  if (frame_count_ == 0 || keypoint_count_ == 0 ||
      maximum_points_per_trace < 2) {
    return;
  }
  bin_count_ =
      std::min(frame_count_, std::max<size_t>(1, maximum_points_per_trace / 2));
  pose_extrema_.resize(bin_count_);
  keypoint_extrema_.resize(bin_count_ * keypoint_count_);
}

bool KeypointQualityOverviewAccumulator::addFrame(
    int64_t camera_frame, const float *pose_confidences,
    const uint8_t *pose_valid, const float *keypoint_confidences,
    const uint8_t *keypoint_valid, size_t observation_count,
    std::string *error) {
  if (bin_count_ == 0 || camera_frame < 0 ||
      static_cast<size_t>(camera_frame) >= frame_count_ ||
      (observation_count > 0 &&
       (pose_confidences == nullptr || pose_valid == nullptr ||
        keypoint_confidences == nullptr || keypoint_valid == nullptr))) {
    assignError(error, "Keypoint-quality overview input is invalid");
    return false;
  }
  const size_t bin_width =
      frame_count_ / bin_count_ + (frame_count_ % bin_count_ != 0);
  const size_t bin =
      std::min(bin_count_ - 1, static_cast<size_t>(camera_frame) / bin_width);
  const auto update = [&](Extremum *destination, float value) {
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
      return false;
    }
    if (!destination->has_value || value < destination->minimum) {
      destination->minimum = value;
      destination->minimum_frame = camera_frame;
    }
    if (!destination->has_value || value > destination->maximum) {
      destination->maximum = value;
      destination->maximum_frame = camera_frame;
    }
    destination->has_value = true;
    return true;
  };
  for (size_t row = 0; row < observation_count; ++row) {
    const bool pose_missing =
        std::isnan(pose_confidences[row]) && !pose_valid[row];
    if (!pose_missing && !update(&pose_extrema_[bin], pose_confidences[row])) {
      assignError(error,
                  "Keypoint-quality overview pose confidence is invalid at "
                  "frame " +
                      std::to_string(camera_frame) + " value " +
                      std::to_string(pose_confidences[row]));
      return false;
    }
    for (size_t point = 0; point < keypoint_count_; ++point) {
      const size_t index = row * keypoint_count_ + point;
      const bool point_missing =
          std::isnan(keypoint_confidences[index]) && !keypoint_valid[index];
      if (!point_missing &&
          !update(&keypoint_extrema_[bin * keypoint_count_ + point],
                  keypoint_confidences[index])) {
        assignError(error,
                    "Keypoint-quality overview point confidence is invalid at "
                    "frame " +
                        std::to_string(camera_frame) + " point " +
                        std::to_string(point) + " value " +
                        std::to_string(keypoint_confidences[index]));
        return false;
      }
    }
  }
  rows_read_ += observation_count;
  if (error) {
    error->clear();
  }
  return true;
}

KeypointQualityTimelineOverview KeypointQualityOverviewAccumulator::finish() {
  KeypointQualityTimelineOverview result;
  if (bin_count_ == 0) {
    result.error = "Keypoint-quality overview configuration is invalid";
    return result;
  }
  result.frame_count = frame_count_;
  result.rows_read = rows_read_;
  result.decoded_bytes =
      rows_read_ * (keypoint_count_ + 1) * (sizeof(float) + sizeof(uint8_t));
  result.keypoint_confidence.resize(keypoint_count_);
  const auto append = [](const Extremum &source,
                         KeypointQualityOverviewTrace *destination) {
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
    append(pose_extrema_[bin], &result.pose_confidence);
    for (size_t point = 0; point < keypoint_count_; ++point) {
      append(keypoint_extrema_[bin * keypoint_count_ + point],
             &result.keypoint_confidence[point]);
    }
  }
  result.status = KeypointQualityTimelineStatus::Ready;
  return result;
}

KeypointQualityTimelineOverview
KeypointQualityTimelineRepository::resolveOverview(
    size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
    const std::function<bool()> &cancelled) const {
  (void)maximum_points_per_trace;
  (void)maximum_decoded_bytes;
  (void)cancelled;
  KeypointQualityTimelineOverview result;
  result.error = "Keypoint-quality overview is not supported";
  return result;
}

KeypointQualityTimelineWindow buildKeypointQualityTimelineWindow(
    const KeypointQualityTimelineDescriptor &descriptor,
    int64_t first_camera_frame, int64_t last_camera_frame,
    const std::vector<int64_t> &offsets, size_t first_row,
    const KeypointQualityColumnData &columns) {
  KeypointQualityTimelineWindow window;
  window.first_camera_frame = first_camera_frame;
  window.last_camera_frame = last_camera_frame;
  if (!descriptor.ready() || first_camera_frame < 0 ||
      last_camera_frame < first_camera_frame ||
      static_cast<size_t>(last_camera_frame) >= descriptor.frame_count ||
      offsets.size() != descriptor.frame_count + 1) {
    window.status = KeypointQualityTimelineStatus::OutOfRange;
    window.error = "Keypoint-quality frame range is invalid";
    return window;
  }
  const size_t first_frame = static_cast<size_t>(first_camera_frame);
  const size_t last_frame = static_cast<size_t>(last_camera_frame);
  const size_t expected_first = static_cast<size_t>(offsets[first_frame]);
  const size_t expected_last = static_cast<size_t>(offsets[last_frame + 1]);
  const size_t rows = expected_last - expected_first;
  if (first_row != expected_first ||
      !validColumnSizes(descriptor, rows, columns) ||
      columns.selected_instance_keys != columns.quality_instance_keys) {
    window.status = KeypointQualityTimelineStatus::ReadFailed;
    window.error = "Keypoint-quality row ranges or identities disagree";
    return window;
  }

  const auto review_indices = indexCodes(descriptor.review_states);
  const auto reason_indices = indexCodes(descriptor.reason_codes);
  uint16_t known_keypoint_flags = 0;
  uint16_t known_pose_flags = 0;
  for (const auto &value : descriptor.keypoint_flags) {
    known_keypoint_flags =
        static_cast<uint16_t>(known_keypoint_flags | value.mask);
  }
  for (const auto &value : descriptor.pose_flags) {
    known_pose_flags = static_cast<uint16_t>(known_pose_flags | value.mask);
  }
  const size_t keypoints = descriptor.keypoint_count;
  const size_t keypoint_metrics = descriptor.keypoint_metrics.size();
  const size_t pose_metrics = descriptor.pose_metrics.size();
  window.frames.reserve(last_frame - first_frame + 1);
  for (size_t frame = first_frame; frame <= last_frame; ++frame) {
    KeypointQualityFrame summary;
    summary.camera_frame = static_cast<int64_t>(frame);
    summary.keypoint_confidence_medians.resize(
        keypoints, std::numeric_limits<double>::quiet_NaN());
    summary.valid_keypoint_counts.resize(keypoints);
    summary.proposed_valid_keypoint_counts.resize(keypoints);
    summary.keypoint_metric_medians.resize(
        keypoints * keypoint_metrics, std::numeric_limits<double>::quiet_NaN());
    summary.pose_metric_medians.resize(
        pose_metrics, std::numeric_limits<double>::quiet_NaN());
    summary.keypoint_flag_counts.resize(descriptor.keypoint_flags.size());
    summary.pose_flag_counts.resize(descriptor.pose_flags.size());
    summary.review_state_counts.resize(descriptor.review_states.size());
    summary.reason_code_counts.resize(descriptor.reason_codes.size());
    const size_t begin = static_cast<size_t>(offsets[frame]) - first_row;
    const size_t end = static_cast<size_t>(offsets[frame + 1]) - first_row;
    summary.observation_count = static_cast<uint32_t>(end - begin);
    std::vector<double> pose_confidences;
    std::vector<std::vector<double>> keypoint_confidences(keypoints);
    std::vector<std::vector<double>> keypoint_values(keypoints *
                                                     keypoint_metrics);
    std::vector<std::vector<double>> pose_values(pose_metrics);
    for (size_t row = begin; row < end; ++row) {
      const float pose_confidence = columns.pose_confidences[row];
      if (std::isfinite(pose_confidence) && pose_confidence >= 0.0f &&
          pose_confidence <= 1.0f) {
        pose_confidences.push_back(pose_confidence);
      } else if (!std::isnan(pose_confidence) || columns.source_success[row]) {
        window.error = "Keypoint pose confidence violates its contract";
        return window;
      }
      summary.source_success_count += columns.source_success[row] != 0;
      summary.refined_success_count += columns.refined_success[row] != 0;
      summary.usable_count += columns.usable_keypoints[row] != 0;
      summary.proposed_usable_count += columns.proposed_pose_usable[row] != 0;
      summary.flip_corrected_count += columns.flip_corrected[row] != 0;
      const auto review = review_indices.find(columns.review_state_codes[row]);
      const auto reason = reason_indices.find(columns.reason_codes[row]);
      if (review == review_indices.end() || reason == reason_indices.end() ||
          (columns.pose_quality_flags[row] & ~known_pose_flags) != 0) {
        window.error = "Keypoint quality code or pose flag is unregistered";
        return window;
      }
      ++summary.review_state_counts[review->second];
      ++summary.reason_code_counts[reason->second];
      for (size_t flag = 0; flag < descriptor.pose_flags.size(); ++flag) {
        summary.pose_flag_counts[flag] +=
            (columns.pose_quality_flags[row] &
             descriptor.pose_flags[flag].mask) != 0;
      }
      for (size_t metric = 0; metric < pose_metrics; ++metric) {
        const size_t index = row * pose_metrics + metric;
        const float value = columns.pose_metric_values[index];
        if (columns.pose_metric_valid[index]) {
          if (!std::isfinite(value)) {
            window.error = "Valid pose quality metric is not finite";
            return window;
          }
          pose_values[metric].push_back(value);
        } else if (!std::isnan(value)) {
          window.error = "Invalid pose quality metric is not canonical NaN";
          return window;
        }
      }
      for (size_t point = 0; point < keypoints; ++point) {
        const size_t point_index = row * keypoints + point;
        const float confidence = columns.keypoint_confidences[point_index];
        if ((columns.keypoint_quality_flags[point_index] &
             ~known_keypoint_flags) != 0) {
          window.error = "Keypoint quality flag is invalid";
          return window;
        }
        if (std::isfinite(confidence) && confidence >= 0.0f &&
            confidence <= 1.0f) {
          keypoint_confidences[point].push_back(confidence);
        } else if (!std::isnan(confidence) ||
                   columns.keypoint_valid[point_index]) {
          window.error = "Keypoint confidence violates its contract";
          return window;
        }
        summary.valid_keypoint_counts[point] +=
            columns.keypoint_valid[point_index] != 0;
        summary.proposed_valid_keypoint_counts[point] +=
            columns.proposed_keypoint_valid[point_index] != 0;
        summary.edited_keypoint_count +=
            columns.keypoint_edit_flags[point_index] != 0;
        for (size_t flag = 0; flag < descriptor.keypoint_flags.size(); ++flag) {
          summary.keypoint_flag_counts[flag] +=
              (columns.keypoint_quality_flags[point_index] &
               descriptor.keypoint_flags[flag].mask) != 0;
        }
        for (size_t metric = 0; metric < keypoint_metrics; ++metric) {
          const size_t metric_index =
              (row * keypoints + point) * keypoint_metrics + metric;
          const float value = columns.keypoint_metric_values[metric_index];
          if (columns.keypoint_metric_valid[metric_index]) {
            if (!std::isfinite(value)) {
              window.error = "Valid keypoint quality metric is not finite";
              return window;
            }
            keypoint_values[point * keypoint_metrics + metric].push_back(value);
          } else if (!std::isnan(value)) {
            window.error =
                "Invalid keypoint quality metric is not canonical NaN";
            return window;
          }
        }
      }
    }
    summary.pose_confidence_median = median(std::move(pose_confidences));
    for (size_t point = 0; point < keypoints; ++point) {
      summary.keypoint_confidence_medians[point] =
          median(std::move(keypoint_confidences[point]));
    }
    for (size_t index = 0; index < keypoint_values.size(); ++index) {
      summary.keypoint_metric_medians[index] =
          median(std::move(keypoint_values[index]));
    }
    for (size_t index = 0; index < pose_values.size(); ++index) {
      summary.pose_metric_medians[index] =
          median(std::move(pose_values[index]));
    }
    window.frames.push_back(std::move(summary));
  }
  window.rows_read = rows;
  window.decoded_bytes =
      rows *
      (2 * sizeof(uint64_t) + sizeof(float) + 6 * sizeof(uint8_t) +
       2 * sizeof(uint16_t) +
       keypoints * (sizeof(float) + 3 * sizeof(uint8_t) + sizeof(uint16_t)) +
       keypoints * keypoint_metrics * (sizeof(float) + sizeof(uint8_t)) +
       pose_metrics * (sizeof(float) + sizeof(uint8_t)));
  window.status = KeypointQualityTimelineStatus::Ready;
  return window;
}

std::unique_ptr<KeypointQualityTimelineRepository>
MakeKeypointQualityTimelineRepository(
    KeypointQualityTimelineDescriptor descriptor,
    KeypointQualityColumnData columns, std::string *error) {
  if (!descriptor.ready() ||
      !validOffsets(columns.frame_row_offsets, descriptor.frame_count,
                    descriptor.row_count) ||
      !validColumnSizes(descriptor, descriptor.row_count, columns)) {
    assignError(error, "Keypoint-quality column contract is invalid");
    return nullptr;
  }
  if (error) {
    error->clear();
  }
  return std::make_unique<VectorRepository>(std::move(descriptor),
                                            std::move(columns));
}

const char *
keypointQualityTimelineStatusName(KeypointQualityTimelineStatus status) {
  switch (status) {
  case KeypointQualityTimelineStatus::Ready:
    return "ready";
  case KeypointQualityTimelineStatus::OutOfRange:
    return "out of range";
  case KeypointQualityTimelineStatus::ReadFailed:
    return "read failed";
  }
  return "unknown";
}

} // namespace crimson::timeline
