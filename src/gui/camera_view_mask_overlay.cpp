#include "gui/camera_view_overlay_renderer.h"
#include "gui/camera_view_eye_angle_overlay.h"
#include "gui/camera_view_overlay_style.h"

#include "imgui.h"
#include "implot.h"
#include <GL/glew.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
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

    resetCameraViewEyeAngleOverlaySmoothing(smoothing_run_id);
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

        CameraViewEyeAngleOverlayState eye_angle_overlay_state;
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

            if (draw_axes_and_angles) {
                const auto* shape =
                    findSubjectShapeForMask(det_idx, mask_info.roi_index);
                drawCameraViewEyeAngleOverlayForEye(
                    mask_info,
                    shape,
                    mask_details.boxes[det_idx],
                    eye,
                    base_id,
                    base_color,
                    cell_w,
                    cell_h,
                    scene_height_f,
                    options,
                    metrics,
                    eye_angle_overlay_state);
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
