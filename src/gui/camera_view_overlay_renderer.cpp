#include "gui/camera_view_overlay_renderer.h"

#include "global.h"
#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace {

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

struct StimulusOverlayState {
    std::string text;
    int last_event_frame = std::numeric_limits<int>::min();
};

ImVec4 chooseKeypointColor(const std::string& lowered_label, size_t kp_idx) {
    if (lowered_label.find("swim") != std::string::npos ||
        lowered_label.find("bladder") != std::string::npos) {
        return ImVec4(1.0f, 0.85f, 0.15f, 1.0f);
    }
    if (lowered_label.find("left") != std::string::npos) {
        return ImVec4(0.3f, 0.95f, 0.4f, 1.0f);
    }
    if (lowered_label.find("right") != std::string::npos) {
        return ImVec4(0.75f, 0.4f, 0.95f, 1.0f);
    }
    static const ImVec4 kFallbackColors[] = {
        ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
        ImVec4(0.35f, 0.85f, 0.55f, 1.0f),
        ImVec4(0.6f, 0.5f, 0.95f, 1.0f),
        ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
        ImVec4(0.4f, 0.75f, 0.95f, 1.0f),
    };
    return kFallbackColors[kp_idx %
                            (sizeof(kFallbackColors) / sizeof(kFallbackColors[0]))];
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
    float image_height_px) {
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
    if (det_count == 0 || valid_count != det_count || heading_count != det_count) {
        return;
    }

    ImDrawList* plot_draw_list = ImPlot::GetPlotDrawList();
    const ImU32 arrow_color =
        ImGui::GetColorU32(ImVec4(1.0f, 0.25f, 0.1f, 0.95f));
    constexpr float arrow_thickness = 2.0f;
    const float scene_height_f = image_height_px;

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
        if (!std::isfinite(base_x) || !std::isfinite(base_y)) {
            base_x = 0.5f * (box[0] + box[2]);
            base_y = 0.5f * (box[1] + box[3]);
        }

        const float heading_deg = heading_details.headings_deg[det_idx];
        const float heading_rad =
            heading_deg * static_cast<float>(M_PI) / 180.0f;
        const float bbox_scale = std::max(box_width, box_height) * 1.25f;
        const float frame_scale = scene_height_f * 0.02f;
        const float arrow_len =
            std::max(60.0f, std::max(bbox_scale, frame_scale));
        const float end_x = base_x + std::cos(heading_rad) * arrow_len;
        const float end_y = base_y - std::sin(heading_rad) * arrow_len;

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

void drawCameraViewStimulusEventOverlay(
    int view_idx,
    int current_frame_num,
    const std::vector<std::string>& frame_events) {
    static std::array<StimulusOverlayState, MAX_VIEWS> s_overlay_cache;
    if (view_idx < 0 || view_idx >= static_cast<int>(s_overlay_cache.size())) {
        return;
    }

    if (!frame_events.empty()) {
        std::string events_text;
        for (size_t i = 0; i < frame_events.size(); ++i) {
            if (i > 0) {
                events_text += "\n";
            }
            events_text += frame_events[i];
        }
        s_overlay_cache[view_idx].text = std::move(events_text);
        s_overlay_cache[view_idx].last_event_frame = current_frame_num;
    }

    if (s_overlay_cache[view_idx].text.empty() ||
        current_frame_num < s_overlay_cache[view_idx].last_event_frame) {
        return;
    }

    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 overlay_origin(plot_pos.x + 12.0f, plot_pos.y + 12.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(
        s_overlay_cache[view_idx].text.c_str(), nullptr, false, -1.0f);
    const ImVec2 box_min = overlay_origin;
    const ImVec2 box_max(box_min.x + text_size.x + 12.0f,
                         box_min.y + text_size.y + 8.0f);

    draw_list->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 180), 4.0f);
    draw_list->AddRect(box_min, box_max, IM_COL32(80, 180, 255, 220), 4.0f);
    draw_list->AddText(ImVec2(box_min.x + 6.0f, box_min.y + 4.0f),
                       IM_COL32(200, 220, 255, 255),
                       s_overlay_cache[view_idx].text.c_str());
}
