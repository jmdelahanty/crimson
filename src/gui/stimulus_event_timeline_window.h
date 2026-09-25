#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ZarrDetectionLoader;

struct TimelineScrollState {
    bool enabled = false;
    float window_half_span_s = 5.0f;
    bool prev_enabled = false;
};

struct StimulusEventTimelineWindowState {
    struct CachedTypeSeries {
        int32_t event_type_id = -1;
        std::string type_label;
        std::vector<double> x_values;
        std::vector<double> y_values;
    };

    struct PreparedTimelineCache {
        bool valid = false;
        std::string archive_path;
        size_t timeline_generation = std::numeric_limits<size_t>::max();
        size_t timeline_count = 0;
        size_t step_count = 0;
        size_t total_frames = 0;
        double video_fps = 0.0;
        size_t timeline_signature = 0;
        double timeline_min_time = 0.0;
        double timeline_max_time = 0.5;
        double rebuild_ms = 0.0;
        size_t missing_camera_count = 0;
        std::vector<double> x_values;
        std::vector<double> y_values;
        std::vector<int32_t> display_frames;
        std::vector<std::string> row_labels;
        std::vector<std::pair<int32_t, std::string>> event_type_labels;
        std::vector<CachedTypeSeries> type_series;
    };

    size_t last_logged_timeline_count = std::numeric_limits<size_t>::max();
    size_t last_logged_step_count = std::numeric_limits<size_t>::max();
    std::unordered_map<int32_t, bool> event_type_filter;
    bool filter_initialized = false;
    int selected_event_idx = -1;
    size_t cached_timeline_signature = 0;
    PreparedTimelineCache prepared_cache;
};

struct StimulusEventTimelineWindowContext {
    const ZarrDetectionLoader& zarr_loader;
    TimelineScrollState& scroll_state;
    int current_frame_num = 0;
    double video_fps = 0.0;
};

struct StimulusEventTimelineWindowResult {
    std::optional<int> seek_target_frame;
};

StimulusEventTimelineWindowResult drawStimulusEventTimelineWindow(
    const StimulusEventTimelineWindowContext& context,
    StimulusEventTimelineWindowState& state);
