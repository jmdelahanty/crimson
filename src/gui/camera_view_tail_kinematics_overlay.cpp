#include "gui/camera_view_overlay_renderer.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

std::pair<double, double> roiToScene(
    const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
    const std::array<float, 2>& roi_point,
    float scene_height_f) {
    if (!std::isfinite(roi_point[0]) || !std::isfinite(roi_point[1]) ||
        shape.coordinate_width <= 0.0f || shape.coordinate_height <= 0.0f) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    }
    const double px = shape.offset_x +
                      static_cast<double>(roi_point[0]) /
                          static_cast<double>(shape.coordinate_width) *
                          static_cast<double>(shape.roi_width);
    const double py = shape.offset_y +
                      static_cast<double>(roi_point[1]) /
                          static_cast<double>(shape.coordinate_height) *
                          static_cast<double>(shape.roi_height);
    return {px, scene_height_f - py};
}

void drawPolyline(
    const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
    const std::vector<std::array<float, 2>>& points,
    const std::string& label,
    const ImVec4& color,
    float thickness,
    float scene_height_f) {
    if (points.size() < 2) {
        return;
    }
    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(points.size());
    ys.reserve(points.size());
    for (const auto& point : points) {
        auto scene = roiToScene(shape, point, scene_height_f);
        if (!std::isfinite(scene.first) || !std::isfinite(scene.second)) {
            continue;
        }
        xs.push_back(scene.first);
        ys.push_back(scene.second);
    }
    if (xs.size() < 2) {
        return;
    }
    ImPlot::SetNextLineStyle(color, thickness);
    ImPlot::PlotLine(label.c_str(),
                     xs.data(),
                     ys.data(),
                     static_cast<int>(xs.size()));
}

std::array<float, 2> normalized(std::array<float, 2> value) {
    const float len = std::sqrt(value[0] * value[0] + value[1] * value[1]);
    if (len <= 1e-6f || !std::isfinite(len)) {
        return {std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
    }
    return {value[0] / len, value[1] / len};
}

}  // namespace

void drawCameraViewTailKinematicsOverlay(
    const ZarrDetectionLoader::FrameDetections& detection_details,
    const ZarrDetectionData::TailKinematicsData& tail_kinematics,
    float image_height_px,
    const CameraViewTailKinematicsOverlayOptions& options) {
    if (!options.show_overlay || !tail_kinematics.loaded ||
        !detection_details.includes_subject_shapes ||
        detection_details.subject_shapes.empty()) {
        return;
    }

    constexpr double kPi = 3.14159265358979323846;
    const float scene_height_f = image_height_px;
    for (size_t det_idx = 0; det_idx < detection_details.subject_shapes.size();
         ++det_idx) {
        if (!detection_details.detection_source.empty() &&
            det_idx < detection_details.detection_source.size() &&
            detection_details.detection_source[det_idx] != 0) {
            continue;
        }
        const auto& shape = detection_details.subject_shapes[det_idx];
        if (!shape.valid || shape.roi_index < 0 ||
            !std::isfinite(shape.offset_x) ||
            !std::isfinite(shape.offset_y) || shape.roi_width <= 0.0f ||
            shape.roi_height <= 0.0f ||
            shape.coordinate_width <= 0.0f ||
            shape.coordinate_height <= 0.0f) {
            continue;
        }
        const size_t row = static_cast<size_t>(shape.roi_index);
        const size_t k = tail_kinematics.tail_angle_sample_xy_count;
        if (row >= tail_kinematics.row_count || k == 0) {
            continue;
        }
        const size_t base = row * k * 2;
        if (base + k * 2 > tail_kinematics.tail_angle_sample_xy.size()) {
            continue;
        }

        std::vector<std::array<float, 2>> samples;
        samples.reserve(k);
        for (size_t sample_idx = 0; sample_idx < k; ++sample_idx) {
            const float x =
                tail_kinematics.tail_angle_sample_xy[base + sample_idx * 2 + 0];
            const float y =
                tail_kinematics.tail_angle_sample_xy[base + sample_idx * 2 + 1];
            if (std::isfinite(x) && std::isfinite(y)) {
                samples.push_back({x, y});
            }
        }
        if (samples.empty()) {
            continue;
        }

        const bool row_valid =
            row < tail_kinematics.valid.size() &&
            tail_kinematics.valid[row] != 0;
        const ImVec4 sample_color =
            (!row_valid && options.color_invalid_frames)
                ? ImVec4(1.0f, 0.2f, 0.12f, 0.95f)
                : ImVec4(0.95f, 0.85f, 0.12f, 0.95f);
        const ImVec4 segment_color =
            (!row_valid && options.color_invalid_frames)
                ? ImVec4(1.0f, 0.25f, 0.15f, 0.75f)
                : ImVec4(0.95f, 0.8f, 0.15f, 0.8f);

        if (options.show_segments) {
            drawPolyline(shape,
                         samples,
                         "##tail_kinematics_samples_line_" +
                             std::to_string(det_idx),
                         segment_color,
                         2.1f,
                         scene_height_f);
        }

        if (options.show_samples) {
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(samples.size());
            ys.reserve(samples.size());
            for (const auto& sample : samples) {
                auto scene = roiToScene(shape, sample, scene_height_f);
                if (!std::isfinite(scene.first) ||
                    !std::isfinite(scene.second)) {
                    continue;
                }
                xs.push_back(scene.first);
                ys.push_back(scene.second);
            }
            if (!xs.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond,
                                           5.5f,
                                           sample_color,
                                           1.2f,
                                           ImVec4(0.0f, 0.0f, 0.0f, 0.8f));
                ImPlot::PlotScatter(
                    ("##tail_kinematics_samples_" + std::to_string(det_idx))
                        .c_str(),
                    xs.data(),
                    ys.data(),
                    static_cast<int>(xs.size()));
            }
        }

        const auto forward = normalized(shape.body_forward_axis_xy);
        const auto left = normalized(shape.body_left_axis_xy);
        const bool have_body_axes =
            std::isfinite(forward[0]) && std::isfinite(forward[1]) &&
            std::isfinite(left[0]) && std::isfinite(left[1]);

        if (options.show_angle_vectors && have_body_axes &&
            !tail_kinematics.tail_angle_deg.empty()) {
            const float vector_len =
                std::max(shape.coordinate_width, shape.coordinate_height) *
                0.035f;
            const size_t angle_base = row * tail_kinematics.sample_count;
            if (tail_kinematics.sample_count >= k &&
                angle_base + k <= tail_kinematics.tail_angle_deg.size()) {
                for (size_t sample_idx = 0; sample_idx < samples.size();
                     ++sample_idx) {
                    const float angle_rad =
                        tail_kinematics.tail_angle_deg[angle_base +
                                                       sample_idx] *
                        static_cast<float>(kPi / 180.0);
                    if (!std::isfinite(angle_rad)) {
                        continue;
                    }
                    const float caudal_x = -forward[0];
                    const float caudal_y = -forward[1];
                    const float dir_x =
                        caudal_x * std::cos(angle_rad) +
                        left[0] * std::sin(angle_rad);
                    const float dir_y =
                        caudal_y * std::cos(angle_rad) +
                        left[1] * std::sin(angle_rad);
                    std::vector<std::array<float, 2>> vector = {
                        samples[sample_idx],
                        {samples[sample_idx][0] + dir_x * vector_len,
                         samples[sample_idx][1] + dir_y * vector_len}};
                    drawPolyline(shape,
                                 vector,
                                 "##tail_kinematics_angle_vector_" +
                                     std::to_string(det_idx) + "_" +
                                     std::to_string(sample_idx),
                                 ImVec4(1.0f, 0.95f, 0.35f, 0.75f),
                                 1.2f,
                                 scene_height_f);
                }
            }
        }

        if (options.show_lateral_deflection && have_body_axes &&
            !tail_kinematics.tail_lateral_deflection_px.empty()) {
            const size_t deflection_base = row * tail_kinematics.sample_count;
            if (tail_kinematics.sample_count >= k &&
                deflection_base + k <=
                    tail_kinematics.tail_lateral_deflection_px.size()) {
                for (size_t sample_idx = 0; sample_idx < samples.size();
                     ++sample_idx) {
                    const float deflection =
                        tail_kinematics
                            .tail_lateral_deflection_px[deflection_base +
                                                        sample_idx];
                    if (!std::isfinite(deflection)) {
                        continue;
                    }
                    std::vector<std::array<float, 2>> vector = {
                        {samples[sample_idx][0] - left[0] * deflection,
                         samples[sample_idx][1] - left[1] * deflection},
                        samples[sample_idx]};
                    drawPolyline(shape,
                                 vector,
                                 "##tail_kinematics_lateral_deflection_" +
                                     std::to_string(det_idx) + "_" +
                                     std::to_string(sample_idx),
                                 ImVec4(1.0f, 0.55f, 0.1f, 0.6f),
                                 1.0f,
                                 scene_height_f);
                }
            }
        }
    }
}
