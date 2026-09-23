#pragma once

#include "detection_quality_timeline.h"
#include "keypoint_quality_timeline.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace crimson::gui {

enum class QualityTimelineLoadState {
  Closed,
  Opening,
  Ready,
  Failed,
};

struct DetectionOverviewSeries {
  std::vector<double> times;
  std::vector<double> values;
};

struct DetectionQualityTimelineControls {
  float half_span_seconds = 10.0f;
  bool full_recording = false;
  bool show_score_range = true;
  bool show_source_median = true;
  bool show_accepted_median = true;
  bool show_counts = true;
  bool show_reasons = true;
  std::unordered_map<uint16_t, bool> reason_visibility;
  std::shared_ptr<const timeline::DetectionQualityTimelineWindow>
      prepared_window;
  std::shared_ptr<const timeline::DetectionQualityTimelineOverview>
      prepared_overview;
  double prepared_fps = 0.0;
  std::vector<double> times;
  std::vector<double> score_min;
  std::vector<double> score_median;
  std::vector<double> score_max;
  std::vector<double> accepted_score_median;
  std::vector<double> source_counts;
  std::vector<double> accepted_counts;
  std::vector<double> filtered_counts;
  std::vector<double> duplicate_counts;
  std::vector<double> manual_clear_counts;
  std::vector<double> manual_counts;
  std::vector<std::vector<double>> reason_counts;
  DetectionOverviewSeries overview_source_confidence;
  DetectionOverviewSeries overview_accepted_confidence;
  DetectionOverviewSeries overview_source_count;
  DetectionOverviewSeries overview_accepted_count;
  DetectionOverviewSeries overview_filtered_count;
  DetectionOverviewSeries overview_duplicate_count;
  DetectionOverviewSeries overview_manual_clear_count;
  DetectionOverviewSeries overview_manual_count;
};

struct KeypointQualityTimelineControls {
  float half_span_seconds = 10.0f;
  bool full_recording = false;
  bool show_counts = true;
  bool show_metrics = true;
  bool show_findings = true;
  std::unordered_map<size_t, bool> keypoint_visibility;
  std::unordered_map<uint16_t, bool> review_visibility;
  std::unordered_map<uint16_t, bool> reason_visibility;
  std::shared_ptr<const timeline::KeypointQualityTimelineWindow>
      prepared_window;
  std::shared_ptr<const timeline::KeypointQualityTimelineOverview>
      prepared_overview;
  double prepared_fps = 0.0;
  std::vector<double> times;
  std::vector<double> pose_confidence;
  std::vector<std::vector<double>> keypoint_confidence;
  std::vector<double> observation_counts;
  std::vector<double> source_success_counts;
  std::vector<double> refined_success_counts;
  std::vector<double> usable_counts;
  std::vector<double> proposed_usable_counts;
  std::vector<double> edited_keypoint_counts;
  std::vector<double> flip_corrected_counts;
  std::vector<std::vector<double>> pose_metrics;
  std::vector<std::vector<double>> keypoint_metrics;
  std::vector<std::vector<double>> keypoint_flag_counts;
  std::vector<std::vector<double>> pose_flag_counts;
  std::vector<std::vector<double>> review_counts;
  std::vector<std::vector<double>> reason_counts;
  std::vector<double> overview_pose_times;
  std::vector<double> overview_pose_confidence;
  std::vector<std::vector<double>> overview_keypoint_times;
  std::vector<std::vector<double>> overview_keypoint_confidence;
};

using TimelineSeekRequest = std::function<bool(int64_t)>;

bool drawDetectionQualityTimelineWindow(
    DetectionQualityTimelineControls *controls, bool *open,
    QualityTimelineLoadState load_state,
    const timeline::DetectionQualityTimelineDescriptor *descriptor,
    const std::shared_ptr<const timeline::DetectionQualityTimelineWindow>
        &window,
    const std::shared_ptr<const timeline::DetectionQualityTimelineOverview>
        &overview,
    const std::string &error, int64_t current_frame, double fps,
    const TimelineSeekRequest &request_seek, bool interactive);

bool drawKeypointQualityTimelineWindow(
    KeypointQualityTimelineControls *controls, bool *open,
    QualityTimelineLoadState load_state,
    const timeline::KeypointQualityTimelineDescriptor *descriptor,
    const std::shared_ptr<const timeline::KeypointQualityTimelineWindow>
        &window,
    const std::shared_ptr<const timeline::KeypointQualityTimelineOverview>
        &overview,
    const std::string &error, int64_t current_frame, double fps,
    const TimelineSeekRequest &request_seek, bool interactive);

} // namespace crimson::gui
