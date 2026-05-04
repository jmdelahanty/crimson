#include "gui/camera_view_eye_angle_overlay.h"

#include "implot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

float cross(const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

float signedArea(const std::vector<ImVec2>& polygon) {
    if (polygon.size() < 3) {
        return 0.0f;
    }
    double area = 0.0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const ImVec2& a = polygon[i];
        const ImVec2& b = polygon[(i + 1) % polygon.size()];
        area += static_cast<double>(a.x) * static_cast<double>(b.y) -
                static_cast<double>(b.x) * static_cast<double>(a.y);
    }
    return static_cast<float>(area * 0.5);
}

bool insideClipEdge(const ImVec2& point,
                    const ImVec2& edge_start,
                    const ImVec2& edge_end,
                    float clip_area_sign) {
    const float edge_cross = cross(edge_start, edge_end, point);
    return clip_area_sign >= 0.0f ? edge_cross >= -0.01f
                                  : edge_cross <= 0.01f;
}

ImVec2 lineIntersection(const ImVec2& segment_start,
                        const ImVec2& segment_end,
                        const ImVec2& clip_start,
                        const ImVec2& clip_end) {
    const ImVec2 segment_delta(segment_end.x - segment_start.x,
                               segment_end.y - segment_start.y);
    const ImVec2 clip_delta(clip_end.x - clip_start.x,
                            clip_end.y - clip_start.y);
    const float denom =
        segment_delta.x * clip_delta.y - segment_delta.y * clip_delta.x;
    if (std::fabs(denom) <= 1e-5f) {
        return segment_end;
    }
    const ImVec2 start_delta(clip_start.x - segment_start.x,
                             clip_start.y - segment_start.y);
    const float t =
        (start_delta.x * clip_delta.y - start_delta.y * clip_delta.x) /
        denom;
    return ImVec2(segment_start.x + t * segment_delta.x,
                  segment_start.y + t * segment_delta.y);
}

std::vector<ImVec2> clipConvexPolygon(const std::vector<ImVec2>& subject,
                                      const std::vector<ImVec2>& clip) {
    if (subject.size() < 3 || clip.size() < 3) {
        return {};
    }

    std::vector<ImVec2> output = subject;
    const float clip_area_sign = signedArea(clip);
    if (std::fabs(clip_area_sign) <= 1e-3f) {
        return {};
    }

    for (size_t clip_idx = 0; clip_idx < clip.size(); ++clip_idx) {
        const ImVec2 edge_start = clip[clip_idx];
        const ImVec2 edge_end = clip[(clip_idx + 1) % clip.size()];
        const std::vector<ImVec2> input = output;
        output.clear();
        if (input.empty()) {
            break;
        }

        ImVec2 previous = input.back();
        bool previous_inside =
            insideClipEdge(previous, edge_start, edge_end, clip_area_sign);
        for (const ImVec2& current : input) {
            const bool current_inside =
                insideClipEdge(current, edge_start, edge_end, clip_area_sign);
            if (current_inside) {
                if (!previous_inside) {
                    output.push_back(lineIntersection(
                        previous, current, edge_start, edge_end));
                }
                output.push_back(current);
            } else if (previous_inside) {
                output.push_back(lineIntersection(
                    previous, current, edge_start, edge_end));
            }
            previous = current;
            previous_inside = current_inside;
        }
    }
    return output;
}

struct EyeOrientationSmoother {
    struct History {
        std::deque<double> angles;
        double last_unwrapped = std::numeric_limits<double>::quiet_NaN();
    };

    static constexpr size_t kMaxSamples = 60;

    void resetIfRunChanged(const std::string& run_id) {
        if (run_id != current_run_) {
            histories_.clear();
            current_run_ = run_id;
        }
    }

    ImVec2 smoothDirection(int32_t roi_index, int eye, const ImVec2& raw_dir) {
        if (roi_index < 0 || eye < 0 || eye >= 2) {
            return raw_dir;
        }
        double raw_angle = std::atan2(raw_dir.y, raw_dir.x);
        if (!std::isfinite(raw_angle)) {
            return raw_dir;
        }

        auto& history = histories_[roi_index][eye];
        double unwrapped = raw_angle;
        if (std::isfinite(history.last_unwrapped)) {
            while (unwrapped - history.last_unwrapped > M_PI) {
                unwrapped -= 2.0 * M_PI;
            }
            while (unwrapped - history.last_unwrapped < -M_PI) {
                unwrapped += 2.0 * M_PI;
            }
        }
        history.last_unwrapped = unwrapped;

        history.angles.push_back(unwrapped);
        if (history.angles.size() > kMaxSamples) {
            history.angles.pop_front();
        }

        std::vector<double> sorted(history.angles.begin(), history.angles.end());
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];
        if ((sorted.size() % 2) == 0 && sorted.size() >= 2) {
            median = 0.5 * (sorted[sorted.size() / 2 - 1] +
                            sorted[sorted.size() / 2]);
        }

        double wrapped = std::fmod(median, 2.0 * M_PI);
        if (wrapped <= -M_PI) {
            wrapped += 2.0 * M_PI;
        }
        if (wrapped > M_PI) {
            wrapped -= 2.0 * M_PI;
        }

        ImVec2 smoothed(static_cast<float>(std::cos(wrapped)),
                        static_cast<float>(std::sin(wrapped)));
        const float len =
            std::sqrt(smoothed.x * smoothed.x + smoothed.y * smoothed.y);
        if (len > 1e-6f) {
            smoothed.x /= len;
            smoothed.y /= len;
            return smoothed;
        }
        return raw_dir;
    }

private:
    std::unordered_map<int32_t, std::array<History, 2>> histories_;
    std::string current_run_;
};

EyeOrientationSmoother& eyeOrientationSmoother() {
    static EyeOrientationSmoother smoother;
    return smoother;
}

void drawPixelTextBox(const char* text,
                      const ImVec2& center,
                      const ImVec4& accent_color,
                      float font_scale = 1.25f) {
    if (text == nullptr || text[0] == '\0' || !std::isfinite(center.x) ||
        !std::isfinite(center.y)) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const float base_font_size = ImGui::GetFontSize();
    const float font_size = base_font_size * font_scale;
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 scaled_text_size(text_size.x * font_scale,
                                  text_size.y * font_scale);
    const ImVec2 padding(7.0f, 4.0f);
    const ImVec2 box_min(center.x - scaled_text_size.x * 0.5f - padding.x,
                         center.y - scaled_text_size.y * 0.5f - padding.y);
    const ImVec2 box_max(center.x + scaled_text_size.x * 0.5f + padding.x,
                         center.y + scaled_text_size.y * 0.5f + padding.y);

    ImVec4 border = accent_color;
    border.w = 0.95f;
    draw_list->AddRectFilled(box_min,
                             box_max,
                             IM_COL32(8, 10, 14, 214),
                             4.0f);
    draw_list->AddRect(box_min,
                       box_max,
                       ImGui::ColorConvertFloat4ToU32(border),
                       4.0f,
                       0,
                       1.2f);
    const ImVec2 text_pos(center.x - scaled_text_size.x * 0.5f,
                          center.y - scaled_text_size.y * 0.5f);
    draw_list->AddText(ImGui::GetFont(),
                       font_size,
                       ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
                       IM_COL32(0, 0, 0, 210),
                       text);
    draw_list->AddText(ImGui::GetFont(),
                       font_size,
                       text_pos,
                       IM_COL32(255, 255, 245, 255),
                       text);
}

std::array<double, 2> normalize(double x, double y) {
    const double len = std::sqrt(x * x + y * y);
    if (len <= 1e-6 || !std::isfinite(len)) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    }
    return {x / len, y / len};
}

}  // namespace

void resetCameraViewEyeAngleOverlaySmoothing(const std::string& run_id) {
    eyeOrientationSmoother().resetIfRunChanged(run_id);
}

void drawCameraViewEyeAngleOverlayForEye(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask_info,
    const ZarrDetectionLoader::FrameDetections::SubjectShape* subject_shape,
    const std::array<float, 4>& detection_box,
    int eye,
    const std::string& base_id,
    const ImVec4& base_color,
    double cell_w,
    double cell_h,
    float scene_height_f,
    const CameraViewMaskOverlayOptions& options,
    CameraViewMaskPerfMetrics& metrics,
    CameraViewEyeAngleOverlayState& state) {
    if (eye < 0 || eye >= 2 || !mask_info.has_feret_axes || cell_w <= 0.0 ||
        cell_h <= 0.0) {
        return;
    }

    const auto axis_start = std::chrono::steady_clock::now();
    auto roiToWorld = [&](float roi_x,
                          float roi_y) -> std::pair<double, double> {
        const double px = mask_info.offset_x + static_cast<double>(roi_x) * cell_w;
        const double py = mask_info.offset_y + static_cast<double>(roi_y) * cell_h;
        return {px, py};
    };
    auto worldToScene =
        [&](double world_x, double world_y) -> std::pair<double, double> {
        return {world_x, scene_height_f - world_y};
    };
    auto roiToScene = [&](float roi_x,
                          float roi_y) -> std::pair<double, double> {
        auto world = roiToWorld(roi_x, roi_y);
        return worldToScene(world.first, world.second);
    };
    auto draw_axis =
        [&](const ZarrDetectionLoader::FrameDetections::EyeMask::AxisSegment&
                axis,
            const std::string& label,
            const ImVec4& color,
            float thickness) {
            if (!axis.valid) {
                return;
            }
            auto p0 = roiToScene(axis.x0, axis.y0);
            auto p1 = roiToScene(axis.x1, axis.y1);
            const double x_vals[2] = {p0.first, p1.first};
            const double y_vals[2] = {p0.second, p1.second};
            ImPlot::SetNextLineStyle(color, thickness);
            ImPlot::PlotLine(label.c_str(), x_vals, y_vals, 2);
            metrics.axes_drawn++;
        };
    auto draw_eye_angle_arc =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            const std::pair<double, double>& center_world,
            float angle_deg,
            const std::string& label,
            const ImVec4& color,
            double minor_axis_len_world) -> bool {
        if (!shape.body_frame_valid ||
            !std::isfinite(shape.body_forward_axis_xy[0]) ||
            !std::isfinite(shape.body_forward_axis_xy[1]) ||
            !std::isfinite(shape.body_left_axis_xy[0]) ||
            !std::isfinite(shape.body_left_axis_xy[1]) ||
            shape.coordinate_width <= 0.0f ||
            shape.coordinate_height <= 0.0f ||
            shape.roi_width <= 0.0f ||
            shape.roi_height <= 0.0f ||
            !std::isfinite(angle_deg)) {
            return false;
        }

        const auto forward = normalize(
            static_cast<double>(shape.body_forward_axis_xy[0]) *
                static_cast<double>(shape.roi_width) /
                static_cast<double>(shape.coordinate_width),
            static_cast<double>(shape.body_forward_axis_xy[1]) *
                static_cast<double>(shape.roi_height) /
                static_cast<double>(shape.coordinate_height));
        const auto left = normalize(
            static_cast<double>(shape.body_left_axis_xy[0]) *
                static_cast<double>(shape.roi_width) /
                static_cast<double>(shape.coordinate_width),
            static_cast<double>(shape.body_left_axis_xy[1]) *
                static_cast<double>(shape.roi_height) /
                static_cast<double>(shape.coordinate_height));
        if (!std::isfinite(forward[0]) || !std::isfinite(forward[1]) ||
            !std::isfinite(left[0]) || !std::isfinite(left[1])) {
            return false;
        }

        const double roi_span = std::max(static_cast<double>(mask_info.roi_width),
                                         static_cast<double>(mask_info.roi_height));
        const double radius = std::clamp(std::max(minor_axis_len_world * 0.75,
                                                  roi_span * 0.075),
                                         10.0,
                                         std::max(12.0, roi_span * 0.18));
        const double angle_rad = std::clamp(
            static_cast<double>(angle_deg) * M_PI / 180.0, -M_PI, M_PI);
        const int steps = std::clamp(
            static_cast<int>(std::ceil(std::fabs(angle_rad) / (M_PI / 24.0))),
            6,
            32);

        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(static_cast<size_t>(steps + 1));
        ys.reserve(static_cast<size_t>(steps + 1));
        for (int step = 0; step <= steps; ++step) {
            const double t =
                angle_rad * static_cast<double>(step) / static_cast<double>(steps);
            const double vx = std::cos(t) * forward[0] + std::sin(t) * left[0];
            const double vy = std::cos(t) * forward[1] + std::sin(t) * left[1];
            auto scene = worldToScene(center_world.first + vx * radius,
                                      center_world.second + vy * radius);
            xs.push_back(scene.first);
            ys.push_back(scene.second);
        }
        if (xs.size() < 2) {
            return false;
        }

        const double start_x[2] = {
            center_world.first,
            center_world.first + forward[0] * radius};
        const double start_y[2] = {
            scene_height_f - center_world.second,
            scene_height_f - (center_world.second + forward[1] * radius)};
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 0.42f), 1.0f);
        ImPlot::PlotLine((label + "_body_axis").c_str(), start_x, start_y, 2);

        ImVec4 arc_color = color;
        arc_color.w = 0.95f;
        ImPlot::SetNextLineStyle(arc_color, 2.4f);
        ImPlot::PlotLine(label.c_str(),
                         xs.data(),
                         ys.data(),
                         static_cast<int>(xs.size()));

        const double end_x[2] = {center_world.first, xs.back()};
        const double end_y[2] = {scene_height_f - center_world.second, ys.back()};
        ImVec4 end_color = color;
        end_color.w = 0.65f;
        ImPlot::SetNextLineStyle(end_color, 1.1f);
        ImPlot::PlotLine((label + "_gaze_axis").c_str(), end_x, end_y, 2);

        return true;
    };

    ImVec4 major_color = base_color;
    major_color.w = 0.9f;
    ImVec4 minor_color = base_color;
    minor_color.x = std::min(1.0f, minor_color.x + 0.15f);
    minor_color.y = std::min(1.0f, minor_color.y + 0.15f);
    minor_color.z = std::min(1.0f, minor_color.z + 0.15f);
    minor_color.w = 0.75f;

    draw_axis(mask_info.feret_major[eye],
              base_id + "_feret_major",
              major_color,
              2.5f);
    draw_axis(mask_info.feret_minor[eye],
              base_id + "_feret_minor",
              minor_color,
              1.8f);

    const auto& minor_axis = mask_info.feret_minor[eye];
    if (!minor_axis.valid) {
        metrics.axis_draw_ms +=
            durationMs(std::chrono::steady_clock::now() - axis_start);
        return;
    }

    auto endpoint0_world = roiToWorld(minor_axis.x0, minor_axis.y0);
    auto endpoint1_world = roiToWorld(minor_axis.x1, minor_axis.y1);
    auto center_world = roiToWorld(0.5f * (minor_axis.x0 + minor_axis.x1),
                                   0.5f * (minor_axis.y0 + minor_axis.y1));
    const double minor_axis_len_world =
        std::hypot(endpoint1_world.first - endpoint0_world.first,
                   endpoint1_world.second - endpoint0_world.second);

    if (options.show_eye_gaze_rays && mask_info.has_gaze_vectors &&
        mask_info.gaze_vector_valid[eye] != 0) {
        const auto gaze = mask_info.gaze_vector_xy[eye];
        if (std::isfinite(gaze[0]) && std::isfinite(gaze[1])) {
            const double gaze_len =
                std::hypot(static_cast<double>(gaze[0]),
                           static_cast<double>(gaze[1]));
            if (gaze_len > 1e-6) {
                const double roi_span =
                    std::max(static_cast<double>(mask_info.roi_width),
                             static_cast<double>(mask_info.roi_height));
                const double ray_len =
                    std::max(roi_span * 0.32, minor_axis_len_world * 1.35);
                auto ray_end_scene = worldToScene(
                    center_world.first +
                        static_cast<double>(gaze[0]) / gaze_len * ray_len,
                    center_world.second +
                        static_cast<double>(gaze[1]) / gaze_len * ray_len);
                auto ray_start_scene =
                    worldToScene(center_world.first, center_world.second);
                const double ray_x[2] = {ray_start_scene.first,
                                         ray_end_scene.first};
                const double ray_y[2] = {ray_start_scene.second,
                                         ray_end_scene.second};
                ImVec4 ray_color = minor_color;
                ray_color.w = 0.95f;
                ImPlot::SetNextLineStyle(ray_color, 2.2f);
                ImPlot::PlotLine((base_id + "_gaze_ray").c_str(),
                                 ray_x,
                                 ray_y,
                                 2);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                           3.0f,
                                           ray_color,
                                           1.0f,
                                           ray_color);
                const double tip_x[1] = {ray_end_scene.first};
                const double tip_y[1] = {ray_end_scene.second};
                ImPlot::PlotScatter(
                    (base_id + "_gaze_ray_tip").c_str(), tip_x, tip_y, 1);
                metrics.gaze_rays_drawn++;
            }
        }
    }

    if (mask_info.has_eye_angles && mask_info.feret_angle_valid[eye] &&
        options.show_eye_angle_arcs && subject_shape != nullptr) {
        draw_eye_angle_arc(*subject_shape,
                           center_world,
                           mask_info.feret_minor_angle_deg[eye],
                           base_id + "_angle_arc",
                           minor_color,
                           minor_axis_len_world);
    }

    if (options.show_eye_angle_labels) {
        const bool has_eye_frame_label =
            mask_info.has_eye_frame_angles &&
            mask_info.eye_frame_angle_valid[eye] != 0 &&
            std::isfinite(mask_info.eye_frame_angle_deg[eye]);
        const bool has_gaze_fallback_label =
            !has_eye_frame_label && mask_info.has_eye_angles &&
            mask_info.feret_angle_valid[eye] != 0 &&
            std::isfinite(mask_info.feret_minor_angle_deg[eye]);
        if (has_eye_frame_label || has_gaze_fallback_label) {
            auto center_scene =
                worldToScene(center_world.first, center_world.second);
            ImVec2 label_center = ImPlot::PlotToPixels(
                ImPlotPoint(center_scene.first, center_scene.second));
            label_center.y += (eye == 0) ? -24.0f : 24.0f;
            char angle_label[64];
            if (has_eye_frame_label) {
                std::snprintf(angle_label,
                              sizeof(angle_label),
                              "%s eye-frame %+.1f°",
                              eye == 0 ? "Left" : "Right",
                              mask_info.eye_frame_angle_deg[eye]);
            } else {
                std::snprintf(angle_label,
                              sizeof(angle_label),
                              "%s gaze signed %+.1f°",
                              eye == 0 ? "Left" : "Right",
                              mask_info.feret_minor_angle_deg[eye]);
            }
            drawPixelTextBox(angle_label, label_center, minor_color, 1.05f);
            metrics.angle_labels_drawn++;
        }
    }

    const double det_center_x =
        0.5 * (detection_box[0] + detection_box[2]);
    const double det_center_y =
        0.5 * (detection_box[1] + detection_box[3]);
    auto squaredDistance = [](double ax, double ay, double bx, double by) {
        const double dx = ax - bx;
        const double dy = ay - by;
        return dx * dx + dy * dy;
    };

    const double dist0 =
        squaredDistance(endpoint0_world.first, endpoint0_world.second,
                        det_center_x, det_center_y);
    const double dist1 =
        squaredDistance(endpoint1_world.first, endpoint1_world.second,
                        det_center_x, det_center_y);
    const auto outward_endpoint =
        (dist0 >= dist1) ? endpoint0_world : endpoint1_world;

    ImVec2 dir_world(
        static_cast<float>(outward_endpoint.first - center_world.first),
        static_cast<float>(outward_endpoint.second - center_world.second));
    if (mask_info.has_gaze_vectors && mask_info.gaze_vector_valid[eye] != 0) {
        const auto gaze = mask_info.gaze_vector_xy[eye];
        if (std::isfinite(gaze[0]) && std::isfinite(gaze[1])) {
            const double gaze_len =
                std::hypot(static_cast<double>(gaze[0]),
                           static_cast<double>(gaze[1]));
            if (gaze_len > 1e-6) {
                dir_world.x = gaze[0];
                dir_world.y = gaze[1];
            }
        }
    }
    float dir_len =
        std::sqrt(dir_world.x * dir_world.x + dir_world.y * dir_world.y);
    if (options.show_eye_direction_beams && dir_len > 1e-3f) {
        dir_world.x /= dir_len;
        dir_world.y /= dir_len;

        ImVec2 smoothed_dir =
            eyeOrientationSmoother().smoothDirection(
                mask_info.roi_index, eye, dir_world);
        const float smooth_len = std::sqrt(smoothed_dir.x * smoothed_dir.x +
                                           smoothed_dir.y * smoothed_dir.y);
        if (smooth_len > 1e-3f) {
            smoothed_dir.x /= smooth_len;
            smoothed_dir.y /= smooth_len;
            dir_world = smoothed_dir;
        }

        const float roi_span =
            std::max(mask_info.roi_width, mask_info.roi_height);
        const float cone_length = std::clamp(roi_span * 1.15f, 90.0f, 520.0f);
        constexpr float kEyeVisualAngleDeg = 163.0f;
        const float half_angle_rad =
            (kEyeVisualAngleDeg * 0.5f) * static_cast<float>(M_PI / 180.0);
        constexpr int kConeSteps = 30;

        std::vector<ImVec2> cone_points;
        std::vector<double> arc_x;
        std::vector<double> arc_y;
        cone_points.reserve(kConeSteps + 2);
        arc_x.reserve(kConeSteps + 1);
        arc_y.reserve(kConeSteps + 1);

        auto center_scene = worldToScene(center_world.first, center_world.second);
        cone_points.push_back(ImPlot::PlotToPixels(
            ImPlotPoint(center_scene.first, center_scene.second)));
        const auto label_axis_scene = worldToScene(
            center_world.first +
                static_cast<double>(dir_world.x) * cone_length * 0.42,
            center_world.second +
                static_cast<double>(dir_world.y) * cone_length * 0.42);
        state.visual_cone_label_points[eye] =
            ImPlot::PlotToPixels(ImPlotPoint(label_axis_scene.first,
                                             label_axis_scene.second));
        state.visual_cone_dirs_world[eye] = dir_world;
        state.visual_cone_valid[eye] = true;

        for (int step = 0; step <= kConeSteps; ++step) {
            const float t =
                -half_angle_rad +
                (2.0f * half_angle_rad * static_cast<float>(step) /
                 static_cast<float>(kConeSteps));
            const float c = std::cos(t);
            const float s = std::sin(t);
            const float vx = c * dir_world.x - s * dir_world.y;
            const float vy = s * dir_world.x + c * dir_world.y;
            auto scene = worldToScene(center_world.first +
                                          static_cast<double>(vx) * cone_length,
                                      center_world.second +
                                          static_cast<double>(vy) * cone_length);
            arc_x.push_back(scene.first);
            arc_y.push_back(scene.second);
            cone_points.push_back(
                ImPlot::PlotToPixels(ImPlotPoint(scene.first, scene.second)));
        }

        ImVec4 cone_fill_color = base_color;
        cone_fill_color.w = 0.13f;
        ImDrawList* cone_draw_list = ImPlot::GetPlotDrawList();
        cone_draw_list->AddConvexPolyFilled(
            cone_points.data(),
            static_cast<int>(cone_points.size()),
            ImGui::ColorConvertFloat4ToU32(cone_fill_color));

        ImVec4 cone_outline_color = base_color;
        cone_outline_color.w = 0.55f;
        if (!arc_x.empty()) {
            ImPlot::SetNextLineStyle(cone_outline_color, 1.3f);
            ImPlot::PlotLine((base_id + "_visual_cone_arc").c_str(),
                             arc_x.data(),
                             arc_y.data(),
                             static_cast<int>(arc_x.size()));
            const double side0_x[2] = {center_scene.first, arc_x.front()};
            const double side0_y[2] = {center_scene.second, arc_y.front()};
            const double side1_x[2] = {center_scene.first, arc_x.back()};
            const double side1_y[2] = {center_scene.second, arc_y.back()};
            ImPlot::SetNextLineStyle(cone_outline_color, 1.0f);
            ImPlot::PlotLine(
                (base_id + "_visual_cone_side0").c_str(), side0_x, side0_y, 2);
            ImPlot::SetNextLineStyle(cone_outline_color, 1.0f);
            ImPlot::PlotLine(
                (base_id + "_visual_cone_side1").c_str(), side1_x, side1_y, 2);
        }
        metrics.visual_cones_drawn++;
        state.visual_cone_polygons[eye] = cone_points;
        if (!state.visual_cone_polygons[0].empty() &&
            !state.visual_cone_polygons[1].empty()) {
            const std::vector<ImVec2> overlap =
                clipConvexPolygon(state.visual_cone_polygons[0],
                                  state.visual_cone_polygons[1]);
            if (overlap.size() >= 3) {
                constexpr ImVec4 kVergenceOverlapColor =
                    ImVec4(0.34f, 1.0f, 0.42f, 0.24f);
                cone_draw_list->AddConvexPolyFilled(
                    overlap.data(),
                    static_cast<int>(overlap.size()),
                    ImGui::ColorConvertFloat4ToU32(kVergenceOverlapColor));
                metrics.visual_cone_overlaps_drawn++;
            }
            if (options.show_eye_angle_labels && !state.drew_vergence_label &&
                state.visual_cone_valid[0] && state.visual_cone_valid[1]) {
                const bool has_eye_frame_vergence =
                    mask_info.has_eye_frame_angles &&
                    mask_info.eye_frame_vergence_valid != 0 &&
                    std::isfinite(mask_info.eye_frame_vergence_deg);
                float label_value_deg = mask_info.eye_frame_vergence_deg;
                char vergence_label[64];
                if (has_eye_frame_vergence) {
                    std::snprintf(vergence_label,
                                  sizeof(vergence_label),
                                  "Eye-frame vergence %+.1f°",
                                  label_value_deg);
                } else {
                    const float dot =
                        std::clamp(state.visual_cone_dirs_world[0].x *
                                           state.visual_cone_dirs_world[1].x +
                                       state.visual_cone_dirs_world[0].y *
                                           state.visual_cone_dirs_world[1].y,
                                   -1.0f,
                                   1.0f);
                    label_value_deg =
                        std::acos(dot) * static_cast<float>(180.0 / M_PI);
                    if (std::isfinite(label_value_deg)) {
                        std::snprintf(vergence_label,
                                      sizeof(vergence_label),
                                      "Gaze-axis sep. %.1f°",
                                      label_value_deg);
                    }
                }
                if (std::isfinite(label_value_deg)) {
                    ImVec2 vergence_label_center(
                        0.5f * (state.visual_cone_label_points[0].x +
                                 state.visual_cone_label_points[1].x),
                        0.5f * (state.visual_cone_label_points[0].y +
                                 state.visual_cone_label_points[1].y) -
                            18.0f);
                    const ImVec2 plot_pos = ImPlot::GetPlotPos();
                    const ImVec2 plot_size = ImPlot::GetPlotSize();
                    const float min_label_x = plot_pos.x + 48.0f;
                    const float max_label_x = plot_pos.x + plot_size.x - 48.0f;
                    const float min_label_y = plot_pos.y + 18.0f;
                    const float max_label_y = plot_pos.y + plot_size.y - 18.0f;
                    if (min_label_x <= max_label_x) {
                        vergence_label_center.x =
                            std::clamp(vergence_label_center.x,
                                       min_label_x,
                                       max_label_x);
                    }
                    if (min_label_y <= max_label_y) {
                        vergence_label_center.y =
                            std::clamp(vergence_label_center.y,
                                       min_label_y,
                                       max_label_y);
                    }
                    drawPixelTextBox(vergence_label,
                                     vergence_label_center,
                                     ImVec4(0.34f, 1.0f, 0.42f, 0.92f),
                                     1.15f);
                    metrics.angle_labels_drawn++;
                    state.drew_vergence_label = true;
                }
            }
        }
    }

    metrics.axis_draw_ms +=
        durationMs(std::chrono::steady_clock::now() - axis_start);
}
