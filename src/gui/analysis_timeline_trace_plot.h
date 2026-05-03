#pragma once

#include "stimulus_event_timeline_window.h"

#include "imgui.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct AnalysisTimelineTrace {
    std::string label;
    std::string units;
    bool has_color = false;
    ImVec4 color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    float line_width = 2.0f;
    std::vector<double> xs;
    std::vector<double> ys;
};

std::optional<double> sampleTimeFromFrame(int32_t frame, double video_fps);

void extendTraceRange(const AnalysisTimelineTrace& trace,
                      double& x_min,
                      double& x_max,
                      double& y_min,
                      double& y_max);

void drawCurrentTimeMarker(double current_time,
                           const char* label = "##current_time");

void drawAnalysisTracePlot(const char* title,
                           const char* y_axis_label,
                           const std::vector<AnalysisTimelineTrace>& traces,
                           const TimelineScrollState& scroll_state,
                           double current_time,
                           const char* current_marker_id);
