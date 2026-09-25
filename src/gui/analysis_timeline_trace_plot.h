#pragma once

#include "stimulus_event_timeline_window.h"

#include "imgui.h"

#include <cstddef>
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

struct AnalysisTimelinePlotBand {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    ImVec4 color = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    bool core = false;
};

struct AnalysisTimelinePlotBandCounts {
    size_t bands_drawn = 0;
    size_t core_bands_drawn = 0;
};

struct AnalysisTimelineTracePlotRow {
    std::string title;
    std::string y_axis_label;
    std::vector<AnalysisTimelineTrace> traces;
    double current_time = -1.0;
    std::string current_marker_id;
    float row_weight = 1.0f;
    std::vector<AnalysisTimelinePlotBand> bands;
};

struct AnalysisTimelineXAxisLimits {
    bool valid = false;
    double min = 0.0;
    double max = 0.0;
    bool force = false;
};

std::optional<double> sampleTimeFromFrame(int32_t frame, double video_fps);

void extendTraceRange(const AnalysisTimelineTrace& trace,
                      double& x_min,
                      double& x_max,
                      double& y_min,
                      double& y_max);

void drawCurrentTimeMarker(double current_time,
                           const char* label = "##current_time");

uint64_t plotAnalysisTimelineLine(const char* label,
                                  const double* xs,
                                  const double* ys,
                                  size_t count);

void prewarmAnalysisTimelineLineLod(const double* xs,
                                    const double* ys,
                                    size_t count,
                                    size_t target_bucket_count);

bool drawAnalysisTracePlotRow(
    const AnalysisTimelineTracePlotRow& row,
    const TimelineScrollState& scroll_state,
    const AnalysisTimelineXAxisLimits* linked_x_limits = nullptr,
    bool embedded_in_subplots = false,
    uint64_t* submitted_points_out = nullptr);

bool drawAnalysisTracePlotRow(
    const AnalysisTimelineTracePlotRow& row,
    const TimelineScrollState& scroll_state,
    const AnalysisTimelineXAxisLimits* linked_x_limits,
    bool embedded_in_subplots,
    uint64_t* submitted_points_out,
    AnalysisTimelinePlotBandCounts* band_counts_out);

void drawAnalysisTracePlot(const char* title,
                           const char* y_axis_label,
                           const std::vector<AnalysisTimelineTrace>& traces,
                           const TimelineScrollState& scroll_state,
                           double current_time,
                           const char* current_marker_id);

void drawAnalysisTracePlot(const char* title,
                           const char* y_axis_label,
                           const std::vector<AnalysisTimelineTrace>& traces,
                           const TimelineScrollState& scroll_state,
                           double current_time,
                           const char* current_marker_id,
                           const std::vector<AnalysisTimelinePlotBand>* bands,
                           AnalysisTimelinePlotBandCounts* band_counts_out);
