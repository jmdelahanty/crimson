#include "gui/quality_timeline_window.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace crimson::gui {

bool drawDetectionQualityTimelineWindow(
    DetectionQualityTimelineControls *controls, bool *open,
    QualityTimelineLoadState load_state,
    const timeline::DetectionQualityTimelineDescriptor *descriptor,
    const std::shared_ptr<const timeline::DetectionQualityTimelineWindow>
        &window,
    const std::shared_ptr<const timeline::DetectionQualityTimelineOverview>
        &overview,
    const std::string &error, int64_t current_frame, double fps,
    const TimelineSeekRequest &request_seek, bool interactive) {
  if (controls == nullptr || open == nullptr || !*open) {
    return false;
  }
  ImGui::SetNextWindowSize(ImVec2(780.0f, 620.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Detection Timeline", open)) {
    ImGui::End();
    return false;
  }
  if (!interactive) {
    ImGui::BeginDisabled();
  }
  if (descriptor != nullptr) {
    ImGui::Text("%s  |  %s",
                descriptor->surface_kind ==
                        crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1
                    ? "Refined detections"
                    : "Canonical detections",
                descriptor->run_name.c_str());
    if (!descriptor->model_artifact_sha256.empty()) {
      ImGui::Text("Model SHA-256: %.12s...",
                  descriptor->model_artifact_sha256.c_str());
    }
    if (!descriptor->producer_id.empty()) {
      ImGui::Text("Producer: %s %s", descriptor->producer_id.c_str(),
                  descriptor->producer_version.c_str());
    }
  }
  if (ImGui::RadioButton("Local", !controls->full_recording)) {
    controls->full_recording = false;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Full recording", controls->full_recording)) {
    controls->full_recording = true;
  }
  if (!controls->full_recording) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::SliderFloat("Window (+/- s)", &controls->half_span_seconds, 1.0f,
                       60.0f, "%.0f");
    ImGui::SameLine();
    ImGui::Checkbox("Counts", &controls->show_counts);
    ImGui::SameLine();
    ImGui::Checkbox("Reasons", &controls->show_reasons);
  }

  if (load_state == QualityTimelineLoadState::Opening) {
    ImGui::TextUnformatted("Opening detection audit data...");
  } else if (load_state == QualityTimelineLoadState::Failed) {
    ImGui::TextWrapped("Detection timeline unavailable: %s", error.c_str());
  } else if (load_state != QualityTimelineLoadState::Ready ||
             descriptor == nullptr) {
    ImGui::TextUnformatted("Detection timeline is not open.");
  } else if (controls->full_recording && !overview) {
    ImGui::TextUnformatted("Loading full-recording detection overview...");
  } else if (controls->full_recording && !overview->ready()) {
    ImGui::TextWrapped("Detection overview unavailable: %s",
                       overview->error.c_str());
  } else if (!controls->full_recording && !window) {
    ImGui::TextUnformatted("Loading the visible detection window...");
  } else if (!controls->full_recording && !window->ready()) {
    ImGui::TextWrapped(
        "Detection timeline %s: %s",
        crimson::timeline::detectionQualityTimelineStatusName(window->status),
        window->error.c_str());
  } else if (controls->full_recording) {
    const double fps = fps;
    const double cursor_time = fps > 0.0 ? current_frame / fps : 0.0;
    const double recording_end =
        fps > 0.0 && overview->frame_count > 1
            ? static_cast<double>(overview->frame_count - 1) / fps
            : 1.0;
    if (controls->prepared_overview.get() != overview.get() ||
        controls->prepared_fps != fps) {
      controls->prepared_overview = overview;
      controls->prepared_fps = fps;
      const auto prepare_trace =
          [&](const crimson::timeline::DetectionQualityOverviewTrace &source,
              DetectionOverviewSeries *destination) {
            destination->times.clear();
            destination->values.clear();
            destination->times.reserve(source.camera_frames.size());
            destination->values.reserve(source.values.size());
            for (size_t index = 0; index < source.camera_frames.size();
                 ++index) {
              destination->times.push_back(
                  fps > 0.0 ? source.camera_frames[index] / fps : 0.0);
              destination->values.push_back(source.values[index]);
            }
          };
      prepare_trace(overview->source_confidence,
                    &controls->overview_source_confidence);
      prepare_trace(overview->accepted_confidence,
                    &controls->overview_accepted_confidence);
      prepare_trace(overview->source_count, &controls->overview_source_count);
      prepare_trace(overview->accepted_count,
                    &controls->overview_accepted_count);
      prepare_trace(overview->filtered_count,
                    &controls->overview_filtered_count);
      prepare_trace(overview->duplicate_count,
                    &controls->overview_duplicate_count);
      prepare_trace(overview->manual_clear_count,
                    &controls->overview_manual_clear_count);
      prepare_trace(overview->manual_count, &controls->overview_manual_count);
    }
    const auto seek_from_plot = [&]() {
      if (!interactive || !ImPlot::IsPlotHovered() ||
          !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || fps <= 0.0) {
        return false;
      }
      const int64_t frame = std::clamp<int64_t>(
          static_cast<int64_t>(std::llround(ImPlot::GetPlotMousePos().x * fps)),
          0, static_cast<int64_t>(overview->frame_count) - 1);
      return (request_seek ? request_seek(frame) : false);
    };
    bool camera_discontinuity = false;
    ImGui::Checkbox("Source min/max", &controls->show_source_median);
    if (descriptor->source_audit) {
      ImGui::SameLine();
      ImGui::Checkbox("Accepted min/max", &controls->show_accepted_median);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Counts", &controls->show_counts);
    if (ImPlot::BeginPlot("##detection-confidence-overview",
                          ImVec2(-1.0f, 250.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Confidence", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, recording_end, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
      if (controls->show_source_median) {
        ImPlot::PlotLine(
            "Source min/max", controls->overview_source_confidence.times.data(),
            controls->overview_source_confidence.values.data(),
            static_cast<int>(
                controls->overview_source_confidence.times.size()));
      }
      if (controls->show_accepted_median && descriptor->source_audit) {
        ImPlot::PlotLine(
            "Accepted min/max",
            controls->overview_accepted_confidence.times.data(),
            controls->overview_accepted_confidence.values.data(),
            static_cast<int>(
                controls->overview_accepted_confidence.times.size()));
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f),
                   "Frame %lld", static_cast<long long>(current_frame));
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (controls->show_counts &&
        ImPlot::BeginPlot("##detection-counts-overview", ImVec2(-1.0f, 240.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Detections", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, recording_end, ImPlotCond_Always);
      const auto plot = [](const char *label,
                           const DetectionOverviewSeries &series) {
        ImPlot::PlotLine(label, series.times.data(), series.values.data(),
                         static_cast<int>(series.times.size()));
      };
      plot("Source", controls->overview_source_count);
      if (descriptor->source_audit) {
        plot("Accepted", controls->overview_accepted_count);
        plot("Filtered", controls->overview_filtered_count);
        plot("Duplicate", controls->overview_duplicate_count);
        plot("Manual clear", controls->overview_manual_clear_count);
        plot("Manual", controls->overview_manual_count);
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (!interactive) {
      ImGui::EndDisabled();
    }
    ImGui::End();
    return camera_discontinuity;
  } else {
    const double fps = fps;
    const double cursor_time = fps > 0.0 ? current_frame / fps : 0.0;
    if (controls->prepared_window.get() != window.get() ||
        controls->prepared_fps != fps) {
      controls->prepared_window = window;
      controls->prepared_fps = fps;
      controls->times.clear();
      controls->score_min.clear();
      controls->score_median.clear();
      controls->score_max.clear();
      controls->accepted_score_median.clear();
      controls->source_counts.clear();
      controls->accepted_counts.clear();
      controls->filtered_counts.clear();
      controls->duplicate_counts.clear();
      controls->manual_clear_counts.clear();
      controls->manual_counts.clear();
      controls->reason_counts.assign(descriptor->source_reason_codes.size(),
                                     {});
      const size_t frame_count = window->frames.size();
      controls->times.reserve(frame_count);
      controls->score_min.reserve(frame_count);
      controls->score_median.reserve(frame_count);
      controls->score_max.reserve(frame_count);
      controls->accepted_score_median.reserve(frame_count);
      controls->source_counts.reserve(frame_count);
      controls->accepted_counts.reserve(frame_count);
      controls->filtered_counts.reserve(frame_count);
      controls->duplicate_counts.reserve(frame_count);
      controls->manual_clear_counts.reserve(frame_count);
      controls->manual_counts.reserve(frame_count);
      for (auto &counts : controls->reason_counts) {
        counts.reserve(frame_count);
      }
      for (const auto &frame : window->frames) {
        controls->times.push_back(fps > 0.0 ? frame.camera_frame / fps : 0.0);
        controls->score_min.push_back(frame.score_min);
        controls->score_median.push_back(frame.score_median);
        controls->score_max.push_back(frame.score_max);
        controls->accepted_score_median.push_back(frame.accepted_score_median);
        controls->source_counts.push_back(frame.source_count);
        controls->accepted_counts.push_back(frame.accepted_count);
        controls->filtered_counts.push_back(frame.filtered_count);
        controls->duplicate_counts.push_back(frame.duplicate_count);
        controls->manual_clear_counts.push_back(frame.manual_clear_count);
        controls->manual_counts.push_back(frame.manual_count);
        for (size_t reason_index = 0;
             reason_index < controls->reason_counts.size(); ++reason_index) {
          controls->reason_counts[reason_index].push_back(
              reason_index < frame.reason_counts.size()
                  ? frame.reason_counts[reason_index]
                  : 0);
        }
      }
    }
    const auto &times = controls->times;

    auto seekFromPlot = [&]() {
      if (!interactive || !ImPlot::IsPlotHovered() ||
          !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || fps <= 0.0) {
        return false;
      }
      const int64_t frame = std::clamp<int64_t>(
          static_cast<int64_t>(std::llround(ImPlot::GetPlotMousePos().x * fps)),
          window->first_camera_frame, window->last_camera_frame);
      return (request_seek ? request_seek(frame) : false);
    };

    bool camera_discontinuity = false;
    ImGui::Checkbox("Score range", &controls->show_score_range);
    ImGui::SameLine();
    ImGui::Checkbox("Source median", &controls->show_source_median);
    ImGui::SameLine();
    ImGui::Checkbox("Accepted median", &controls->show_accepted_median);
    if (ImPlot::BeginPlot("##detection-confidence", ImVec2(-1.0f, 230.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Confidence", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(
          ImAxis_X1, cursor_time - controls->half_span_seconds,
          cursor_time + controls->half_span_seconds, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
      const int count = static_cast<int>(times.size());
      if (controls->show_score_range) {
        ImPlot::PlotShaded("Source range", times.data(),
                           controls->score_min.data(),
                           controls->score_max.data(), count);
      }
      if (controls->show_source_median) {
        ImPlot::PlotLine("Source median", times.data(),
                         controls->score_median.data(), count);
      }
      if (controls->show_accepted_median && descriptor->source_audit) {
        ImPlot::PlotLine("Accepted median", times.data(),
                         controls->accepted_score_median.data(), count);
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f),
                   "Frame %lld", static_cast<long long>(current_frame));
      camera_discontinuity = seekFromPlot() || camera_discontinuity;
      ImPlot::EndPlot();
    }

    if (controls->show_counts &&
        ImPlot::BeginPlot("##detection-counts", ImVec2(-1.0f, 180.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Detections", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(
          ImAxis_X1, cursor_time - controls->half_span_seconds,
          cursor_time + controls->half_span_seconds, ImPlotCond_Always);
      const int count = static_cast<int>(times.size());
      ImPlot::PlotStairs("Source", times.data(), controls->source_counts.data(),
                         count);
      ImPlot::PlotStairs("Accepted", times.data(),
                         controls->accepted_counts.data(), count);
      if (descriptor->source_audit) {
        ImPlot::PlotStairs("Filtered", times.data(),
                           controls->filtered_counts.data(), count);
        ImPlot::PlotStairs("Duplicate", times.data(),
                           controls->duplicate_counts.data(), count);
        ImPlot::PlotStairs("Manual clear", times.data(),
                           controls->manual_clear_counts.data(), count);
        ImPlot::PlotStairs("Manual", times.data(),
                           controls->manual_counts.data(), count);
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      camera_discontinuity = seekFromPlot() || camera_discontinuity;
      ImPlot::EndPlot();
    }

    if (controls->show_reasons && descriptor->source_audit) {
      if (ImGui::BeginTable("##detection-reason-controls", 3,
                            ImGuiTableFlags_SizingStretchSame)) {
        for (const auto &reason : descriptor->source_reason_codes) {
          bool &visible = controls->reason_visibility
                              .try_emplace(reason.code, reason.code != 0)
                              .first->second;
          ImGui::TableNextColumn();
          ImGui::PushID(static_cast<int>(reason.code));
          ImGui::Checkbox(reason.label.c_str(), &visible);
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (ImPlot::BeginPlot("##detection-reasons", ImVec2(-1.0f, 180.0f),
                            ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes("Time (s)", "Source rows", ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
        ImPlot::SetupAxisLimits(
            ImAxis_X1, cursor_time - controls->half_span_seconds,
            cursor_time + controls->half_span_seconds, ImPlotCond_Always);
        for (size_t reason_index = 0;
             reason_index < descriptor->source_reason_codes.size();
             ++reason_index) {
          const auto &reason = descriptor->source_reason_codes[reason_index];
          if (!controls->reason_visibility[reason.code]) {
            continue;
          }
          ImPlot::PlotStairs(reason.label.c_str(), times.data(),
                             controls->reason_counts[reason_index].data(),
                             static_cast<int>(times.size()));
        }
        ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
        camera_discontinuity = seekFromPlot() || camera_discontinuity;
        ImPlot::EndPlot();
      }
    }
    if (!interactive) {
      ImGui::EndDisabled();
    }
    ImGui::End();
    return camera_discontinuity;
  }
  if (!interactive) {
    ImGui::EndDisabled();
  }
  ImGui::End();
  return false;
}

bool drawKeypointQualityTimelineWindow(
    KeypointQualityTimelineControls *controls, bool *open,
    QualityTimelineLoadState load_state,
    const timeline::KeypointQualityTimelineDescriptor *descriptor,
    const std::shared_ptr<const timeline::KeypointQualityTimelineWindow>
        &window,
    const std::shared_ptr<const timeline::KeypointQualityTimelineOverview>
        &overview,
    const std::string &error, int64_t current_frame, double fps,
    const TimelineSeekRequest &request_seek, bool interactive) {
  if (controls == nullptr || open == nullptr || !*open) {
    return false;
  }
  ImGui::SetNextWindowSize(ImVec2(820.0f, 680.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Keypoint Quality Timeline", open)) {
    ImGui::End();
    return false;
  }
  if (!interactive)
    ImGui::BeginDisabled();
  if (descriptor != nullptr) {
    ImGui::Text("%s  |  %s",
                descriptor->refined ? "Refined keypoints" : "Raw keypoints",
                descriptor->run_name.c_str());
    ImGui::Text("Quality: %s", descriptor->quality_run_name.c_str());
  }
  if (ImGui::RadioButton("Local", !controls->full_recording)) {
    controls->full_recording = false;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Full recording", controls->full_recording)) {
    controls->full_recording = true;
  }
  if (!controls->full_recording) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::SliderFloat("Window (+/- s)", &controls->half_span_seconds, 1.0f,
                       60.0f, "%.0f");
    ImGui::SameLine();
    ImGui::Checkbox("Counts", &controls->show_counts);
    ImGui::SameLine();
    ImGui::Checkbox("Metrics", &controls->show_metrics);
    ImGui::SameLine();
    ImGui::Checkbox("Findings", &controls->show_findings);
  }

  if (load_state == QualityTimelineLoadState::Opening) {
    ImGui::TextUnformatted("Opening keypoint quality data...");
  } else if (load_state == QualityTimelineLoadState::Failed) {
    ImGui::TextWrapped("Keypoint timeline unavailable: %s", error.c_str());
  } else if (load_state != QualityTimelineLoadState::Ready ||
             descriptor == nullptr) {
    ImGui::TextUnformatted("Keypoint timeline is not open.");
  } else if (controls->full_recording && !overview) {
    ImGui::TextUnformatted("Loading full-recording confidence overview...");
  } else if (controls->full_recording && !overview->ready()) {
    ImGui::TextWrapped("Keypoint overview unavailable: %s",
                       overview->error.c_str());
  } else if (!controls->full_recording && !window) {
    ImGui::TextUnformatted("Loading the visible keypoint window...");
  } else if (!controls->full_recording && !window->ready()) {
    ImGui::TextWrapped(
        "Keypoint timeline %s: %s",
        crimson::timeline::keypointQualityTimelineStatusName(window->status),
        window->error.c_str());
  } else if (controls->full_recording) {
    const double fps = fps;
    const double cursor_time = fps > 0.0 ? current_frame / fps : 0.0;
    const double recording_end =
        fps > 0.0 && overview->frame_count > 1
            ? static_cast<double>(overview->frame_count - 1) / fps
            : 1.0;
    if (controls->prepared_overview.get() != overview.get() ||
        controls->prepared_fps != fps) {
      controls->prepared_overview = overview;
      controls->prepared_fps = fps;
      const auto prepare_trace =
          [&](const crimson::timeline::KeypointQualityOverviewTrace &source,
              std::vector<double> *times, std::vector<double> *values) {
            times->clear();
            values->clear();
            times->reserve(source.camera_frames.size());
            values->reserve(source.values.size());
            for (size_t index = 0; index < source.camera_frames.size();
                 ++index) {
              times->push_back(fps > 0.0 ? source.camera_frames[index] / fps
                                         : 0.0);
              values->push_back(source.values[index]);
            }
          };
      prepare_trace(overview->pose_confidence, &controls->overview_pose_times,
                    &controls->overview_pose_confidence);
      controls->overview_keypoint_times.resize(
          overview->keypoint_confidence.size());
      controls->overview_keypoint_confidence.resize(
          overview->keypoint_confidence.size());
      for (size_t point = 0; point < overview->keypoint_confidence.size();
           ++point) {
        prepare_trace(overview->keypoint_confidence[point],
                      &controls->overview_keypoint_times[point],
                      &controls->overview_keypoint_confidence[point]);
      }
    }
    if (ImGui::BeginTable("##keypoint-overview-series-controls", 4,
                          ImGuiTableFlags_SizingStretchSame)) {
      for (size_t point = 0; point < descriptor->keypoint_count; ++point) {
        bool &visible = controls->keypoint_visibility.try_emplace(point, true)
                            .first->second;
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(point));
        ImGui::Checkbox(descriptor->keypoint_labels[point].c_str(), &visible);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    const auto seek_from_plot = [&]() {
      if (!interactive || !ImPlot::IsPlotHovered() ||
          !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || fps <= 0.0) {
        return false;
      }
      const int64_t frame = std::clamp<int64_t>(
          static_cast<int64_t>(std::llround(ImPlot::GetPlotMousePos().x * fps)),
          0, static_cast<int64_t>(overview->frame_count) - 1);
      return (request_seek ? request_seek(frame) : false);
    };
    bool camera_discontinuity = false;
    if (ImPlot::BeginPlot("##pose-confidence-overview", ImVec2(-1.0f, 185.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Pose confidence", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, recording_end, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
      ImPlot::PlotLine("Source pose", controls->overview_pose_times.data(),
                       controls->overview_pose_confidence.data(),
                       static_cast<int>(controls->overview_pose_times.size()));
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f),
                   "Frame %lld", static_cast<long long>(current_frame));
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("##keypoint-confidence-overview",
                          ImVec2(-1.0f, 260.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Point confidence", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, recording_end, ImPlotCond_Always);
      ImPlot::SetupAxisFormat(ImAxis_Y1, "%.4f");
      for (size_t point = 0;
           point < controls->overview_keypoint_confidence.size(); ++point) {
        if (!controls->keypoint_visibility[point]) {
          continue;
        }
        ImPlot::PlotLine(
            descriptor->keypoint_labels[point].c_str(),
            controls->overview_keypoint_times[point].data(),
            controls->overview_keypoint_confidence[point].data(),
            static_cast<int>(controls->overview_keypoint_times[point].size()));
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f),
                   "Frame %lld", static_cast<long long>(current_frame));
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (!interactive)
      ImGui::EndDisabled();
    ImGui::End();
    return camera_discontinuity;
  } else {
    const double fps = fps;
    const double cursor_time = fps > 0.0 ? current_frame / fps : 0.0;
    if (controls->prepared_window.get() != window.get() ||
        controls->prepared_fps != fps) {
      controls->prepared_window = window;
      controls->prepared_fps = fps;
      controls->times.clear();
      controls->pose_confidence.clear();
      controls->observation_counts.clear();
      controls->source_success_counts.clear();
      controls->refined_success_counts.clear();
      controls->usable_counts.clear();
      controls->proposed_usable_counts.clear();
      controls->edited_keypoint_counts.clear();
      controls->flip_corrected_counts.clear();
      controls->keypoint_confidence.assign(descriptor->keypoint_count, {});
      controls->pose_metrics.assign(descriptor->pose_metrics.size(), {});
      controls->keypoint_metrics.assign(
          descriptor->keypoint_count * descriptor->keypoint_metrics.size(), {});
      controls->keypoint_flag_counts.assign(descriptor->keypoint_flags.size(),
                                            {});
      controls->pose_flag_counts.assign(descriptor->pose_flags.size(), {});
      controls->review_counts.assign(descriptor->review_states.size(), {});
      controls->reason_counts.assign(descriptor->reason_codes.size(), {});
      for (const auto &frame : window->frames) {
        controls->times.push_back(fps > 0.0 ? frame.camera_frame / fps : 0.0);
        controls->pose_confidence.push_back(frame.pose_confidence_median);
        controls->observation_counts.push_back(frame.observation_count);
        controls->source_success_counts.push_back(frame.source_success_count);
        controls->refined_success_counts.push_back(frame.refined_success_count);
        controls->usable_counts.push_back(frame.usable_count);
        controls->proposed_usable_counts.push_back(frame.proposed_usable_count);
        controls->edited_keypoint_counts.push_back(frame.edited_keypoint_count);
        controls->flip_corrected_counts.push_back(frame.flip_corrected_count);
        for (size_t index = 0; index < controls->keypoint_confidence.size();
             ++index) {
          controls->keypoint_confidence[index].push_back(
              index < frame.keypoint_confidence_medians.size()
                  ? frame.keypoint_confidence_medians[index]
                  : std::numeric_limits<double>::quiet_NaN());
        }
        for (size_t index = 0; index < controls->pose_metrics.size(); ++index) {
          controls->pose_metrics[index].push_back(
              index < frame.pose_metric_medians.size()
                  ? frame.pose_metric_medians[index]
                  : std::numeric_limits<double>::quiet_NaN());
        }
        for (size_t index = 0; index < controls->keypoint_metrics.size();
             ++index) {
          controls->keypoint_metrics[index].push_back(
              index < frame.keypoint_metric_medians.size()
                  ? frame.keypoint_metric_medians[index]
                  : std::numeric_limits<double>::quiet_NaN());
        }
        for (size_t index = 0; index < controls->keypoint_flag_counts.size();
             ++index) {
          controls->keypoint_flag_counts[index].push_back(
              index < frame.keypoint_flag_counts.size()
                  ? frame.keypoint_flag_counts[index]
                  : 0);
        }
        for (size_t index = 0; index < controls->pose_flag_counts.size();
             ++index) {
          controls->pose_flag_counts[index].push_back(
              index < frame.pose_flag_counts.size()
                  ? frame.pose_flag_counts[index]
                  : 0);
        }
        for (size_t index = 0; index < controls->review_counts.size();
             ++index) {
          controls->review_counts[index].push_back(
              index < frame.review_state_counts.size()
                  ? frame.review_state_counts[index]
                  : 0);
        }
        for (size_t index = 0; index < controls->reason_counts.size();
             ++index) {
          controls->reason_counts[index].push_back(
              index < frame.reason_code_counts.size()
                  ? frame.reason_code_counts[index]
                  : 0);
        }
      }
    }
    const auto seek_from_plot = [&]() {
      if (!interactive || !ImPlot::IsPlotHovered() ||
          !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || fps <= 0.0) {
        return false;
      }
      const int64_t frame = std::clamp<int64_t>(
          static_cast<int64_t>(std::llround(ImPlot::GetPlotMousePos().x * fps)),
          window->first_camera_frame, window->last_camera_frame);
      return (request_seek ? request_seek(frame) : false);
    };
    bool camera_discontinuity = false;
    if (ImGui::BeginTable("##keypoint-series-controls", 4,
                          ImGuiTableFlags_SizingStretchSame)) {
      for (size_t point = 0; point < descriptor->keypoint_count; ++point) {
        bool &visible = controls->keypoint_visibility.try_emplace(point, true)
                            .first->second;
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(point));
        ImGui::Checkbox(descriptor->keypoint_labels[point].c_str(), &visible);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (ImPlot::BeginPlot("##pose-confidence", ImVec2(-1.0f, 145.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Pose confidence", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(
          ImAxis_X1, cursor_time - controls->half_span_seconds,
          cursor_time + controls->half_span_seconds, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
      const int count = static_cast<int>(controls->times.size());
      ImPlot::PlotLine("Source pose", controls->times.data(),
                       controls->pose_confidence.data(), count);
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f),
                   "Frame %lld", static_cast<long long>(current_frame));
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("##keypoint-confidence", ImVec2(-1.0f, 230.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Point confidence", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(
          ImAxis_X1, cursor_time - controls->half_span_seconds,
          cursor_time + controls->half_span_seconds, ImPlotCond_Always);
      ImPlot::SetupAxisFormat(ImAxis_Y1, "%.4f");
      const int count = static_cast<int>(controls->times.size());
      for (size_t point = 0; point < descriptor->keypoint_count; ++point) {
        if (controls->keypoint_visibility[point]) {
          ImPlot::PlotLine(descriptor->keypoint_labels[point].c_str(),
                           controls->times.data(),
                           controls->keypoint_confidence[point].data(), count);
        }
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      ImPlot::TagX(cursor_time, ImVec4(0.94f, 0.94f, 0.94f, 0.90f),
                   "Frame %lld", static_cast<long long>(current_frame));
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (controls->show_counts &&
        ImPlot::BeginPlot("##keypoint-counts", ImVec2(-1.0f, 180.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Observations", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(
          ImAxis_X1, cursor_time - controls->half_span_seconds,
          cursor_time + controls->half_span_seconds, ImPlotCond_Always);
      const int count = static_cast<int>(controls->times.size());
      ImPlot::PlotStairs("All", controls->times.data(),
                         controls->observation_counts.data(), count);
      ImPlot::PlotStairs("Source success", controls->times.data(),
                         controls->source_success_counts.data(), count);
      if (descriptor->refined) {
        ImPlot::PlotStairs("Refined success", controls->times.data(),
                           controls->refined_success_counts.data(), count);
        ImPlot::PlotStairs("Usable", controls->times.data(),
                           controls->usable_counts.data(), count);
      }
      ImPlot::PlotStairs("Quality proposed", controls->times.data(),
                         controls->proposed_usable_counts.data(), count);
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (controls->show_metrics && !descriptor->pose_metrics.empty() &&
        ImPlot::BeginPlot("##keypoint-quality-metrics", ImVec2(-1.0f, 180.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Quality metric", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(
          ImAxis_X1, cursor_time - controls->half_span_seconds,
          cursor_time + controls->half_span_seconds, ImPlotCond_Always);
      const int count = static_cast<int>(controls->times.size());
      for (size_t metric = 0; metric < descriptor->pose_metrics.size();
           ++metric) {
        const std::string label =
            "Pose: " + descriptor->pose_metrics[metric].id;
        ImPlot::PlotLine(label.c_str(), controls->times.data(),
                         controls->pose_metrics[metric].data(), count);
      }
      for (size_t point = 0; point < descriptor->keypoint_count; ++point) {
        if (!controls->keypoint_visibility[point])
          continue;
        for (size_t metric = 0; metric < descriptor->keypoint_metrics.size();
             ++metric) {
          const size_t index =
              point * descriptor->keypoint_metrics.size() + metric;
          const std::string label = descriptor->keypoint_labels[point] + ": " +
                                    descriptor->keypoint_metrics[metric].id;
          ImPlot::PlotLine(label.c_str(), controls->times.data(),
                           controls->keypoint_metrics[index].data(), count);
        }
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (controls->show_findings && descriptor->refined &&
        ImGui::BeginTable("##keypoint-finding-controls", 3,
                          ImGuiTableFlags_SizingStretchSame)) {
      for (const auto &review : descriptor->review_states) {
        bool &visible = controls->review_visibility
                            .try_emplace(review.code, review.code != 0)
                            .first->second;
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(review.code));
        ImGui::Checkbox(("Review: " + review.label).c_str(), &visible);
        ImGui::PopID();
      }
      for (const auto &reason : descriptor->reason_codes) {
        bool &visible = controls->reason_visibility
                            .try_emplace(reason.code, reason.code != 0)
                            .first->second;
        ImGui::TableNextColumn();
        ImGui::PushID(0x10000 + static_cast<int>(reason.code));
        ImGui::Checkbox(("Reason: " + reason.label).c_str(), &visible);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (controls->show_findings &&
        (!descriptor->keypoint_flags.empty() ||
         !descriptor->pose_flags.empty() || descriptor->refined) &&
        ImPlot::BeginPlot("##keypoint-quality-findings", ImVec2(-1.0f, 180.0f),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("Time (s)", "Findings", ImPlotAxisFlags_NoMenus,
                        ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
      ImPlot::SetupAxisLimits(
          ImAxis_X1, cursor_time - controls->half_span_seconds,
          cursor_time + controls->half_span_seconds, ImPlotCond_Always);
      const int count = static_cast<int>(controls->times.size());
      if (descriptor->refined) {
        ImPlot::PlotStairs("Edited landmarks", controls->times.data(),
                           controls->edited_keypoint_counts.data(), count);
        ImPlot::PlotStairs("Flip corrected", controls->times.data(),
                           controls->flip_corrected_counts.data(), count);
      }
      for (size_t index = 0; index < descriptor->keypoint_flags.size();
           ++index) {
        const std::string label =
            "Landmark: " + descriptor->keypoint_flags[index].label;
        ImPlot::PlotStairs(label.c_str(), controls->times.data(),
                           controls->keypoint_flag_counts[index].data(), count);
      }
      for (size_t index = 0; index < descriptor->pose_flags.size(); ++index) {
        const std::string label =
            "Pose: " + descriptor->pose_flags[index].label;
        ImPlot::PlotStairs(label.c_str(), controls->times.data(),
                           controls->pose_flag_counts[index].data(), count);
      }
      if (descriptor->refined) {
        for (size_t index = 0; index < descriptor->review_states.size();
             ++index) {
          const auto &review = descriptor->review_states[index];
          if (!controls->review_visibility[review.code])
            continue;
          const std::string label = "Review: " + review.label;
          ImPlot::PlotStairs(label.c_str(), controls->times.data(),
                             controls->review_counts[index].data(), count);
        }
        for (size_t index = 0; index < descriptor->reason_codes.size();
             ++index) {
          const auto &reason = descriptor->reason_codes[index];
          if (!controls->reason_visibility[reason.code])
            continue;
          const std::string label = "Reason: " + reason.label;
          ImPlot::PlotStairs(label.c_str(), controls->times.data(),
                             controls->reason_counts[index].data(), count);
        }
      }
      ImPlot::PlotInfLines("Current frame", &cursor_time, 1);
      camera_discontinuity = seek_from_plot() || camera_discontinuity;
      ImPlot::EndPlot();
    }
    if (!interactive)
      ImGui::EndDisabled();
    ImGui::End();
    return camera_discontinuity;
  }
  if (!interactive)
    ImGui::EndDisabled();
  ImGui::End();
  return false;
}

} // namespace crimson::gui
