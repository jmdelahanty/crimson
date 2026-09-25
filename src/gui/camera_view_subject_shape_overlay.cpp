#include "gui/camera_view_overlay_renderer.h"
#include "gui/camera_view_overlay_style.h"

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

using camera_view_overlay::isEyeMaskComponent;
using camera_view_overlay::subjectMaskContourColor;

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

void drawPoint(
    const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
    const std::array<float, 2>& point,
    const std::string& label,
    ImPlotMarker marker,
    float size,
    const ImVec4& fill,
    const ImVec4& outline,
    float scene_height_f) {
    auto scene = roiToScene(shape, point, scene_height_f);
    if (!std::isfinite(scene.first) || !std::isfinite(scene.second)) {
        return;
    }
    const double x = scene.first;
    const double y = scene.second;
    ImPlot::SetNextMarkerStyle(marker, size, fill, 1.5f, outline);
    ImPlot::PlotScatter(label.c_str(), &x, &y, 1);
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

void drawMaskContour(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask,
    const ZarrDetectionLoader::FrameDetections::EyeMask::SubjectMaskComponent&
        component,
    const std::string& label,
    const ImVec4& color,
    float thickness,
    float scene_height_f) {
    if (!component.has_contour || component.contour_xy.size() < 2 ||
        mask.cols <= 0 || mask.rows <= 0 || mask.roi_width <= 0.0f ||
        mask.roi_height <= 0.0f) {
        return;
    }
    const double cell_w = mask.roi_width / static_cast<double>(mask.cols);
    const double cell_h = mask.roi_height / static_cast<double>(mask.rows);
    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(component.contour_xy.size() + 1);
    ys.reserve(component.contour_xy.size() + 1);
    for (const auto& point : component.contour_xy) {
        const double px = mask.offset_x + static_cast<double>(point[0]) * cell_w;
        const double py = mask.offset_y + static_cast<double>(point[1]) * cell_h;
        xs.push_back(px);
        ys.push_back(scene_height_f - py);
    }
    const auto& first = component.contour_xy.front();
    const auto& last = component.contour_xy.back();
    if (component.contour_xy.size() > 2 &&
        (std::fabs(first[0] - last[0]) > 1e-5f ||
         std::fabs(first[1] - last[1]) > 1e-5f)) {
        const double px = mask.offset_x + static_cast<double>(first[0]) * cell_w;
        const double py = mask.offset_y + static_cast<double>(first[1]) * cell_h;
        xs.push_back(px);
        ys.push_back(scene_height_f - py);
    }
    ImPlot::SetNextLineStyle(color, thickness);
    ImPlot::PlotLine(label.c_str(),
                     xs.data(),
                     ys.data(),
                     static_cast<int>(xs.size()));
}

void drawBodyAxis(
    const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
    const std::array<float, 2>& axis,
    const std::string& label,
    const ImVec4& color,
    float scene_height_f) {
    if (!shape.body_frame_valid || !std::isfinite(shape.body_origin_xy[0]) ||
        !std::isfinite(shape.body_origin_xy[1]) || !std::isfinite(axis[0]) ||
        !std::isfinite(axis[1])) {
        return;
    }
    const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1]);
    if (len <= 1e-5f) {
        return;
    }
    const float axis_len =
        std::max(shape.coordinate_width, shape.coordinate_height) * 0.12f;
    const std::array<float, 2> p0 = {shape.body_origin_xy[0],
                                     shape.body_origin_xy[1]};
    const std::array<float, 2> p1 = {
        shape.body_origin_xy[0] + axis[0] / len * axis_len,
        shape.body_origin_xy[1] + axis[1] / len * axis_len};
    std::vector<std::array<float, 2>> points = {p0, p1};
    drawPolyline(shape, points, label, color, 2.0f, scene_height_f);
}

void drawTailNormals(
    const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
    size_t det_idx,
    float scene_height_f) {
    if (shape.tail_sample_xy.empty() ||
        shape.tail_sample_xy.size() != shape.tail_normal_xy.size()) {
        return;
    }
    const float normal_len =
        std::max(shape.coordinate_width, shape.coordinate_height) * 0.035f;
    for (size_t i = 0; i < shape.tail_sample_xy.size(); ++i) {
        const auto& sample = shape.tail_sample_xy[i];
        const auto& normal = shape.tail_normal_xy[i];
        if (!std::isfinite(sample[0]) || !std::isfinite(sample[1]) ||
            !std::isfinite(normal[0]) || !std::isfinite(normal[1])) {
            continue;
        }
        const float len =
            std::sqrt(normal[0] * normal[0] + normal[1] * normal[1]);
        if (len <= 1e-5f) {
            continue;
        }
        const std::array<float, 2> p0 = {
            sample[0] - normal[0] / len * normal_len,
            sample[1] - normal[1] / len * normal_len};
        const std::array<float, 2> p1 = {
            sample[0] + normal[0] / len * normal_len,
            sample[1] + normal[1] / len * normal_len};
        std::vector<std::array<float, 2>> points = {p0, p1};
        drawPolyline(shape,
                     points,
                     "##shape_tail_normal_" + std::to_string(det_idx) + "_" +
                         std::to_string(i),
                     ImVec4(0.65f, 0.95f, 1.0f, 0.65f),
                     1.0f,
                     scene_height_f);
    }
}

}  // namespace

void drawCameraViewSubjectShapeOverlay(
    const ZarrDetectionLoader::FrameDetections& detection_details,
    float image_height_px,
    const CameraViewSubjectShapeOverlayOptions& options) {
    if (!options.show_overlay ||
        !detection_details.includes_subject_shapes ||
        detection_details.subject_shapes.empty()) {
        return;
    }

    const float scene_height_f = image_height_px;
    for (size_t det_idx = 0; det_idx < detection_details.subject_shapes.size();
         ++det_idx) {
        if (!detection_details.detection_source.empty() &&
            det_idx < detection_details.detection_source.size() &&
            detection_details.detection_source[det_idx] != 0) {
            continue;
        }
        const auto& shape = detection_details.subject_shapes[det_idx];
        if (!shape.valid || !std::isfinite(shape.offset_x) ||
            !std::isfinite(shape.offset_y) || shape.roi_width <= 0.0f ||
            shape.roi_height <= 0.0f) {
            continue;
        }

        if (det_idx < detection_details.eye_masks.size()) {
            const auto& mask = detection_details.eye_masks[det_idx];
            for (const auto& component : mask.subject_mask_components) {
                const bool draw_component =
                    (component.label == "subject_body" &&
                     options.show_body_contour) ||
                    (component.label == "swim_bladder" &&
                     options.show_swim_bladder_contour) ||
                    (isEyeMaskComponent(component.label) &&
                     options.show_eye_contours);
                if (!draw_component) {
                    continue;
                }
                drawMaskContour(mask,
                                component,
                                "##shape_contour_" + component.label + "_" +
                                    std::to_string(det_idx),
                                subjectMaskContourColor(component.label),
                                component.label == "subject_body" ? 1.6f : 2.0f,
                                scene_height_f);
            }
        }

        if (options.show_body_frame_axes) {
            drawBodyAxis(shape,
                         shape.body_forward_axis_xy,
                         "##shape_body_forward_" + std::to_string(det_idx),
                         ImVec4(1.0f, 0.55f, 0.15f, 0.9f),
                         scene_height_f);
            drawBodyAxis(shape,
                         shape.body_left_axis_xy,
                         "##shape_body_left_" + std::to_string(det_idx),
                         ImVec4(0.1f, 0.9f, 0.95f, 0.85f),
                         scene_height_f);
        }

        if (options.show_centerline && shape.centerline_valid) {
            drawPolyline(shape,
                         shape.centerline_xy,
                         "##shape_centerline_" + std::to_string(det_idx),
                         ImVec4(1.0f, 0.92f, 0.25f, 0.95f),
                         2.2f,
                         scene_height_f);
        }
        if (options.show_bspline_sample && shape.bspline_valid) {
            drawPolyline(shape,
                         shape.bspline_sample_xy,
                         "##shape_bspline_" + std::to_string(det_idx),
                         ImVec4(0.2f, 1.0f, 0.7f, 0.95f),
                         2.0f,
                         scene_height_f);
        }
        if (options.show_bspline_debug_points && shape.bspline_valid) {
            for (size_t i = 0; i < shape.bspline_sample_xy.size(); ++i) {
                drawPoint(shape,
                          shape.bspline_sample_xy[i],
                          "##shape_bspline_debug_point_" +
                              std::to_string(det_idx) + "_" +
                              std::to_string(i),
                          ImPlotMarker_Circle,
                          2.4f,
                          ImVec4(0.2f, 1.0f, 0.7f, 0.45f),
                          ImVec4(0.0f, 0.0f, 0.0f, 0.45f),
                          scene_height_f);
            }
        }
        if (options.show_bspline_control_points) {
            drawPolyline(shape,
                         shape.bspline_control_points_xy,
                         "##shape_bspline_controls_line_" +
                             std::to_string(det_idx),
                         ImVec4(0.2f, 0.9f, 0.7f, 0.35f),
                         1.0f,
                         scene_height_f);
            for (size_t i = 0; i < shape.bspline_control_points_xy.size(); ++i) {
                drawPoint(shape,
                          shape.bspline_control_points_xy[i],
                          "##shape_bspline_control_" +
                              std::to_string(det_idx) + "_" +
                              std::to_string(i),
                          ImPlotMarker_Square,
                          4.0f,
                          ImVec4(0.2f, 1.0f, 0.7f, 0.65f),
                          ImVec4(0.0f, 0.0f, 0.0f, 0.7f),
                          scene_height_f);
            }
        }
        if (options.show_tail_samples && shape.tail_sample_valid) {
            for (size_t i = 0; i < shape.tail_sample_xy.size(); ++i) {
                drawPoint(shape,
                          shape.tail_sample_xy[i],
                          "##shape_tail_sample_" + std::to_string(det_idx) +
                              "_" + std::to_string(i),
                          ImPlotMarker_Circle,
                          3.0f,
                          ImVec4(0.85f, 0.6f, 1.0f, 0.7f),
                          ImVec4(0.0f, 0.0f, 0.0f, 0.65f),
                          scene_height_f);
            }
        }
        if (options.show_tail_normals && shape.tail_sample_valid) {
            drawTailNormals(shape, det_idx, scene_height_f);
        }

        if (options.show_snout_tip && shape.snout_tip_valid) {
            drawPoint(shape,
                      shape.snout_tip_xy,
                      "##shape_snout_" + std::to_string(det_idx),
                      ImPlotMarker_Circle,
                      7.0f,
                      ImVec4(1.0f, 0.45f, 0.1f, 0.95f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.9f),
                      scene_height_f);
        }
        if (options.show_tail_base && shape.tail_base_valid) {
            drawPoint(shape,
                      shape.tail_base_xy,
                      "##shape_tail_base_" + std::to_string(det_idx),
                      ImPlotMarker_Diamond,
                      6.5f,
                      ImVec4(0.95f, 0.55f, 1.0f, 0.9f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.85f),
                      scene_height_f);
        }
        if (options.show_tail_tip) {
            drawPoint(shape,
                      shape.tail_tip_xy,
                      "##shape_tail_tip_" + std::to_string(det_idx),
                      ImPlotMarker_Cross,
                      6.5f,
                      ImVec4(0.75f, 0.45f, 1.0f, 0.9f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.85f),
                      scene_height_f);
        }
        if (options.show_caudal_anchor && shape.caudal_contour_valid) {
            drawPoint(shape,
                      shape.caudal_contour_point_xy,
                      "##shape_caudal_anchor_" + std::to_string(det_idx),
                      ImPlotMarker_Up,
                      6.5f,
                      ImVec4(0.25f, 0.75f, 1.0f, 0.95f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.9f),
                      scene_height_f);
        }
    }
}
