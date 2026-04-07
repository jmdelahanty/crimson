#include "gui/camera_view_overlay_renderer.h"

#include "global.h"
#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
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

struct StimulusOverlayState {
    std::string text;
    int last_event_frame = std::numeric_limits<int>::min();
};

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

void drawCameraViewEyeMaskOverlay(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    float image_height_px,
    const std::string& smoothing_run_id) {
    eyeOrientationSmoother().resetIfRunChanged(smoothing_run_id);
    if (!mask_details.includes_eye_masks) {
        return;
    }

    const size_t mask_count =
        std::min(mask_details.eye_masks.size(), mask_details.boxes.size());
    if (mask_count == 0) {
        return;
    }

    const float scene_height_f = image_height_px;
    for (size_t det_idx = 0; det_idx < mask_count; ++det_idx) {
        const auto& mask_info = mask_details.eye_masks[det_idx];
        if (!mask_details.detection_source.empty() &&
            det_idx < mask_details.detection_source.size() &&
            mask_details.detection_source[det_idx] != 0) {
            continue;
        }
        if (!mask_info.valid || !std::isfinite(mask_info.offset_x) ||
            !std::isfinite(mask_info.offset_y) || mask_info.roi_width <= 0.0f ||
            mask_info.roi_height <= 0.0f || mask_info.rows <= 0 ||
            mask_info.cols <= 0) {
            continue;
        }

        const double cell_w =
            mask_info.roi_width / static_cast<double>(mask_info.cols);
        const double cell_h =
            mask_info.roi_height / static_cast<double>(mask_info.rows);

        for (int eye = 0; eye < 2; ++eye) {
            const auto& pixel_indices = mask_info.pixel_indices[eye];
            const bool has_pixels = !pixel_indices.empty();
            const std::string base_id =
                (eye == 0) ? "##eye_mask_left_" + std::to_string(det_idx)
                           : "##eye_mask_right_" + std::to_string(det_idx);
            ImVec4 base_color = (eye == 0)
                                    ? ImVec4(0.2f, 0.6f, 1.0f, 0.35f)
                                    : ImVec4(1.0f, 0.3f, 0.6f, 0.35f);

            std::vector<double> xs;
            std::vector<double> ys;
            if (has_pixels) {
                xs.reserve(pixel_indices.size());
                ys.reserve(pixel_indices.size());
                for (uint16_t linear : pixel_indices) {
                    const uint16_t row =
                        linear / static_cast<uint16_t>(mask_info.cols);
                    const uint16_t col =
                        linear % static_cast<uint16_t>(mask_info.cols);
                    const double px = mask_info.offset_x +
                                      (static_cast<double>(col) + 0.5) * cell_w;
                    const double py = mask_info.offset_y +
                                      (static_cast<double>(row) + 0.5) * cell_h;
                    xs.push_back(px);
                    ys.push_back(scene_height_f - py);
                }
                if (!xs.empty()) {
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                               3.0f,
                                               base_color,
                                               1.0f,
                                               base_color);
                    ImPlot::PlotScatter((base_id + "_pts").c_str(),
                                        xs.data(),
                                        ys.data(),
                                        static_cast<int>(xs.size()));
                }
            }

            if (mask_info.has_feret_axes && cell_w > 0.0 && cell_h > 0.0) {
                auto roiToWorld = [&](float roi_x,
                                      float roi_y) -> std::pair<double, double> {
                    const double px = mask_info.offset_x +
                                      static_cast<double>(roi_x) * cell_w;
                    const double py = mask_info.offset_y +
                                      static_cast<double>(roi_y) * cell_h;
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
                if (minor_axis.valid) {
                    auto endpoint0_world = roiToWorld(minor_axis.x0, minor_axis.y0);
                    auto endpoint1_world = roiToWorld(minor_axis.x1, minor_axis.y1);
                    auto center_world = roiToWorld(
                        0.5f * (minor_axis.x0 + minor_axis.x1),
                        0.5f * (minor_axis.y0 + minor_axis.y1));
                    if (mask_info.has_eye_angles &&
                        mask_info.feret_angle_valid[eye]) {
                        auto center_scene =
                            worldToScene(center_world.first, center_world.second);
                        char angle_label[32];
                        std::snprintf(angle_label,
                                      sizeof(angle_label),
                                      "%+.1f°",
                                      mask_info.feret_minor_angle_deg[eye]);
                        ImPlot::PlotText(angle_label,
                                         center_scene.first,
                                         center_scene.second,
                                         ImVec2(0.0f, -12.0f));
                    }

                    const double det_center_x = 0.5 *
                        (mask_details.boxes[det_idx][0] + mask_details.boxes[det_idx][2]);
                    const double det_center_y = 0.5 *
                        (mask_details.boxes[det_idx][1] + mask_details.boxes[det_idx][3]);
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
                    float dir_len =
                        std::sqrt(dir_world.x * dir_world.x + dir_world.y * dir_world.y);
                    if (dir_len > 1e-3f) {
                        dir_world.x /= dir_len;
                        dir_world.y /= dir_len;

                        ImVec2 smoothed_dir = eyeOrientationSmoother().smoothDirection(
                            mask_info.roi_index, eye, dir_world);
                        const float smooth_len = std::sqrt(
                            smoothed_dir.x * smoothed_dir.x +
                            smoothed_dir.y * smoothed_dir.y);
                        if (smooth_len > 1e-3f) {
                            smoothed_dir.x /= smooth_len;
                            smoothed_dir.y /= smooth_len;
                            dir_world = smoothed_dir;
                        }

                        const float roi_span =
                            std::max(mask_info.roi_width, mask_info.roi_height);
                        const float beam_length = std::max(roi_span * 3.5f, 80.0f);
                        const float beam_width = std::max(roi_span * 0.75f, 25.0f);

                        ImVec2 apex_world(
                            static_cast<float>(center_world.first -
                                               dir_world.x * (roi_span * 0.15f)),
                            static_cast<float>(center_world.second -
                                               dir_world.y * (roi_span * 0.15f)));
                        ImVec2 base_center_world(
                            apex_world.x + dir_world.x * beam_length,
                            apex_world.y + dir_world.y * beam_length);

                        ImVec2 perp_world(-dir_world.y, dir_world.x);
                        const float perp_len = std::sqrt(
                            perp_world.x * perp_world.x +
                            perp_world.y * perp_world.y);
                        if (perp_len > 1e-3f) {
                            perp_world.x /= perp_len;
                            perp_world.y /= perp_len;
                        }

                        ImVec2 left_world(
                            base_center_world.x +
                                perp_world.x * (beam_width * 0.5f),
                            base_center_world.y +
                                perp_world.y * (beam_width * 0.5f));
                        ImVec2 right_world(
                            base_center_world.x -
                                perp_world.x * (beam_width * 0.5f),
                            base_center_world.y -
                                perp_world.y * (beam_width * 0.5f));

                        auto left_scene_pair =
                            worldToScene(left_world.x, left_world.y);
                        auto right_scene_pair =
                            worldToScene(right_world.x, right_world.y);
                        auto apex_scene_pair =
                            worldToScene(apex_world.x, apex_world.y);

                        ImVec2 tri_points[3];
                        tri_points[0] = ImPlot::PlotToPixels(
                            ImPlotPoint(left_scene_pair.first, left_scene_pair.second));
                        tri_points[1] = ImPlot::PlotToPixels(
                            ImPlotPoint(right_scene_pair.first, right_scene_pair.second));
                        tri_points[2] = ImPlot::PlotToPixels(
                            ImPlotPoint(apex_scene_pair.first, apex_scene_pair.second));

                        ImVec4 beam_color = base_color;
                        beam_color.w = 0.16f;
                        ImDrawList* beam_draw_list = ImPlot::GetPlotDrawList();
                        beam_draw_list->AddConvexPolyFilled(
                            tri_points, 3,
                            ImGui::ColorConvertFloat4ToU32(beam_color));
                    }
                }
            }
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
