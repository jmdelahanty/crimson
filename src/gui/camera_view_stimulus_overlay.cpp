#include "gui/camera_view_overlay_renderer.h"

#include "global.h"
#include "imgui.h"
#include "implot.h"
#include "stimulus_playback.h"
#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kStimulusInsetCloseFrameSlack = 1;

struct ChaserStateOverlay {
    int chaser_index = -1;
    double target_plot_x = 0.0;
    double target_plot_y = 0.0;
    size_t target_bbox_index = std::numeric_limits<size_t>::max();
    bool has_target = false;
    double chaser_plot_x = 0.0;
    double chaser_plot_y = 0.0;
    bool has_chaser = false;
    ImVec4 chaser_color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    std::vector<ImVec2> chaser_footprint_pixels;
    bool has_chaser_footprint = false;
    bool enable_chase = false;
    bool has_enable_chase = false;
    bool enable_random_movement = false;
    bool has_enable_random_movement = false;
};

ImVec4 rgbaToImVec4(const std::array<float, 4>& rgba) {
    return ImVec4(std::clamp(rgba[0], 0.0f, 1.0f),
                  std::clamp(rgba[1], 0.0f, 1.0f),
                  std::clamp(rgba[2], 0.0f, 1.0f),
                  std::clamp(rgba[3], 0.0f, 1.0f));
}

ImVec4 chaserColorForIndex(
    int32_t chaser_index,
    const std::unordered_map<int32_t, ImVec4>& colors_by_index) {
    auto it = colors_by_index.find(chaser_index);
    if (it != colors_by_index.end()) {
        return it->second;
    }
    return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
}

ImVec4 withAlpha(ImVec4 color, float alpha) {
    color.w = std::clamp(alpha, 0.0f, 1.0f);
    return color;
}

bool shouldDrawChaserTargetLine(const ChaserStateOverlay& overlay) {
    return !overlay.has_enable_chase || overlay.enable_chase;
}

char chaserBehaviorGlyph(const ChaserStateOverlay& overlay) {
    if (!overlay.has_enable_chase) {
        return '\0';
    }
    if (overlay.enable_chase) {
        return 'A';
    }
    if (!overlay.has_enable_random_movement) {
        return '\0';
    }
    return overlay.enable_random_movement ? 'N' : 'I';
}

void drawChaserBehaviorGlyph(double plot_x,
                             double plot_y,
                             char glyph) {
    if (glyph == '\0') {
        return;
    }

    char text[2] = {glyph, '\0'};
    const float font_size = ImGui::GetFontSize() * 0.78f;
    const ImVec2 text_size =
        ImGui::GetFont()->CalcTextSizeA(font_size,
                                        std::numeric_limits<float>::max(),
                                        0.0f,
                                        text);
    const ImVec2 center =
        ImPlot::PlotToPixels(ImPlotPoint(plot_x, plot_y));
    const ImVec2 text_pos(center.x - text_size.x * 0.5f,
                          center.y - text_size.y * 0.52f);
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    draw_list->AddText(ImGui::GetFont(),
                       font_size,
                       ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
                       IM_COL32(0, 0, 0, 230),
                       text);
    draw_list->AddText(ImGui::GetFont(),
                       font_size,
                       text_pos,
                       IM_COL32(255, 255, 255, 250),
                       text);
}

bool projectStimulusPointToCamera(const ZarrDetectionLoader::ChaserState& state,
                                  float stim_x,
                                  float stim_y,
                                  const CameraParams& camera_params,
                                  int image_width_px,
                                  int image_height_px,
                                  double& out_x,
                                  double& out_y) {
    if (!std::isfinite(stim_x) || !std::isfinite(stim_y)) {
        return false;
    }

    const float offset_x =
        state.has_stimulus_canvas_offset
            ? static_cast<float>(state.stimulus_canvas_offset_x)
            : camera_params.stimulus_offset_x;
    const float offset_y =
        state.has_stimulus_canvas_offset
            ? static_cast<float>(state.stimulus_canvas_offset_y)
            : camera_params.stimulus_offset_y;
    if (camera_params.has_valid_homography) {
        std::vector<cv::Point2f> src_points(1);
        std::vector<cv::Point2f> dst_points;
        src_points[0] = cv::Point2f(stim_x + offset_x, stim_y + offset_y);
        cv::perspectiveTransform(src_points,
                                 dst_points,
                                 camera_params.inverse_homography_matrix);
        if (dst_points.empty()) {
            return false;
        }
        out_x = dst_points[0].x;
        out_y = static_cast<double>(image_height_px) - dst_points[0].y;
        return true;
    }

    if (state.has_stimulus_canvas_offset) {
        return false;
    }

    constexpr double kProjectorExtent = 358.0;
    const double tex_x = stim_x + offset_x;
    const double tex_y = stim_y + offset_y;
    out_x = (tex_x / kProjectorExtent) * static_cast<double>(image_width_px);
    const double y =
        (tex_y / kProjectorExtent) * static_cast<double>(image_height_px);
    out_y = static_cast<double>(image_height_px) - y;
    return true;
}

bool projectStimulusCenterToCamera(const ZarrDetectionLoader::ChaserState& state,
                                   float stim_x,
                                   float stim_y,
                                   bool is_target,
                                   const CameraParams& camera_params,
                                   int image_width_px,
                                   int image_height_px,
                                   double& out_x,
                                   double& out_y) {
    if (state.has_camera_coords) {
        const double cx = is_target ? state.target_camera_x : state.chaser_camera_x;
        const double cy = is_target ? state.target_camera_y : state.chaser_camera_y;
        if (std::isfinite(cx) && std::isfinite(cy)) {
            out_x = cx;
            out_y = static_cast<double>(image_height_px) - cy;
            return true;
        }
    }

    return projectStimulusPointToCamera(state,
                                        stim_x,
                                        stim_y,
                                        camera_params,
                                        image_width_px,
                                        image_height_px,
                                        out_x,
                                        out_y);
}

bool buildProjectedChaserFootprint(
    const ZarrDetectionLoader::ChaserState& state,
    const CameraParams& camera_params,
    int image_width_px,
    int image_height_px,
    double& center_plot_x,
    double& center_plot_y,
    std::vector<ImVec2>& out_pixels) {
    if (!std::isfinite(state.chaser_radius_px) || state.chaser_radius_px <= 0.0f) {
        return false;
    }

    double center_x = 0.0;
    double center_y = 0.0;
    if (!projectStimulusPointToCamera(state,
                                      state.chaser_pos_x,
                                      state.chaser_pos_y,
                                      camera_params,
                                      image_width_px,
                                      image_height_px,
                                      center_x,
                                      center_y)) {
        return false;
    }

    constexpr int kSegments = 40;
    std::vector<ImVec2> pixels;
    pixels.reserve(kSegments);
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int i = 0; i < kSegments; ++i) {
        const double theta = kTwoPi * static_cast<double>(i) /
                             static_cast<double>(kSegments);
        const float stim_x =
            state.chaser_pos_x +
            std::cos(theta) * state.chaser_radius_px;
        const float stim_y =
            state.chaser_pos_y +
            std::sin(theta) * state.chaser_radius_px;
        double plot_x = 0.0;
        double plot_y = 0.0;
        if (!projectStimulusPointToCamera(state,
                                          stim_x,
                                          stim_y,
                                          camera_params,
                                          image_width_px,
                                          image_height_px,
                                          plot_x,
                                          plot_y)) {
            return false;
        }
        pixels.push_back(ImPlot::PlotToPixels(ImPlotPoint(plot_x, plot_y)));
    }

    center_plot_x = center_x;
    center_plot_y = center_y;
    out_pixels = std::move(pixels);
    return out_pixels.size() >= 3;
}

void drawProjectedChaserFootprint(const ChaserStateOverlay& overlay) {
    if (!overlay.has_chaser_footprint ||
        overlay.chaser_footprint_pixels.size() < 3) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec4 fill_color = withAlpha(
        overlay.chaser_color,
        std::clamp(overlay.chaser_color.w * 0.28f, 0.12f, 0.42f));
    const ImVec4 outline_color = withAlpha(
        overlay.chaser_color,
        std::clamp(overlay.chaser_color.w, 0.45f, 1.0f));
    draw_list->AddConvexPolyFilled(
        overlay.chaser_footprint_pixels.data(),
        static_cast<int>(overlay.chaser_footprint_pixels.size()),
        ImGui::ColorConvertFloat4ToU32(fill_color));
    draw_list->AddPolyline(
        overlay.chaser_footprint_pixels.data(),
        static_cast<int>(overlay.chaser_footprint_pixels.size()),
        ImGui::ColorConvertFloat4ToU32(outline_color),
        ImDrawFlags_Closed,
        2.0f);
}

size_t selectBoundingBoxForTarget(
    const std::vector<ZarrDetectionLoader::ChaserBoundingBox>& chaser_bboxes,
    double cam_x,
    double cam_y) {
    constexpr size_t kInvalidBBoxIndex = std::numeric_limits<size_t>::max();
    auto choose = [&](auto predicate) -> size_t {
        double best_distance = std::numeric_limits<double>::infinity();
        size_t best_index = kInvalidBBoxIndex;
        for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
            const auto& box = chaser_bboxes[idx];
            if (!predicate(box)) {
                continue;
            }
            if (!std::isfinite(box.centroid_x) || !std::isfinite(box.centroid_y)) {
                continue;
            }
            const double dx = cam_x - static_cast<double>(box.centroid_x);
            const double dy = cam_y - static_cast<double>(box.centroid_y);
            const double dist_sq = dx * dx + dy * dy;
            if (dist_sq < best_distance) {
                best_distance = dist_sq;
                best_index = idx;
            }
        }
        return best_index;
    };

    size_t idx = choose([](const auto& box) { return box.is_target; });
    if (idx != kInvalidBBoxIndex) {
        return idx;
    }
    idx = choose([](const auto& box) { return box.fish_id < 0; });
    if (idx != kInvalidBBoxIndex) {
        return idx;
    }
    return choose([](const auto&) { return true; });
}

}  // namespace

void drawCameraViewChaserOverlay(
    std::vector<ZarrDetectionLoader::ChaserBoundingBox> chaser_bboxes,
    const std::vector<ZarrDetectionLoader::ChaserState>& chaser_states,
    const CameraParams& camera_params,
    int image_width_px,
    int image_height_px) {
    constexpr size_t kInvalidBBoxIndex = std::numeric_limits<size_t>::max();
    std::vector<uint8_t> target_bbox_usage(chaser_bboxes.size(), 0);
    std::vector<ChaserStateOverlay> state_overlays;
    state_overlays.reserve(chaser_states.size());
    std::unordered_map<int32_t, ImVec4> chaser_colors_by_index;

    for (const auto& state : chaser_states) {
        ChaserStateOverlay overlay;
        overlay.chaser_index = state.chaser_index;
        overlay.enable_chase = state.enable_chase;
        overlay.has_enable_chase = state.has_enable_chase;
        overlay.enable_random_movement = state.enable_random_movement;
        overlay.has_enable_random_movement =
            state.has_enable_random_movement;
        if (state.has_chaser_rgba) {
            overlay.chaser_color = rgbaToImVec4(state.chaser_rgba);
            if (state.chaser_index >= 0) {
                chaser_colors_by_index[state.chaser_index] =
                    overlay.chaser_color;
            }
        }

        double target_plot_x = 0.0;
        double target_plot_y = 0.0;
        if (projectStimulusCenterToCamera(state,
                                          state.target_pos_x,
                                          state.target_pos_y,
                                          true,
                                          camera_params,
                                          image_width_px,
                                          image_height_px,
                                          target_plot_x,
                                          target_plot_y)) {
            overlay.has_target = true;
            const double target_cam_x = target_plot_x;
            const double target_cam_y =
                static_cast<double>(image_height_px) - target_plot_y;
            const size_t bbox_idx =
                chaser_bboxes.empty()
                    ? kInvalidBBoxIndex
                    : selectBoundingBoxForTarget(
                          chaser_bboxes, target_cam_x, target_cam_y);
            if (bbox_idx != kInvalidBBoxIndex) {
                auto& bbox = chaser_bboxes[bbox_idx];
                overlay.target_plot_x = static_cast<double>(bbox.centroid_x);
                overlay.target_plot_y =
                    static_cast<double>(image_height_px) -
                    static_cast<double>(bbox.centroid_y);
                overlay.target_bbox_index = bbox_idx;
                target_bbox_usage[bbox_idx] = 1;
                bbox.is_target = true;
                if (bbox.chaser_index < 0 && state.chaser_index >= 0) {
                    bbox.chaser_index = state.chaser_index;
                }
            } else {
                overlay.target_plot_x = target_plot_x;
                overlay.target_plot_y = target_plot_y;
            }
        }

        double chaser_plot_x = 0.0;
        double chaser_plot_y = 0.0;
        if (buildProjectedChaserFootprint(state,
                                          camera_params,
                                          image_width_px,
                                          image_height_px,
                                          chaser_plot_x,
                                          chaser_plot_y,
                                          overlay.chaser_footprint_pixels)) {
            overlay.has_chaser = true;
            overlay.has_chaser_footprint = true;
            overlay.chaser_plot_x = chaser_plot_x;
            overlay.chaser_plot_y = chaser_plot_y;
        } else if (projectStimulusCenterToCamera(state,
                                                state.chaser_pos_x,
                                                state.chaser_pos_y,
                                                false,
                                                camera_params,
                                                image_width_px,
                                                image_height_px,
                                                chaser_plot_x,
                                                chaser_plot_y)) {
            overlay.has_chaser = true;
            overlay.chaser_plot_x = chaser_plot_x;
            overlay.chaser_plot_y = chaser_plot_y;
        }

        state_overlays.push_back(overlay);
    }

    for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
        const auto& bbox = chaser_bboxes[idx];
        if (!std::isfinite(bbox.x_px) || !std::isfinite(bbox.y_px) ||
            !std::isfinite(bbox.width_px) || !std::isfinite(bbox.height_px)) {
            continue;
        }

        const bool highlight_target =
            bbox.is_target ||
            (idx < target_bbox_usage.size() && target_bbox_usage[idx] != 0);
        const ImVec4 chaser_color =
            chaserColorForIndex(bbox.chaser_index, chaser_colors_by_index);
        const ImVec4 box_color = highlight_target
                                     ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                                     : chaser_color;
        const float line_width = highlight_target ? 2.75f : 2.0f;
        const double x0 = static_cast<double>(bbox.x_px);
        const double x1 = static_cast<double>(bbox.x_px + bbox.width_px);
        const double y0 = static_cast<double>(bbox.y_px);
        const double y1 = static_cast<double>(bbox.y_px + bbox.height_px);
        const double x_coords[5] = {x0, x1, x1, x0, x0};
        const double y_coords[5] = {
            static_cast<double>(image_height_px) - y0,
            static_cast<double>(image_height_px) - y0,
            static_cast<double>(image_height_px) - y1,
            static_cast<double>(image_height_px) - y1,
            static_cast<double>(image_height_px) - y0};

        ImPlot::SetNextLineStyle(box_color, line_width);
        auto format_label_id = [&](int32_t candidate, size_t fallback) -> int32_t {
            if (candidate >= 0) {
                return candidate;
            }
            if (bbox.fish_id >= 0) {
                return bbox.fish_id;
            }
            return static_cast<int32_t>(fallback);
        };
        const int32_t label_id = format_label_id(bbox.chaser_index, idx);
        const std::string label =
            highlight_target
                ? "Target BBox " + std::to_string(label_id) +
                      "##target_bbox_" + std::to_string(idx)
                : "Chaser BBox " + std::to_string(label_id) +
                      "##chaser_bbox_" + std::to_string(idx);
        ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);

        if (!highlight_target && std::isfinite(bbox.centroid_x) &&
            std::isfinite(bbox.centroid_y)) {
            const double centroid_x = static_cast<double>(bbox.centroid_x);
            const double centroid_y = static_cast<double>(image_height_px) -
                                      static_cast<double>(bbox.centroid_y);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                       5.0f,
                                       chaser_color,
                                       IMPLOT_AUTO,
                                       chaser_color);
            const std::string centroid_label =
                "Chaser BBox " + std::to_string(label_id) +
                " Centroid##chaser_centroid_" + std::to_string(idx);
            ImPlot::PlotScatter(centroid_label.c_str(), &centroid_x, &centroid_y, 1);
        }
    }

    for (const auto& overlay : state_overlays) {
        if (overlay.has_target) {
            const double plot_x = overlay.target_plot_x;
            const double plot_y = overlay.target_plot_y;
            const ImVec4 target_color(0.0f, 1.0f, 0.0f, 1.0f);
            const float outline =
                overlay.target_bbox_index != kInvalidBBoxIndex ? 2.5f : 2.0f;
            const float target_outline = outline * 0.25f;
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                       3.0f,
                                       target_color,
                                       target_outline,
                                       target_color);
            const std::string target_label =
                "Target_" + std::to_string(overlay.chaser_index);
            ImPlot::PlotScatter(target_label.c_str(), &plot_x, &plot_y, 1);
        }

        if (overlay.has_chaser && overlay.has_target &&
            shouldDrawChaserTargetLine(overlay)) {
            const double line_x[2] = {overlay.chaser_plot_x,
                                      overlay.target_plot_x};
            const double line_y[2] = {overlay.chaser_plot_y,
                                      overlay.target_plot_y};
            const double dx = overlay.target_plot_x - overlay.chaser_plot_x;
            const double dy = overlay.target_plot_y - overlay.chaser_plot_y;
            const double dist = std::sqrt(dx * dx + dy * dy);
            const double max_dim =
                static_cast<double>(std::max(image_width_px, image_height_px));
            const double max_dist =
                (max_dim > 0.0) ? (max_dim * (2.0 / 3.0)) : 200.0;
            const double t = std::clamp(dist / max_dist, 0.0, 1.0);
            const ImVec4 close_color(1.0f, 0.15f, 0.1f, 1.0f);
            const ImVec4 far_color(0.15f, 0.9f, 0.2f, 1.0f);
            const ImVec4 line_color(
                close_color.x * static_cast<float>(1.0 - t) +
                    far_color.x * static_cast<float>(t),
                close_color.y * static_cast<float>(1.0 - t) +
                    far_color.y * static_cast<float>(t),
                close_color.z * static_cast<float>(1.0 - t) +
                    far_color.z * static_cast<float>(t),
                1.0f);
            ImPlot::SetNextLineStyle(line_color, 2.5f);
            const std::string line_label =
                "ChaserTargetLine##" +
                std::to_string(reinterpret_cast<uintptr_t>(&overlay));
            ImPlot::PlotLine(line_label.c_str(), line_x, line_y, 2);
        }

        if (overlay.has_chaser) {
            const double plot_x = overlay.chaser_plot_x;
            const double plot_y = overlay.chaser_plot_y;
            if (overlay.has_chaser_footprint) {
                drawProjectedChaserFootprint(overlay);
            } else {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                           8.0f,
                                           overlay.chaser_color,
                                           2.0f,
                                           overlay.chaser_color);
                const std::string chaser_label =
                    "Chaser_state_" + std::to_string(overlay.chaser_index);
                ImPlot::PlotScatter(chaser_label.c_str(), &plot_x, &plot_y, 1);
            }
            drawChaserBehaviorGlyph(plot_x,
                                     plot_y,
                                     chaserBehaviorGlyph(overlay));
        }
    }
}

void drawCameraViewStimulusCameraOverlay(
    const crimson::stimulus::StimulusCameraOverlayScene& scene,
    double display_origin_x,
    double display_origin_y) {
    if (!scene.ready()) {
        return;
    }
    const ImVec2 plot_pos(static_cast<float>(display_origin_x),
                          static_cast<float>(display_origin_y));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(
        plot_pos,
        ImVec2(plot_pos.x + static_cast<float>(scene.viewport.width_px),
               plot_pos.y + static_cast<float>(scene.viewport.height_px)),
        true);
    auto point = [&](const auto& value) {
        return ImVec2(plot_pos.x + static_cast<float>(value.x),
                      plot_pos.y + static_cast<float>(value.y));
    };
    auto color = [](const auto& value) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(
            static_cast<float>(value.red), static_cast<float>(value.green),
            static_cast<float>(value.blue), static_cast<float>(value.alpha)));
    };

    using Layer = crimson::stimulus::StimulusCameraOverlaySceneLayer;
    constexpr std::array<Layer, 5> kLayerOrder = {
        Layer::EventPanel, Layer::EventText, Layer::StepPanel,
        Layer::StepText, Layer::StepArrow};
    for (const Layer layer : kLayerOrder) {
        for (const auto& primitive : scene.primitives) {
            if (primitive.layer != layer) {
                continue;
            }
            using Type =
                crimson::stimulus::StimulusCameraOverlayPrimitiveType;
            switch (primitive.type) {
            case Type::RoundedRectangle:
                if (primitive.has_fill) {
                    draw_list->AddRectFilled(
                        point(primitive.first), point(primitive.second),
                        color(primitive.fill),
                        static_cast<float>(primitive.corner_radius_px));
                }
                if (primitive.has_stroke) {
                    draw_list->AddRect(
                        point(primitive.first), point(primitive.second),
                        color(primitive.stroke),
                        static_cast<float>(primitive.corner_radius_px), 0,
                        static_cast<float>(primitive.stroke_width_px));
                }
                break;
            case Type::Line:
                if (primitive.has_stroke) {
                    draw_list->AddLine(
                        point(primitive.first), point(primitive.second),
                        color(primitive.stroke),
                        static_cast<float>(primitive.stroke_width_px));
                }
                break;
            case Type::Triangle:
                if (primitive.has_fill) {
                    draw_list->AddTriangleFilled(
                        point(primitive.first), point(primitive.second),
                        point(primitive.third), color(primitive.fill));
                }
                break;
            }
        }
        for (const auto& text : scene.text) {
            if (text.layer == layer) {
                draw_list->AddText(point(text.anchor), color(text.color),
                                   text.content.c_str());
            }
        }
    }
    draw_list->PopClipRect();
}

void drawCameraViewStimulusInsetOverlay(
    const StimulusPlayback* stimulus_player,
    int target_stimulus_frame,
    const CameraViewStimulusInsetOptions& options) {
    if (!options.show_inset || stimulus_player == nullptr ||
        !stimulus_player->loaded || target_stimulus_frame < 0) {
        return;
    }

    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x < 120.0f || plot_size.y < 100.0f) {
        return;
    }

    const float max_width = std::min(plot_size.x - 24.0f, plot_size.x * 0.36f);
    const float inset_width =
        std::clamp(options.width_px, 96.0f, std::max(96.0f, max_width));
    const float aspect =
        (stimulus_player->width > 0 && stimulus_player->height > 0)
            ? static_cast<float>(stimulus_player->height) /
                  static_cast<float>(stimulus_player->width)
            : 1.0f;
    const float image_height =
        std::clamp(inset_width * aspect, 64.0f, plot_size.y * 0.42f);
    const float label_height = options.show_frame_label ? 20.0f : 0.0f;
    constexpr float kPad = 7.0f;
    const ImVec2 box_min(plot_pos.x + 12.0f,
                         plot_pos.y + plot_size.y -
                             (image_height + label_height + kPad * 2.0f) -
                             12.0f);
    const ImVec2 box_max(box_min.x + inset_width + kPad * 2.0f,
                         box_min.y + image_height + label_height +
                             kPad * 2.0f);

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    draw_list->AddRectFilled(box_min, box_max, IM_COL32(4, 8, 14, 190), 6.0f);
    draw_list->AddRect(box_min, box_max, IM_COL32(125, 185, 255, 190), 6.0f);

    const ImVec2 image_min(box_min.x + kPad, box_min.y + kPad);
    const ImVec2 image_max(image_min.x + inset_width,
                           image_min.y + image_height);
    const bool has_uploaded_frame =
        stimulus_player->texture != 0 && stimulus_player->last_displayed_frame >= 0;
    if (has_uploaded_frame) {
        const int alpha = static_cast<int>(
            std::clamp(options.opacity, 0.15f, 1.0f) * 255.0f);
        draw_list->AddImage(
            (ImTextureID)(intptr_t)stimulus_player->texture,
            image_min,
            image_max,
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 1.0f),
            IM_COL32(255, 255, 255, alpha));
    } else {
        draw_list->AddRectFilled(image_min, image_max, IM_COL32(12, 18, 28, 210), 3.0f);
        draw_list->AddText(ImVec2(image_min.x + 8.0f, image_min.y + 8.0f),
                           IM_COL32(205, 220, 240, 235),
                           "Waiting for stimulus frame");
    }

    if (options.show_frame_label) {
        std::ostringstream label;
        label << "Stimulus";
        if (stimulus_player->last_displayed_frame >= 0) {
            const int delta =
                stimulus_player->last_displayed_frame - target_stimulus_frame;
            label << " " << stimulus_player->last_displayed_frame
                  << " / target " << target_stimulus_frame;
            if (std::abs(delta) > kStimulusInsetCloseFrameSlack) {
                label << " (" << (delta > 0 ? "+" : "") << delta << ")";
            }
        } else {
            label << " target " << target_stimulus_frame;
        }
        const std::string label_text = label.str();
        draw_list->AddText(ImVec2(box_min.x + kPad, image_max.y + 4.0f),
                           IM_COL32(225, 238, 255, 245),
                           label_text.c_str());
    }
}

void drawCameraViewChaserDistancePolarInsetOverlay(
    const crimson::polar::ChaserDistancePolarScene& scene) {
    if (!scene.ready()) {
        return;
    }

    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    auto point = [&](crimson::polar::ChaserDistancePolarScenePoint value) {
        return ImVec2(plot_pos.x + static_cast<float>(value.x),
                      plot_pos.y + static_cast<float>(value.y));
    };
    auto color = [](const crimson::polar::ChaserDistancePolarRgba& value) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(
            static_cast<float>(std::clamp(value.red, 0.0, 1.0)),
            static_cast<float>(std::clamp(value.green, 0.0, 1.0)),
            static_cast<float>(std::clamp(value.blue, 0.0, 1.0)),
            static_cast<float>(std::clamp(value.alpha, 0.0, 1.0))));
    };
    for (const auto& primitive : scene.primitives) {
        switch (primitive.type) {
            case crimson::polar::ChaserDistancePolarScenePrimitiveType::
                RoundedRectangle:
                if (primitive.has_fill) {
                    draw_list->AddRectFilled(
                        point(primitive.first), point(primitive.second),
                        color(primitive.fill),
                        static_cast<float>(primitive.corner_radius_px));
                }
                if (primitive.has_stroke) {
                    draw_list->AddRect(
                        point(primitive.first), point(primitive.second),
                        color(primitive.stroke),
                        static_cast<float>(primitive.corner_radius_px), 0,
                        static_cast<float>(primitive.stroke_width_px));
                }
                break;
            case crimson::polar::ChaserDistancePolarScenePrimitiveType::Circle:
            case crimson::polar::ChaserDistancePolarScenePrimitiveType::Marker:
                if (primitive.has_fill) {
                    draw_list->AddCircleFilled(
                        point(primitive.first),
                        static_cast<float>(primitive.radius_px),
                        color(primitive.fill),
                        static_cast<int>(primitive.segment_count));
                }
                if (primitive.has_stroke) {
                    draw_list->AddCircle(
                        point(primitive.first),
                        static_cast<float>(primitive.radius_px),
                        color(primitive.stroke),
                        static_cast<int>(primitive.segment_count),
                        static_cast<float>(primitive.stroke_width_px));
                }
                break;
            case crimson::polar::ChaserDistancePolarScenePrimitiveType::Line:
                if (primitive.has_stroke) {
                    draw_list->AddLine(
                        point(primitive.first), point(primitive.second),
                        color(primitive.stroke),
                        static_cast<float>(primitive.stroke_width_px));
                }
                break;
        }
    }
    for (const auto& annotation : scene.text) {
        ImVec2 anchor = point(annotation.anchor);
        if (annotation.centered) {
            const ImVec2 text_size =
                ImGui::CalcTextSize(annotation.content.c_str());
            anchor.x -= text_size.x * 0.5f;
            anchor.y -= text_size.y * 0.5f;
        }
        draw_list->AddText(anchor, color(annotation.color),
                           annotation.content.c_str());
    }
}
