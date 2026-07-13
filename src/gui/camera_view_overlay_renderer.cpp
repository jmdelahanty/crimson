#include "gui/camera_view_overlay_renderer.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

void drawBoxedOverlayText(const ImVec2& top_left,
                          const std::vector<std::string>& lines,
                          ImU32 text_color,
                          ImU32 fill_color,
                          ImU32 border_color) {
    if (lines.empty()) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 padding(7.0f, 5.0f);
    const float line_spacing = 2.0f;
    float width = 0.0f;
    float height = padding.y * 2.0f;
    for (const auto& line : lines) {
        const ImVec2 size = ImGui::CalcTextSize(line.c_str());
        width = std::max(width, size.x);
        height += size.y + line_spacing;
    }
    height -= line_spacing;
    const ImVec2 box_min = top_left;
    const ImVec2 box_max(top_left.x + width + padding.x * 2.0f,
                         top_left.y + height);

    draw_list->AddRectFilled(box_min, box_max, fill_color, 4.0f);
    draw_list->AddRect(box_min, box_max, border_color, 4.0f, 0, 1.0f);

    float text_y = top_left.y + padding.y;
    for (const auto& line : lines) {
        draw_list->AddText(ImVec2(top_left.x + padding.x, text_y),
                           text_color,
                           line.c_str());
        text_y += ImGui::GetTextLineHeight() + line_spacing;
    }
}

ImVec2 boxedOverlayTextSize(const std::vector<std::string>& lines) {
    const ImVec2 padding(7.0f, 5.0f);
    const float line_spacing = 2.0f;
    float width = 0.0f;
    float height = padding.y * 2.0f;
    for (const auto& line : lines) {
        const ImVec2 size = ImGui::CalcTextSize(line.c_str());
        width = std::max(width, size.x);
        height += size.y + line_spacing;
    }
    if (!lines.empty()) {
        height -= line_spacing;
    }
    return ImVec2(width + padding.x * 2.0f, height);
}

ImVec2 clampOverlayTextPosition(ImVec2 top_left,
                                const ImVec2& box_size) {
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    constexpr float margin = 4.0f;
    const float min_x = plot_pos.x + margin;
    const float min_y = plot_pos.y + margin;
    const float max_x = plot_pos.x + plot_size.x - box_size.x - margin;
    const float max_y = plot_pos.y + plot_size.y - box_size.y - margin;
    if (min_x <= max_x) {
        top_left.x = std::clamp(top_left.x, min_x, max_x);
    } else {
        top_left.x = min_x;
    }
    if (min_y <= max_y) {
        top_left.y = std::clamp(top_left.y, min_y, max_y);
    } else {
        top_left.y = min_y;
    }
    return top_left;
}

bool boxCoordinatesAreFinite(const std::array<float, 4>& box) {
    return std::isfinite(static_cast<double>(box[0])) &&
           std::isfinite(static_cast<double>(box[1])) &&
           std::isfinite(static_cast<double>(box[2])) &&
           std::isfinite(static_cast<double>(box[3]));
}

const std::array<float, 4>* findMovementAnchorBox(
    const ZarrDetectionLoader::MovementFrameSample& movement_sample,
    const ZarrDetectionLoader::FrameDetections* detection_details) {
    if (detection_details == nullptr || detection_details->boxes.empty()) {
        return nullptr;
    }

    const std::array<float, 4>* best_box = nullptr;
    float best_score = std::numeric_limits<float>::infinity();
    for (const auto& box : detection_details->boxes) {
        if (!boxCoordinatesAreFinite(box)) {
            continue;
        }
        if (!movement_sample.has_position_px) {
            return &box;
        }
        const float cx = 0.5f * (box[0] + box[2]);
        const float cy = 0.5f * (box[1] + box[3]);
        const float dx = cx - movement_sample.x_px;
        const float dy = cy - movement_sample.y_px;
        const float score = dx * dx + dy * dy;
        if (score < best_score) {
            best_score = score;
            best_box = &box;
        }
    }
    return best_box;
}

ImVec4 toImVec4(const crimson::overlay::Color& color) {
    return ImVec4(color.red, color.green, color.blue, color.alpha);
}

ImPlotMarker toImPlotMarker(crimson::overlay::MarkerShape shape) {
    using crimson::overlay::MarkerShape;
    switch (shape) {
        case MarkerShape::Circle:
            return ImPlotMarker_Circle;
        case MarkerShape::Square:
            return ImPlotMarker_Square;
        case MarkerShape::Diamond:
            return ImPlotMarker_Diamond;
        case MarkerShape::Cross:
            return ImPlotMarker_Cross;
        case MarkerShape::Plus:
            return ImPlotMarker_Plus;
        case MarkerShape::TriangleUp:
            return ImPlotMarker_Up;
        case MarkerShape::TriangleDown:
            return ImPlotMarker_Down;
    }
    return ImPlotMarker_Circle;
}

crimson::overlay::FrameIdentity overlayIdentity(int view_idx,
                                                int presented_frame,
                                                int current_frame_num) {
    return {view_idx, presented_frame, view_idx, current_frame_num};
}

crimson::overlay::Rect overlayRectFromXyxy(
    const std::array<float, 4>& box) {
    return {box[0], box[1], box[2] - box[0], box[3] - box[1]};
}

std::vector<crimson::overlay::Point> overlayPoints(
    const std::vector<std::array<float, 2>>& points) {
    std::vector<crimson::overlay::Point> result;
    result.reserve(points.size());
    for (const auto& point : points) {
        result.push_back({point[0], point[1]});
    }
    return result;
}

}  // namespace

crimson::overlay::ReadOnlyOverlayScene
buildCameraViewBoundingBoxOverlayScene(
    const std::vector<LoggedBoundingBox>& zarr_boxes,
    const ZarrDetectionLoader::FrameDetections& detection_details,
    const ZarrBBoxEditState& bbox_edit_state,
    int view_idx,
    int presented_frame,
    int current_frame_num,
    float image_width_px,
    float image_height_px,
    bool frame_has_bbox_edits,
    bool active_dataset_has_synthetic_detections,
    bool is_zarr_interpolated) {
    using crimson::overlay::BoxProvenance;
    crimson::overlay::ReadOnlyOverlayInput input;
    input.identity =
        overlayIdentity(view_idx, presented_frame, current_frame_num);
    input.source_width = image_width_px;
    input.source_height = image_height_px;
    input.show_headings = false;
    input.show_keypoints = false;
    input.detections.reserve(zarr_boxes.size());

    auto classify_box_provenance = [&](size_t box_idx) -> BoxProvenance {
        if (!detection_details.detection_reason.empty() &&
            box_idx < detection_details.detection_reason.size()) {
            std::string reason =
                toLowerCopy(detection_details.detection_reason[box_idx]);
            if (reason == "manual" ||
                reason.find("manual") != std::string::npos) {
                return BoxProvenance::Manual;
            }
            if (reason == "interpolated" ||
                reason.find("interp") != std::string::npos) {
                return BoxProvenance::Interpolated;
            }
            if (reason == "clean") {
                return BoxProvenance::Clean;
            }
        }

        bool detection_is_interp = active_dataset_has_synthetic_detections;
        if (!detection_details.detection_source.empty()) {
            if (box_idx < detection_details.detection_source.size()) {
                detection_is_interp =
                    detection_details.detection_source[box_idx] != 0;
            } else {
                detection_is_interp = false;
            }
        }
        if (is_zarr_interpolated && active_dataset_has_synthetic_detections &&
            detection_details.detection_source.empty()) {
            detection_is_interp = true;
        }
        return detection_is_interp ? BoxProvenance::Interpolated
                                   : BoxProvenance::Clean;
    };

    for (size_t box_idx = 0; box_idx < zarr_boxes.size(); ++box_idx) {
        const auto& box = zarr_boxes[box_idx];
        BoxProvenance box_provenance = classify_box_provenance(box_idx);
        const bool box_selected =
            (bbox_edit_state.selected_frame == current_frame_num) &&
            (bbox_edit_state.selected_box == static_cast<int>(box_idx));
        const bool box_is_added =
            bbox_edit_state.isAddedBox(current_frame_num,
                                       static_cast<int>(box_idx));
        const bool box_is_manual =
            bbox_edit_state.isManualBox(current_frame_num,
                                        static_cast<int>(box_idx));
        if (box_is_manual) {
            box_provenance = BoxProvenance::Manual;
        }
        crimson::overlay::DetectionOverlayInput detection;
        detection.box = crimson::overlay::DetectionBoxInput{
            {box.x_min, box.y_min, box.width, box.height},
            box.class_id,
            box_provenance,
            box_selected,
            box_is_added,
            frame_has_bbox_edits};
        input.detections.push_back(std::move(detection));
    }
    return crimson::overlay::buildReadOnlyOverlayScene(input);
}

void drawCameraViewDetectionKeypointMarkers(
    const ZarrDetectionLoader::FrameDetections& detection_details,
    bool show_keypoint_markers,
    float image_width_px,
    float image_height_px,
    int view_idx,
    int presented_frame,
    int current_frame_num,
    int skip_detection_index) {
    if (!(show_keypoint_markers && detection_details.has_keypoints &&
          !detection_details.keypoints_pixels.empty() &&
          detection_details.keypoints_per_detection > 0)) {
        return;
    }

    crimson::overlay::ReadOnlyOverlayInput input;
    input.identity =
        overlayIdentity(view_idx, presented_frame, current_frame_num);
    input.source_width = image_width_px;
    input.source_height = image_height_px;
    input.keypoint_labels = detection_details.keypoint_labels;
    input.skeleton_edges = detection_details.skeleton_edges;
    input.show_boxes = false;
    input.show_headings = false;
    const size_t detection_count =
        std::min(detection_details.keypoints_pixels.size(),
                 detection_details.boxes.size());
    input.detections.reserve(detection_count);
    for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
        const auto& keypoints = detection_details.keypoints_pixels[det_idx];
        if (keypoints.size() != detection_details.keypoints_per_detection) {
            continue;
        }
        crimson::overlay::DetectionOverlayInput detection;
        detection.box = crimson::overlay::DetectionBoxInput{
            overlayRectFromXyxy(detection_details.boxes[det_idx])};
        detection.keypoints = overlayPoints(keypoints);
        detection.suppress_keypoint_markers =
            skip_detection_index >= 0 &&
            det_idx == static_cast<size_t>(skip_detection_index);
        if (!detection_details.detection_source.empty() &&
            det_idx < detection_details.detection_source.size()) {
            detection.detection_interpolated =
                detection_details.detection_source[det_idx] != 0;
        }
        detection.heading_valid = true;
        if (!detection_details.heading_valid.empty() &&
            det_idx < detection_details.heading_valid.size()) {
            detection.heading_valid =
                detection_details.heading_valid[det_idx] != 0;
        }
        detection.refined_keypoints =
            detection_details.is_refined_keypoints;
        if (det_idx < detection_details.keypoint_usable.size()) {
            detection.keypoint_usable =
                detection_details.keypoint_usable[det_idx] != 0;
        }
        if (det_idx < detection_details.keypoint_detection_source.size()) {
            detection.keypoint_detection_interpolated =
                detection_details.keypoint_detection_source[det_idx] != 0;
        }
        if (det_idx < detection_details.keypoint_flip_corrected.size()) {
            detection.keypoint_flip_corrected =
                detection_details.keypoint_flip_corrected[det_idx] != 0;
        }
        input.detections.push_back(std::move(detection));
    }
    drawCameraViewReadOnlyOverlayScene(
        crimson::overlay::buildReadOnlyOverlayScene(input), image_height_px);
}

void drawCameraViewHeadingOverlay(
    const ZarrDetectionLoader::FrameDetections& heading_details,
    float image_width_px,
    float image_height_px,
    int view_idx,
    int presented_frame,
    int current_frame_num) {
    const size_t det_count = heading_details.boxes.size();
    const size_t valid_count = heading_details.heading_valid.size();
    if (det_count == 0 || valid_count != det_count) {
        return;
    }
    crimson::overlay::ReadOnlyOverlayInput input;
    input.identity =
        overlayIdentity(view_idx, presented_frame, current_frame_num);
    input.source_width = image_width_px;
    input.source_height = image_height_px;
    const bool has_keypoint_heading_source =
        heading_details.has_keypoints &&
        !heading_details.keypoints_pixels.empty() &&
        !heading_details.keypoint_labels.empty();
    if (has_keypoint_heading_source) {
        input.keypoint_labels = heading_details.keypoint_labels;
    }
    input.show_boxes = false;
    input.show_keypoints = false;
    input.detections.reserve(det_count);
    for (size_t det_idx = 0; det_idx < det_count; ++det_idx) {
        crimson::overlay::DetectionOverlayInput detection;
        detection.box = crimson::overlay::DetectionBoxInput{
            overlayRectFromXyxy(heading_details.boxes[det_idx])};
        detection.heading_valid =
            heading_details.heading_valid[det_idx] != 0;
        if (!heading_details.detection_source.empty() &&
            det_idx < heading_details.detection_source.size()) {
            detection.detection_interpolated =
                heading_details.detection_source[det_idx] != 0;
        }
        if (det_idx < heading_details.swim_bladder_pixels.size()) {
            const auto& point = heading_details.swim_bladder_pixels[det_idx];
            detection.heading_origin =
                crimson::overlay::Point{point[0], point[1]};
        }
        if (det_idx < heading_details.headings_deg.size()) {
            detection.heading_degrees =
                heading_details.headings_deg[det_idx];
        }
        if (has_keypoint_heading_source &&
            det_idx < heading_details.keypoints_pixels.size()) {
            detection.keypoints =
                overlayPoints(heading_details.keypoints_pixels[det_idx]);
        }
        input.detections.push_back(std::move(detection));
    }
    drawCameraViewReadOnlyOverlayScene(
        crimson::overlay::buildReadOnlyOverlayScene(input), image_height_px);
}

void drawCameraViewReadOnlyOverlayScene(
    const crimson::overlay::ReadOnlyOverlayScene& scene,
    float image_height_px) {
    if (!scene.ready()) {
        return;
    }
    for (const auto& primitive : scene.primitives) {
        if (primitive.type == crimson::overlay::PrimitiveType::Polyline) {
            if (primitive.points.size() < 2) {
                continue;
            }
            std::vector<double> x;
            std::vector<double> y;
            x.reserve(primitive.points.size());
            y.reserve(primitive.points.size());
            for (const auto& point : primitive.points) {
                x.push_back(point.x);
                y.push_back(static_cast<double>(image_height_px) - point.y);
            }
            ImPlot::SetNextLineStyle(
                toImVec4(primitive.stroke),
                static_cast<float>(primitive.stroke_width_px));
            ImPlot::PlotLine(primitive.label.c_str(), x.data(), y.data(),
                             static_cast<int>(x.size()));
            continue;
        }
        if (primitive.type == crimson::overlay::PrimitiveType::Marker) {
            if (primitive.points.size() != 1) {
                continue;
            }
            double x = primitive.points[0].x;
            double y = static_cast<double>(image_height_px) -
                       primitive.points[0].y;
            ImPlot::SetNextMarkerStyle(
                toImPlotMarker(primitive.marker_shape),
                static_cast<float>(primitive.marker_size_px),
                toImVec4(primitive.fill),
                static_cast<float>(primitive.outline_width_px),
                toImVec4(primitive.outline));
            ImPlot::PlotScatter(primitive.label.c_str(), &x, &y, 1);
            continue;
        }
        if (primitive.type != crimson::overlay::PrimitiveType::Arrow ||
            primitive.points.size() != 2) {
            continue;
        }
        const ImVec2 start = ImPlot::PlotToPixels(ImPlotPoint(
            primitive.points[0].x,
            static_cast<double>(image_height_px) - primitive.points[0].y));
        const ImVec2 end = ImPlot::PlotToPixels(ImPlotPoint(
            primitive.points[1].x,
            static_cast<double>(image_height_px) - primitive.points[1].y));
        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
        draw_list->AddLine(
            start, end, ImGui::GetColorU32(toImVec4(primitive.stroke)),
            static_cast<float>(primitive.stroke_width_px));
        ImVec2 direction(start.x - end.x, start.y - end.y);
        const float length = std::hypot(direction.x, direction.y);
        if (length <= 1e-3f) {
            continue;
        }
        direction.x /= length;
        direction.y /= length;
        const float head = static_cast<float>(primitive.arrow_head_size_px);
        const ImVec2 left(
            end.x + direction.x * head + direction.y * head * 0.5f,
            end.y + direction.y * head - direction.x * head * 0.5f);
        const ImVec2 right(
            end.x + direction.x * head - direction.y * head * 0.5f,
            end.y + direction.y * head + direction.x * head * 0.5f);
        draw_list->AddTriangleFilled(
            end, left, right,
            ImGui::GetColorU32(toImVec4(primitive.fill)));
    }
}

void drawCameraViewMovementOverlay(
    const ZarrDetectionLoader::MovementFrameSample& movement_sample,
    const ZarrDetectionLoader::FrameDetections* detection_details,
    float image_height_px) {
    if (!movement_sample.valid ||
        (!movement_sample.has_speed && !movement_sample.has_heading)) {
        return;
    }

    std::ostringstream metrics;
    metrics << std::fixed << std::setprecision(1);
    bool wrote_metric = false;
    if (movement_sample.has_speed) {
        const std::string label =
            movement_sample.speed_label.empty() ? "Speed"
                                                : movement_sample.speed_label;
        metrics << label << " " << movement_sample.speed << " "
                << (movement_sample.speed_units.empty()
                        ? "units"
                        : movement_sample.speed_units);
        wrote_metric = true;
    }
    if (movement_sample.has_heading) {
        if (wrote_metric) {
            metrics << " | ";
        }
        metrics << (movement_sample.heading_smoothed ? "Heading smoothed "
                                                     : "Heading ")
                << movement_sample.heading_degrees << " deg";
    }

    std::ostringstream source;
    source << "Track kinematics";
    if (!movement_sample.speed_level.empty()) {
        source << " | " << movement_sample.speed_level;
    }
    if (!movement_sample.track_id.empty()) {
        source << " | " << movement_sample.track_id;
    }

    std::vector<std::string> lines = {metrics.str(), source.str()};
    if (movement_sample.has_sample_valid ||
        movement_sample.has_transition_valid) {
        std::ostringstream validity;
        bool wrote = false;
        if (movement_sample.has_sample_valid) {
            validity << "sample "
                     << (movement_sample.sample_valid ? "valid" : "invalid");
            wrote = true;
        }
        if (movement_sample.has_transition_valid) {
            if (wrote) {
                validity << ", ";
            }
            validity << "transition "
                     << (movement_sample.transition_valid ? "valid"
                                                          : "invalid");
        }
        lines.push_back(validity.str());
    }

    ImVec2 label_pos = ImPlot::GetPlotPos();
    label_pos.x += 12.0f;
    label_pos.y += 12.0f;

    const ImVec2 box_size = boxedOverlayTextSize(lines);
    if (const auto* anchor_box =
            findMovementAnchorBox(movement_sample, detection_details)) {
        const ImPlotPoint plot_corner(
            static_cast<double>((*anchor_box)[2]),
            static_cast<double>(image_height_px - (*anchor_box)[3]));
        const ImVec2 corner_px = ImPlot::PlotToPixels(plot_corner);
        constexpr float offset = 8.0f;
        label_pos = ImVec2(corner_px.x + offset, corner_px.y + offset);
        const ImVec2 plot_pos = ImPlot::GetPlotPos();
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        const float plot_max_x = plot_pos.x + plot_size.x;
        const float plot_max_y = plot_pos.y + plot_size.y;
        if (label_pos.x + box_size.x > plot_max_x) {
            label_pos.x = corner_px.x - box_size.x - offset;
        }
        if (label_pos.y + box_size.y > plot_max_y) {
            label_pos.y = corner_px.y - box_size.y - offset;
        }
    }
    label_pos = clampOverlayTextPosition(label_pos, box_size);

    const ImU32 text_color =
        ImGui::GetColorU32(ImVec4(0.93f, 0.99f, 1.0f, 1.0f));
    const ImU32 fill_color =
        ImGui::GetColorU32(ImVec4(0.02f, 0.07f, 0.09f, 0.78f));
    const ImU32 border_color =
        ImGui::GetColorU32(ImVec4(0.15f, 0.9f, 1.0f, 0.88f));
    drawBoxedOverlayText(label_pos, lines, text_color, fill_color, border_color);
}

void drawCameraViewMovementTrailOverlay(
    const std::vector<ZarrDetectionLoader::MovementTrailPoint>& trail_points,
    float image_height_px) {
    if (trail_points.empty()) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    auto to_pixels = [image_height_px](const auto& point) {
        return ImPlot::PlotToPixels(ImPlotPoint(
            static_cast<double>(point.x_px),
            static_cast<double>(image_height_px - point.y_px)));
    };
    auto color_for_alpha = [](float alpha_scale) {
        const float alpha = std::clamp(0.10f + alpha_scale * 0.72f,
                                       0.08f,
                                       0.86f);
        return ImGui::GetColorU32(ImVec4(0.08f, 0.82f, 1.0f, alpha));
    };

    for (size_t i = 1; i < trail_points.size(); ++i) {
        const auto& prev = trail_points[i - 1];
        const auto& curr = trail_points[i];
        if (curr.break_before) {
            continue;
        }
        if (!std::isfinite(static_cast<double>(prev.x_px)) ||
            !std::isfinite(static_cast<double>(prev.y_px)) ||
            !std::isfinite(static_cast<double>(curr.x_px)) ||
            !std::isfinite(static_cast<double>(curr.y_px))) {
            continue;
        }
        const float segment_alpha =
            std::clamp(0.5f * (prev.alpha + curr.alpha), 0.0f, 1.0f);
        const float thickness = 1.4f + segment_alpha * 2.2f;
        draw_list->AddLine(to_pixels(prev),
                           to_pixels(curr),
                           color_for_alpha(segment_alpha),
                           thickness);
    }

    for (size_t i = 0; i < trail_points.size(); ++i) {
        const auto& point = trail_points[i];
        if (!std::isfinite(static_cast<double>(point.x_px)) ||
            !std::isfinite(static_cast<double>(point.y_px))) {
            continue;
        }
        const bool newest = i + 1 == trail_points.size();
        const float radius = newest ? 4.2f : 2.0f + point.alpha * 1.2f;
        draw_list->AddCircleFilled(to_pixels(point),
                                   radius,
                                   color_for_alpha(point.alpha));
        if (newest) {
            draw_list->AddCircle(to_pixels(point),
                                 radius + 1.5f,
                                 ImGui::GetColorU32(
                                     ImVec4(0.92f, 1.0f, 1.0f, 0.9f)),
                                 20,
                                 1.5f);
        }
    }
}
