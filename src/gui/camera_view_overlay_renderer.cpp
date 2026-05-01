#include "gui/camera_view_overlay_renderer.h"
#include "gui/refined_keypoint_style.h"

#include "global.h"
#include "imgui.h"
#include "implot.h"
#include <GL/glew.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

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

ImVec4 subjectMaskComponentColor(const std::string& label) {
    if (label == "subject_body") {
        return ImVec4(0.1f, 0.85f, 0.55f, 0.18f);
    }
    if (label == "swim_bladder") {
        return ImVec4(1.0f, 0.8f, 0.18f, 0.48f);
    }
    if (label == "eye_left") {
        return ImVec4(0.2f, 0.6f, 1.0f, 0.35f);
    }
    if (label == "eye_right") {
        return ImVec4(1.0f, 0.3f, 0.6f, 0.35f);
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 0.25f);
}

ImVec4 subjectMaskContourColor(const std::string& label) {
    ImVec4 color = subjectMaskComponentColor(label);
    color.w = (label == "subject_body") ? 0.75f : 0.95f;
    return color;
}

bool isEyeMaskComponent(const std::string& label) {
    return label == "eye_left" || label == "eye_right";
}

std::string shortSubjectMaskLabel(const std::string& label) {
    if (label == "subject_body") {
        return "Body";
    }
    if (label == "eye_left") {
        return "Left eye";
    }
    if (label == "eye_right") {
        return "Right eye";
    }
    if (label == "swim_bladder") {
        return "Swim bladder";
    }
    return label;
}

struct ChaserStateOverlay {
    int chaser_index = -1;
    double target_plot_x = 0.0;
    double target_plot_y = 0.0;
    size_t target_bbox_index = std::numeric_limits<size_t>::max();
    bool has_target = false;
    double chaser_plot_x = 0.0;
    double chaser_plot_y = 0.0;
    bool has_chaser = false;
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

bool projectStimulusToCamera(const ZarrDetectionLoader::ChaserState& state,
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
        if (!std::isfinite(cx) || !std::isfinite(cy)) {
            return false;
        }
        out_x = cx;
        out_y = static_cast<double>(image_height_px) - cy;
        return true;
    }

    if (!std::isfinite(stim_x) || !std::isfinite(stim_y)) {
        return false;
    }

    const float offset_x = camera_params.stimulus_offset_x;
    const float offset_y = camera_params.stimulus_offset_y;
    if (camera_params.has_valid_homography) {
        std::vector<cv::Point2f> src_points(1);
        std::vector<cv::Point2f> dst_points;
        src_points[0] = cv::Point2f(stim_x + offset_x, stim_y + offset_y);
        cv::perspectiveTransform(src_points, dst_points,
                                 camera_params.inverse_homography_matrix);
        if (dst_points.empty()) {
            return false;
        }
        out_x = dst_points[0].x;
        out_y = static_cast<double>(image_height_px) - dst_points[0].y;
        return true;
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
    auto roiToScene =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            const std::array<float, 2>& roi_point) -> std::pair<double, double> {
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
    };

    auto drawPoint =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            const std::array<float, 2>& point,
            const std::string& label,
            ImPlotMarker marker,
            float size,
            const ImVec4& fill,
            const ImVec4& outline) {
        auto scene = roiToScene(shape, point);
        if (!std::isfinite(scene.first) || !std::isfinite(scene.second)) {
            return;
        }
        const double x = scene.first;
        const double y = scene.second;
        ImPlot::SetNextMarkerStyle(marker, size, fill, 1.5f, outline);
        ImPlot::PlotScatter(label.c_str(), &x, &y, 1);
    };

    auto drawPolyline =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            const std::vector<std::array<float, 2>>& points,
            const std::string& label,
            const ImVec4& color,
            float thickness) {
        if (points.size() < 2) {
            return;
        }
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(points.size());
        ys.reserve(points.size());
        for (const auto& point : points) {
            auto scene = roiToScene(shape, point);
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
    };

    auto drawMaskContour =
        [&](const ZarrDetectionLoader::FrameDetections::EyeMask& mask,
            const ZarrDetectionLoader::FrameDetections::EyeMask::
                SubjectMaskComponent& component,
            const std::string& label,
            const ImVec4& color,
            float thickness) {
        if (!component.has_contour || component.contour_xy.size() < 2 ||
            mask.cols <= 0 || mask.rows <= 0 || mask.roi_width <= 0.0f ||
            mask.roi_height <= 0.0f) {
            return;
        }
        const double cell_w =
            mask.roi_width / static_cast<double>(mask.cols);
        const double cell_h =
            mask.roi_height / static_cast<double>(mask.rows);
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(component.contour_xy.size() + 1);
        ys.reserve(component.contour_xy.size() + 1);
        for (const auto& point : component.contour_xy) {
            const double px = mask.offset_x +
                              static_cast<double>(point[0]) * cell_w;
            const double py = mask.offset_y +
                              static_cast<double>(point[1]) * cell_h;
            xs.push_back(px);
            ys.push_back(scene_height_f - py);
        }
        const auto& first = component.contour_xy.front();
        const auto& last = component.contour_xy.back();
        if (component.contour_xy.size() > 2 &&
            (std::fabs(first[0] - last[0]) > 1e-5f ||
             std::fabs(first[1] - last[1]) > 1e-5f)) {
            const double px = mask.offset_x +
                              static_cast<double>(first[0]) * cell_w;
            const double py = mask.offset_y +
                              static_cast<double>(first[1]) * cell_h;
            xs.push_back(px);
            ys.push_back(scene_height_f - py);
        }
        ImPlot::SetNextLineStyle(color, thickness);
        ImPlot::PlotLine(label.c_str(),
                         xs.data(),
                         ys.data(),
                         static_cast<int>(xs.size()));
    };

    auto drawBodyAxis =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            const std::array<float, 2>& axis,
            const std::string& label,
            const ImVec4& color) {
        if (!shape.body_frame_valid ||
            !std::isfinite(shape.body_origin_xy[0]) ||
            !std::isfinite(shape.body_origin_xy[1]) ||
            !std::isfinite(axis[0]) || !std::isfinite(axis[1])) {
            return;
        }
        const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1]);
        if (len <= 1e-5f) {
            return;
        }
        const float axis_len = std::max(shape.coordinate_width,
                                        shape.coordinate_height) * 0.12f;
        const std::array<float, 2> p0 = {
            shape.body_origin_xy[0],
            shape.body_origin_xy[1]};
        const std::array<float, 2> p1 = {
            shape.body_origin_xy[0] + axis[0] / len * axis_len,
            shape.body_origin_xy[1] + axis[1] / len * axis_len};
        std::vector<std::array<float, 2>> points = {p0, p1};
        drawPolyline(shape, points, label, color, 2.0f);
    };

    auto drawTailNormals =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            size_t det_idx) {
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
                         "##shape_tail_normal_" + std::to_string(det_idx) +
                             "_" + std::to_string(i),
                         ImVec4(0.65f, 0.95f, 1.0f, 0.65f),
                         1.0f);
        }
    };

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
                                component.label == "subject_body" ? 1.6f : 2.0f);
            }
        }

        if (options.show_body_frame_axes) {
            drawBodyAxis(shape,
                         shape.body_forward_axis_xy,
                         "##shape_body_forward_" + std::to_string(det_idx),
                         ImVec4(1.0f, 0.55f, 0.15f, 0.9f));
            drawBodyAxis(shape,
                         shape.body_left_axis_xy,
                         "##shape_body_left_" + std::to_string(det_idx),
                         ImVec4(0.1f, 0.9f, 0.95f, 0.85f));
        }

        if (options.show_centerline && shape.centerline_valid) {
            drawPolyline(shape,
                         shape.centerline_xy,
                         "##shape_centerline_" + std::to_string(det_idx),
                         ImVec4(1.0f, 0.92f, 0.25f, 0.95f),
                         2.2f);
        }
        if (options.show_bspline_sample && shape.bspline_valid) {
            drawPolyline(shape,
                         shape.bspline_sample_xy,
                         "##shape_bspline_" + std::to_string(det_idx),
                         ImVec4(0.2f, 1.0f, 0.7f, 0.95f),
                         2.0f);
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
                          ImVec4(0.0f, 0.0f, 0.0f, 0.45f));
            }
        }
        if (options.show_bspline_control_points) {
            drawPolyline(shape,
                         shape.bspline_control_points_xy,
                         "##shape_bspline_controls_line_" +
                             std::to_string(det_idx),
                         ImVec4(0.2f, 0.9f, 0.7f, 0.35f),
                         1.0f);
            for (size_t i = 0; i < shape.bspline_control_points_xy.size(); ++i) {
                drawPoint(shape,
                          shape.bspline_control_points_xy[i],
                          "##shape_bspline_control_" +
                              std::to_string(det_idx) + "_" +
                              std::to_string(i),
                          ImPlotMarker_Square,
                          4.0f,
                          ImVec4(0.2f, 1.0f, 0.7f, 0.65f),
                          ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
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
                          ImVec4(0.0f, 0.0f, 0.0f, 0.65f));
            }
        }
        if (options.show_tail_normals && shape.tail_sample_valid) {
            drawTailNormals(shape, det_idx);
        }

        if (options.show_snout_tip && shape.snout_tip_valid) {
            drawPoint(shape,
                      shape.snout_tip_xy,
                      "##shape_snout_" + std::to_string(det_idx),
                      ImPlotMarker_Circle,
                      7.0f,
                      ImVec4(1.0f, 0.45f, 0.1f, 0.95f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.9f));
        }
        if (options.show_tail_base && shape.tail_base_valid) {
            drawPoint(shape,
                      shape.tail_base_xy,
                      "##shape_tail_base_" + std::to_string(det_idx),
                      ImPlotMarker_Diamond,
                      6.5f,
                      ImVec4(0.95f, 0.55f, 1.0f, 0.9f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        }
        if (options.show_tail_tip) {
            drawPoint(shape,
                      shape.tail_tip_xy,
                      "##shape_tail_tip_" + std::to_string(det_idx),
                      ImPlotMarker_Cross,
                      6.5f,
                      ImVec4(0.75f, 0.45f, 1.0f, 0.9f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        }
        if (options.show_caudal_anchor && shape.caudal_contour_valid) {
            drawPoint(shape,
                      shape.caudal_contour_point_xy,
                      "##shape_caudal_anchor_" + std::to_string(det_idx),
                      ImPlotMarker_Up,
                      6.5f,
                      ImVec4(0.25f, 0.75f, 1.0f, 0.95f),
                      ImVec4(0.0f, 0.0f, 0.0f, 0.9f));
        }
    }
}

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

    const float scene_height_f = image_height_px;
    auto roiToScene =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            const std::array<float, 2>& roi_point) -> std::pair<double, double> {
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
    };

    auto drawPolyline =
        [&](const ZarrDetectionLoader::FrameDetections::SubjectShape& shape,
            const std::vector<std::array<float, 2>>& points,
            const std::string& label,
            const ImVec4& color,
            float thickness) {
        if (points.size() < 2) {
            return;
        }
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(points.size());
        ys.reserve(points.size());
        for (const auto& point : points) {
            auto scene = roiToScene(shape, point);
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
    };

    auto normalized =
        [](std::array<float, 2> value) -> std::array<float, 2> {
        const float len =
            std::sqrt(value[0] * value[0] + value[1] * value[1]);
        if (len <= 1e-6f || !std::isfinite(len)) {
            return {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::quiet_NaN()};
        }
        return {value[0] / len, value[1] / len};
    };

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
                         2.1f);
        }

        if (options.show_samples) {
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(samples.size());
            ys.reserve(samples.size());
            for (const auto& sample : samples) {
                auto scene = roiToScene(shape, sample);
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
                        static_cast<float>(M_PI / 180.0);
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
                                 1.2f);
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
                                 1.0f);
                }
            }
        }
    }
}

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

    for (const auto& state : chaser_states) {
        ChaserStateOverlay overlay;
        overlay.chaser_index = state.chaser_index;

        double target_plot_x = 0.0;
        double target_plot_y = 0.0;
        if (projectStimulusToCamera(state,
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
            const size_t bbox_idx = chaser_bboxes.empty()
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
        if (projectStimulusToCamera(state,
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
        const ImVec4 box_color = highlight_target
                                     ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                                     : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
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
        const std::string label = highlight_target
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
                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                       IMPLOT_AUTO,
                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
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

        if (overlay.has_chaser && overlay.has_target) {
            const double line_x[2] = {overlay.chaser_plot_x, overlay.target_plot_x};
            const double line_y[2] = {overlay.chaser_plot_y, overlay.target_plot_y};
            const double dx = overlay.target_plot_x - overlay.chaser_plot_x;
            const double dy = overlay.target_plot_y - overlay.chaser_plot_y;
            const double dist = std::sqrt(dx * dx + dy * dy);
            const double max_dim =
                static_cast<double>(std::max(image_width_px, image_height_px));
            const double max_dist = (max_dim > 0.0) ? (max_dim * (2.0 / 3.0)) : 200.0;
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
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                       8.0f,
                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                       2.0f,
                                       ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            const std::string chaser_label =
                "Chaser_state_" + std::to_string(overlay.chaser_index);
            ImPlot::PlotScatter(chaser_label.c_str(), &plot_x, &plot_y, 1);
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
