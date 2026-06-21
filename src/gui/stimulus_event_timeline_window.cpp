#include "gui/stimulus_event_timeline_window.h"

#include "imgui.h"
#include "implot.h"
#include "zarr_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace {

ImVec4 getStimulusEventTypeColor(int32_t event_type_id) {
    uint32_t hash = static_cast<uint32_t>(event_type_id) * 2654435761u;
    float h = (hash % 360) / 360.0f;
    float s = 0.7f + 0.25f * ((hash >> 8) % 100) / 100.0f;
    float v = 0.8f + 0.2f * ((hash >> 16) % 100) / 100.0f;

    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r;
    float g;
    float b;
    if (h < 1.0f / 6.0f) {
        r = c;
        g = x;
        b = 0.0f;
    } else if (h < 2.0f / 6.0f) {
        r = x;
        g = c;
        b = 0.0f;
    } else if (h < 3.0f / 6.0f) {
        r = 0.0f;
        g = c;
        b = x;
    } else if (h < 4.0f / 6.0f) {
        r = 0.0f;
        g = x;
        b = c;
    } else if (h < 5.0f / 6.0f) {
        r = x;
        g = 0.0f;
        b = c;
    } else {
        r = c;
        g = 0.0f;
        b = x;
    }
    return ImVec4(r + m, g + m, b + m, 0.9f);
}

ImU32 stimulusStepFillColor(const ZarrDetectionData::StimulusStep& step) {
    if (step.stimulus_mode == "MOVING_GRATING") {
        return IM_COL32(70, 140, 255, 54);
    }
    if (step.stimulus_mode == "CONCENTRIC_GRATING") {
        return IM_COL32(90, 210, 150, 54);
    }
    if (step.stimulus_mode == "LOOMING_DOT") {
        return IM_COL32(255, 185, 70, 54);
    }
    return IM_COL32(180, 180, 190, 42);
}

ImU32 stimulusStepBorderColor(const ZarrDetectionData::StimulusStep& step) {
    if (step.stimulus_mode == "MOVING_GRATING") {
        return IM_COL32(90, 160, 255, 150);
    }
    if (step.stimulus_mode == "CONCENTRIC_GRATING") {
        return IM_COL32(110, 235, 170, 150);
    }
    if (step.stimulus_mode == "LOOMING_DOT") {
        return IM_COL32(255, 205, 90, 150);
    }
    return IM_COL32(190, 190, 205, 120);
}

std::string stimulusEventTypeLabel(const std::string& label) {
    std::string type_name = label;
    size_t dash_pos = type_name.find(" - ");
    if (dash_pos != std::string::npos) {
        type_name = type_name.substr(0, dash_pos);
    }
    return type_name;
}

std::string stimulusStepDisplayLabel(
    const ZarrDetectionData::StimulusStep& step) {
    std::ostringstream label;
    label << "Step " << step.step_index;
    if (!step.step_name.empty()) {
        label << " " << step.step_name;
    } else if (!step.stimulus_mode.empty()) {
        label << " " << step.stimulus_mode;
    }
    if (step.moving_grating.present &&
        std::isfinite(step.moving_grating.grating_direction_camera_deg)) {
        label << " " << step.moving_grating.grating_direction_camera_deg
              << " deg";
    } else if (step.concentric_grating.present &&
               !step.concentric_grating.radial_polarity_authored.empty()) {
        label << " " << step.concentric_grating.radial_polarity_authored;
    }
    return label.str();
}

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double frameToTimelineTime(int32_t frame, double video_fps) {
    const int32_t clamped = frame >= 0 ? frame : 0;
    return (video_fps > 0.0) ? static_cast<double>(clamped) / video_fps
                             : static_cast<double>(clamped);
}

int32_t resolveTimelineTargetFrame(
    const StimulusEventTimelineWindowContext& context,
    const ZarrDetectionLoader::StimulusEventSummary& evt) {
    if (evt.camera_frame_id >= 0) {
        return evt.camera_frame_id;
    }
    if (evt.stimulus_frame_num >= 0) {
        if (auto mapped = context.zarr_loader.getCameraFrameForStimulusFrame(
                evt.stimulus_frame_num, true)) {
            return *mapped;
        }
    }
    return evt.stimulus_frame_num;
}

std::string stimulusTimelineRowLabel(
    const ZarrDetectionLoader::StimulusEventSummary& evt,
    int32_t resolved_frame) {
    std::ostringstream row_label;
    if (resolved_frame >= 0) {
        row_label << "Cam " << resolved_frame;
        if (evt.camera_frame_id >= 0 && evt.camera_frame_id != resolved_frame) {
            row_label << " (Event Cam " << evt.camera_frame_id << ")";
        }
        if (evt.stimulus_frame_num >= 0 &&
            evt.stimulus_frame_num != resolved_frame) {
            row_label << " (Stim " << evt.stimulus_frame_num << ")";
        }
    } else {
        row_label << "Stim " << evt.stimulus_frame_num;
    }
    row_label << "  " << evt.label;
    return row_label.str();
}

bool stimulusTimelineCacheMatches(
    const StimulusEventTimelineWindowState::PreparedTimelineCache& cache,
    const StimulusEventTimelineWindowContext& context,
    const std::vector<ZarrDetectionLoader::StimulusEventSummary>& timeline,
    const std::vector<ZarrDetectionData::StimulusStep>& steps) {
    return cache.valid &&
           cache.archive_path == context.zarr_loader.getArchivePath() &&
           cache.timeline_generation ==
               context.zarr_loader.getStimulusEventTimelineGeneration() &&
           cache.timeline_count == timeline.size() &&
           cache.step_count == steps.size() &&
           cache.total_frames == context.zarr_loader.getTotalFrames() &&
           cache.video_fps == context.video_fps;
}

const StimulusEventTimelineWindowState::PreparedTimelineCache&
prepareStimulusTimelineCache(
    const StimulusEventTimelineWindowContext& context,
    StimulusEventTimelineWindowState& state,
    const std::vector<ZarrDetectionLoader::StimulusEventSummary>& timeline,
    const std::vector<ZarrDetectionData::StimulusStep>& steps) {
    if (stimulusTimelineCacheMatches(state.prepared_cache,
                                     context,
                                     timeline,
                                     steps)) {
        return state.prepared_cache;
    }

    const auto rebuild_start = std::chrono::steady_clock::now();
    auto& cache = state.prepared_cache;
    cache = StimulusEventTimelineWindowState::PreparedTimelineCache{};
    cache.archive_path = context.zarr_loader.getArchivePath();
    cache.timeline_generation =
        context.zarr_loader.getStimulusEventTimelineGeneration();
    cache.timeline_count = timeline.size();
    cache.step_count = steps.size();
    cache.total_frames = context.zarr_loader.getTotalFrames();
    cache.video_fps = context.video_fps;
    cache.x_values.resize(timeline.size());
    cache.y_values.assign(timeline.size(), 0.0);
    cache.display_frames.resize(timeline.size());
    cache.row_labels.reserve(timeline.size());

    std::unordered_map<int32_t, size_t> type_index_by_id;
    for (size_t i = 0; i < timeline.size(); ++i) {
        const auto& evt = timeline[i];
        const int32_t frame = resolveTimelineTargetFrame(context, evt);
        if (evt.camera_frame_id < 0) {
            ++cache.missing_camera_count;
        }
        cache.display_frames[i] = frame;
        cache.x_values[i] = frameToTimelineTime(frame, context.video_fps);
        cache.row_labels.push_back(stimulusTimelineRowLabel(evt, frame));

        auto type_it = type_index_by_id.find(evt.event_type_id);
        if (type_it == type_index_by_id.end()) {
            const size_t type_index = cache.type_series.size();
            type_index_by_id[evt.event_type_id] = type_index;
            cache.event_type_labels.emplace_back(
                evt.event_type_id, stimulusEventTypeLabel(evt.label));

            StimulusEventTimelineWindowState::CachedTypeSeries series;
            series.event_type_id = evt.event_type_id;
            series.type_label = cache.event_type_labels.back().second;
            cache.type_series.push_back(std::move(series));
            type_it = type_index_by_id.find(evt.event_type_id);
        }

        auto& series = cache.type_series[type_it->second];
        series.x_values.push_back(cache.x_values[i]);
        series.y_values.push_back(0.0);
    }

    std::unordered_map<int32_t, bool> next_filter;
    next_filter.reserve(cache.event_type_labels.size());
    for (const auto& entry : cache.event_type_labels) {
        const int32_t event_type_id = entry.first;
        auto existing = state.event_type_filter.find(event_type_id);
        next_filter[event_type_id] =
            (existing != state.event_type_filter.end()) ? existing->second
                                                        : true;
    }
    state.event_type_filter = std::move(next_filter);
    state.filter_initialized = true;

    double default_max_time =
        (context.video_fps > 0.0)
            ? static_cast<double>(std::max<size_t>(1, cache.total_frames)) /
                  context.video_fps
            : static_cast<double>(std::max<size_t>(1, cache.total_frames));
    for (int32_t frame : cache.display_frames) {
        if (frame >= 0) {
            default_max_time =
                std::max(default_max_time,
                         frameToTimelineTime(frame + 1, context.video_fps));
        }
    }
    for (const auto& step : steps) {
        if (step.start_camera_frame >= 0) {
            default_max_time =
                std::max(default_max_time,
                         frameToTimelineTime(step.start_camera_frame,
                                             context.video_fps));
        }
        if (step.end_camera_frame >= 0) {
            default_max_time =
                std::max(default_max_time,
                         frameToTimelineTime(step.end_camera_frame + 1,
                                             context.video_fps));
        }
    }

    cache.timeline_min_time = 0.0;
    cache.timeline_max_time = default_max_time;
    if (!cache.x_values.empty()) {
        auto minmax =
            std::minmax_element(cache.x_values.begin(), cache.x_values.end());
        cache.timeline_min_time = *minmax.first;
        cache.timeline_max_time = std::max(default_max_time, *minmax.second);
    }
    for (const auto& step : steps) {
        if (step.start_camera_frame >= 0) {
            cache.timeline_min_time =
                std::min(cache.timeline_min_time,
                         frameToTimelineTime(step.start_camera_frame,
                                             context.video_fps));
        }
        if (step.end_camera_frame >= 0) {
            cache.timeline_max_time =
                std::max(cache.timeline_max_time,
                         frameToTimelineTime(step.end_camera_frame + 1,
                                             context.video_fps));
        }
    }
    if (cache.timeline_max_time <= cache.timeline_min_time) {
        cache.timeline_max_time = cache.timeline_min_time + 0.5;
    }

    cache.timeline_signature = timeline.size() + steps.size() * 65537u;
    if (!cache.display_frames.empty()) {
        auto signature_value = [](int32_t frame) -> size_t {
            return static_cast<size_t>(std::max(frame, 0));
        };
        cache.timeline_signature =
            cache.timeline_signature * 1315423911u +
            signature_value(cache.display_frames.front());
        cache.timeline_signature =
            cache.timeline_signature * 2654435761u +
            signature_value(cache.display_frames.back());
    }

    cache.rebuild_ms =
        durationMs(std::chrono::steady_clock::now() - rebuild_start);
    cache.valid = true;

    std::cout << "  [StimulusTimelineCache] rebuilt events="
              << cache.timeline_count << ", steps=" << cache.step_count
              << ", types=" << cache.type_series.size()
              << ", missing_camera_ids=" << cache.missing_camera_count
              << ", fps=" << cache.video_fps
              << ", rebuild_ms=" << cache.rebuild_ms << std::endl;
    return cache;
}

}  // namespace

StimulusEventTimelineWindowResult drawStimulusEventTimelineWindow(
    const StimulusEventTimelineWindowContext& context,
    StimulusEventTimelineWindowState& state) {
    StimulusEventTimelineWindowResult result;
    if (!ImGui::Begin("Stimulus Event Timeline")) {
        ImGui::End();
        return result;
    }

    const auto& timeline = context.zarr_loader.getStimulusEventTimeline();
    const auto& steps = context.zarr_loader.getStimulusSteps();
    if (timeline.size() != state.last_logged_timeline_count) {
        size_t missing_camera = 0;
        for (const auto& evt : timeline) {
            if (evt.camera_frame_id < 0) {
                ++missing_camera;
            }
        }
        std::cout << "  [StimulusTimeline] Entries=" << timeline.size()
                  << ", missing_camera_ids=" << missing_camera << std::endl;
        const size_t preview = std::min<size_t>(timeline.size(), 5);
        for (size_t i = 0; i < preview; ++i) {
            const auto& evt = timeline[i];
            std::cout << "    [" << i << "] stim_frame="
                      << evt.stimulus_frame_num
                      << ", cam_frame=" << evt.camera_frame_id
                      << ", type=" << evt.event_type_id << ", label='"
                      << evt.label << "'" << std::endl;
        }
        state.last_logged_timeline_count = timeline.size();
    }
    if (steps.size() != state.last_logged_step_count) {
        std::cout << "  [StimulusStepsTimeline] Steps=" << steps.size();
        if (!context.zarr_loader.getStimulusStepsRunName().empty()) {
            std::cout << ", run='"
                      << context.zarr_loader.getStimulusStepsRunName()
                      << "'";
        }
        std::cout << std::endl;
        const size_t preview = std::min<size_t>(steps.size(), 5);
        for (size_t i = 0; i < preview; ++i) {
            const auto& step = steps[i];
            std::cout << "    [" << i << "] step=" << step.step_index
                      << ", mode='" << step.stimulus_mode
                      << "', frames=" << step.start_camera_frame << "-"
                      << step.end_camera_frame;
            if (step.moving_grating.present &&
                std::isfinite(
                    step.moving_grating.grating_direction_camera_deg)) {
                std::cout << ", camera_dir="
                          << step.moving_grating.grating_direction_camera_deg;
            }
            std::cout << std::endl;
        }
        state.last_logged_step_count = steps.size();
    }

    if (timeline.empty() && steps.empty()) {
        ImGui::TextUnformatted("No stimulus events or canonical steps found.");
        ImGui::End();
        return result;
    }

    const auto& cache =
        prepareStimulusTimelineCache(context, state, timeline, steps);

    if (state.selected_event_idx >= static_cast<int>(timeline.size())) {
        state.selected_event_idx = -1;
    }

    const auto* current_step =
        context.zarr_loader.getStimulusStepForFrame(context.current_frame_num);
    ImGui::SeparatorText("Current Canonical Step");
    if (current_step != nullptr) {
        ImGui::Text(
            "Run: %s",
            context.zarr_loader.getStimulusStepsRunName().empty()
                ? "unknown"
                : context.zarr_loader.getStimulusStepsRunName().c_str());
        ImGui::Text("Step %d: %s (%s)",
                    current_step->step_index,
                    current_step->step_name.empty()
                        ? "unnamed"
                        : current_step->step_name.c_str(),
                    current_step->stimulus_mode.empty()
                        ? "unknown mode"
                        : current_step->stimulus_mode.c_str());
        ImGui::Text("Frames: %d-%d | Duration: %.3f s",
                    current_step->start_camera_frame,
                    current_step->end_camera_frame,
                    current_step->duration_s);
        if (current_step->moving_grating.present) {
            ImGui::Text(
                "Moving grating camera direction: %.1f deg",
                current_step->moving_grating.grating_direction_camera_deg);
            ImGui::TextDisabled(
                "Authored orientation %.1f deg | Offset %.1f deg | %s%s",
                current_step->moving_grating.orientation_degrees_authored,
                current_step->moving_grating.camera_to_projector_offset_deg,
                current_step->moving_grating.direction_mapping_status.empty()
                    ? "mapping status unknown"
                    : current_step->moving_grating.direction_mapping_status
                          .c_str(),
                current_step->moving_grating.has_direction_mapping_validated
                    ? (current_step->moving_grating.direction_mapping_validated
                           ? " | validated"
                           : " | not auto-validated")
                    : "");
        }
        if (current_step->concentric_grating.present) {
            ImGui::Text("Concentric center: %.1f, %.1f px | %s",
                        current_step->concentric_grating.center_x_px,
                        current_step->concentric_grating.center_y_px,
                        current_step->concentric_grating
                                .radial_polarity_authored.empty()
                            ? "polarity unknown"
                            : current_step->concentric_grating
                                  .radial_polarity_authored.c_str());
            if (current_step->concentric_grating
                    .has_radial_polarity_validated &&
                !current_step->concentric_grating
                     .radial_polarity_validated) {
                ImGui::TextDisabled(
                    "Radial polarity is authored, not independently validated.");
            }
        }
    } else if (!steps.empty()) {
        ImGui::Text("No canonical step covers camera frame %d.",
                    context.current_frame_num);
    } else {
        ImGui::TextUnformatted("Canonical stimulus steps unavailable.");
    }

    ImGui::Spacing();
    ImGui::Checkbox("Scrolling Window (±s)##stimulus",
                    &context.scroll_state.enabled);
    if (context.scroll_state.enabled) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Half-span##stimulus_window_span",
                         &context.scroll_state.window_half_span_s,
                         0.1f,
                         0.5f,
                         60.0f,
                         "%.1f s");
        context.scroll_state.window_half_span_s =
            std::max(0.1f, context.scroll_state.window_half_span_s);
    }

    ImVec2 plot_size = ImVec2(ImGui::GetContentRegionAvail().x, 170.0f);
    if (ImPlot::BeginPlot("##stimulus_timeline_plot",
                          plot_size,
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes("Time (s)",
                          nullptr,
                          ImPlotAxisFlags_NoHighlight,
                          ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxis(ImAxis_Y1,
                          nullptr,
                          ImPlotAxisFlags_NoDecorations |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, 0.5, ImGuiCond_Always);

        double current_time =
            (context.video_fps > 0.0)
                ? static_cast<double>(context.current_frame_num) /
                      context.video_fps
                : static_cast<double>(context.current_frame_num);

        const double timeline_min_time = cache.timeline_min_time;
        const double timeline_max_time = cache.timeline_max_time;

        bool reset_limits =
            (!context.scroll_state.enabled && context.scroll_state.prev_enabled) ||
            (cache.timeline_signature != state.cached_timeline_signature);

        if (context.scroll_state.enabled && current_time >= 0.0 &&
            timeline_max_time > timeline_min_time) {
            double half_span = static_cast<double>(std::max(
                0.1f, context.scroll_state.window_half_span_s));
            double window_min = current_time - half_span;
            double window_max = current_time + half_span;
            if (!cache.x_values.empty()) {
                window_min = std::max(window_min, timeline_min_time);
                window_max = std::min(window_max, timeline_max_time);
            } else {
                window_min = std::max(window_min, 0.0);
                window_max = std::min(window_max, timeline_max_time);
            }
            if (window_max - window_min < 0.1) {
                double pad = std::max(0.1, half_span);
                window_min = std::max(timeline_min_time, current_time - pad);
                window_max = std::min(timeline_max_time, current_time + pad);
                if (window_max <= window_min) {
                    window_min =
                        std::max(timeline_min_time, timeline_max_time - pad);
                    window_max = timeline_max_time;
                }
            }
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    window_min,
                                    window_max,
                                    ImGuiCond_Always);
            state.cached_timeline_signature = cache.timeline_signature;
        } else if (reset_limits) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    timeline_min_time,
                                    timeline_max_time,
                                    ImGuiCond_Always);
            state.cached_timeline_signature = cache.timeline_signature;
        }

        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
        for (const auto& step : steps) {
            if (step.start_camera_frame < 0 || step.end_camera_frame < 0) {
                continue;
            }
            const double x0 =
                (context.video_fps > 0.0)
                    ? static_cast<double>(step.start_camera_frame) /
                          context.video_fps
                    : static_cast<double>(step.start_camera_frame);
            const double x1 =
                (context.video_fps > 0.0)
                    ? static_cast<double>(step.end_camera_frame + 1) /
                          context.video_fps
                    : static_cast<double>(step.end_camera_frame + 1);
            ImVec2 p0 = ImPlot::PlotToPixels(ImPlotPoint(x0, -0.45));
            ImVec2 p1 = ImPlot::PlotToPixels(ImPlotPoint(x1, 0.45));
            ImVec2 rect_min(std::min(p0.x, p1.x), std::min(p0.y, p1.y));
            ImVec2 rect_max(std::max(p0.x, p1.x), std::max(p0.y, p1.y));
            draw_list->AddRectFilled(
                rect_min, rect_max, stimulusStepFillColor(step), 0.0f);
            draw_list->AddRect(
                rect_min, rect_max, stimulusStepBorderColor(step), 0.0f);
            if (rect_max.x - rect_min.x > 80.0f) {
                const std::string label = stimulusStepDisplayLabel(step);
                draw_list->AddText(
                    ImVec2(rect_min.x + 4.0f, rect_min.y + 4.0f),
                    IM_COL32(220, 230, 245, 210),
                    label.c_str());
            }
        }

        for (const auto& series : cache.type_series) {
            if (!state.event_type_filter[series.event_type_id]) {
                continue;
            }

            if (!series.x_values.empty()) {
                ImVec4 color = getStimulusEventTypeColor(series.event_type_id);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                           6.0f,
                                           color,
                                           1.5f,
                                           ImVec4(0, 0, 0, 0));
                ImPlot::PlotScatter(series.type_label.c_str(),
                                    series.x_values.data(),
                                    series.y_values.data(),
                                    static_cast<int>(series.x_values.size()));
            }
        }

        double current_line_x[2] = {current_time, current_time};
        double current_line_y[2] = {-1.0, 1.0};
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 2.0f);
        ImPlot::PlotLine("Current Frame", current_line_x, current_line_y, 2);

        int hovered_event_idx = -1;
        constexpr float kSelectionRadiusPx = 12.0f;
        if (ImPlot::IsPlotHovered()) {
            ImVec2 mouse_pos = ImGui::GetIO().MousePos;
            float best_distance = kSelectionRadiusPx;
            for (size_t i = 0; i < cache.x_values.size(); ++i) {
                ImVec2 event_pixels =
                    ImPlot::PlotToPixels(
                        ImPlotPoint(cache.x_values[i], cache.y_values[i]));
                float dx = mouse_pos.x - event_pixels.x;
                float dy = mouse_pos.y - event_pixels.y;
                float distance = std::sqrt(dx * dx + dy * dy);
                if (distance < best_distance) {
                    best_distance = distance;
                    hovered_event_idx = static_cast<int>(i);
                }
            }

            if (hovered_event_idx != -1 &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                state.selected_event_idx = hovered_event_idx;
                const int target_frame =
                    cache.display_frames[hovered_event_idx];
                if (target_frame >= 0) {
                    result.seek_target_frame = target_frame;
                }
            }
        }

        if (hovered_event_idx != -1) {
            const auto& hovered_evt = timeline[hovered_event_idx];
            const int32_t resolved_frame =
                cache.display_frames[hovered_event_idx];
            const int32_t camera_frame = hovered_evt.camera_frame_id;
            const int32_t stim_frame = hovered_evt.stimulus_frame_num;
            if (resolved_frame >= 0) {
                double event_time = cache.x_values[hovered_event_idx];
                if (camera_frame >= 0 && camera_frame != resolved_frame) {
                    ImGui::SetTooltip(
                        "Camera Frame %d (resolved)\nEvent Camera Frame %d\nStimulus Frame %d\nTime %.3f s\n%s",
                        resolved_frame,
                        camera_frame,
                        std::max(stim_frame, 0),
                        event_time,
                        hovered_evt.label.c_str());
                } else if (stim_frame >= 0 && stim_frame != resolved_frame) {
                    ImGui::SetTooltip(
                        "Camera Frame %d\nStimulus Frame %d\nTime %.3f s\n%s",
                        resolved_frame,
                        stim_frame,
                        event_time,
                        hovered_evt.label.c_str());
                } else {
                    ImGui::SetTooltip("Camera Frame %d\nTime %.3f s\n%s",
                                      resolved_frame,
                                      event_time,
                                      hovered_evt.label.c_str());
                }
            } else {
                ImGui::SetTooltip("Frame %d\nTime %.3f s\n%s",
                                  std::max(stim_frame, 0),
                                  cache.x_values[hovered_event_idx],
                                  hovered_evt.label.c_str());
            }
        }

        if (state.selected_event_idx >= 0 &&
            state.selected_event_idx < static_cast<int>(cache.x_values.size())) {
            double selected_x = cache.x_values[state.selected_event_idx];
            double selected_y = cache.y_values[state.selected_event_idx];
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond,
                                       9.0f,
                                       ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                                       2.0f,
                                       ImVec4(0, 0, 0, 0));
            ImPlot::PlotScatter("Selected Event", &selected_x, &selected_y, 1);
        }
        ImPlot::EndPlot();
    }

    ImGui::SeparatorText("Event Type Legend & Filter");
    ImGui::BeginChild("##event_type_legend", ImVec2(0, 120), true);
    if (ImGui::SmallButton("Show All")) {
        for (auto& [type_id, enabled] : state.event_type_filter) {
            enabled = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Hide All")) {
        for (auto& [type_id, enabled] : state.event_type_filter) {
            enabled = false;
        }
    }

    ImGui::Separator();
    int col_count =
        std::max(1,
                 static_cast<int>(ImGui::GetContentRegionAvail().x / 250.0f));
    if (ImGui::BeginTable("##legend_table",
                          col_count,
                          ImGuiTableFlags_SizingStretchSame)) {
        int col_idx = 0;
        for (const auto& [event_type_id, type_label] : cache.event_type_labels) {
            if (col_idx % col_count == 0) {
                ImGui::TableNextRow();
            }
            ImGui::TableNextColumn();

            ImGui::PushID(event_type_id);
            ImVec4 color = getStimulusEventTypeColor(event_type_id);
            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
            ImGui::SmallButton("  ");
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            bool& enabled = state.event_type_filter[event_type_id];
            ImGui::Checkbox(type_label.c_str(), &enabled);
            ImGui::PopID();
            ++col_idx;
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Event List");
    ImGui::BeginChild("##stimulus_event_list", ImVec2(0, 200), true);
    for (size_t i = 0; i < timeline.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool is_selected = (state.selected_event_idx == static_cast<int>(i));
        const char* row_label =
            (i < cache.row_labels.size()) ? cache.row_labels[i].c_str() : "";
        if (ImGui::Selectable(row_label, is_selected)) {
            state.selected_event_idx = static_cast<int>(i);
            int target_frame = cache.display_frames[i];
            if (target_frame >= 0) {
                result.seek_target_frame = target_frame;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (state.selected_event_idx >= 0 &&
        state.selected_event_idx < static_cast<int>(timeline.size())) {
        const auto& evt = timeline[state.selected_event_idx];
        const int32_t resolved_frame =
            cache.display_frames[state.selected_event_idx];
        ImGui::Separator();
        ImGui::Text("Selected Event:");
        if (resolved_frame >= 0) {
            ImGui::BulletText("Camera Frame: %d", resolved_frame);
        }
        if (evt.camera_frame_id >= 0 && evt.camera_frame_id != resolved_frame) {
            ImGui::BulletText("Event Camera Frame: %d", evt.camera_frame_id);
        }
        if (evt.stimulus_frame_num >= 0) {
            ImGui::BulletText("Stimulus Frame: %d", evt.stimulus_frame_num);
        }
        ImGui::BulletText("Type ID: %d", evt.event_type_id);
        ImGui::BulletText("%s", evt.label.c_str());
    }

    ImGui::End();
    return result;
}
