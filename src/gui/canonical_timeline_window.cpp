#include "gui/canonical_timeline_window.h"

#include "gui/analysis_timeline_trace_plot.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace crimson::gui {
namespace {

void drawProductStatus(const char* label, CanonicalTimelineProductState state,
                       const std::string& error) {
  ImGui::TextDisabled("%s: %s", label,
                      canonicalTimelineProductStateName(state));
  if (state == CanonicalTimelineProductState::Error && !error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                       error.c_str());
  }
}

ImVec4 eyeColor(timeline::EyeAngleTraceRole role) {
  switch (role) {
    case timeline::EyeAngleTraceRole::Left:
      return ImVec4(0.25f, 0.75f, 1.0f, 1.0f);
    case timeline::EyeAngleTraceRole::Right:
      return ImVec4(1.0f, 0.45f, 0.65f, 1.0f);
    case timeline::EyeAngleTraceRole::Vergence:
      return ImVec4(0.95f, 0.78f, 0.2f, 1.0f);
    case timeline::EyeAngleTraceRole::Other:
      return ImVec4(0.7f, 0.7f, 0.75f, 1.0f);
  }
  return ImVec4(0.7f, 0.7f, 0.75f, 1.0f);
}

std::vector<AnalysisTimelineTrace> eyeTraces(
    const timeline::EyeAngleTimelineWindow& window) {
  std::vector<AnalysisTimelineTrace> traces;
  traces.reserve(window.traces.size());
  for (const auto& source : window.traces) {
    AnalysisTimelineTrace trace;
    trace.label = source.field.display_name + "##" + source.field.source_name;
    trace.units = source.field.units;
    trace.has_color = true;
    trace.color = eyeColor(source.field.role);
    trace.xs = source.times_seconds;
    trace.ys = source.values;
    traces.push_back(std::move(trace));
  }
  return traces;
}

std::vector<AnalysisTimelineTrace> speedTraces(
    const timeline::AnalysisSeriesTimelineWindow& window) {
  std::vector<AnalysisTimelineTrace> traces;
  for (const auto& source : window.traces) {
    if (source.descriptor.role !=
            timeline::AnalysisSeriesTraceRole::PrimarySpeed &&
        source.descriptor.role !=
            timeline::AnalysisSeriesTraceRole::SecondarySpeed) {
      continue;
    }
    AnalysisTimelineTrace trace;
    trace.label = source.descriptor.display_name + "##" +
                  source.descriptor.key;
    trace.units = source.descriptor.units;
    trace.has_color = true;
    trace.color =
        source.descriptor.role ==
                timeline::AnalysisSeriesTraceRole::PrimarySpeed
            ? ImVec4(0.25f, 0.85f, 0.45f, 1.0f)
            : ImVec4(0.55f, 0.65f, 0.7f, 0.75f);
    trace.xs = source.times_seconds;
    trace.ys = source.values;
    traces.push_back(std::move(trace));
  }
  return traces;
}

void drawBoutLane(const timeline::SwimBoutTimelineWindow& window,
                  const TimelineScrollState& scroll, double current_time,
                  double frames_per_second) {
  if (!ImPlot::BeginPlot("Swim bouts##canonical_bouts",
                         ImVec2(-1.0f, 165.0f))) {
    return;
  }
  ImPlot::SetupAxes("Time (s)", nullptr,
                    ImPlotAxisFlags_None,
                    ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines);
  if (scroll.enabled && current_time >= 0.0) {
    const double span = std::max(0.1f, scroll.window_half_span_s);
    ImPlot::SetupAxisLimits(ImAxis_X1, current_time - span,
                            current_time + span, ImGuiCond_Always);
  } else if (window.request.first_frame <= window.request.last_frame) {
    ImPlot::SetupAxisLimits(
        ImAxis_X1,
        static_cast<double>(window.request.first_frame) / frames_per_second,
        static_cast<double>(window.request.last_frame) / frames_per_second,
        ImGuiCond_Once);
  }
  ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.3, ImGuiCond_Always);
  for (const auto& interval : window.intervals) {
    const double xs[2] = {
        static_cast<double>(interval.start_frame) / frames_per_second,
        static_cast<double>(interval.end_frame + 1) / frames_per_second};
    const double ys[2] = {0.55, 0.55};
    ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.75f, 1.0f, 1.0f), 8.0f);
    const std::string label = "##bout_" +
                              std::to_string(interval.source_index);
    ImPlot::PlotLine(label.c_str(), xs, ys, 2);
    if (interval.hasCore()) {
      const double core_xs[2] = {
          static_cast<double>(interval.core_start_frame) / frames_per_second,
          static_cast<double>(interval.core_end_frame + 1) /
              frames_per_second};
      const double core_ys[2] = {0.9, 0.9};
      ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), 5.0f);
      const std::string core_label =
          "##bout_core_" + std::to_string(interval.source_index);
      ImPlot::PlotLine(core_label.c_str(), core_xs, core_ys, 2);
    }
  }
  drawCurrentTimeMarker(current_time, "##canonical_bout_current");
  ImPlot::EndPlot();
}

}  // namespace

void drawCanonicalTimelineWindow(const CanonicalTimelineSnapshot& snapshot,
                                 int64_t current_frame,
                                 double frames_per_second,
                                 CanonicalTimelineWindowState* state,
                                 bool* open) {
  if (!state) {
    return;
  }
  const auto* viewport = ImGui::GetMainViewport();
  const float width = std::min(900.0f, viewport->WorkSize.x * 0.58f);
  ImGui::SetNextWindowSize(
      ImVec2(width, std::max(300.0f, viewport->WorkSize.y - 60.0f)),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width - 20.0f,
             viewport->WorkPos.y + 30.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Analysis Timeline", open)) {
    ImGui::End();
    return;
  }
  const double current_time =
      current_frame >= 0 && frames_per_second > 0.0
          ? static_cast<double>(current_frame) / frames_per_second
          : -1.0;
  ImGui::Checkbox("Follow playback", &state->scroll.enabled);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  // With the production 4096/2048 page policy at 30 fps, each page
  // guarantees at least 34 seconds on either side of an interior anchor.
  ImGui::SliderFloat("Half-window (s)", &state->scroll.window_half_span_s, 1.0f,
                     30.0f, "%.1f");

  const bool aligned = ImPlot::BeginAlignedPlots("canonical_timelines", true);

  drawProductStatus("Eye angles", snapshot.eye_angles.state,
                    snapshot.eye_angles.error);
  if (snapshot.eye_angles.window &&
      snapshot.eye_angles.state == CanonicalTimelineProductState::Ready) {
    const auto traces = eyeTraces(*snapshot.eye_angles.window);
    drawAnalysisTracePlot("Eye angles##canonical_eye", "Angle (deg)", traces,
                          state->scroll, current_time,
                          "##canonical_eye_current");
  }

  drawProductStatus("Motion", snapshot.motion.state, snapshot.motion.error);
  if (snapshot.motion.window &&
      snapshot.motion.state == CanonicalTimelineProductState::Ready) {
    const auto traces = speedTraces(*snapshot.motion.window);
    const char* units = traces.empty() ? "Speed" : traces.front().units.c_str();
    drawAnalysisTracePlot("Speed##canonical_speed", units, traces,
                          state->scroll, current_time,
                          "##canonical_speed_current");
  }

  drawProductStatus("Swim bouts", snapshot.swim_bouts.state,
                    snapshot.swim_bouts.error);
  if (snapshot.swim_bouts.window && frames_per_second > 0.0 &&
      (snapshot.swim_bouts.state == CanonicalTimelineProductState::Ready ||
       snapshot.swim_bouts.state == CanonicalTimelineProductState::Empty)) {
    drawBoutLane(*snapshot.swim_bouts.window, state->scroll, current_time,
                 frames_per_second);
    if (snapshot.swim_bouts.window->intervals.empty()) {
      ImGui::TextDisabled("No bout intervals intersect this window.");
    }
    if (!snapshot.swim_bouts.window->detector_values.empty()) {
      AnalysisTimelineTrace detector;
      detector.label = "Detector response##canonical_bout_detector";
      detector.units = "mm/s";
      detector.has_color = true;
      detector.color = ImVec4(0.7f, 0.4f, 1.0f, 1.0f);
      detector.xs = snapshot.swim_bouts.window->detector_times_seconds;
      detector.ys = snapshot.swim_bouts.window->detector_values;
      drawAnalysisTracePlot("Bout detector##canonical_bout_trace", "mm/s",
                            {detector}, state->scroll, current_time,
                            "##canonical_bout_trace_current");
    }
  }
  if (aligned) ImPlot::EndAlignedPlots();
  state->scroll.prev_enabled = state->scroll.enabled;
  ImGui::End();
}

}  // namespace crimson::gui
