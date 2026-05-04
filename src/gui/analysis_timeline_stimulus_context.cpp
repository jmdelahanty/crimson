#include "gui/analysis_timeline_stimulus_context.h"

#include "imgui.h"
#include "implot.h"
#include "zarr_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

double frameToTimelineX(int32_t frame, double video_fps) {
    if (frame < 0) {
        return 0.0;
    }
    if (video_fps > 0.0) {
        return static_cast<double>(frame) / video_fps;
    }
    return static_cast<double>(frame);
}

int32_t resolveStimulusEventFrame(
    const ZarrDetectionLoader& loader,
    const ZarrDetectionLoader::StimulusEventSummary& event) {
    if (event.stimulus_frame_num >= 0) {
        if (auto mapped = loader.getCameraFrameForStimulusFrame(
                event.stimulus_frame_num, true)) {
            return *mapped;
        }
    }
    if (event.camera_frame_id >= 0) {
        return event.camera_frame_id;
    }
    return event.stimulus_frame_num;
}

ImU32 stimulusStepFillColor(const ZarrDetectionData::StimulusStep& step) {
    if (step.stimulus_mode == "MOVING_GRATING") {
        return IM_COL32(70, 140, 255, 48);
    }
    if (step.stimulus_mode == "CONCENTRIC_GRATING") {
        return IM_COL32(90, 210, 150, 48);
    }
    if (step.stimulus_mode == "LOOMING_DOT") {
        return IM_COL32(255, 185, 70, 48);
    }
    return IM_COL32(180, 180, 190, 38);
}

ImU32 stimulusStepBorderColor(const ZarrDetectionData::StimulusStep& step) {
    if (step.stimulus_mode == "MOVING_GRATING") {
        return IM_COL32(90, 160, 255, 145);
    }
    if (step.stimulus_mode == "CONCENTRIC_GRATING") {
        return IM_COL32(110, 235, 170, 145);
    }
    if (step.stimulus_mode == "LOOMING_DOT") {
        return IM_COL32(255, 205, 90, 145);
    }
    return IM_COL32(190, 190, 205, 115);
}

ImU32 stimulusEventColor(int32_t event_type_id) {
    switch (event_type_id) {
    case 0:
    case 1:
    case 4:
        return IM_COL32(235, 235, 245, 210);
    case 11:
    case 12:
        return IM_COL32(255, 210, 90, 230);
    case 56:
        return IM_COL32(120, 190, 255, 235);
    default: {
        const uint32_t hash =
            static_cast<uint32_t>(event_type_id) * 2654435761u;
        const uint8_t r = static_cast<uint8_t>(120u + (hash & 0x7fu));
        const uint8_t g =
            static_cast<uint8_t>(120u + ((hash >> 8u) & 0x7fu));
        const uint8_t b =
            static_cast<uint8_t>(120u + ((hash >> 16u) & 0x7fu));
        return IM_COL32(r, g, b, 220);
    }
    }
}

std::string stimulusStepDisplayLabel(
    const ZarrDetectionData::StimulusStep& step) {
    std::ostringstream label;
    if (!step.step_name.empty()) {
        label << step.step_name;
    } else if (!step.stimulus_mode.empty()) {
        label << step.stimulus_mode;
    } else {
        label << "Step " << step.step_index;
    }
    if (step.moving_grating.present &&
        std::isfinite(step.moving_grating.grating_direction_camera_deg)) {
        label << " | "
              << step.moving_grating.grating_direction_camera_deg << " deg";
    } else if (step.concentric_grating.present &&
               !step.concentric_grating.radial_polarity_authored.empty()) {
        label << " | "
              << step.concentric_grating.radial_polarity_authored;
    }
    return label.str();
}

void drawStimulusDirectionGlyph(const ZarrDetectionData::StimulusStep& step,
                                const ImVec2& rect_min,
                                const ImVec2& rect_max,
                                ImDrawList& draw_list) {
    if (!step.moving_grating.present ||
        !std::isfinite(step.moving_grating.grating_direction_camera_deg)) {
        return;
    }
    const float width = rect_max.x - rect_min.x;
    const float height = rect_max.y - rect_min.y;
    if (width < 42.0f || height < 20.0f) {
        return;
    }
    constexpr double kPi = 3.14159265358979323846;
    const double radians =
        step.moving_grating.grating_direction_camera_deg * kPi / 180.0;
    const float dx = static_cast<float>(std::cos(radians));
    const float dy = static_cast<float>(-std::sin(radians));
    const float len = std::min(42.0f, std::max(18.0f, width * 0.22f));
    const ImVec2 center(std::clamp(rect_min.x + 0.5f * width,
                                   rect_min.x + 24.0f,
                                   rect_max.x - 24.0f),
                        rect_min.y + 0.5f * height);
    const ImVec2 p0(center.x - dx * len * 0.5f,
                    center.y - dy * len * 0.5f);
    const ImVec2 p1(center.x + dx * len * 0.5f,
                    center.y + dy * len * 0.5f);
    const ImU32 color = IM_COL32(185, 220, 255, 235);
    draw_list.AddLine(p0, p1, color, 1.8f);
    const ImVec2 back(-dx, -dy);
    const ImVec2 normal(-dy, dx);
    constexpr float head = 6.0f;
    draw_list.AddTriangleFilled(
        p1,
        ImVec2(p1.x + back.x * head + normal.x * head * 0.55f,
               p1.y + back.y * head + normal.y * head * 0.55f),
        ImVec2(p1.x + back.x * head - normal.x * head * 0.55f,
               p1.y + back.y * head - normal.y * head * 0.55f),
        color);
}

void drawCurrentTimeMarker(double current_time) {
    if (current_time < 0.0) {
        return;
    }
    double xs[2] = {current_time, current_time};
    double ys[2] = {-1.0, 1.0};
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 2.0f);
    ImPlot::PlotLine("Current Frame##stimulus_context_current", xs, ys, 2);
}

}  // namespace

void drawAnalysisTimelineStimulusContext(
    const AnalysisTimelineStimulusContextInput& context) {
    const auto& steps = context.zarr_loader.getStimulusSteps();
    const auto events = context.zarr_loader.getStimulusEventTimeline();
    if (steps.empty() && events.empty()) {
        ImGui::TextDisabled("Stimulus context unavailable.");
        return;
    }

    double min_time = 0.0;
    double max_time = frameToTimelineX(
        static_cast<int32_t>(
            std::max<size_t>(1, context.zarr_loader.getTotalFrames())),
        context.video_fps);
    for (const auto& step : steps) {
        if (step.start_camera_frame >= 0) {
            min_time = std::min(
                min_time,
                frameToTimelineX(step.start_camera_frame, context.video_fps));
        }
        if (step.end_camera_frame >= 0) {
            max_time = std::max(
                max_time,
                frameToTimelineX(step.end_camera_frame + 1,
                                 context.video_fps));
        }
    }

    struct EventPoint {
        const ZarrDetectionLoader::StimulusEventSummary* event = nullptr;
        int32_t frame = -1;
        double time = 0.0;
    };
    std::vector<EventPoint> event_points;
    event_points.reserve(events.size());
    for (const auto& event : events) {
        const int32_t frame = resolveStimulusEventFrame(context.zarr_loader,
                                                        event);
        if (frame < 0) {
            continue;
        }
        const double t = frameToTimelineX(frame, context.video_fps);
        max_time = std::max(max_time, t);
        event_points.push_back(EventPoint{&event, frame, t});
    }
    if (max_time <= min_time) {
        max_time = min_time + 0.5;
    }

    const double current_time =
        frameToTimelineX(context.current_frame_num, context.video_fps);
    double window_min = min_time;
    double window_max = max_time;
    if (context.scroll_state.enabled && current_time >= 0.0) {
        const double half_span =
            static_cast<double>(
                std::max(0.1f, context.scroll_state.window_half_span_s));
        window_min = std::max(min_time, current_time - half_span);
        window_max = std::min(max_time, current_time + half_span);
        if (window_max - window_min < 0.1) {
            window_min = std::max(min_time, current_time - half_span);
            window_max = std::min(max_time, current_time + half_span);
            if (window_max <= window_min) {
                window_min = std::max(min_time, max_time - half_span);
                window_max = max_time;
            }
        }
    }
    if (context.use_external_x_limits &&
        context.external_x_max > context.external_x_min) {
        window_min = context.external_x_min;
        window_max = context.external_x_max;
    }

    const char* x_axis_label = context.video_fps > 0.0 ? "Time (s)" : "Frame";
    if (ImPlot::BeginPlot("Stimulus Context##analysis_timeline_stimulus",
                          context.embedded_in_subplots
                              ? ImVec2(-1.0f, 0.0f)
                              : ImVec2(-1.0f, 110.0f),
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(x_axis_label,
                          nullptr,
                          ImPlotAxisFlags_NoHighlight,
                          ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxis(ImAxis_Y1,
                          nullptr,
                          ImPlotAxisFlags_NoDecorations |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, 0.5, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_X1,
                                window_min,
                                window_max,
                                context.external_x_limits_always ||
                                        context.scroll_state.enabled
                                    ? ImGuiCond_Always
                                    : ImGuiCond_Once);

        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
        ImPlotRect limits = ImPlot::GetPlotLimits();
        const ImVec2 lane_min =
            ImPlot::PlotToPixels(ImPlotPoint(limits.X.Min, 0.45));
        const ImVec2 lane_max =
            ImPlot::PlotToPixels(ImPlotPoint(limits.X.Max, -0.45));

        int hovered_step = -1;
        for (size_t i = 0; i < steps.size(); ++i) {
            const auto& step = steps[i];
            if (step.start_camera_frame < 0 || step.end_camera_frame < 0) {
                continue;
            }
            const double x0 =
                frameToTimelineX(step.start_camera_frame, context.video_fps);
            const double x1 =
                frameToTimelineX(step.end_camera_frame + 1, context.video_fps);
            if (x1 < limits.X.Min || x0 > limits.X.Max) {
                continue;
            }
            const ImVec2 p0 = ImPlot::PlotToPixels(ImPlotPoint(x0, 0.42));
            const ImVec2 p1 = ImPlot::PlotToPixels(ImPlotPoint(x1, -0.42));
            const ImVec2 rect_min(std::min(p0.x, p1.x),
                                  std::min(p0.y, p1.y));
            const ImVec2 rect_max(std::max(p0.x, p1.x),
                                  std::max(p0.y, p1.y));
            draw_list->AddRectFilled(rect_min,
                                     rect_max,
                                     stimulusStepFillColor(step),
                                     0.0f);
            draw_list->AddRect(rect_min,
                               rect_max,
                               stimulusStepBorderColor(step),
                               0.0f);
            drawStimulusDirectionGlyph(step, rect_min, rect_max, *draw_list);
            if (rect_max.x - rect_min.x > 92.0f) {
                const std::string label = stimulusStepDisplayLabel(step);
                draw_list->AddText(ImVec2(rect_min.x + 5.0f,
                                          rect_min.y + 4.0f),
                                   IM_COL32(220, 230, 245, 215),
                                   label.c_str());
            }
            if (ImPlot::IsPlotHovered()) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                if (mouse.x >= rect_min.x && mouse.x <= rect_max.x &&
                    mouse.y >= rect_min.y && mouse.y <= rect_max.y) {
                    hovered_step = static_cast<int>(i);
                }
            }
        }

        int hovered_event = -1;
        if (!event_points.empty()) {
            const ImU32 tick_shadow = IM_COL32(0, 0, 0, 170);
            for (size_t i = 0; i < event_points.size(); ++i) {
                const auto& point = event_points[i];
                if (point.time < limits.X.Min || point.time > limits.X.Max) {
                    continue;
                }
                const ImVec2 top =
                    ImPlot::PlotToPixels(ImPlotPoint(point.time, 0.46));
                const ImVec2 bottom =
                    ImPlot::PlotToPixels(ImPlotPoint(point.time, -0.46));
                draw_list->AddLine(ImVec2(top.x + 1.0f, top.y),
                                   ImVec2(bottom.x + 1.0f, bottom.y),
                                   tick_shadow,
                                   3.0f);
                draw_list->AddLine(top,
                                   bottom,
                                   stimulusEventColor(
                                       point.event->event_type_id),
                                   2.0f);
            }
            if (ImPlot::IsPlotHovered()) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                float best_distance = 8.0f;
                for (size_t i = 0; i < event_points.size(); ++i) {
                    const auto& point = event_points[i];
                    if (point.time < limits.X.Min ||
                        point.time > limits.X.Max) {
                        continue;
                    }
                    const ImVec2 pos =
                        ImPlot::PlotToPixels(ImPlotPoint(point.time, 0.0));
                    const float distance = std::fabs(mouse.x - pos.x);
                    if (mouse.y >= lane_min.y && mouse.y <= lane_max.y &&
                        distance < best_distance) {
                        best_distance = distance;
                        hovered_event = static_cast<int>(i);
                    }
                }
            }
        }

        drawCurrentTimeMarker(current_time);

        if (hovered_event >= 0 &&
            hovered_event < static_cast<int>(event_points.size())) {
            const auto& point = event_points[hovered_event];
            ImGui::SetTooltip("Stimulus event\nCamera frame %d\nTime %.3f s\n%s",
                              point.frame,
                              point.time,
                              point.event->label.c_str());
        } else if (hovered_step >= 0 &&
                   hovered_step < static_cast<int>(steps.size())) {
            const auto& step = steps[hovered_step];
            std::ostringstream tooltip;
            tooltip << "Stimulus step " << step.step_index << "\n"
                    << (step.step_name.empty() ? "unnamed" : step.step_name)
                    << "\n"
                    << (step.stimulus_mode.empty() ? "unknown mode"
                                                   : step.stimulus_mode)
                    << "\nFrames " << step.start_camera_frame << "-"
                    << step.end_camera_frame;
            if (std::isfinite(step.duration_s)) {
                tooltip << "\nDuration " << step.duration_s << " s";
            }
            if (step.moving_grating.present) {
                tooltip << "\nCamera direction "
                        << step.moving_grating.grating_direction_camera_deg
                        << " deg";
                if (!step.moving_grating.direction_mapping_status.empty()) {
                    tooltip << "\n"
                            << step.moving_grating.direction_mapping_status;
                }
            }
            if (step.concentric_grating.present) {
                tooltip << "\nPolarity "
                        << (step.concentric_grating
                                    .radial_polarity_authored.empty()
                                ? "unknown"
                                : step.concentric_grating
                                      .radial_polarity_authored);
            }
            ImGui::SetTooltip("%s", tooltip.str().c_str());
        }

        ImPlot::EndPlot();
    }
}
