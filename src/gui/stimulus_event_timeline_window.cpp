#include "gui/stimulus_event_timeline_window.h"

#include "imgui.h"
#include "implot.h"
#include "zarr_loader.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
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

std::string stimulusEventTypeLabel(const std::string& label) {
    std::string type_name = label;
    size_t dash_pos = type_name.find(" - ");
    if (dash_pos != std::string::npos) {
        type_name = type_name.substr(0, dash_pos);
    }
    return type_name;
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

    auto timeline = context.zarr_loader.getStimulusEventTimeline();
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

    if (timeline.empty()) {
        ImGui::TextUnformatted("No stimulus events found.");
        ImGui::End();
        return result;
    }

    std::unordered_map<int32_t, std::string> event_type_labels;
    for (const auto& evt : timeline) {
        if (event_type_labels.find(evt.event_type_id) == event_type_labels.end()) {
            event_type_labels[evt.event_type_id] =
                stimulusEventTypeLabel(evt.label);
            if (!state.filter_initialized) {
                state.event_type_filter[evt.event_type_id] = true;
            }
        }
    }
    state.filter_initialized = true;

    if (state.selected_event_idx >= static_cast<int>(timeline.size())) {
        state.selected_event_idx = -1;
    }

    auto resolveTimelineTargetFrame =
        [&](const ZarrDetectionLoader::StimulusEventSummary& evt) -> int32_t {
        if (evt.stimulus_frame_num >= 0) {
            if (auto mapped = context.zarr_loader.getCameraFrameForStimulusFrame(
                    evt.stimulus_frame_num, true)) {
                return *mapped;
            }
        }
        if (evt.camera_frame_id >= 0) {
            return evt.camera_frame_id;
        }
        return evt.stimulus_frame_num;
    };

    std::vector<double> x_values(timeline.size());
    std::vector<double> y_values(timeline.size(), 0.0);
    std::vector<int32_t> display_frames(timeline.size());
    for (size_t i = 0; i < timeline.size(); ++i) {
        const auto& evt = timeline[i];
        int32_t frame = resolveTimelineTargetFrame(evt);
        display_frames[i] = frame;
        int32_t clamped = frame >= 0 ? frame : 0;
        x_values[i] = (context.video_fps > 0.0)
                          ? static_cast<double>(clamped) / context.video_fps
                          : static_cast<double>(clamped);
    }

    size_t timeline_signature = timeline.size();
    if (!timeline.empty()) {
        auto signature_value = [&](int32_t frame) -> size_t {
            return static_cast<size_t>(std::max(frame, 0));
        };
        timeline_signature = timeline_signature * 1315423911u +
                             signature_value(display_frames.front());
        timeline_signature = timeline_signature * 2654435761u +
                             signature_value(display_frames.back());
    }

    double default_max_time =
        (context.video_fps > 0.0)
            ? static_cast<double>(
                  std::max<size_t>(1, context.zarr_loader.getTotalFrames())) /
                  context.video_fps
            : static_cast<double>(
                  std::max<size_t>(1, context.zarr_loader.getTotalFrames()));
    for (int32_t frame : display_frames) {
        if (frame >= 0) {
            double candidate =
                (context.video_fps > 0.0)
                    ? static_cast<double>(frame + 1) / context.video_fps
                    : static_cast<double>(frame + 1);
            default_max_time = std::max(default_max_time, candidate);
        }
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

        double timeline_min_time = 0.0;
        double timeline_max_time = default_max_time;
        if (!x_values.empty()) {
            auto minmax = std::minmax_element(x_values.begin(), x_values.end());
            timeline_min_time = *minmax.first;
            timeline_max_time = std::max(default_max_time, *minmax.second);
        }
        if (timeline_max_time <= timeline_min_time) {
            timeline_max_time = timeline_min_time + 0.5;
        }

        bool reset_limits =
            (!context.scroll_state.enabled && context.scroll_state.prev_enabled) ||
            (timeline_signature != state.cached_timeline_signature);

        if (context.scroll_state.enabled && current_time >= 0.0 &&
            timeline_max_time > timeline_min_time) {
            double half_span = static_cast<double>(std::max(
                0.1f, context.scroll_state.window_half_span_s));
            double window_min = current_time - half_span;
            double window_max = current_time + half_span;
            if (!x_values.empty()) {
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
            state.cached_timeline_signature = timeline_signature;
        } else if (reset_limits) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    timeline_min_time,
                                    timeline_max_time,
                                    ImGuiCond_Always);
            state.cached_timeline_signature = timeline_signature;
        }

        for (const auto& [event_type_id, type_label] : event_type_labels) {
            if (!state.event_type_filter[event_type_id]) {
                continue;
            }

            std::vector<double> type_x_values;
            std::vector<double> type_y_values;
            for (size_t i = 0; i < timeline.size(); ++i) {
                if (timeline[i].event_type_id == event_type_id) {
                    type_x_values.push_back(x_values[i]);
                    type_y_values.push_back(y_values[i]);
                }
            }

            if (!type_x_values.empty()) {
                ImVec4 color = getStimulusEventTypeColor(event_type_id);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                           6.0f,
                                           color,
                                           1.5f,
                                           ImVec4(0, 0, 0, 0));
                ImPlot::PlotScatter(type_label.c_str(),
                                    type_x_values.data(),
                                    type_y_values.data(),
                                    static_cast<int>(type_x_values.size()));
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
            for (size_t i = 0; i < x_values.size(); ++i) {
                ImVec2 event_pixels =
                    ImPlot::PlotToPixels(ImPlotPoint(x_values[i], y_values[i]));
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
                const int target_frame = display_frames[hovered_event_idx];
                if (target_frame >= 0) {
                    result.seek_target_frame = target_frame;
                }
            }
        }

        if (hovered_event_idx != -1) {
            const auto& hovered_evt = timeline[hovered_event_idx];
            const int32_t resolved_frame = display_frames[hovered_event_idx];
            const int32_t camera_frame = hovered_evt.camera_frame_id;
            const int32_t stim_frame = hovered_evt.stimulus_frame_num;
            if (resolved_frame >= 0) {
                double event_time = x_values[hovered_event_idx];
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
                                  x_values[hovered_event_idx],
                                  hovered_evt.label.c_str());
            }
        }

        if (state.selected_event_idx >= 0 &&
            state.selected_event_idx < static_cast<int>(x_values.size())) {
            double selected_x = x_values[state.selected_event_idx];
            double selected_y = y_values[state.selected_event_idx];
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
        for (const auto& [event_type_id, type_label] : event_type_labels) {
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
        const auto& evt = timeline[i];
        int32_t resolved_frame = display_frames[i];
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
        ImGui::PushID(static_cast<int>(i));
        bool is_selected = (state.selected_event_idx == static_cast<int>(i));
        if (ImGui::Selectable(row_label.str().c_str(), is_selected)) {
            state.selected_event_idx = static_cast<int>(i);
            int target_frame = display_frames[i];
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
        const int32_t resolved_frame = display_frames[state.selected_event_idx];
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
