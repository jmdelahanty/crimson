#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

class ZarrDetectionLoader;

struct TimelineScrollState {
    bool enabled = false;
    float window_half_span_s = 5.0f;
    bool prev_enabled = false;
};

struct StimulusEventTimelineWindowState {
    size_t last_logged_timeline_count = std::numeric_limits<size_t>::max();
    size_t last_logged_step_count = std::numeric_limits<size_t>::max();
    std::unordered_map<int32_t, bool> event_type_filter;
    bool filter_initialized = false;
    int selected_event_idx = -1;
    size_t cached_timeline_signature = 0;
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
