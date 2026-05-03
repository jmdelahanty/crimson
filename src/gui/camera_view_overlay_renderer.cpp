#include "gui/camera_view_overlay_renderer.h"
#include "gui/refined_keypoint_style.h"

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

ImVec4 chooseKeypointColor(const std::string& lowered_label, size_t kp_idx) {
    return chooseRefinedKeypointColor(lowered_label, kp_idx);
}

ImPlotMarker chooseKeypointMarker(const std::string& lowered_label,
                                  size_t kp_idx) {
    if (lowered_label.find("swim") != std::string::npos ||
        lowered_label.find("bladder") != std::string::npos) {
        return ImPlotMarker_Circle;
    }
    if (lowered_label.find("left") != std::string::npos) {
        return ImPlotMarker_Square;
    }
    if (lowered_label.find("right") != std::string::npos) {
        return ImPlotMarker_Diamond;
    }
    static const ImPlotMarker kFallbackMarkers[] = {
        ImPlotMarker_Circle, ImPlotMarker_Square, ImPlotMarker_Diamond,
        ImPlotMarker_Cross,  ImPlotMarker_Plus,   ImPlotMarker_Up,
        ImPlotMarker_Down,
    };
    return kFallbackMarkers[kp_idx %
                            (sizeof(kFallbackMarkers) / sizeof(kFallbackMarkers[0]))];
}

float chooseKeypointSize(const std::string& lowered_label) {
    if (lowered_label.find("swim") != std::string::npos ||
        lowered_label.find("bladder") != std::string::npos) {
        return 4.5f;
    }
    if (lowered_label.find("left") != std::string::npos ||
        lowered_label.find("right") != std::string::npos) {
        return 4.5f;
    }
    return 7.0f;
}

bool isFinitePoint(const std::array<float, 2>& point) {
    return std::isfinite(point[0]) && std::isfinite(point[1]);
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

size_t findKeypointIndex(const std::vector<std::string>& labels,
                         bool want_swim_bladder,
                         bool want_left_eye,
                         bool want_right_eye) {
    for (size_t idx = 0; idx < labels.size(); ++idx) {
        const std::string label = toLowerCopy(labels[idx]);
        const bool has_eye = label.find("eye") != std::string::npos;
        const bool has_left = label.find("left") != std::string::npos;
        const bool has_right = label.find("right") != std::string::npos;
        const bool has_swim = label.find("swim") != std::string::npos;
        const bool has_bladder = label.find("bladder") != std::string::npos;
        if (want_swim_bladder && has_swim && has_bladder) {
            return idx;
        }
        if (want_left_eye && has_eye && has_left) {
            return idx;
        }
        if (want_right_eye && has_eye && has_right) {
            return idx;
        }
    }
    return SIZE_MAX;
}

}  // namespace

std::vector<FullFrameRectOverlayItem> buildCameraViewBoundingBoxOverlayItems(
    const std::vector<LoggedBoundingBox>& zarr_boxes,
    const ZarrDetectionLoader::FrameDetections& detection_details,
    const ZarrBBoxEditState& bbox_edit_state,
    int current_frame_num,
    bool frame_has_bbox_edits,
    bool active_dataset_has_synthetic_detections,
    bool is_zarr_interpolated) {
    enum class BoxProvenance {
        Clean = 0,
        Interpolated = 1,
        Manual = 2,
    };

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

    std::vector<FullFrameRectOverlayItem> overlay_items;
    overlay_items.reserve(zarr_boxes.size());
    for (size_t box_idx = 0; box_idx < zarr_boxes.size(); ++box_idx) {
        const auto& box = zarr_boxes[box_idx];
        BoxProvenance box_provenance = classify_box_provenance(box_idx);
        ImVec4 box_color = ImVec4(0.2f, 0.6f, 1.0f, 1.0f);
        float line_width = 2.0f;
        if (box_provenance == BoxProvenance::Interpolated) {
            box_color = ImVec4(1.0f, 0.7f, 0.0f, 0.9f);
            line_width = 2.5f;
        } else if (box_provenance == BoxProvenance::Manual) {
            box_color = ImVec4(0.0f, 0.85f, 0.65f, 1.0f);
            line_width = 2.75f;
        }

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
        if (box_selected) {
            box_color = ImVec4(1.0f, 0.25f, 0.95f, 1.0f);
            line_width = 3.5f;
        } else if (box_is_added) {
            box_color = ImVec4(0.95f, 0.35f, 0.15f, 1.0f);
            line_width = std::max(line_width, 3.0f);
        } else if (frame_has_bbox_edits) {
            line_width = std::max(line_width, 2.5f);
        }

        std::string label = "Zarr_" + std::to_string(box.class_id);
        if (box_provenance == BoxProvenance::Interpolated) {
            label += " [I]";
        } else if (box_provenance == BoxProvenance::Manual) {
            label += " [MAN]";
        }
        if (box_is_added) {
            label += " [A]";
        }
        if (box_selected) {
            label += " [S]";
        } else if (frame_has_bbox_edits) {
            label += " [M]";
        }

        overlay_items.push_back({{box.x_min, box.y_min, box.width, box.height},
                                 box_color,
                                 line_width,
                                 std::move(label)});
    }

    return overlay_items;
}

void drawCameraViewDetectionKeypointMarkers(
    const ZarrDetectionLoader::FrameDetections& detection_details,
    bool show_keypoint_markers,
    float image_height_px,
    int skip_detection_index) {
    if (!(show_keypoint_markers && detection_details.has_keypoints &&
          !detection_details.keypoints_pixels.empty() &&
          detection_details.keypoints_per_detection > 0)) {
        return;
    }

    const size_t kp_per_det = detection_details.keypoints_per_detection;
    std::vector<std::string> lowered_labels(kp_per_det);
    for (size_t kp_idx = 0; kp_idx < kp_per_det; ++kp_idx) {
        if (kp_idx < detection_details.keypoint_labels.size()) {
            lowered_labels[kp_idx] =
                toLowerCopy(detection_details.keypoint_labels[kp_idx]);
        }
    }

    const size_t detection_count =
        std::min(detection_details.keypoints_pixels.size(),
                 detection_details.boxes.size());
    const double img_h = static_cast<double>(image_height_px);

    if (!detection_details.skeleton_edges.empty()) {
        ImPlot::PushStyleColor(ImPlotCol_Line,
                               ImVec4(1.0f, 1.0f, 1.0f, 0.63f));
        for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
            const auto& keypoints = detection_details.keypoints_pixels[det_idx];
            if (keypoints.size() != kp_per_det) {
                continue;
            }
            for (const auto& edge : detection_details.skeleton_edges) {
                const size_t a = edge[0];
                const size_t b = edge[1];
                if (a >= kp_per_det || b >= kp_per_det) {
                    continue;
                }
                const float ax = keypoints[a][0];
                const float ay = keypoints[a][1];
                const float bx = keypoints[b][0];
                const float by = keypoints[b][1];
                if (!std::isfinite(ax) || !std::isfinite(ay) ||
                    !std::isfinite(bx) || !std::isfinite(by)) {
                    continue;
                }
                const double xs[2] = {static_cast<double>(ax),
                                      static_cast<double>(bx)};
                const double ys[2] = {
                    img_h - static_cast<double>(ay),
                    img_h - static_cast<double>(by),
                };
                const std::string label =
                    "##edge_" + std::to_string(det_idx) + "_" +
                    std::to_string(a) + "_" + std::to_string(b);
                ImPlot::PlotLine(label.c_str(), xs, ys, 2);
            }
        }
        ImPlot::PopStyleColor();
    }

    for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
        if (skip_detection_index >= 0 &&
            det_idx == static_cast<size_t>(skip_detection_index)) {
            continue;
        }
        const auto& keypoints = detection_details.keypoints_pixels[det_idx];
        if (keypoints.size() != kp_per_det) {
            continue;
        }
        bool detection_is_interp = false;
        if (!detection_details.detection_source.empty() &&
            det_idx < detection_details.detection_source.size()) {
            detection_is_interp =
                detection_details.detection_source[det_idx] != 0;
        }
        uint8_t heading_valid_flag = 1;
        if (!detection_details.heading_valid.empty() &&
            det_idx < detection_details.heading_valid.size()) {
            heading_valid_flag = detection_details.heading_valid[det_idx];
        }

        for (size_t kp_idx = 0; kp_idx < kp_per_det; ++kp_idx) {
            const auto& kp = keypoints[kp_idx];
            const float kp_x = kp[0];
            const float kp_y = kp[1];
            if (!std::isfinite(kp_x) || !std::isfinite(kp_y)) {
                continue;
            }

            double plot_x = static_cast<double>(kp_x);
            double plot_y = img_h - static_cast<double>(kp_y);

            ImVec4 base_color =
                chooseKeypointColor(lowered_labels[kp_idx], kp_idx);
            float alpha_scale = 1.0f;
            if (heading_valid_flag == 0) {
                alpha_scale *= 0.4f;
            }
            if (detection_is_interp) {
                alpha_scale *= 0.65f;
            }

            bool kp_unusable = false;
            bool kp_flip_corrected = false;
            if (detection_details.is_refined_keypoints) {
                if (det_idx < detection_details.keypoint_usable.size() &&
                    detection_details.keypoint_usable[det_idx] == 0) {
                    alpha_scale *= 0.35f;
                    kp_unusable = true;
                }
                if (det_idx <
                        detection_details.keypoint_detection_source.size() &&
                    detection_details.keypoint_detection_source[det_idx] != 0) {
                    alpha_scale *= 0.65f;
                }
                if (det_idx < detection_details.keypoint_flip_corrected.size() &&
                    detection_details.keypoint_flip_corrected[det_idx] != 0) {
                    kp_flip_corrected = true;
                }
            }

            alpha_scale = std::clamp(alpha_scale, 0.25f, 1.0f);

            ImVec4 fill_color = base_color;
            fill_color.w *= alpha_scale;
            ImVec4 outline_color = base_color;
            outline_color.w = std::max(alpha_scale, 0.6f);
            if (kp_flip_corrected) {
                outline_color =
                    ImVec4(0.0f, 0.9f, 0.9f, outline_color.w);
            }
            if (kp_unusable) {
                outline_color =
                    ImVec4(0.95f, 0.3f, 0.3f, outline_color.w);
            }

            ImPlot::SetNextMarkerStyle(
                chooseKeypointMarker(lowered_labels[kp_idx], kp_idx),
                chooseKeypointSize(lowered_labels[kp_idx]),
                fill_color,
                2.0f,
                outline_color);
            const std::string label =
                "##kp_" + std::to_string(det_idx) + "_" +
                std::to_string(kp_idx);
            ImPlot::PlotScatter(label.c_str(), &plot_x, &plot_y, 1);
        }
    }
}

void drawCameraViewHeadingOverlay(
    const ZarrDetectionLoader::FrameDetections& heading_details,
    float image_height_px) {
    const size_t det_count = heading_details.boxes.size();
    const size_t valid_count = heading_details.heading_valid.size();
    const size_t heading_count = heading_details.headings_deg.size();
    if (det_count == 0 || valid_count != det_count) {
        return;
    }

    constexpr double kPi = 3.14159265358979323846;
    ImDrawList* plot_draw_list = ImPlot::GetPlotDrawList();
    const ImU32 arrow_color =
        ImGui::GetColorU32(ImVec4(1.0f, 0.25f, 0.1f, 0.95f));
    constexpr float arrow_thickness = 2.0f;
    const float scene_height_f = image_height_px;
    const bool has_keypoint_heading_source =
        heading_details.has_keypoints &&
        !heading_details.keypoints_pixels.empty() &&
        !heading_details.keypoint_labels.empty();
    const size_t swim_bladder_kp_idx =
        has_keypoint_heading_source
            ? findKeypointIndex(heading_details.keypoint_labels, true, false,
                                false)
            : SIZE_MAX;
    const size_t left_eye_kp_idx =
        has_keypoint_heading_source
            ? findKeypointIndex(heading_details.keypoint_labels, false, true,
                                false)
            : SIZE_MAX;
    const size_t right_eye_kp_idx =
        has_keypoint_heading_source
            ? findKeypointIndex(heading_details.keypoint_labels, false, false,
                                true)
            : SIZE_MAX;

    for (size_t det_idx = 0; det_idx < det_count; ++det_idx) {
        if (heading_details.heading_valid[det_idx] == 0) {
            continue;
        }
        if (!heading_details.detection_source.empty() &&
            det_idx < heading_details.detection_source.size() &&
            heading_details.detection_source[det_idx] != 0) {
            continue;
        }
        if (det_idx >= heading_details.swim_bladder_pixels.size()) {
            continue;
        }

        const auto& box = heading_details.boxes[det_idx];
        const float box_width = std::max(0.0f, box[2] - box[0]);
        const float box_height = std::max(0.0f, box[3] - box[1]);
        float base_x = heading_details.swim_bladder_pixels[det_idx][0];
        float base_y = heading_details.swim_bladder_pixels[det_idx][1];
        bool used_keypoint_direction = false;
        float end_x = 0.0f;
        float end_y = 0.0f;

        if (has_keypoint_heading_source &&
            swim_bladder_kp_idx != SIZE_MAX &&
            left_eye_kp_idx != SIZE_MAX &&
            right_eye_kp_idx != SIZE_MAX &&
            det_idx < heading_details.keypoints_pixels.size()) {
            const auto& keypoints = heading_details.keypoints_pixels[det_idx];
            if (swim_bladder_kp_idx < keypoints.size() &&
                left_eye_kp_idx < keypoints.size() &&
                right_eye_kp_idx < keypoints.size()) {
                const auto& swim_bladder = keypoints[swim_bladder_kp_idx];
                const auto& left_eye = keypoints[left_eye_kp_idx];
                const auto& right_eye = keypoints[right_eye_kp_idx];
                if (isFinitePoint(swim_bladder) && isFinitePoint(left_eye) &&
                    isFinitePoint(right_eye)) {
                    base_x = swim_bladder[0];
                    base_y = swim_bladder[1];
                    const float eye_mid_x = 0.5f * (left_eye[0] + right_eye[0]);
                    const float eye_mid_y = 0.5f * (left_eye[1] + right_eye[1]);
                    const float dir_x = eye_mid_x - base_x;
                    const float dir_y = eye_mid_y - base_y;
                    const float dir_len =
                        std::sqrt(dir_x * dir_x + dir_y * dir_y);
                    if (dir_len > 1e-3f) {
                        const float bbox_scale =
                            std::max(box_width, box_height) * 1.25f;
                        const float frame_scale = scene_height_f * 0.02f;
                        const float extension =
                            std::max(20.0f, dir_len * 0.35f);
                        const float arrow_len =
                            std::max(dir_len + extension,
                                     std::max(60.0f,
                                              std::max(bbox_scale,
                                                       frame_scale)));
                        const float shortened_arrow_len =
                            std::max(dir_len + 8.0f, arrow_len * (2.0f / 3.0f));
                        end_x =
                            base_x + (dir_x / dir_len) * shortened_arrow_len;
                        end_y =
                            base_y + (dir_y / dir_len) * shortened_arrow_len;
                        used_keypoint_direction = true;
                    }
                }
            }
        }

        if (!std::isfinite(base_x) || !std::isfinite(base_y)) {
            base_x = 0.5f * (box[0] + box[2]);
            base_y = 0.5f * (box[1] + box[3]);
        }

        if (!used_keypoint_direction) {
            if (det_idx >= heading_count) {
                continue;
            }
            const float heading_deg = heading_details.headings_deg[det_idx];
            const float heading_rad =
                heading_deg * static_cast<float>(kPi) / 180.0f;
            const float bbox_scale = std::max(box_width, box_height) * 1.25f;
            const float frame_scale = scene_height_f * 0.02f;
            const float arrow_len =
                std::max(60.0f, std::max(bbox_scale, frame_scale));
            const float shortened_arrow_len = arrow_len * (2.0f / 3.0f);
            end_x = base_x + std::cos(heading_rad) * shortened_arrow_len;
            end_y = base_y - std::sin(heading_rad) * shortened_arrow_len;
        }

        ImPlotPoint plot_start(base_x, scene_height_f - base_y);
        ImPlotPoint plot_end(end_x, scene_height_f - end_y);
        const ImVec2 p0 = ImPlot::PlotToPixels(plot_start);
        const ImVec2 p1 = ImPlot::PlotToPixels(plot_end);
        plot_draw_list->AddLine(p0, p1, arrow_color, arrow_thickness);

        ImVec2 dir(p0.x - p1.x, p0.y - p1.y);
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len > 1e-3f) {
            dir.x /= len;
            dir.y /= len;
            constexpr float head_size = 8.0f;
            const ImVec2 left(
                p1.x + dir.x * head_size + dir.y * head_size * 0.5f,
                p1.y + dir.y * head_size - dir.x * head_size * 0.5f);
            const ImVec2 right(
                p1.x + dir.x * head_size - dir.y * head_size * 0.5f,
                p1.y + dir.y * head_size + dir.x * head_size * 0.5f);
            plot_draw_list->AddTriangleFilled(p1, left, right, arrow_color);
        }
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
