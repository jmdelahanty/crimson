#pragma once

#include <cstdint>

struct AnalysisTimelinePerfStats {
    double source_selector_ms = 0.0;
    double controls_ms = 0.0;
    double prepare_motion_ms = 0.0;
    double build_position_ms = 0.0;
    double build_eye_ms = 0.0;
    double build_tail_ms = 0.0;
    double draw_plots_ms = 0.0;
    double draw_stimulus_context_ms = 0.0;
    double draw_speed_plot_ms = 0.0;
    double draw_bout_rects_ms = 0.0;
    double draw_heading_plot_ms = 0.0;
    double draw_distance_plot_ms = 0.0;
    double draw_extra_rows_ms = 0.0;
    double summary_ms = 0.0;
    double standalone_eye_ms = 0.0;
    double standalone_tail_ms = 0.0;
    double total_window_ms = 0.0;

    uint64_t motion_points_prepared = 0;
    uint64_t position_points_prepared = 0;
    uint64_t eye_points_prepared = 0;
    uint64_t tail_points_prepared = 0;
    uint64_t prepared_points_total = 0;
    uint64_t submitted_points_total = 0;
    uint64_t speed_submitted_points = 0;
    uint64_t heading_submitted_points = 0;
    uint64_t distance_submitted_points = 0;
    uint64_t extra_submitted_points = 0;

    uint32_t prepared_traces = 0;
    uint32_t submitted_traces = 0;
    uint32_t plot_rows = 0;
    uint32_t extra_rows = 0;
    uint32_t bout_rects_considered = 0;
    uint32_t bout_rects_drawn = 0;
    uint32_t bout_core_rects_drawn = 0;
};
