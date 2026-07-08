#include "gui/camera_view_overlay_renderer.h"
#include "gui/camera_view_eye_angle_overlay.h"
#include "gui/camera_view_overlay_style.h"
#include "gui/refined_keypoint_style.h"
#include "refined_keypoint_repository.h"

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

ImVec2 clampPlotOverlayPosition(ImVec2 top_left, const ImVec2& box_size) {
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

void drawPlotTextBox(const std::string& text,
                     const ImPlotPoint& plot_anchor,
                     const ImVec4& accent_color,
                     const ImVec2& pixel_offset = ImVec2(6.0f, 6.0f)) {
    if (text.empty() || !std::isfinite(plot_anchor.x) ||
        !std::isfinite(plot_anchor.y)) {
        return;
    }

    const ImVec2 padding(7.0f, 4.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 box_size(text_size.x + padding.x * 2.0f,
                          text_size.y + padding.y * 2.0f);
    ImVec2 top_left = ImPlot::PlotToPixels(plot_anchor);
    top_left.x += pixel_offset.x;
    top_left.y += pixel_offset.y;
    top_left = clampPlotOverlayPosition(top_left, box_size);

    const ImVec2 box_min = top_left;
    const ImVec2 box_max(top_left.x + box_size.x, top_left.y + box_size.y);
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    draw_list->AddRectFilled(box_min, box_max, IM_COL32(8, 10, 14, 220), 4.0f);
    draw_list->AddRect(box_min,
                       box_max,
                       ImGui::ColorConvertFloat4ToU32(accent_color),
                       4.0f,
                       0,
                       1.1f);
    const ImVec2 text_pos(top_left.x + padding.x, top_left.y + padding.y);
    draw_list->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
                       IM_COL32(0, 0, 0, 210),
                       text.c_str());
    draw_list->AddText(text_pos,
                       ImGui::ColorConvertFloat4ToU32(accent_color),
                       text.c_str());
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

std::vector<uint32_t> previewPixelIndices(
    const CameraViewSubjectMaskPreview& preview) {
    std::vector<uint32_t> pixels;
    if (!preview.active || preview.binary_mask == nullptr ||
        preview.rows <= 0 || preview.cols <= 0) {
        return pixels;
    }
    const size_t expected_size =
        static_cast<size_t>(preview.rows) * static_cast<size_t>(preview.cols);
    if (preview.binary_mask->size() != expected_size) {
        return pixels;
    }
    pixels.reserve(expected_size / 4);
    for (size_t idx = 0; idx < expected_size; ++idx) {
        if ((*preview.binary_mask)[idx] != 0) {
            pixels.push_back(static_cast<uint32_t>(idx));
        }
    }
    return pixels;
}

bool previewReplacesComponent(
    const CameraViewSubjectMaskPreview* preview,
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask_info,
    const std::string& component_name) {
    return preview != nullptr && preview->active && preview->dirty &&
           preview->roi_index == mask_info.roi_index &&
           preview->component_name == component_name &&
           preview->rows == mask_info.rows &&
           preview->cols == mask_info.cols &&
           preview->binary_mask != nullptr;
}

bool validMaskForTexturePrewarm(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask_info) {
    return mask_info.valid && mask_info.roi_index >= 0 && mask_info.rows > 0 &&
           mask_info.cols > 0 && mask_info.roi_width > 0.0f &&
           mask_info.roi_height > 0.0f && std::isfinite(mask_info.offset_x) &&
           std::isfinite(mask_info.offset_y);
}

void prewarmMaskTexturePixels(const std::string& smoothing_run_id,
                              const ZarrDetectionLoader::FrameDetections::
                                  EyeMask& mask_info,
                              const std::string& layer_key,
                              const std::vector<uint32_t>& pixels,
                              const ImVec4& color,
                              CameraViewMaskPerfMetrics& metrics) {
    if (pixels.empty()) {
        return;
    }
    const GLuint texture_id = eyeMaskTextureCache().getOrCreatePixels(
        smoothing_run_id,
        mask_info.roi_index,
        layer_key,
        mask_info.rows,
        mask_info.cols,
        pixels,
        color,
        &metrics);
    if (texture_id != 0) {
        metrics.component_fill_count++;
    }
}

void prewarmFullMaskTextures(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask_info,
    const std::string& smoothing_run_id,
    const CameraViewMaskOverlayOptions& options,
    const CameraViewSubjectMaskPreview* edit_preview,
    CameraViewMaskPerfMetrics& metrics) {
    for (const auto& component : mask_info.subject_mask_components) {
        if (!component.valid || component.pixel_indices.empty() ||
            isEyeMaskComponent(component.label) ||
            !shouldDrawSubjectMaskComponent(component.label, options) ||
            previewReplacesComponent(edit_preview, mask_info, component.label)) {
            continue;
        }
        const std::string layer_key =
            "component:" + component.label + ":" +
            std::to_string(component.channel_index);
        prewarmMaskTexturePixels(smoothing_run_id,
                                 mask_info,
                                 layer_key,
                                 component.pixel_indices,
                                 subjectMaskComponentColor(component.label),
                                 metrics);
    }

    for (int eye = 0; eye < 2; ++eye) {
        if ((eye == 0 && !options.show_eye_left) ||
            (eye == 1 && !options.show_eye_right)) {
            continue;
        }
        const std::string eye_label = eye == 0 ? "eye_left" : "eye_right";
        if (previewReplacesComponent(edit_preview, mask_info, eye_label)) {
            continue;
        }
        ImVec4 base_color = eye == 0
                                ? ImVec4(0.2f, 0.6f, 1.0f, 0.35f)
                                : ImVec4(1.0f, 0.3f, 0.6f, 0.35f);
        const GLuint texture_id = eyeMaskTextureCache().getOrCreate(
            smoothing_run_id, mask_info, eye, base_color, &metrics);
        if (texture_id != 0) {
            metrics.component_fill_count++;
        }
    }

    if (edit_preview != nullptr && edit_preview->active &&
        edit_preview->dirty && edit_preview->roi_index == mask_info.roi_index &&
        !edit_preview->component_name.empty() &&
        shouldDrawSubjectMaskComponent(edit_preview->component_name, options)) {
        std::vector<uint32_t> pixels = previewPixelIndices(*edit_preview);
        ImVec4 preview_color =
            subjectMaskComponentColor(edit_preview->component_name);
        preview_color.w = std::max(preview_color.w, 0.62f);
        const std::string layer_key =
            "preview:" + edit_preview->component_name + ":" +
            std::to_string(edit_preview->revision);
        prewarmMaskTexturePixels(smoothing_run_id,
                                 mask_info,
                                 layer_key,
                                 pixels,
                                 preview_color,
                                 metrics);
    }
}

void prewarmInsetMaskTextures(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask_info,
    const std::string& smoothing_run_id,
    const CameraViewMaskOverlayOptions& mask_options,
    const CameraViewActiveRoiInsetOptions& inset_options,
    const CameraViewSubjectMaskPreview* edit_preview,
    CameraViewMaskPerfMetrics& metrics) {
    if (!inset_options.show_inset) {
        return;
    }
    auto find_component = [&](const std::string& label) {
        return std::find_if(
            mask_info.subject_mask_components.begin(),
            mask_info.subject_mask_components.end(),
            [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                    SubjectMaskComponent& component) {
                return component.label == label;
            });
    };
    auto component_visible = [&](const std::string& label) {
        if (!inset_options.mirror_enabled_overlays &&
            !mask_options.highlighted_component_name.empty()) {
            return label == mask_options.highlighted_component_name;
        }
        return shouldDrawSubjectMaskComponent(label, mask_options);
    };
    auto prewarm_pixels = [&](const std::string& label,
                              const std::vector<uint32_t>& pixels,
                              const std::string& layer_suffix,
                              float min_alpha) {
        if (pixels.empty()) {
            return;
        }
        ImVec4 overlay_color = subjectMaskComponentColor(label);
        overlay_color.w = std::max(overlay_color.w, min_alpha);
        prewarmMaskTexturePixels(smoothing_run_id,
                                 mask_info,
                                 "roi_inset:" + label + ":" + layer_suffix,
                                 pixels,
                                 overlay_color,
                                 metrics);
    };

    constexpr const char* kDrawOrder[] = {
        "subject_body",
        "swim_bladder",
        "eye_left",
        "eye_right",
    };
    for (const char* label_cstr : kDrawOrder) {
        const std::string label(label_cstr);
        if (!component_visible(label)) {
            continue;
        }
        const auto component_it = find_component(label);
        const bool component_present =
            component_it != mask_info.subject_mask_components.end() &&
            component_it->valid;
        if (previewReplacesComponent(edit_preview, mask_info, label) &&
            edit_preview != nullptr) {
            const std::vector<uint32_t> preview_pixels =
                previewPixelIndices(*edit_preview);
            prewarm_pixels(label,
                           preview_pixels,
                           "preview:" +
                               std::to_string(edit_preview->revision),
                           0.70f);
            continue;
        }
        if (component_present) {
            prewarm_pixels(label,
                           component_it->pixel_indices,
                           "component:" +
                               std::to_string(component_it->channel_index),
                           0.46f);
            continue;
        }
        if (label == "eye_left" && !mask_info.pixel_indices[0].empty()) {
            prewarm_pixels(label, mask_info.pixel_indices[0], "legacy_eye:0", 0.46f);
        } else if (label == "eye_right" &&
                   !mask_info.pixel_indices[1].empty()) {
            prewarm_pixels(label, mask_info.pixel_indices[1], "legacy_eye:1", 0.46f);
        }
    }
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
    dst.invalid_roi_count += src.invalid_roi_count;
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
    const CameraViewMaskOverlayOptions& options,
    const CameraViewSubjectMaskPreview* edit_preview) {
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

    const size_t mask_count = mask_details.eye_masks.size();
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
            metrics.invalid_roi_count++;
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
                !shouldDrawSubjectMaskComponent(component.label, options) ||
                previewReplacesComponent(edit_preview,
                                         mask_info,
                                         component.label)) {
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
                if (isEyeMaskComponent(component.label) ||
                    previewReplacesComponent(edit_preview,
                                             mask_info,
                                             component.label)) {
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

        auto draw_edit_preview =
            [&](const CameraViewSubjectMaskPreview& preview) {
                if (!preview.active || !preview.dirty ||
                    preview.roi_index != mask_info.roi_index ||
                    preview.component_name.empty() ||
                    preview.rows != mask_info.rows ||
                    preview.cols != mask_info.cols ||
                    !shouldDrawSubjectMaskComponent(preview.component_name,
                                                    options)) {
                    return;
                }
                std::vector<uint32_t> pixels = previewPixelIndices(preview);
                ImVec4 preview_color =
                    subjectMaskComponentColor(preview.component_name);
                preview_color.w =
                    std::max(preview_color.w, 0.62f);
                const std::string layer_key =
                    "preview:" + preview.component_name + ":" +
                    std::to_string(preview.revision);
                const std::string base_id =
                    "##subject_mask_preview_" + preview.component_name + "_" +
                    std::to_string(det_idx);
                if (!pixels.empty()) {
                    GLuint texture_id = eyeMaskTextureCache().getOrCreatePixels(
                        smoothing_run_id,
                        preview.roi_index,
                        layer_key,
                        preview.rows,
                        preview.cols,
                        pixels,
                        preview_color,
                        &metrics);
                    if (texture_id != 0) {
                        const double x_min = mask_info.offset_x;
                        const double x_max =
                            mask_info.offset_x + mask_info.roi_width;
                        const double y_min =
                            scene_height_f -
                            (mask_info.offset_y + mask_info.roi_height);
                        const double y_max = scene_height_f - mask_info.offset_y;
                        const auto fill_draw_start =
                            std::chrono::steady_clock::now();
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
                    } else {
                        const auto fill_draw_start =
                            std::chrono::steady_clock::now();
                        std::vector<double> xs;
                        std::vector<double> ys;
                        xs.reserve(pixels.size());
                        ys.reserve(pixels.size());
                        for (uint32_t linear : pixels) {
                            const uint32_t row =
                                linear / static_cast<uint32_t>(mask_info.cols);
                            const uint32_t col =
                                linear % static_cast<uint32_t>(mask_info.cols);
                            const double px = mask_info.offset_x +
                                              (static_cast<double>(col) + 0.5) *
                                                  cell_w;
                            const double py = mask_info.offset_y +
                                              (static_cast<double>(row) + 0.5) *
                                                  cell_h;
                            xs.push_back(px);
                            ys.push_back(scene_height_f - py);
                        }
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                                   2.5f,
                                                   preview_color,
                                                   1.0f,
                                                   preview_color);
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

                constexpr ImVec4 kPreviewBorderColor =
                    ImVec4(1.0f, 1.0f, 1.0f, 0.92f);
                draw_roi_selection_border(base_id + "_border",
                                          kPreviewBorderColor,
                                          1.8f);
                drawPlotTextBox(
                    pixels.empty()
                        ? ("Preview empty: " +
                           shortSubjectMaskLabel(preview.component_name))
                        : ("Preview: " +
                           shortSubjectMaskLabel(preview.component_name)),
                    ImPlotPoint(mask_info.offset_x,
                                scene_height_f - mask_info.offset_y),
                    kPreviewBorderColor,
                    ImVec2(6.0f, 6.0f));
            };

        CameraViewEyeAngleOverlayState eye_angle_overlay_state;
        for (int eye = 0; eye < 2; ++eye) {
            if ((eye == 0 && !options.show_eye_left) ||
                (eye == 1 && !options.show_eye_right)) {
                continue;
            }
            const auto& pixel_indices = mask_info.pixel_indices[eye];
            const std::string eye_label = (eye == 0) ? "eye_left" : "eye_right";
            const bool preview_replaces_eye =
                previewReplacesComponent(edit_preview, mask_info, eye_label);
            const bool has_pixels = !pixel_indices.empty();
            const std::string base_id =
                (eye == 0) ? "##eye_mask_left_" + std::to_string(det_idx)
                           : "##eye_mask_right_" + std::to_string(det_idx);
            ImVec4 base_color = (eye == 0)
                                    ? ImVec4(0.2f, 0.6f, 1.0f, 0.35f)
                                    : ImVec4(1.0f, 0.3f, 0.6f, 0.35f);

            bool drew_texture = false;
            if (has_pixels && !preview_replaces_eye) {
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

            auto contour_component = std::find_if(
                mask_info.subject_mask_components.begin(),
                mask_info.subject_mask_components.end(),
                [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                        SubjectMaskComponent& component) {
                    return component.label == eye_label;
                });
            if (draw_all_contours &&
                contour_component != mask_info.subject_mask_components.end() &&
                !preview_replaces_eye) {
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
                const std::array<float, 4> mask_box =
                    det_idx < mask_details.boxes.size()
                        ? mask_details.boxes[det_idx]
                        : std::array<float, 4>{
                              mask_info.offset_x,
                              mask_info.offset_y,
                              mask_info.offset_x + mask_info.roi_width,
                              mask_info.offset_y + mask_info.roi_height};
                drawCameraViewEyeAngleOverlayForEye(
                    mask_info,
                    shape,
                    mask_box,
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

        if (edit_preview != nullptr) {
            draw_edit_preview(*edit_preview);
        }

        const bool selected_roi =
            options.highlighted_roi_index >= 0 &&
            mask_info.roi_index == options.highlighted_roi_index &&
            !options.highlighted_component_name.empty();
        if (selected_roi) {
            constexpr ImVec4 kSelectionColor =
                ImVec4(1.0f, 0.95f, 0.18f, 0.98f);
            constexpr ImVec4 kEditableRoiColor =
                ImVec4(1.0f, 0.95f, 0.18f, 0.72f);
            draw_roi_selection_border(
                "##selected_subject_mask_edit_roi_" + std::to_string(det_idx),
                kEditableRoiColor,
                2.0f);
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
                    "##selected_subject_mask_edit_roi_fallback_" +
                        std::to_string(det_idx),
                    kSelectionColor,
                    2.8f);
            }

            drawPlotTextBox(
                "ROI edit: " +
                    shortSubjectMaskLabel(options.highlighted_component_name) +
                    " | ROI " + std::to_string(mask_info.roi_index),
                ImPlotPoint(mask_info.offset_x,
                            scene_height_f - mask_info.offset_y),
                kSelectionColor,
                ImVec2(6.0f, 6.0f));
            metrics.selected_highlight_drawn = true;
        }
    }
    return finish();
}

CameraViewMaskPerfMetrics prewarmCameraViewEyeMaskOverlayTextures(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    const std::string& smoothing_run_id,
    const CameraViewMaskOverlayOptions& options,
    bool prewarm_full_overlay,
    const CameraViewActiveRoiInsetOptions* active_roi_inset_options,
    const CameraViewSubjectMaskPreview* edit_preview) {
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
    if (!mask_details.includes_eye_masks || mask_details.eye_masks.empty()) {
        return finish();
    }

    metrics.roi_count = static_cast<int>(mask_details.eye_masks.size());
    const bool prewarm_inset =
        active_roi_inset_options != nullptr &&
        active_roi_inset_options->show_inset;
    int32_t inset_roi_index = options.highlighted_roi_index;
    const ZarrDetectionLoader::FrameDetections::EyeMask* inset_mask = nullptr;

    for (const auto& mask_info : mask_details.eye_masks) {
        if (!validMaskForTexturePrewarm(mask_info)) {
            continue;
        }
        metrics.visible_roi_count++;
        if (prewarm_full_overlay) {
            prewarmFullMaskTextures(mask_info,
                                    smoothing_run_id,
                                    options,
                                    edit_preview,
                                    metrics);
        }
        if (!prewarm_inset || inset_mask != nullptr) {
            continue;
        }
        if (inset_roi_index < 0 || mask_info.roi_index == inset_roi_index) {
            inset_mask = &mask_info;
        }
    }

    if (prewarm_inset && inset_mask != nullptr) {
        prewarmInsetMaskTextures(*inset_mask,
                                 smoothing_run_id,
                                 options,
                                 *active_roi_inset_options,
                                 edit_preview,
                                 metrics);
    }
    return finish();
}

void drawCameraViewActiveRoiInsetOverlay(
    unsigned int camera_texture_id,
    int image_width_px,
    int image_height_px,
    const ZarrDetectionLoader::FrameDetections* mask_details,
    const ZarrDetectionLoader::FrameDetections* detection_details,
    const RefinedKeypointSelection* selected_keypoint_selection,
    const CameraViewActiveRoiInsetTarget* fallback_target,
    const std::string& smoothing_run_id,
    const CameraViewMaskOverlayOptions& mask_options,
    bool show_keypoint_markers,
    const CameraViewSubjectMaskPreview* edit_preview,
    const CameraViewActiveRoiInsetOptions& options) {
    if (!options.show_inset || camera_texture_id == 0 || image_width_px <= 0 ||
        image_height_px <= 0) {
        return;
    }

    auto valid_mask = [](const ZarrDetectionLoader::FrameDetections::EyeMask& mask) {
            return mask.valid &&
                   mask.rows > 0 && mask.cols > 0 && mask.roi_width > 0.0f &&
                   mask.roi_height > 0.0f && std::isfinite(mask.offset_x) &&
                   std::isfinite(mask.offset_y);
    };
    auto valid_fallback = [](const CameraViewActiveRoiInsetTarget* target) {
        return target != nullptr && target->valid &&
               std::isfinite(target->offset_x) &&
               std::isfinite(target->offset_y) &&
               std::isfinite(target->roi_width) &&
               std::isfinite(target->roi_height) &&
               target->roi_width > 0.0f && target->roi_height > 0.0f;
    };
    const ZarrDetectionLoader::FrameDetections::EyeMask* selected_mask = nullptr;
    size_t detection_index = 0;
    bool mask_backed = false;
    if (mask_details != nullptr && mask_details->includes_eye_masks) {
        auto find_mask_by_roi = [&](int32_t roi_index) {
            if (roi_index < 0) {
                return mask_details->eye_masks.end();
            }
            return std::find_if(
                mask_details->eye_masks.begin(),
                mask_details->eye_masks.end(),
                [&](const ZarrDetectionLoader::FrameDetections::EyeMask& mask) {
                    return valid_mask(mask) && mask.roi_index == roi_index;
                });
        };
        auto mask_it = mask_details->eye_masks.end();
        if (mask_options.highlighted_roi_index >= 0) {
            mask_it = find_mask_by_roi(mask_options.highlighted_roi_index);
        }
        if (mask_it == mask_details->eye_masks.end() &&
            selected_keypoint_selection != nullptr &&
            selected_keypoint_selection->valid) {
            mask_it = find_mask_by_roi(selected_keypoint_selection->roi_index);
            if (mask_it == mask_details->eye_masks.end() &&
                selected_keypoint_selection->detection_index <
                    mask_details->eye_masks.size() &&
                valid_mask(mask_details->eye_masks
                               [selected_keypoint_selection->detection_index])) {
                mask_it = mask_details->eye_masks.begin() +
                          static_cast<std::ptrdiff_t>(
                              selected_keypoint_selection->detection_index);
            }
        }
        if (mask_it == mask_details->eye_masks.end()) {
            mask_it = std::find_if(mask_details->eye_masks.begin(),
                                   mask_details->eye_masks.end(),
                                   valid_mask);
        }
        if (mask_it != mask_details->eye_masks.end()) {
            selected_mask = &*mask_it;
            detection_index = static_cast<size_t>(
                std::distance(mask_details->eye_masks.begin(), mask_it));
            mask_backed = true;
        }
    }

    ZarrDetectionLoader::FrameDetections::EyeMask fallback_mask;
    if (selected_mask == nullptr && valid_fallback(fallback_target)) {
        fallback_mask.valid = true;
        fallback_mask.rows = fallback_target->rows > 0
                                 ? fallback_target->rows
                                 : std::max(1, static_cast<int>(
                                                   std::round(
                                                       fallback_target
                                                           ->roi_height)));
        fallback_mask.cols = fallback_target->cols > 0
                                 ? fallback_target->cols
                                 : std::max(1, static_cast<int>(
                                                   std::round(
                                                       fallback_target
                                                           ->roi_width)));
        fallback_mask.offset_x = fallback_target->offset_x;
        fallback_mask.offset_y = fallback_target->offset_y;
        fallback_mask.roi_width = fallback_target->roi_width;
        fallback_mask.roi_height = fallback_target->roi_height;
        fallback_mask.roi_index = fallback_target->roi_index;
        if (fallback_target->has_detection_index) {
            detection_index = fallback_target->detection_index;
        }
        selected_mask = &fallback_mask;
    }
    if (selected_mask == nullptr) {
        return;
    }
    const auto& mask = *selected_mask;

    auto resolve_heading_for_detection =
        [&](const ZarrDetectionLoader::FrameDetections* details,
            float& out_heading_deg) {
            if (details == nullptr ||
                detection_index >= details->headings_deg.size()) {
                return false;
            }
            if (!details->heading_valid.empty() &&
                (detection_index >= details->heading_valid.size() ||
                 details->heading_valid[detection_index] == 0)) {
                return false;
            }
            const float heading_deg = details->headings_deg[detection_index];
            if (!std::isfinite(heading_deg)) {
                return false;
            }
            out_heading_deg = heading_deg;
            return true;
    };
    float inset_heading_deg = 0.0f;
    const bool heading_available =
        resolve_heading_for_detection(mask_details, inset_heading_deg) ||
        resolve_heading_for_detection(detection_details, inset_heading_deg);
    const bool use_heading_normalized =
        options.heading_normalized_view && heading_available;

    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x < 160.0f || plot_size.y < 120.0f) {
        return;
    }

    constexpr float kPad = 7.0f;
    constexpr float kOuterMargin = 12.0f;
    const float max_width = std::min(plot_size.x - kOuterMargin * 2.0f,
                                     plot_size.x * 0.34f);
    float image_width = std::clamp(options.width_px,
                                   120.0f,
                                   std::max(120.0f, max_width));
    float image_height = image_width;
    const float max_image_height = plot_size.y * 0.42f;
    if (!use_heading_normalized) {
        const float aspect =
            mask.roi_width > 0.0f ? mask.roi_height / mask.roi_width : 1.0f;
        image_height = image_width * std::clamp(aspect, 0.35f, 2.4f);
        if (image_height > max_image_height) {
            image_height = max_image_height;
            image_width =
                image_height /
                std::max(0.35f, std::clamp(aspect, 0.35f, 2.4f));
        }
    } else if (image_height > max_image_height) {
        image_width = max_image_height;
        image_height = max_image_height;
    }
    image_width = std::max(96.0f, image_width);
    image_height = std::max(64.0f, image_height);

    const float label_height = options.show_label ? 22.0f : 0.0f;
    const ImVec2 box_min(
        plot_pos.x + plot_size.x - image_width - kPad * 2.0f - kOuterMargin,
        plot_pos.y + plot_size.y - image_height - label_height - kPad * 2.0f -
            kOuterMargin);
    const ImVec2 box_max(box_min.x + image_width + kPad * 2.0f,
                         box_min.y + image_height + label_height + kPad * 2.0f);
    const ImVec2 image_min(box_min.x + kPad, box_min.y + kPad);
    const ImVec2 image_max(image_min.x + image_width, image_min.y + image_height);

    const double crop_x0 = std::clamp(static_cast<double>(mask.offset_x),
                                      0.0,
                                      static_cast<double>(image_width_px));
    const double crop_y0 = std::clamp(static_cast<double>(mask.offset_y),
                                      0.0,
                                      static_cast<double>(image_height_px));
    const double crop_x1 = std::clamp(
        static_cast<double>(mask.offset_x + mask.roi_width),
        0.0,
        static_cast<double>(image_width_px));
    const double crop_y1 = std::clamp(
        static_cast<double>(mask.offset_y + mask.roi_height),
        0.0,
        static_cast<double>(image_height_px));
    if (crop_x1 <= crop_x0 || crop_y1 <= crop_y0) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    draw_list->AddRectFilled(box_min, box_max, IM_COL32(5, 8, 13, 218), 6.0f);
    draw_list->AddRect(box_min, box_max, IM_COL32(255, 240, 80, 220), 6.0f);

    constexpr float kPi = 3.14159265358979323846f;
    const float rotation_rad =
        use_heading_normalized ? -inset_heading_deg * kPi / 180.0f : 0.0f;
    const float rotation_c = std::cos(rotation_rad);
    const float rotation_s = std::sin(rotation_rad);

    const ImVec2 image_center(image_min.x + image_width * 0.5f,
                              image_min.y + image_height * 0.5f);
    auto unit_to_inset = [&](float sx, float sy) {
        sx = std::clamp(sx, 0.0f, 1.0f);
        sy = std::clamp(sy, 0.0f, 1.0f);
        const float dx = (sx - 0.5f) * image_width;
        const float dy = (sy - 0.5f) * image_height;
        const float rx =
            use_heading_normalized
                ? rotation_c * dx + rotation_s * dy
                : dx;
        const float ry =
            use_heading_normalized
                ? -rotation_s * dx + rotation_c * dy
                : dy;
        return ImVec2(image_center.x + rx, image_center.y + ry);
    };
    auto draw_textured_inset_quad = [&](ImTextureID texture_id,
                                        ImVec2 uv_min,
                                        ImVec2 uv_max,
                                        ImU32 tint) {
        draw_list->AddImageQuad(
            texture_id,
            unit_to_inset(0.0f, 0.0f),
            unit_to_inset(1.0f, 0.0f),
            unit_to_inset(1.0f, 1.0f),
            unit_to_inset(0.0f, 1.0f),
            ImVec2(uv_min.x, uv_min.y),
            ImVec2(uv_max.x, uv_min.y),
            ImVec2(uv_max.x, uv_max.y),
            ImVec2(uv_min.x, uv_max.y),
            tint);
    };

    draw_list->PushClipRect(image_min, image_max, true);
    draw_textured_inset_quad(
        (ImTextureID)(intptr_t)camera_texture_id,
        ImVec2(static_cast<float>(crop_x0 / image_width_px),
               static_cast<float>(crop_y0 / image_height_px)),
        ImVec2(static_cast<float>(crop_x1 / image_width_px),
               static_cast<float>(crop_y1 / image_height_px)),
        IM_COL32(255, 255, 255, 255));

    auto roi_to_inset = [&](float roi_x, float roi_y) {
        const float sx =
            mask.cols > 0 ? std::clamp(roi_x / static_cast<float>(mask.cols),
                                       0.0f,
                                       1.0f)
                          : 0.0f;
        const float sy =
            mask.rows > 0 ? std::clamp(roi_y / static_cast<float>(mask.rows),
                                      0.0f,
                                       1.0f)
                          : 0.0f;
        return unit_to_inset(sx, sy);
    };
    auto image_to_inset = [&](float image_x, float image_y) {
        const float sx = static_cast<float>(
            (static_cast<double>(image_x) - crop_x0) / (crop_x1 - crop_x0));
        const float sy = static_cast<float>(
            (static_cast<double>(image_y) - crop_y0) / (crop_y1 - crop_y0));
        return unit_to_inset(sx, sy);
    };
    auto image_point_inside_crop = [&](float image_x, float image_y) {
        return std::isfinite(image_x) && std::isfinite(image_y) &&
               static_cast<double>(image_x) >= crop_x0 &&
               static_cast<double>(image_x) <= crop_x1 &&
               static_cast<double>(image_y) >= crop_y0 &&
               static_cast<double>(image_y) <= crop_y1;
    };

    auto find_component = [&](const std::string& label) {
        return std::find_if(
            mask.subject_mask_components.begin(),
            mask.subject_mask_components.end(),
            [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                    SubjectMaskComponent& component) {
                return component.label == label;
            });
    };
    auto draw_pixels = [&](const std::string& label,
                           const std::vector<uint32_t>& pixels,
                           const std::string& layer_suffix,
                           float min_alpha) {
        if (pixels.empty()) {
            return;
        }
        eyeMaskTextureCache().resetIfSourceChanged(smoothing_run_id);
        ImVec4 overlay_color = subjectMaskComponentColor(label);
        overlay_color.w = std::max(overlay_color.w, min_alpha);
        const std::string layer_key = "roi_inset:" + label + ":" + layer_suffix;
        const GLuint texture_id = eyeMaskTextureCache().getOrCreatePixels(
            smoothing_run_id,
            mask.roi_index,
            layer_key,
            mask.rows,
            mask.cols,
            pixels,
            overlay_color,
            nullptr);
        if (texture_id != 0) {
            draw_textured_inset_quad((ImTextureID)(intptr_t)texture_id,
                                     ImVec2(0.0f, 0.0f),
                                     ImVec2(1.0f, 1.0f),
                                     IM_COL32(255, 255, 255, 255));
        }
    };
    auto draw_contour = [&](const std::string& label,
                            const std::vector<std::array<float, 2>>& contour_xy,
                            float thickness) {
        if (contour_xy.size() < 2) {
            return;
        }
        std::vector<ImVec2> contour;
        contour.reserve(contour_xy.size() + 1);
        for (const auto& point : contour_xy) {
            contour.push_back(roi_to_inset(point[0], point[1]));
        }
        const auto& first = contour_xy.front();
        const auto& last = contour_xy.back();
        if (contour_xy.size() > 2 &&
            (std::fabs(first[0] - last[0]) > 1e-5f ||
             std::fabs(first[1] - last[1]) > 1e-5f)) {
            contour.push_back(roi_to_inset(first[0], first[1]));
        }
        draw_list->AddPolyline(contour.data(),
                               static_cast<int>(contour.size()),
                               ImGui::ColorConvertFloat4ToU32(
                                   subjectMaskContourColor(label)),
                               0,
                               thickness);
    };
    auto component_visible = [&](const std::string& label) {
        if (!options.mirror_enabled_overlays &&
            !mask_options.highlighted_component_name.empty()) {
            return label == mask_options.highlighted_component_name;
        }
        return shouldDrawSubjectMaskComponent(label, mask_options);
    };

    bool drew_preview = false;
    if (mask_backed) {
        constexpr const char* kDrawOrder[] = {
            "subject_body",
            "swim_bladder",
            "eye_left",
            "eye_right",
        };
        for (const char* label_cstr : kDrawOrder) {
            const std::string label(label_cstr);
            if (!component_visible(label)) {
                continue;
            }
            const auto component_it = find_component(label);
            const bool component_present =
                component_it != mask.subject_mask_components.end() &&
                component_it->valid;
            if (previewReplacesComponent(edit_preview, mask, label) &&
                edit_preview != nullptr) {
                const std::vector<uint32_t> preview_pixels =
                    previewPixelIndices(*edit_preview);
                draw_pixels(label,
                            preview_pixels,
                            "preview:" +
                                std::to_string(edit_preview->revision),
                            0.70f);
                drew_preview = true;
                continue;
            }
            if (component_present) {
                draw_pixels(label,
                            component_it->pixel_indices,
                            "component:" +
                                std::to_string(component_it->channel_index),
                            0.46f);
                if (component_it->has_contour) {
                    draw_contour(label, component_it->contour_xy, 1.8f);
                }
                continue;
            }
            if (label == "eye_left" && !mask.pixel_indices[0].empty()) {
                draw_pixels(label,
                            mask.pixel_indices[0],
                            "legacy_eye:0",
                            0.46f);
            } else if (label == "eye_right" &&
                       !mask.pixel_indices[1].empty()) {
                draw_pixels(label,
                            mask.pixel_indices[1],
                            "legacy_eye:1",
                            0.46f);
            }
        }

        if (!mask_options.highlighted_component_name.empty()) {
            const auto selected_component =
                find_component(mask_options.highlighted_component_name);
            if (selected_component != mask.subject_mask_components.end() &&
                selected_component->has_contour) {
                draw_contour(mask_options.highlighted_component_name,
                             selected_component->contour_xy,
                             2.8f);
            }
        }
    }

    const ZarrDetectionLoader::FrameDetections* keypoint_details =
        detection_details != nullptr ? detection_details : mask_details;
    if (options.mirror_enabled_overlays && show_keypoint_markers &&
        keypoint_details != nullptr &&
        keypoint_details->has_keypoints &&
        detection_index < keypoint_details->keypoints_pixels.size()) {
        const auto& keypoints =
            keypoint_details->keypoints_pixels[detection_index];
        const size_t kp_per_det =
            std::min(keypoints.size(), keypoint_details->keypoints_per_detection);
        if (!keypoint_details->skeleton_edges.empty()) {
            for (const auto& edge : keypoint_details->skeleton_edges) {
                const size_t a = edge[0];
                const size_t b = edge[1];
                if (a >= kp_per_det || b >= kp_per_det ||
                    !image_point_inside_crop(keypoints[a][0], keypoints[a][1]) ||
                    !image_point_inside_crop(keypoints[b][0], keypoints[b][1])) {
                    continue;
                }
                const ImVec2 pa =
                    image_to_inset(keypoints[a][0], keypoints[a][1]);
                const ImVec2 pb =
                    image_to_inset(keypoints[b][0], keypoints[b][1]);
                draw_list->AddLine(pa, pb, IM_COL32(255, 255, 255, 150), 1.4f);
            }
        }
        for (size_t kp_idx = 0; kp_idx < kp_per_det; ++kp_idx) {
            const auto& kp = keypoints[kp_idx];
            if (!image_point_inside_crop(kp[0], kp[1])) {
                continue;
            }
            const std::string label =
                kp_idx < keypoint_details->keypoint_labels.size()
                    ? keypoint_details->keypoint_labels[kp_idx]
                    : std::string{};
            const ImVec2 center = image_to_inset(kp[0], kp[1]);
            const ImU32 fill =
                chooseRefinedKeypointColorU32(label, kp_idx, 0.96f);
            draw_list->AddCircleFilled(center, 4.0f, fill, 14);
            draw_list->AddCircle(center, 4.0f, IM_COL32(0, 0, 0, 220), 14, 1.3f);
        }
    }

    if (use_heading_normalized) {
        const ImU32 aperture_fill = IM_COL32(5, 8, 13, 245);
        const ImU32 aperture_outline = IM_COL32(255, 240, 80, 245);
        const float radius =
            std::max(1.0f, std::min(image_width, image_height) * 0.5f - 2.0f);
        constexpr int kBands = 96;
        const float band_height = image_height / static_cast<float>(kBands);
        for (int band = 0; band < kBands; ++band) {
            const float y0 = image_min.y + band_height * static_cast<float>(band);
            const float y1 =
                band == kBands - 1
                    ? image_max.y
                    : image_min.y +
                          band_height * static_cast<float>(band + 1);
            const float y_mid = 0.5f * (y0 + y1);
            const float dy = y_mid - image_center.y;
            if (std::fabs(dy) >= radius) {
                draw_list->AddRectFilled(ImVec2(image_min.x, y0),
                                         ImVec2(image_max.x, y1),
                                         aperture_fill);
                continue;
            }
            const float x_half =
                std::sqrt(std::max(0.0f, radius * radius - dy * dy));
            const float left_x = image_center.x - x_half;
            const float right_x = image_center.x + x_half;
            if (left_x > image_min.x) {
                draw_list->AddRectFilled(ImVec2(image_min.x, y0),
                                         ImVec2(left_x, y1),
                                         aperture_fill);
            }
            if (right_x < image_max.x) {
                draw_list->AddRectFilled(ImVec2(right_x, y0),
                                         ImVec2(image_max.x, y1),
                                         aperture_fill);
            }
        }
        draw_list->AddCircle(image_center, radius, aperture_outline, 96, 1.6f);
    }

    draw_list->PopClipRect();
    draw_list->AddRect(image_min,
                       image_max,
                       IM_COL32(255, 240, 80, 245),
                       0.0f,
                       0,
                       1.4f);

    if (options.show_label) {
        std::string label = mask.roi_index >= 0
                                ? "ROI " + std::to_string(mask.roi_index)
                                : std::string("ROI");
        if (!mask_backed && fallback_target != nullptr &&
            fallback_target->source_label != nullptr) {
            label += " | ";
            label += fallback_target->source_label;
        }
        if (!mask_options.highlighted_component_name.empty()) {
            label += " | " +
                     shortSubjectMaskLabel(mask_options.highlighted_component_name);
        }
        if (use_heading_normalized) {
            label += " | heading-normalized";
        } else if (options.heading_normalized_view) {
            label += " | heading unavailable";
        }
        if (drew_preview) {
            label += " preview";
        }
        draw_list->AddText(ImVec2(box_min.x + kPad, image_max.y + 4.0f),
                           IM_COL32(255, 244, 180, 255),
                           label.c_str());
    }
}
