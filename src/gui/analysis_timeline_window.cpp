#include "gui/analysis_timeline_window.h"

#include "gui/analysis_timeline_eye_angle.h"
#include "gui/analysis_timeline_motion_controls.h"
#include "gui/analysis_timeline_motion_data.h"
#include "gui/analysis_timeline_motion_plot.h"
#include "gui/analysis_timeline_motion_summary.h"
#include "gui/analysis_timeline_stimulus_context.h"
#include "gui/analysis_timeline_tail_kinematics.h"
#include "imgui.h"
#include "zarr_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

std::optional<double> currentTimeSeconds(const AnalysisTimelineWindowContext& context) {
    if (context.current_frame_num < 0 || context.video_fps <= 0.0) {
        return std::nullopt;
    }
    return static_cast<double>(context.current_frame_num) / context.video_fps;
}

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

uint64_t traceRowPointCount(const AnalysisTimelineTracePlotRow& row) {
    uint64_t count = 0;
    for (const auto& trace : row.traces) {
        count += static_cast<uint64_t>(trace.xs.size());
    }
    return count;
}

void addPreparedTraceRowStats(AnalysisTimelinePerfStats* perf,
                              const AnalysisTimelineTracePlotRow& row,
                              uint64_t& bucket) {
    if (perf == nullptr) {
        return;
    }
    const uint64_t points = traceRowPointCount(row);
    bucket += points;
    perf->prepared_points_total += points;
    perf->prepared_traces += static_cast<uint32_t>(row.traces.size());
}

struct MotionPreparedCacheKey {
    const float* time_data = nullptr;
    size_t time_size = 0;
    const int32_t* frame_indices_data = nullptr;
    size_t frame_indices_size = 0;
    const float* smoothed_speed_data = nullptr;
    size_t smoothed_speed_size = 0;
    const float* instant_speed_data = nullptr;
    size_t instant_speed_size = 0;
    const float* distance_data = nullptr;
    size_t distance_size = 0;
    const float* heading_data = nullptr;
    size_t heading_size = 0;
    const float* smoothed_heading_data = nullptr;
    size_t smoothed_heading_size = 0;
    const uint8_t* heading_success_data = nullptr;
    size_t heading_success_size = 0;
    const float* heading_per_second_data = nullptr;
    size_t heading_per_second_size = 0;
    const float* heading_per_second_resultant_data = nullptr;
    size_t heading_per_second_resultant_size = 0;
    const float* heading_per_second_time_data = nullptr;
    size_t heading_per_second_time_size = 0;
    const ZarrDetectionData::SwimBoutSeries* selected_swim_bouts = nullptr;
    std::string motion_source_key;
    std::string swim_bouts_key;
    bool smoothed_available = false;
    bool instant_available = false;
    bool heading_per_second_available = false;
    bool show_detector_response = false;
    double video_fps = 0.0;
};

bool operator==(const MotionPreparedCacheKey& lhs,
                const MotionPreparedCacheKey& rhs) {
    return lhs.time_data == rhs.time_data && lhs.time_size == rhs.time_size &&
           lhs.frame_indices_data == rhs.frame_indices_data &&
           lhs.frame_indices_size == rhs.frame_indices_size &&
           lhs.smoothed_speed_data == rhs.smoothed_speed_data &&
           lhs.smoothed_speed_size == rhs.smoothed_speed_size &&
           lhs.instant_speed_data == rhs.instant_speed_data &&
           lhs.instant_speed_size == rhs.instant_speed_size &&
           lhs.distance_data == rhs.distance_data &&
           lhs.distance_size == rhs.distance_size &&
           lhs.heading_data == rhs.heading_data &&
           lhs.heading_size == rhs.heading_size &&
           lhs.smoothed_heading_data == rhs.smoothed_heading_data &&
           lhs.smoothed_heading_size == rhs.smoothed_heading_size &&
           lhs.heading_success_data == rhs.heading_success_data &&
           lhs.heading_success_size == rhs.heading_success_size &&
           lhs.heading_per_second_data == rhs.heading_per_second_data &&
           lhs.heading_per_second_size == rhs.heading_per_second_size &&
           lhs.heading_per_second_resultant_data ==
               rhs.heading_per_second_resultant_data &&
           lhs.heading_per_second_resultant_size ==
               rhs.heading_per_second_resultant_size &&
           lhs.heading_per_second_time_data ==
               rhs.heading_per_second_time_data &&
           lhs.heading_per_second_time_size ==
               rhs.heading_per_second_time_size &&
           lhs.selected_swim_bouts == rhs.selected_swim_bouts &&
           lhs.motion_source_key == rhs.motion_source_key &&
           lhs.swim_bouts_key == rhs.swim_bouts_key &&
           lhs.smoothed_available == rhs.smoothed_available &&
           lhs.instant_available == rhs.instant_available &&
           lhs.heading_per_second_available ==
               rhs.heading_per_second_available &&
           lhs.show_detector_response == rhs.show_detector_response &&
           lhs.video_fps == rhs.video_fps;
}

template <typename T>
const T* vectorDataOrNull(const std::vector<T>& values) {
    return values.empty() ? nullptr : values.data();
}

MotionPreparedCacheKey makeMotionPreparedCacheKey(
    const AnalysisTimelineMotionDataInput& input,
    const ZarrDetectionData::MovementSeries* selected_series) {
    std::string motion_source_key;
    if (selected_series != nullptr) {
        motion_source_key = selected_series->category + "\n" +
                            selected_series->run_name + "\n" +
                            selected_series->track_id + "\n" +
                            selected_series->speed_level + "\n" +
                            selected_series->primary_speed_source_path + "\n" +
                            selected_series->secondary_speed_source_path;
    }
    std::string swim_bouts_key;
    if (input.selected_swim_bouts != nullptr) {
        swim_bouts_key = input.selected_swim_bouts->run_name + "\n" +
                         input.selected_swim_bouts->speed_level + "\n" +
                         std::to_string(input.selected_swim_bouts->candidate_id) + "\n" +
                         std::to_string(input.selected_swim_bouts->signal_id) + "\n" +
                         input.selected_swim_bouts->detection_signal_source_path;
    }
    return MotionPreparedCacheKey{
        vectorDataOrNull(input.time_seconds),
        input.time_seconds.size(),
        vectorDataOrNull(input.frame_indices),
        input.frame_indices.size(),
        vectorDataOrNull(input.smoothed_speed),
        input.smoothed_speed.size(),
        vectorDataOrNull(input.instant_speed),
        input.instant_speed.size(),
        vectorDataOrNull(input.distance_mm),
        input.distance_mm.size(),
        vectorDataOrNull(input.heading_degrees),
        input.heading_degrees.size(),
        vectorDataOrNull(input.smoothed_heading_degrees),
        input.smoothed_heading_degrees.size(),
        vectorDataOrNull(input.heading_keypoint_success),
        input.heading_keypoint_success.size(),
        vectorDataOrNull(input.heading_per_second_degrees),
        input.heading_per_second_degrees.size(),
        vectorDataOrNull(input.heading_per_second_resultant),
        input.heading_per_second_resultant.size(),
        vectorDataOrNull(input.heading_per_second_time),
        input.heading_per_second_time.size(),
        input.selected_swim_bouts,
        std::move(motion_source_key),
        std::move(swim_bouts_key),
        input.smoothed_available,
        input.instant_available,
        input.heading_per_second_available,
        input.show_detector_response,
        input.video_fps,
    };
}

struct MotionPreparedCache {
    bool valid = false;
    MotionPreparedCacheKey key;
    AnalysisTimelineMotionPreparedData data;
};

uint64_t motionPreparedPointCount(
    const AnalysisTimelineMotionPreparedData& motion_data) {
    return static_cast<uint64_t>(motion_data.smoothed_plot.size()) +
           static_cast<uint64_t>(motion_data.instant_plot.size()) +
           static_cast<uint64_t>(motion_data.detector_value_plot.size()) +
           static_cast<uint64_t>(motion_data.heading_raw_plot.size()) +
           static_cast<uint64_t>(motion_data.heading_smoothed_plot.size()) +
           static_cast<uint64_t>(motion_data.heading_per_second_plot.size()) +
           static_cast<uint64_t>(
               motion_data.heading_per_second_resultant_plot.size()) +
           static_cast<uint64_t>(motion_data.distance_units.size());
}

struct SingleTraceRowCache {
    bool valid = false;
    std::string key;
    std::optional<AnalysisTimelineTracePlotRow> row;
};

struct TraceRowsCache {
    bool valid = false;
    std::string key;
    std::vector<AnalysisTimelineTracePlotRow> rows;
};

std::string pointerKey(const void* ptr) {
    return std::to_string(reinterpret_cast<uintptr_t>(ptr));
}

void appendKeyPart(std::string& key, const std::string& value) {
    key += value;
    key.push_back('\n');
}

void appendKeyPart(std::string& key, bool value) {
    key += value ? "1\n" : "0\n";
}

void appendKeyPart(std::string& key, size_t value) {
    key += std::to_string(value);
    key.push_back('\n');
}

void appendKeyPart(std::string& key, double value) {
    key += std::to_string(value);
    key.push_back('\n');
}

std::string makePositionTraceCacheKey(
    const AnalysisTimelineMotionSummaryContext& context) {
    std::string key;
    key.reserve(256);
    const auto* series = context.selected_series;
    appendKeyPart(key, pointerKey(series));
    appendKeyPart(key, context.state.show_track_position);
    appendKeyPart(key, context.state.show_track_position_x);
    appendKeyPart(key, context.state.show_track_position_y);
    appendKeyPart(key, pointerKey(vectorDataOrNull(context.time_data)));
    appendKeyPart(key, context.time_data.size());
    if (series != nullptr) {
        appendKeyPart(key, series->category);
        appendKeyPart(key, series->run_name);
        appendKeyPart(key, series->track_id);
        appendKeyPart(key, series->speed_level);
        const bool use_mm_positions = !series->positions_mm.empty();
        appendKeyPart(key, use_mm_positions);
        if (use_mm_positions) {
            appendKeyPart(key, pointerKey(vectorDataOrNull(series->positions_mm)));
            appendKeyPart(key, series->positions_mm.size());
        } else {
            appendKeyPart(key, pointerKey(vectorDataOrNull(series->positions_px)));
            appendKeyPart(key, series->positions_px.size());
        }
    }
    return key;
}

std::string makeEyeTraceCacheKey(const AnalysisTimelineEyeAngleContext& context,
                                 const AnalysisTimelineWindowState& state) {
    const auto& eye = context.zarr_loader.getEyeAngleAnalysisData();
    std::string key;
    key.reserve(256);
    appendKeyPart(key, pointerKey(&eye));
    appendKeyPart(key, eye.run_name);
    appendKeyPart(key, eye.scalar_fields.size());
    appendKeyPart(key, eye.representations.size());
    appendKeyPart(key, state.show_eye_angle_traces);
    appendKeyPart(key, state.show_eye_left_trace);
    appendKeyPart(key, state.show_eye_right_trace);
    appendKeyPart(key, state.show_eye_vergence_trace);
    appendKeyPart(key, static_cast<size_t>(
                           std::max(0, state.eye_angle_representation_index)));
    if (state.eye_angle_representation_index >= 0 &&
        static_cast<size_t>(state.eye_angle_representation_index) <
            eye.representations.size()) {
        appendKeyPart(
            key,
            eye.representations[static_cast<size_t>(
                                    state.eye_angle_representation_index)]
                .key);
    }
    appendKeyPart(key, context.video_fps);
    return key;
}

std::string makeTailTraceCacheKey(
    const AnalysisTimelineTailKinematicsContext& context,
    const AnalysisTimelineWindowState& state) {
    const auto& tail = context.zarr_loader.getTailKinematicsData();
    std::string key;
    key.reserve(256);
    appendKeyPart(key, pointerKey(&tail));
    appendKeyPart(key, tail.run_name);
    appendKeyPart(key, tail.row_count);
    appendKeyPart(key, tail.sample_count);
    appendKeyPart(key, state.show_tail_tip_angle);
    appendKeyPart(key, state.show_tail_tip_lateral_deflection);
    appendKeyPart(key, state.show_tail_curvature);
    appendKeyPart(key, pointerKey(vectorDataOrNull(tail.frame_index)));
    appendKeyPart(key, tail.frame_index.size());
    appendKeyPart(key, pointerKey(vectorDataOrNull(tail.row_to_frame)));
    appendKeyPart(key, tail.row_to_frame.size());
    appendKeyPart(key, pointerKey(vectorDataOrNull(tail.tail_tip_angle_deg)));
    appendKeyPart(key, tail.tail_tip_angle_deg.size());
    appendKeyPart(key, pointerKey(vectorDataOrNull(tail.max_abs_tail_angle_deg)));
    appendKeyPart(key, tail.max_abs_tail_angle_deg.size());
    appendKeyPart(
        key, pointerKey(vectorDataOrNull(tail.tail_tip_lateral_deflection_px)));
    appendKeyPart(key, tail.tail_tip_lateral_deflection_px.size());
    appendKeyPart(
        key, pointerKey(vectorDataOrNull(tail.max_abs_tail_curvature_px_inv)));
    appendKeyPart(key, tail.max_abs_tail_curvature_px_inv.size());
    appendKeyPart(key, context.video_fps);
    return key;
}

double currentEyeTimelineTime(const AnalysisTimelineEyeAngleContext& context) {
    const auto& eye = context.zarr_loader.getEyeAngleAnalysisData();
    double current_eye_time = context.fallback_current_time;
    if (auto current_row =
            context.zarr_loader.findEyeAngleRowForFrame(
                context.current_frame_num)) {
        if (*current_row < eye.roi_time_seconds.size() &&
            std::isfinite(eye.roi_time_seconds[*current_row])) {
            current_eye_time =
                static_cast<double>(eye.roi_time_seconds[*current_row]);
        }
    } else if (context.current_frame_num >= 0 &&
               static_cast<size_t>(context.current_frame_num) <
                   eye.frame_time_seconds.size() &&
               std::isfinite(
                   eye.frame_time_seconds[context.current_frame_num])) {
        current_eye_time =
            static_cast<double>(
                eye.frame_time_seconds[context.current_frame_num]);
    }
    return current_eye_time;
}

double currentTailTimelineTime(
    const AnalysisTimelineTailKinematicsContext& context) {
    const auto& tail = context.zarr_loader.getTailKinematicsData();
    const std::vector<int32_t>& tail_frame_index =
        !tail.frame_index.empty() ? tail.frame_index : tail.row_to_frame;
    double current_tail_time = context.fallback_current_time;
    if (auto current_row =
            context.zarr_loader.findTailKinematicsRowForFrame(
                context.current_frame_num)) {
        if (*current_row < tail_frame_index.size()) {
            current_tail_time =
                sampleTimeFromFrame(tail_frame_index[*current_row],
                                    context.video_fps)
                    .value_or(current_tail_time);
        }
    }
    return current_tail_time;
}

void appendTraceWarmupKey(std::string& key,
                          const std::vector<double>& xs,
                          const std::vector<double>& ys) {
    appendKeyPart(key, pointerKey(vectorDataOrNull(xs)));
    appendKeyPart(key, pointerKey(vectorDataOrNull(ys)));
    appendKeyPart(key, xs.size());
    appendKeyPart(key, ys.size());
}

void prewarmTraceLod(const std::vector<double>& xs,
                     const std::vector<double>& ys,
                     size_t target_bucket_count) {
    if (xs.empty() || ys.empty() || xs.size() != ys.size()) {
        return;
    }
    prewarmAnalysisTimelineLineLod(xs.data(),
                                   ys.data(),
                                   xs.size(),
                                   target_bucket_count);
}

void appendMotionWarmupKey(std::string& key,
                           const AnalysisTimelineWindowState& state,
                           const AnalysisTimelineMotionPreparedData& motion) {
    if (state.show_smoothed && !motion.smoothed_plot.empty()) {
        appendTraceWarmupKey(key, motion.time_plot, motion.smoothed_plot);
    }
    if (state.show_instantaneous && !motion.instant_plot.empty()) {
        appendTraceWarmupKey(key, motion.time_plot, motion.instant_plot);
    }
    if (!motion.detector_time_plot.empty() &&
        motion.detector_time_plot.size() == motion.detector_value_plot.size()) {
        appendTraceWarmupKey(key,
                             motion.detector_time_plot,
                             motion.detector_value_plot);
    }
    if (state.show_heading_raw && !motion.heading_raw_plot.empty()) {
        appendTraceWarmupKey(key,
                             motion.heading_time_raw,
                             motion.heading_raw_plot);
    }
    if (state.show_heading_smoothed && !motion.heading_smoothed_plot.empty()) {
        appendTraceWarmupKey(key,
                             motion.heading_time_smoothed,
                             motion.heading_smoothed_plot);
    }
    if (state.show_heading_per_second &&
        !motion.heading_per_second_plot.empty()) {
        appendTraceWarmupKey(key,
                             motion.heading_per_second_time_plot,
                             motion.heading_per_second_plot);
        if (!motion.heading_per_second_resultant_plot.empty()) {
            appendTraceWarmupKey(key,
                                 motion.heading_per_second_time_plot,
                                 motion.heading_per_second_resultant_plot);
        }
    }
    if (state.show_distance_trace && !motion.distance_time.empty()) {
        appendTraceWarmupKey(key, motion.distance_time, motion.distance_units);
    }
}

void maybePrewarmTraceLod(const std::vector<double>& xs,
                          const std::vector<double>& ys,
                          size_t target_bucket_count,
                          size_t& cursor,
                          size_t& visited,
                          size_t& built,
                          size_t build_limit) {
    if (xs.empty() || ys.empty() || xs.size() != ys.size()) {
        return;
    }
    if (visited >= cursor && built < build_limit) {
        prewarmTraceLod(xs, ys, target_bucket_count);
        ++cursor;
        ++built;
    }
    ++visited;
}

void prewarmMotionLodsStep(const AnalysisTimelineWindowState& state,
                           const AnalysisTimelineMotionPreparedData& motion,
                           size_t target_bucket_count,
                           size_t& cursor,
                           size_t& visited,
                           size_t& built,
                           size_t build_limit) {
    if (state.show_smoothed && !motion.smoothed_plot.empty()) {
        maybePrewarmTraceLod(motion.time_plot,
                             motion.smoothed_plot,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
    }
    if (state.show_instantaneous && !motion.instant_plot.empty()) {
        maybePrewarmTraceLod(motion.time_plot,
                             motion.instant_plot,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
    }
    if (!motion.detector_time_plot.empty() &&
        motion.detector_time_plot.size() == motion.detector_value_plot.size()) {
        maybePrewarmTraceLod(motion.detector_time_plot,
                             motion.detector_value_plot,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
    }
    if (state.show_heading_raw && !motion.heading_raw_plot.empty()) {
        maybePrewarmTraceLod(motion.heading_time_raw,
                             motion.heading_raw_plot,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
    }
    if (state.show_heading_smoothed && !motion.heading_smoothed_plot.empty()) {
        maybePrewarmTraceLod(motion.heading_time_smoothed,
                             motion.heading_smoothed_plot,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
    }
    if (state.show_heading_per_second &&
        !motion.heading_per_second_plot.empty()) {
        maybePrewarmTraceLod(motion.heading_per_second_time_plot,
                             motion.heading_per_second_plot,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
        if (!motion.heading_per_second_resultant_plot.empty()) {
            maybePrewarmTraceLod(motion.heading_per_second_time_plot,
                                 motion.heading_per_second_resultant_plot,
                                 target_bucket_count,
                                 cursor,
                                 visited,
                                 built,
                                 build_limit);
        }
    }
    if (state.show_distance_trace && !motion.distance_time.empty()) {
        maybePrewarmTraceLod(motion.distance_time,
                             motion.distance_units,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
    }
}

void appendRowWarmupKey(std::string& key,
                        const AnalysisTimelineTracePlotRow& row) {
    appendKeyPart(key, row.title);
    appendKeyPart(key, row.traces.size());
    for (const auto& trace : row.traces) {
        appendKeyPart(key, trace.label);
        appendTraceWarmupKey(key, trace.xs, trace.ys);
    }
}

void prewarmRowLodsStep(const AnalysisTimelineTracePlotRow& row,
                        size_t target_bucket_count,
                        size_t& cursor,
                        size_t& visited,
                        size_t& built,
                        size_t build_limit) {
    for (const auto& trace : row.traces) {
        maybePrewarmTraceLod(trace.xs,
                             trace.ys,
                             target_bucket_count,
                             cursor,
                             visited,
                             built,
                             build_limit);
    }
}

std::string makeTimelineLodWarmupKey(
    const AnalysisTimelineWindowState& state,
    const AnalysisTimelineMotionPreparedData& motion,
    const std::vector<const AnalysisTimelineTracePlotRow*>& extra_rows,
    size_t target_bucket_count) {
    std::string key;
    key.reserve(1024);
    appendKeyPart(key, target_bucket_count);
    appendMotionWarmupKey(key, state, motion);
    appendKeyPart(key, extra_rows.size());
    for (const auto* row : extra_rows) {
        appendKeyPart(key, pointerKey(row));
        if (row != nullptr) {
            appendRowWarmupKey(key, *row);
        }
    }
    return key;
}

void prewarmTimelineLodsStep(
    const AnalysisTimelineWindowState& state,
    const AnalysisTimelineMotionPreparedData& motion,
    const std::vector<const AnalysisTimelineTracePlotRow*>& extra_rows,
    size_t target_bucket_count,
    size_t& cursor,
    size_t build_limit) {
    size_t visited = 0;
    size_t built = 0;
    prewarmMotionLodsStep(state,
                          motion,
                          target_bucket_count,
                          cursor,
                          visited,
                          built,
                          build_limit);
    for (const auto* row : extra_rows) {
        if (row != nullptr) {
            prewarmRowLodsStep(*row,
                               target_bucket_count,
                               cursor,
                               visited,
                               built,
                               build_limit);
        }
    }
    if (cursor > visited) {
        cursor = visited;
    }
}

}  // namespace

void drawAnalysisTimelineWindow(const AnalysisTimelineWindowContext& context,
                                AnalysisTimelineWindowState& state) {
    if (!ImGui::Begin("Analysis Timeline")) {
        ImGui::End();
        return;
    }

    AnalysisTimelinePerfStats* perf = context.perf_stats;
    const auto source_selector_start = std::chrono::steady_clock::now();
    const auto* selected_series =
        drawAnalysisTimelineMotionSourceSelector(context.zarr_loader);
    if (perf != nullptr) {
        perf->source_selector_ms +=
            durationMs(std::chrono::steady_clock::now() -
                       source_selector_start);
    }

    const auto& time_data = context.zarr_loader.getMovementTimeSeconds();
    const auto& smoothed_speed =
        context.zarr_loader.getMovementSmoothedSpeedMm();
    const auto& instant_speed =
        context.zarr_loader.getMovementInstantaneousSpeedMm();
    const auto& distance_mm =
        context.zarr_loader.getMovementDistanceToTargetMm();
    const auto& heading_degrees =
        context.zarr_loader.getMovementHeadingDegrees();
    const auto& smoothed_heading_degrees =
        context.zarr_loader.getMovementSmoothedHeadingDegrees();
    const auto& heading_keypoint_success =
        context.zarr_loader.getMovementHeadingKeypointSuccess();
    const auto& heading_per_second_degrees =
        context.zarr_loader.getMovementHeadingPerSecondDegrees();
    const auto& heading_per_second_resultant =
        context.zarr_loader.getMovementHeadingPerSecondResultant();
    const auto& heading_per_second_time =
        context.zarr_loader.getMovementHeadingPerSecondTimeSeconds();
    const auto& frame_indices = context.zarr_loader.getMovementFrameIndices();
    AnalysisTimelineMotionSelection motion_selection;
    motion_selection.selected_series = selected_series;

    bool smoothed_available = !smoothed_speed.empty();
    bool instant_available = !instant_speed.empty();
    bool distance_available = !distance_mm.empty();
    bool heading_sample_available =
        !heading_degrees.empty() || !smoothed_heading_degrees.empty();
    bool heading_per_second_available =
        !heading_per_second_degrees.empty() &&
        !heading_per_second_time.empty() &&
        heading_per_second_degrees.size() == heading_per_second_time.size();
    const std::string primary_speed_label =
        context.zarr_loader.getMovementPrimarySpeedLabel();
    const std::string primary_speed_units =
        context.zarr_loader.getMovementPrimarySpeedUnits();
    const std::string secondary_speed_label =
        context.zarr_loader.getMovementSecondarySpeedLabel();
    const std::string secondary_speed_units =
        context.zarr_loader.getMovementSecondarySpeedUnits();

    const bool has_track_timeline =
        selected_series != nullptr && !time_data.empty() &&
        (smoothed_available || instant_available || distance_available ||
         heading_sample_available || heading_per_second_available);
    const bool has_eye_angle_timeline =
        context.zarr_loader.hasEyeAngleAnalysisData();
    const bool has_tail_kinematics_timeline =
        context.zarr_loader.hasTailKinematicsData();
    const bool has_stimulus_context =
        context.zarr_loader.hasStimulusSteps() ||
        context.zarr_loader.hasStimulusEvents();

    if (!has_track_timeline && !has_eye_angle_timeline &&
        !has_tail_kinematics_timeline && !has_stimulus_context) {
        ImGui::TextUnformatted("No analysis timeline data available.");
        ImGui::End();
        return;
    }

    {
        const auto controls_start = std::chrono::steady_clock::now();
        ImGui::BeginDisabled(!has_stimulus_context);
        ImGui::Checkbox("Show stimulus context", &state.show_stimulus_context);
        ImGui::EndDisabled();
        if (!has_stimulus_context && state.show_stimulus_context) {
            ImGui::TextDisabled("Stimulus context unavailable.");
        }
        if (perf != nullptr) {
            perf->controls_ms +=
                durationMs(std::chrono::steady_clock::now() - controls_start);
        }
    }

    const double fallback_current_time =
        currentTimeSeconds(context).value_or(-1.0);

    if (has_track_timeline) {
        const auto motion_controls_start = std::chrono::steady_clock::now();
        motion_selection = drawAnalysisTimelineMotionControls({
            context.zarr_loader,
            context.scroll_state,
            selected_series,
            time_data.size(),
            smoothed_available,
            instant_available,
            distance_available,
            heading_sample_available,
            heading_per_second_available,
            primary_speed_label,
            primary_speed_units,
            secondary_speed_label,
        }, state);
        if (perf != nullptr) {
            perf->controls_ms +=
                durationMs(std::chrono::steady_clock::now() -
                           motion_controls_start);
        }

        static MotionPreparedCache motion_cache;
        AnalysisTimelineMotionDataInput motion_input{
            time_data,
            frame_indices,
            smoothed_speed,
            instant_speed,
            distance_mm,
            heading_degrees,
            smoothed_heading_degrees,
            heading_keypoint_success,
            heading_per_second_degrees,
            heading_per_second_resultant,
            heading_per_second_time,
            motion_selection.selected_swim_bouts,
            smoothed_available,
            instant_available,
            heading_per_second_available,
            state.show_detector_response,
            context.video_fps,
        };
        {
            const auto prepare_start = std::chrono::steady_clock::now();
            const MotionPreparedCacheKey cache_key =
                makeMotionPreparedCacheKey(motion_input, selected_series);
            const bool cache_hit =
                motion_cache.valid && motion_cache.key == cache_key;
            if (!cache_hit) {
                motion_cache.data =
                    prepareAnalysisTimelineMotionData(motion_input);
                motion_cache.key = cache_key;
                motion_cache.valid = true;
            }
            if (perf != nullptr) {
                perf->prepare_motion_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               prepare_start);
                if (!cache_hit) {
                    const uint64_t motion_points =
                        motionPreparedPointCount(motion_cache.data);
                    perf->motion_points_prepared += motion_points;
                    perf->prepared_points_total += motion_points;
                }
            }
        }
        const AnalysisTimelineMotionPreparedData& motion_data =
            motion_cache.data;
        std::vector<const AnalysisTimelineTracePlotRow*> linked_extra_rows;
        AnalysisTimelineMotionSummaryContext motion_summary_context{
            state,
            context.scroll_state,
            motion_selection.selected_series,
            time_data,
            smoothed_speed,
            motion_data,
            fallback_current_time,
            primary_speed_label,
            primary_speed_units,
        };
        static SingleTraceRowCache position_row_cache;
        const auto position_start = std::chrono::steady_clock::now();
        const std::string position_cache_key =
            makePositionTraceCacheKey(motion_summary_context);
        const bool position_cache_hit =
            position_row_cache.valid &&
            position_row_cache.key == position_cache_key;
        if (!position_cache_hit) {
            position_row_cache.row =
                buildAnalysisTimelineTrackPositionRow(motion_summary_context);
            position_row_cache.key = position_cache_key;
            position_row_cache.valid = true;
            if (perf != nullptr) {
                if (position_row_cache.row.has_value()) {
                    addPreparedTraceRowStats(
                        perf,
                        *position_row_cache.row,
                        perf->position_points_prepared);
                }
            }
        }
        if (position_row_cache.row.has_value()) {
            position_row_cache.row->current_time = fallback_current_time;
            linked_extra_rows.push_back(&*position_row_cache.row);
        }
        if (perf != nullptr) {
            perf->build_position_ms +=
                durationMs(std::chrono::steady_clock::now() - position_start);
        }
        if (has_eye_angle_timeline) {
            AnalysisTimelineEyeAngleContext eye_context{
                context.zarr_loader,
                context.scroll_state,
                context.current_frame_num,
                context.video_fps,
                fallback_current_time,
                context.perf_stats,
            };
            const auto eye_controls_start = std::chrono::steady_clock::now();
            drawAnalysisTimelineEyeAngleControls(eye_context, state);
            if (perf != nullptr) {
                perf->controls_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               eye_controls_start);
            }
            static SingleTraceRowCache eye_row_cache;
            const auto eye_build_start = std::chrono::steady_clock::now();
            const std::string eye_cache_key =
                makeEyeTraceCacheKey(eye_context, state);
            const bool eye_cache_hit =
                eye_row_cache.valid && eye_row_cache.key == eye_cache_key;
            if (!eye_cache_hit) {
                eye_row_cache.row =
                    buildAnalysisTimelineEyeAngleRow(eye_context, state);
                eye_row_cache.key = eye_cache_key;
                eye_row_cache.valid = true;
                if (perf != nullptr) {
                    if (eye_row_cache.row.has_value()) {
                        addPreparedTraceRowStats(perf,
                                                 *eye_row_cache.row,
                                                 perf->eye_points_prepared);
                    }
                }
            }
            if (eye_row_cache.row.has_value()) {
                eye_row_cache.row->current_time =
                    currentEyeTimelineTime(eye_context);
                linked_extra_rows.push_back(&*eye_row_cache.row);
            }
            if (perf != nullptr) {
                perf->build_eye_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               eye_build_start);
            }
        }
        if (has_tail_kinematics_timeline) {
            AnalysisTimelineTailKinematicsContext tail_context{
                context.zarr_loader,
                context.scroll_state,
                context.current_frame_num,
                context.video_fps,
                fallback_current_time,
                context.perf_stats,
            };
            const auto tail_controls_start = std::chrono::steady_clock::now();
            drawAnalysisTimelineTailKinematicsControls(tail_context, state);
            if (perf != nullptr) {
                perf->controls_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               tail_controls_start);
            }
            static TraceRowsCache tail_rows_cache;
            const auto tail_build_start = std::chrono::steady_clock::now();
            const std::string tail_cache_key =
                makeTailTraceCacheKey(tail_context, state);
            const bool tail_cache_hit =
                tail_rows_cache.valid &&
                tail_rows_cache.key == tail_cache_key;
            if (!tail_cache_hit) {
                tail_rows_cache.rows =
                    buildAnalysisTimelineTailKinematicsRows(tail_context,
                                                            state);
                tail_rows_cache.key = tail_cache_key;
                tail_rows_cache.valid = true;
                if (perf != nullptr) {
                    for (const auto& row : tail_rows_cache.rows) {
                        addPreparedTraceRowStats(perf,
                                                 row,
                                                 perf->tail_points_prepared);
                    }
                }
            }
            const double current_tail_time =
                currentTailTimelineTime(tail_context);
            for (auto& row : tail_rows_cache.rows) {
                row.current_time = current_tail_time;
                linked_extra_rows.push_back(&row);
            }
            if (perf != nullptr) {
                perf->build_tail_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               tail_build_start);
            }
        }
        const size_t lod_warmup_bucket_count =
            std::max<size_t>(
                1,
                static_cast<size_t>(
                    std::ceil(std::max(1.0f,
                                       ImGui::GetContentRegionAvail().x))));
        const std::string lod_warmup_key =
            makeTimelineLodWarmupKey(state,
                                     motion_data,
                                     linked_extra_rows,
                                     lod_warmup_bucket_count);
        if (state.timeline_lod_warmup_key != lod_warmup_key) {
            state.timeline_lod_warmup_key = lod_warmup_key;
            state.timeline_lod_warmup_cursor = 0;
        }
        if (!state.timeline_lod_warmup_key.empty()) {
            prewarmTimelineLodsStep(state,
                                    motion_data,
                                    linked_extra_rows,
                                    lod_warmup_bucket_count,
                                    state.timeline_lod_warmup_cursor,
                                    2);
        }
        const double current_time_line = drawAnalysisTimelineMotionPlots({
            state,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            time_data,
            frame_indices,
            motion_data.time_plot,
            motion_data.smoothed_plot,
            motion_data.instant_plot,
            motion_data.detector_time_plot,
            motion_data.detector_value_plot,
            motion_data.heading_time_raw,
            motion_data.heading_raw_plot,
            motion_data.heading_time_smoothed,
            motion_data.heading_smoothed_plot,
            motion_data.heading_per_second_time_plot,
            motion_data.heading_per_second_plot,
            motion_data.heading_per_second_resultant_plot,
            motion_data.distance_time,
            motion_data.distance_units,
            motion_selection.selected_swim_bouts,
            primary_speed_label,
            primary_speed_units,
            secondary_speed_label,
            secondary_speed_units,
            motion_data.heading_axis_min,
            motion_data.heading_axis_max,
            motion_data.max_distance_mm,
            &context.zarr_loader,
            state.show_stimulus_context && has_stimulus_context,
            &linked_extra_rows,
            context.perf_stats,
        });
        const auto summary_start = std::chrono::steady_clock::now();
        drawAnalysisTimelineMotionSummary({
            state,
            context.scroll_state,
            motion_selection.selected_series,
            time_data,
            smoothed_speed,
            motion_data,
            current_time_line,
            primary_speed_label,
            primary_speed_units,
        });
        if (perf != nullptr) {
            perf->summary_ms +=
                durationMs(std::chrono::steady_clock::now() - summary_start);
        }
    } else {
        ImGui::TextDisabled("Track kinematics traces unavailable.");
        if (state.show_stimulus_context && has_stimulus_context) {
            const auto stimulus_start = std::chrono::steady_clock::now();
            drawAnalysisTimelineStimulusContext({
                context.zarr_loader,
                context.scroll_state,
                context.current_frame_num,
                context.video_fps,
            });
            if (perf != nullptr) {
                perf->draw_stimulus_context_ms +=
                    durationMs(std::chrono::steady_clock::now() -
                               stimulus_start);
            }
        }
    }

    if (!has_track_timeline && has_eye_angle_timeline) {
        const auto eye_start = std::chrono::steady_clock::now();
        drawAnalysisTimelineEyeAngleSection({
            context.zarr_loader,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            fallback_current_time,
            context.perf_stats,
        }, state);
        if (perf != nullptr) {
            perf->standalone_eye_ms +=
                durationMs(std::chrono::steady_clock::now() - eye_start);
        }
    }

    if (!has_track_timeline && has_tail_kinematics_timeline) {
        const auto tail_start = std::chrono::steady_clock::now();
        drawAnalysisTimelineTailKinematicsSection({
            context.zarr_loader,
            context.scroll_state,
            context.current_frame_num,
            context.video_fps,
            fallback_current_time,
            context.perf_stats,
        }, state);
        if (perf != nullptr) {
            perf->standalone_tail_ms +=
                durationMs(std::chrono::steady_clock::now() - tail_start);
        }
    }

    ImGui::End();
}
