#include "gui/camera_view_overlay_renderer.h"
#include "gui/camera_view_overlay_style.h"

#include "imgui.h"
#include "implot.h"
#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using camera_view_overlay::isEyeMaskComponent;
using camera_view_overlay::shortSubjectMaskLabel;
using camera_view_overlay::subjectMaskComponentColor;
using camera_view_overlay::subjectMaskContourColor;

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

struct EyeMaskTextureCacheEntry {
    std::string source_key;
    int32_t roi_index = -1;
    std::string layer_key;
    int rows = 0;
    int cols = 0;
    size_t pixel_count = 0;
    GLuint texture_id = 0;
    uint64_t last_used = 0;
};

class EyeMaskTextureCache {
public:
    GLuint getOrCreatePixels(const std::string& source_key,
                             int32_t roi_index,
                             const std::string& layer_key,
                             int rows,
                             int cols,
                             const std::vector<uint32_t>& pixels,
                             const ImVec4& color,
                             CameraViewMaskPerfMetrics* perf = nullptr) {
        if (rows <= 0 || cols <= 0 || roi_index < 0 || pixels.empty()) {
            return 0;
        }

        ++clock_;
        const auto lookup_start = std::chrono::steady_clock::now();
        for (auto& entry : entries_) {
            if (entry.source_key == source_key &&
                entry.roi_index == roi_index &&
                entry.layer_key == layer_key &&
                entry.rows == rows &&
                entry.cols == cols &&
                entry.pixel_count == pixels.size()) {
                entry.last_used = clock_;
                if (perf != nullptr) {
                    perf->texture_cache_hits++;
                    perf->texture_lookup_ms += durationMs(
                        std::chrono::steady_clock::now() - lookup_start);
                }
                return entry.texture_id;
            }
        }
        if (perf != nullptr) {
            perf->texture_cache_misses++;
            perf->texture_lookup_ms += durationMs(
                std::chrono::steady_clock::now() - lookup_start);
        }

        const auto upload_start = std::chrono::steady_clock::now();
        GLuint texture_id = createTexture(rows, cols, pixels, color);
        if (perf != nullptr) {
            perf->texture_upload_ms += durationMs(
                std::chrono::steady_clock::now() - upload_start);
        }
        if (texture_id == 0) {
            return 0;
        }
        if (perf != nullptr) {
            perf->texture_uploads++;
        }

        evictIfNeeded();
        EyeMaskTextureCacheEntry entry;
        entry.source_key = source_key;
        entry.roi_index = roi_index;
        entry.layer_key = layer_key;
        entry.rows = rows;
        entry.cols = cols;
        entry.pixel_count = pixels.size();
        entry.texture_id = texture_id;
        entry.last_used = clock_;
        entries_.push_back(std::move(entry));
        return texture_id;
    }

    GLuint getOrCreate(const std::string& source_key,
                       const ZarrDetectionLoader::FrameDetections::EyeMask& mask,
                       int eye_index,
                       const ImVec4& color,
                       CameraViewMaskPerfMetrics* perf = nullptr) {
        if (eye_index < 0 ||
            eye_index >= static_cast<int>(mask.pixel_indices.size()) ||
            mask.rows <= 0 || mask.cols <= 0 || mask.roi_index < 0) {
            return 0;
        }

        const auto& pixels = mask.pixel_indices[eye_index];
        if (pixels.empty()) {
            return 0;
        }

        return getOrCreatePixels(source_key,
                                 mask.roi_index,
                                 "eye:" + std::to_string(eye_index),
                                 mask.rows,
                                 mask.cols,
                                 pixels,
                                 color,
                                 perf);
    }

    void resetIfSourceChanged(const std::string& source_key) {
        if (source_key == active_source_key_) {
            return;
        }
        clear();
        active_source_key_ = source_key;
    }

private:
    static constexpr size_t kCapacity = 64;

    static uint8_t colorComponent(float value) {
        return static_cast<uint8_t>(
            std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    GLuint createTexture(int rows,
                         int cols,
                         const std::vector<uint32_t>& pixels,
                         const ImVec4& color) {
        const size_t texel_count =
            static_cast<size_t>(rows) * static_cast<size_t>(cols);
        if (texel_count == 0) {
            return 0;
        }

        std::vector<uint8_t> rgba(texel_count * 4, 0);
        const uint8_t r = colorComponent(color.x);
        const uint8_t g = colorComponent(color.y);
        const uint8_t b = colorComponent(color.z);
        const uint8_t a = colorComponent(color.w);
        for (uint32_t linear : pixels) {
            if (linear >= texel_count) {
                continue;
            }
            const size_t base = static_cast<size_t>(linear) * 4;
            rgba[base + 0] = r;
            rgba[base + 1] = g;
            rgba[base + 2] = b;
            rgba[base + 3] = a;
        }

        GLint previous_texture = 0;
        GLint previous_unpack_alignment = 4;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);

        GLuint texture_id = 0;
        glGenTextures(1, &texture_id);
        glBindTexture(GL_TEXTURE_2D, texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     cols,
                     rows,
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     rgba.data());
        const GLenum error = glGetError();

        glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));

        if (error != GL_NO_ERROR) {
            if (texture_id != 0) {
                glDeleteTextures(1, &texture_id);
            }
            return 0;
        }
        return texture_id;
    }

    void evictIfNeeded() {
        while (entries_.size() >= kCapacity) {
            auto oldest = std::min_element(
                entries_.begin(),
                entries_.end(),
                [](const EyeMaskTextureCacheEntry& lhs,
                   const EyeMaskTextureCacheEntry& rhs) {
                    return lhs.last_used < rhs.last_used;
                });
            if (oldest == entries_.end()) {
                return;
            }
            if (oldest->texture_id != 0) {
                glDeleteTextures(1, &oldest->texture_id);
            }
            entries_.erase(oldest);
        }
    }

    void clear() {
        for (auto& entry : entries_) {
            if (entry.texture_id != 0) {
                glDeleteTextures(1, &entry.texture_id);
                entry.texture_id = 0;
            }
        }
        entries_.clear();
    }

    std::vector<EyeMaskTextureCacheEntry> entries_;
    std::string active_source_key_;
    uint64_t clock_ = 0;
};

EyeMaskTextureCache& eyeMaskTextureCache() {
    static EyeMaskTextureCache cache;
    return cache;
}

bool shouldDrawSubjectMaskComponent(
    const std::string& label,
    const CameraViewMaskOverlayOptions& options) {
    if (label == "subject_body") {
        return options.show_subject_body;
    }
    if (label == "eye_left") {
        return options.show_eye_left;
    }
    if (label == "eye_right") {
        return options.show_eye_right;
    }
    if (label == "swim_bladder") {
        return options.show_swim_bladder;
    }
    return true;
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

void drawPlotTextBox(const char* text,
                     double plot_x,
                     double plot_y,
                     const ImVec4& accent_color,
                     float font_scale = 1.25f) {
    if (text == nullptr || text[0] == '\0' || !std::isfinite(plot_x) ||
        !std::isfinite(plot_y)) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 center =
        ImPlot::PlotToPixels(ImPlotPoint(plot_x, plot_y));
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

}  // namespace

const char* cameraViewMaskOverlayModeLabel(CameraViewMaskOverlayMode mode) {
    switch (mode) {
    case CameraViewMaskOverlayMode::Realtime:
        return "Realtime";
    case CameraViewMaskOverlayMode::Review:
        return "Review";
    case CameraViewMaskOverlayMode::Debug:
        return "Debug";
    }
    return "Review";
}

void accumulateCameraViewMaskPerfMetrics(CameraViewMaskPerfMetrics& dst,
                                         const CameraViewMaskPerfMetrics& src) {
    dst.attempted = dst.attempted || src.attempted;
    if (dst.mode.empty()) {
        dst.mode = src.mode;
    }
    dst.roi_count += src.roi_count;
    dst.visible_roi_count += src.visible_roi_count;
    dst.component_fill_count += src.component_fill_count;
    dst.fallback_scatter_count += src.fallback_scatter_count;
    dst.texture_cache_hits += src.texture_cache_hits;
    dst.texture_cache_misses += src.texture_cache_misses;
    dst.texture_uploads += src.texture_uploads;
    dst.contours_drawn += src.contours_drawn;
    dst.selected_contours_drawn += src.selected_contours_drawn;
    dst.contour_points += src.contour_points;
    dst.axes_drawn += src.axes_drawn;
    dst.visual_cones_drawn += src.visual_cones_drawn;
    dst.visual_cone_overlaps_drawn += src.visual_cone_overlaps_drawn;
    dst.gaze_rays_drawn += src.gaze_rays_drawn;
    dst.angle_labels_drawn += src.angle_labels_drawn;
    dst.selected_highlight_drawn =
        dst.selected_highlight_drawn || src.selected_highlight_drawn;
    dst.pick_attempted = dst.pick_attempted || src.pick_attempted;
    dst.pick_hit = dst.pick_hit || src.pick_hit;
    dst.texture_lookup_ms += src.texture_lookup_ms;
    dst.texture_upload_ms += src.texture_upload_ms;
    dst.fill_draw_ms += src.fill_draw_ms;
    dst.contour_build_ms += src.contour_build_ms;
    dst.contour_draw_ms += src.contour_draw_ms;
    dst.axis_draw_ms += src.axis_draw_ms;
    dst.pick_ms += src.pick_ms;
    dst.total_draw_ms += src.total_draw_ms;
}

CameraViewMaskPerfMetrics drawCameraViewEyeMaskOverlay(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    const ZarrDetectionLoader::FrameDetections* subject_shape_details,
    float image_height_px,
    const std::string& smoothing_run_id,
    const CameraViewMaskOverlayOptions& options) {
    CameraViewMaskPerfMetrics metrics;
    metrics.attempted = true;
    metrics.mode = cameraViewMaskOverlayModeLabel(options.mode);
    const auto total_start = std::chrono::steady_clock::now();

    auto finish = [&]() -> CameraViewMaskPerfMetrics {
        metrics.total_draw_ms +=
            durationMs(std::chrono::steady_clock::now() - total_start);
        return metrics;
    };

    eyeOrientationSmoother().resetIfRunChanged(smoothing_run_id);
    eyeMaskTextureCache().resetIfSourceChanged(smoothing_run_id);
    if (!mask_details.includes_eye_masks) {
        return finish();
    }

    const size_t mask_count =
        std::min(mask_details.eye_masks.size(), mask_details.boxes.size());
    metrics.roi_count = static_cast<int>(mask_count);
    if (mask_count == 0) {
        return finish();
    }

    const bool realtime_mode =
        options.mode == CameraViewMaskOverlayMode::Realtime;
    const bool draw_all_contours = !realtime_mode;
    const bool draw_axes_and_angles = !realtime_mode;
    const float scene_height_f = image_height_px;
    auto findSubjectShapeForMask =
        [&](size_t det_idx,
            int32_t roi_index)
            -> const ZarrDetectionLoader::FrameDetections::SubjectShape* {
        if (subject_shape_details == nullptr ||
            !subject_shape_details->includes_subject_shapes ||
            subject_shape_details->subject_shapes.empty() || roi_index < 0) {
            return nullptr;
        }
        if (det_idx < subject_shape_details->subject_shapes.size()) {
            const auto& shape = subject_shape_details->subject_shapes[det_idx];
            if (shape.valid && shape.roi_index == roi_index) {
                return &shape;
            }
        }
        auto it = std::find_if(
            subject_shape_details->subject_shapes.begin(),
            subject_shape_details->subject_shapes.end(),
            [&](const ZarrDetectionLoader::FrameDetections::SubjectShape&
                    shape) {
                return shape.valid && shape.roi_index == roi_index;
            });
        return it == subject_shape_details->subject_shapes.end() ? nullptr
                                                                 : &*it;
    };
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
        metrics.visible_roi_count++;

        const double cell_w =
            mask_info.roi_width / static_cast<double>(mask_info.cols);
        const double cell_h =
            mask_info.roi_height / static_cast<double>(mask_info.rows);

        auto draw_component_contour =
            [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                    SubjectMaskComponent& component,
                const std::string& label,
                float thickness,
                const ImVec4& color,
                bool respect_visibility) {
                if (!component.has_contour ||
                    component.contour_xy.size() < 2) {
                    return;
                }
                if (respect_visibility &&
                    !shouldDrawSubjectMaskComponent(component.label, options)) {
                    return;
                }

                const auto build_start = std::chrono::steady_clock::now();
                std::vector<double> xs;
                std::vector<double> ys;
                xs.reserve(component.contour_xy.size() + 1);
                ys.reserve(component.contour_xy.size() + 1);
                for (const auto& point : component.contour_xy) {
                    const double px = mask_info.offset_x +
                                      static_cast<double>(point[0]) * cell_w;
                    const double py = mask_info.offset_y +
                                      static_cast<double>(point[1]) * cell_h;
                    xs.push_back(px);
                    ys.push_back(scene_height_f - py);
                }
                const auto& first = component.contour_xy.front();
                const auto& last = component.contour_xy.back();
                if (component.contour_xy.size() > 2 &&
                    (std::fabs(first[0] - last[0]) > 1e-5f ||
                     std::fabs(first[1] - last[1]) > 1e-5f)) {
                    const double px = mask_info.offset_x +
                                      static_cast<double>(first[0]) * cell_w;
                    const double py = mask_info.offset_y +
                                      static_cast<double>(first[1]) * cell_h;
                    xs.push_back(px);
                    ys.push_back(scene_height_f - py);
                }
                metrics.contour_build_ms +=
                    durationMs(std::chrono::steady_clock::now() - build_start);

                const auto draw_start = std::chrono::steady_clock::now();
                ImPlot::SetNextLineStyle(color, thickness);
                ImPlot::PlotLine(label.c_str(),
                                 xs.data(),
                                 ys.data(),
                                 static_cast<int>(xs.size()));
                metrics.contour_draw_ms +=
                    durationMs(std::chrono::steady_clock::now() - draw_start);
                metrics.contours_drawn++;
                metrics.contour_points += static_cast<int>(xs.size());
            };
        auto draw_roi_selection_border =
            [&](const std::string& label,
                const ImVec4& color,
                float thickness) {
                const double x_min = mask_info.offset_x;
                const double x_max = mask_info.offset_x + mask_info.roi_width;
                const double y_top = scene_height_f - mask_info.offset_y;
                const double y_bottom =
                    scene_height_f - (mask_info.offset_y + mask_info.roi_height);
                const double xs[5] = {x_min, x_max, x_max, x_min, x_min};
                const double ys[5] = {y_top, y_top, y_bottom, y_bottom, y_top};
                ImPlot::SetNextLineStyle(color, thickness);
                ImPlot::PlotLine(label.c_str(), xs, ys, 5);
            };

        for (const auto& component : mask_info.subject_mask_components) {
            if (!component.valid || component.pixel_indices.empty() ||
                isEyeMaskComponent(component.label) ||
                !shouldDrawSubjectMaskComponent(component.label, options)) {
                continue;
            }

            const ImVec4 component_color =
                subjectMaskComponentColor(component.label);
            const std::string layer_key =
                "component:" + component.label + ":" +
                std::to_string(component.channel_index);
            const std::string base_id =
                "##subject_mask_" + component.label + "_" +
                std::to_string(det_idx);

            bool drew_texture = false;
            GLuint texture_id = eyeMaskTextureCache().getOrCreatePixels(
                smoothing_run_id,
                mask_info.roi_index,
                layer_key,
                mask_info.rows,
                mask_info.cols,
                component.pixel_indices,
                component_color,
                &metrics);
            if (texture_id != 0) {
                const double x_min = mask_info.offset_x;
                const double x_max = mask_info.offset_x + mask_info.roi_width;
                const double y_min =
                    scene_height_f - (mask_info.offset_y + mask_info.roi_height);
                const double y_max = scene_height_f - mask_info.offset_y;
                const auto fill_draw_start = std::chrono::steady_clock::now();
                ImPlot::PlotImage(
                    (base_id + "_texture").c_str(),
                    (ImTextureID)(intptr_t)texture_id,
                    ImPlotPoint(x_min, y_min),
                    ImPlotPoint(x_max, y_max),
                    ImVec2(0, 0),
                    ImVec2(1, 1),
                    ImVec4(1, 1, 1, 1));
                metrics.fill_draw_ms += durationMs(
                    std::chrono::steady_clock::now() - fill_draw_start);
                drew_texture = true;
            }

            if (!drew_texture) {
                const auto fill_draw_start = std::chrono::steady_clock::now();
                std::vector<double> xs;
                std::vector<double> ys;
                xs.reserve(component.pixel_indices.size());
                ys.reserve(component.pixel_indices.size());
                for (uint32_t linear : component.pixel_indices) {
                    const uint32_t row =
                        linear / static_cast<uint32_t>(mask_info.cols);
                    const uint32_t col =
                        linear % static_cast<uint32_t>(mask_info.cols);
                    const double px = mask_info.offset_x +
                                      (static_cast<double>(col) + 0.5) * cell_w;
                    const double py = mask_info.offset_y +
                                      (static_cast<double>(row) + 0.5) * cell_h;
                    xs.push_back(px);
                    ys.push_back(scene_height_f - py);
                }
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                           2.0f,
                                           component_color,
                                           1.0f,
                                           component_color);
                ImPlot::PlotScatter((base_id + "_pts").c_str(),
                                    xs.data(),
                                    ys.data(),
                                    static_cast<int>(xs.size()));
                metrics.fill_draw_ms += durationMs(
                    std::chrono::steady_clock::now() - fill_draw_start);
                metrics.fallback_scatter_count++;
            }
            metrics.component_fill_count++;
        }

        if (draw_all_contours) {
            for (const auto& component : mask_info.subject_mask_components) {
                if (isEyeMaskComponent(component.label)) {
                    continue;
                }
                draw_component_contour(
                    component,
                    "##subject_mask_" + component.label + "_" +
                        std::to_string(det_idx) + "_contour",
                    component.label == "subject_body" ? 1.4f : 1.8f,
                    subjectMaskContourColor(component.label),
                    true);
            }
        }

        std::array<std::vector<ImVec2>, 2> visual_cone_polygons;
        for (int eye = 0; eye < 2; ++eye) {
            if ((eye == 0 && !options.show_eye_left) ||
                (eye == 1 && !options.show_eye_right)) {
                continue;
            }
            const auto& pixel_indices = mask_info.pixel_indices[eye];
            const bool has_pixels = !pixel_indices.empty();
            const std::string base_id =
                (eye == 0) ? "##eye_mask_left_" + std::to_string(det_idx)
                           : "##eye_mask_right_" + std::to_string(det_idx);
            ImVec4 base_color = (eye == 0)
                                    ? ImVec4(0.2f, 0.6f, 1.0f, 0.35f)
                                    : ImVec4(1.0f, 0.3f, 0.6f, 0.35f);

            bool drew_texture = false;
            if (has_pixels) {
                GLuint texture_id = eyeMaskTextureCache().getOrCreate(
                    smoothing_run_id,
                    mask_info,
                    eye,
                    base_color,
                    &metrics);
                if (texture_id != 0) {
                    const double x_min = mask_info.offset_x;
                    const double x_max = mask_info.offset_x + mask_info.roi_width;
                    const double y_min =
                        scene_height_f - (mask_info.offset_y + mask_info.roi_height);
                    const double y_max = scene_height_f - mask_info.offset_y;
                    const auto fill_draw_start = std::chrono::steady_clock::now();
                    ImPlot::PlotImage(
                        (base_id + "_texture").c_str(),
                        (ImTextureID)(intptr_t)texture_id,
                        ImPlotPoint(x_min, y_min),
                        ImPlotPoint(x_max, y_max),
                        ImVec2(0, 0),
                        ImVec2(1, 1),
                        ImVec4(1, 1, 1, 1));
                    metrics.fill_draw_ms += durationMs(
                        std::chrono::steady_clock::now() - fill_draw_start);
                    drew_texture = true;
                }

                if (!drew_texture) {
                    const auto fill_draw_start = std::chrono::steady_clock::now();
                    std::vector<double> xs;
                    std::vector<double> ys;
                    xs.reserve(pixel_indices.size());
                    ys.reserve(pixel_indices.size());
                    for (uint32_t linear : pixel_indices) {
                        const uint32_t row =
                            linear / static_cast<uint32_t>(mask_info.cols);
                        const uint32_t col =
                            linear % static_cast<uint32_t>(mask_info.cols);
                        const double px = mask_info.offset_x +
                                          (static_cast<double>(col) + 0.5) * cell_w;
                        const double py = mask_info.offset_y +
                                          (static_cast<double>(row) + 0.5) * cell_h;
                        xs.push_back(px);
                        ys.push_back(scene_height_f - py);
                    }
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                               3.0f,
                                               base_color,
                                               1.0f,
                                               base_color);
                    ImPlot::PlotScatter((base_id + "_pts").c_str(),
                                        xs.data(),
                                        ys.data(),
                                        static_cast<int>(xs.size()));
                    metrics.fill_draw_ms += durationMs(
                        std::chrono::steady_clock::now() - fill_draw_start);
                    metrics.fallback_scatter_count++;
                }
                metrics.component_fill_count++;
            }

            const std::string eye_label = (eye == 0) ? "eye_left" : "eye_right";
            auto contour_component = std::find_if(
                mask_info.subject_mask_components.begin(),
                mask_info.subject_mask_components.end(),
                [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                        SubjectMaskComponent& component) {
                    return component.label == eye_label;
                });
            if (draw_all_contours &&
                contour_component != mask_info.subject_mask_components.end()) {
                draw_component_contour(
                    *contour_component,
                    base_id + "_contour",
                    1.6f,
                    subjectMaskContourColor(contour_component->label),
                    true);
            }

            if (draw_axes_and_angles && mask_info.has_feret_axes &&
                cell_w > 0.0 && cell_h > 0.0) {
                const auto axis_start = std::chrono::steady_clock::now();
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
                auto draw_eye_angle_arc =
                    [&](const ZarrDetectionLoader::FrameDetections::
                            SubjectShape& shape,
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

                    auto normalize = [](double x,
                                        double y) -> std::array<double, 2> {
                        const double len = std::sqrt(x * x + y * y);
                        if (len <= 1e-6 || !std::isfinite(len)) {
                            return {std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::quiet_NaN()};
                        }
                        return {x / len, y / len};
                    };
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
                    if (!std::isfinite(forward[0]) ||
                        !std::isfinite(forward[1]) ||
                        !std::isfinite(left[0]) || !std::isfinite(left[1])) {
                        return false;
                    }

                    const double roi_span = std::max(
                        static_cast<double>(mask_info.roi_width),
                        static_cast<double>(mask_info.roi_height));
                    const double radius = std::clamp(
                        std::max(minor_axis_len_world * 0.75,
                                 roi_span * 0.075),
                        10.0,
                        std::max(12.0, roi_span * 0.18));
                    const double angle_rad = std::clamp(
                        static_cast<double>(angle_deg) * M_PI / 180.0,
                        -M_PI,
                        M_PI);
                    const int steps = std::clamp(
                        static_cast<int>(std::ceil(
                            std::fabs(angle_rad) / (M_PI / 24.0))),
                        6,
                        32);

                    std::vector<double> xs;
                    std::vector<double> ys;
                    xs.reserve(static_cast<size_t>(steps + 1));
                    ys.reserve(static_cast<size_t>(steps + 1));
                    for (int step = 0; step <= steps; ++step) {
                        const double t =
                            angle_rad * static_cast<double>(step) /
                            static_cast<double>(steps);
                        const double vx =
                            std::cos(t) * forward[0] + std::sin(t) * left[0];
                        const double vy =
                            std::cos(t) * forward[1] + std::sin(t) * left[1];
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
                    ImPlot::SetNextLineStyle(
                        ImVec4(1.0f, 1.0f, 1.0f, 0.42f), 1.0f);
                    ImPlot::PlotLine((label + "_body_axis").c_str(),
                                     start_x,
                                     start_y,
                                     2);

                    ImVec4 arc_color = color;
                    arc_color.w = 0.95f;
                    ImPlot::SetNextLineStyle(arc_color, 2.4f);
                    ImPlot::PlotLine(label.c_str(),
                                     xs.data(),
                                     ys.data(),
                                     static_cast<int>(xs.size()));

                    const double end_x[2] = {center_world.first, xs.back()};
                    const double end_y[2] = {
                        scene_height_f - center_world.second,
                        ys.back()};
                    ImVec4 end_color = color;
                    end_color.w = 0.65f;
                    ImPlot::SetNextLineStyle(end_color, 1.1f);
                    ImPlot::PlotLine((label + "_gaze_axis").c_str(),
                                     end_x,
                                     end_y,
                                     2);

                    if (options.show_eye_angle_labels) {
                        const double mid_t = angle_rad * 0.5;
                        const double vx = std::cos(mid_t) * forward[0] +
                                          std::sin(mid_t) * left[0];
                        const double vy = std::cos(mid_t) * forward[1] +
                                          std::sin(mid_t) * left[1];
                        auto label_scene =
                            worldToScene(center_world.first + vx * radius * 1.25,
                                         center_world.second + vy * radius * 1.25);
                        char angle_label[32];
                        std::snprintf(angle_label,
                                      sizeof(angle_label),
                                      "%+.1f°",
                                      angle_deg);
                        drawPlotTextBox(angle_label,
                                        label_scene.first,
                                        label_scene.second,
                                        color,
                                        1.3f);
                        metrics.angle_labels_drawn++;
                    }
                    return true;
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
                    const double minor_axis_len_world =
                        std::hypot(endpoint1_world.first - endpoint0_world.first,
                                   endpoint1_world.second - endpoint0_world.second);
                    if (options.show_eye_gaze_rays &&
                        mask_info.has_gaze_vectors &&
                        mask_info.gaze_vector_valid[eye] != 0) {
                        const auto gaze = mask_info.gaze_vector_xy[eye];
                        if (std::isfinite(gaze[0]) &&
                            std::isfinite(gaze[1])) {
                            const double gaze_len =
                                std::hypot(static_cast<double>(gaze[0]),
                                           static_cast<double>(gaze[1]));
                            if (gaze_len > 1e-6) {
                                const double roi_span = std::max(
                                    static_cast<double>(mask_info.roi_width),
                                    static_cast<double>(mask_info.roi_height));
                                const double ray_len =
                                    std::max(roi_span * 0.32,
                                             minor_axis_len_world * 1.35);
                                auto ray_end_scene = worldToScene(
                                    center_world.first +
                                        static_cast<double>(gaze[0]) /
                                            gaze_len * ray_len,
                                    center_world.second +
                                        static_cast<double>(gaze[1]) /
                                            gaze_len * ray_len);
                                auto ray_start_scene = worldToScene(
                                    center_world.first,
                                    center_world.second);
                                const double ray_x[2] = {
                                    ray_start_scene.first,
                                    ray_end_scene.first};
                                const double ray_y[2] = {
                                    ray_start_scene.second,
                                    ray_end_scene.second};
                                ImVec4 ray_color = minor_color;
                                ray_color.w = 0.95f;
                                ImPlot::SetNextLineStyle(ray_color, 2.2f);
                                ImPlot::PlotLine(
                                    (base_id + "_gaze_ray").c_str(),
                                    ray_x,
                                    ray_y,
                                    2);
                                ImPlot::SetNextMarkerStyle(
                                    ImPlotMarker_Circle,
                                    3.0f,
                                    ray_color,
                                    1.0f,
                                    ray_color);
                                const double tip_x[1] = {ray_end_scene.first};
                                const double tip_y[1] = {ray_end_scene.second};
                                ImPlot::PlotScatter(
                                    (base_id + "_gaze_ray_tip").c_str(),
                                    tip_x,
                                    tip_y,
                                    1);
                                metrics.gaze_rays_drawn++;
                            }
                        }
                    }
                    if (mask_info.has_eye_angles &&
                        mask_info.feret_angle_valid[eye]) {
                        bool drew_arc = false;
                        if (options.show_eye_angle_arcs) {
                            const auto* shape = findSubjectShapeForMask(
                                det_idx, mask_info.roi_index);
                            if (shape != nullptr) {
                                drew_arc = draw_eye_angle_arc(
                                    *shape,
                                    center_world,
                                    mask_info.feret_minor_angle_deg[eye],
                                    base_id + "_angle_arc",
                                    minor_color,
                                    minor_axis_len_world);
                            }
                        }
                        if (!drew_arc && options.show_eye_angle_labels) {
                            auto center_scene = worldToScene(
                                center_world.first, center_world.second);
                            char angle_label[32];
                            std::snprintf(angle_label,
                                          sizeof(angle_label),
                                          "%+.1f°",
                                          mask_info.feret_minor_angle_deg[eye]);
                            drawPlotTextBox(angle_label,
                                            center_scene.first,
                                            center_scene.second,
                                            minor_color,
                                            1.3f);
                            metrics.angle_labels_drawn++;
                        }
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
                    if (mask_info.has_gaze_vectors &&
                        mask_info.gaze_vector_valid[eye] != 0) {
                        const auto gaze = mask_info.gaze_vector_xy[eye];
                        if (std::isfinite(gaze[0]) &&
                            std::isfinite(gaze[1])) {
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
                        const float cone_length =
                            std::clamp(roi_span * 1.15f, 90.0f, 520.0f);
                        constexpr float kEyeVisualAngleDeg = 163.0f;
                        const float half_angle_rad =
                            (kEyeVisualAngleDeg * 0.5f) *
                            static_cast<float>(M_PI / 180.0);
                        constexpr int kConeSteps = 30;

                        std::vector<ImVec2> cone_points;
                        std::vector<double> arc_x;
                        std::vector<double> arc_y;
                        cone_points.reserve(kConeSteps + 2);
                        arc_x.reserve(kConeSteps + 1);
                        arc_y.reserve(kConeSteps + 1);

                        auto center_scene =
                            worldToScene(center_world.first, center_world.second);
                        cone_points.push_back(ImPlot::PlotToPixels(
                            ImPlotPoint(center_scene.first, center_scene.second)));

                        for (int step = 0; step <= kConeSteps; ++step) {
                            const float t =
                                -half_angle_rad +
                                (2.0f * half_angle_rad *
                                 static_cast<float>(step) /
                                 static_cast<float>(kConeSteps));
                            const float c = std::cos(t);
                            const float s = std::sin(t);
                            const float vx = c * dir_world.x - s * dir_world.y;
                            const float vy = s * dir_world.x + c * dir_world.y;
                            auto scene =
                                worldToScene(center_world.first +
                                                 static_cast<double>(vx) *
                                                     cone_length,
                                             center_world.second +
                                                 static_cast<double>(vy) *
                                                     cone_length);
                            arc_x.push_back(scene.first);
                            arc_y.push_back(scene.second);
                            cone_points.push_back(ImPlot::PlotToPixels(
                                ImPlotPoint(scene.first, scene.second)));
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
                            ImPlot::PlotLine(
                                (base_id + "_visual_cone_arc").c_str(),
                                arc_x.data(),
                                arc_y.data(),
                                static_cast<int>(arc_x.size()));
                            const double side0_x[2] = {
                                center_scene.first, arc_x.front()};
                            const double side0_y[2] = {
                                center_scene.second, arc_y.front()};
                            const double side1_x[2] = {
                                center_scene.first, arc_x.back()};
                            const double side1_y[2] = {
                                center_scene.second, arc_y.back()};
                            ImPlot::SetNextLineStyle(cone_outline_color, 1.0f);
                            ImPlot::PlotLine(
                                (base_id + "_visual_cone_side0").c_str(),
                                side0_x,
                                side0_y,
                                2);
                            ImPlot::SetNextLineStyle(cone_outline_color, 1.0f);
                            ImPlot::PlotLine(
                                (base_id + "_visual_cone_side1").c_str(),
                                side1_x,
                                side1_y,
                                2);
                        }
                        metrics.visual_cones_drawn++;
                        visual_cone_polygons[eye] = cone_points;
                        if (!visual_cone_polygons[0].empty() &&
                            !visual_cone_polygons[1].empty()) {
                            const std::vector<ImVec2> overlap =
                                clipConvexPolygon(visual_cone_polygons[0],
                                                  visual_cone_polygons[1]);
                            if (overlap.size() >= 3) {
                                constexpr ImVec4 kVergenceOverlapColor =
                                    ImVec4(0.34f, 1.0f, 0.42f, 0.24f);
                                cone_draw_list->AddConvexPolyFilled(
                                    overlap.data(),
                                    static_cast<int>(overlap.size()),
                                    ImGui::ColorConvertFloat4ToU32(
                                        kVergenceOverlapColor));
                                metrics.visual_cone_overlaps_drawn++;
                            }
                        }
                    }
                }
                metrics.axis_draw_ms += durationMs(
                    std::chrono::steady_clock::now() - axis_start);
            }
        }

        const bool selected_roi =
            options.highlighted_roi_index >= 0 &&
            mask_info.roi_index == options.highlighted_roi_index &&
            !options.highlighted_component_name.empty();
        if (selected_roi) {
            constexpr ImVec4 kSelectionColor =
                ImVec4(1.0f, 0.95f, 0.18f, 0.98f);
            auto selected_component = std::find_if(
                mask_info.subject_mask_components.begin(),
                mask_info.subject_mask_components.end(),
                [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                        SubjectMaskComponent& component) {
                    return component.label ==
                           options.highlighted_component_name;
                });
            bool drew_selection_contour = false;
            if (selected_component != mask_info.subject_mask_components.end()) {
                draw_component_contour(
                    *selected_component,
                    "##selected_subject_mask_" +
                        options.highlighted_component_name + "_" +
                        std::to_string(det_idx),
                    3.2f,
                    kSelectionColor,
                    false);
                drew_selection_contour =
                    selected_component->has_contour &&
                    selected_component->contour_xy.size() > 1;
                if (drew_selection_contour) {
                    metrics.selected_contours_drawn++;
                }
            }
            if (!drew_selection_contour) {
                draw_roi_selection_border(
                    "##selected_subject_mask_roi_" + std::to_string(det_idx),
                    kSelectionColor,
                    2.8f);
            }

            ImPlot::PlotText(
                shortSubjectMaskLabel(options.highlighted_component_name).c_str(),
                mask_info.offset_x,
                scene_height_f - mask_info.offset_y,
                ImVec2(6.0f, 8.0f));
            metrics.selected_highlight_drawn = true;
        }
    }
    return finish();
}
