#include "gui/camera_view_window.h"

#include "global.h"
#include "gui/camera_view_manual_keypoint_input.h"
#include "gui/camera_view_overlay_renderer.h"
#include "overlay_scene_contract.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

using crimson::overlay::CameraOverlayLayer;

double durationMs(std::chrono::steady_clock::duration delta) {
    return std::chrono::duration<double, std::milli>(delta).count();
}

void drawCvContours(const std::vector<cv::Rect>& boxes,
                    const std::vector<std::string>& labels,
                    const std::vector<int>& class_ids,
                    int image_height) {
    for (size_t i = 0; i < boxes.size() && i < labels.size() &&
                       i < class_ids.size();
         ++i) {
        double x[5] = {static_cast<double>(boxes[i].x),
                       static_cast<double>(boxes[i].x),
                       static_cast<double>(boxes[i].x + boxes[i].width),
                       static_cast<double>(boxes[i].x + boxes[i].width),
                       static_cast<double>(boxes[i].x)};
        double y[5] = {static_cast<double>(image_height - boxes[i].y),
                       static_cast<double>(image_height - boxes[i].y -
                                           boxes[i].height),
                       static_cast<double>(image_height - boxes[i].y -
                                           boxes[i].height),
                       static_cast<double>(image_height - boxes[i].y),
                       static_cast<double>(image_height - boxes[i].y)};
        if (class_ids[i] == 0) {
            ImPlot::SetNextLineStyle(ImVec4(1.0, 0.0, 1.0, 1.0), 3.0);
        } else {
            ImPlot::SetNextLineStyle(ImVec4(0.5, 1.0, 1.0, 1.0), 3.0);
        }
        ImPlot::PlotLine(labels[i].c_str(), &x[0], &y[0], 5);
    }
}

bool subjectMaskComponentVisible(const std::string& label,
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

const ZarrDetectionLoader::FrameDetections::EyeMask::SubjectMaskComponent*
findSubjectMaskComponent(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask,
    const std::string& label) {
    auto it = std::find_if(
        mask.subject_mask_components.begin(),
        mask.subject_mask_components.end(),
        [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                SubjectMaskComponent& component) {
            return component.label == label;
        });
    return it == mask.subject_mask_components.end() ? nullptr : &*it;
}

bool componentContainsPixel(
    const ZarrDetectionLoader::FrameDetections::EyeMask::SubjectMaskComponent&
        component,
    uint32_t linear_pixel) {
    if (!component.valid || component.pixel_indices.empty()) {
        return false;
    }
    return std::binary_search(component.pixel_indices.begin(),
                              component.pixel_indices.end(),
                              linear_pixel);
}

CameraViewSubjectMaskPick pickSubjectMaskAtPlotPoint(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    const CameraViewMaskOverlayOptions& options,
    float image_height_px,
    const ImPlotPoint& plot_point) {
    CameraViewSubjectMaskPick pick;
    if (!mask_details.includes_eye_masks) {
        return pick;
    }

    const double image_x = plot_point.x;
    const double image_y = static_cast<double>(image_height_px) - plot_point.y;
    constexpr const char* kPriority[] = {
        "eye_left",
        "eye_right",
        "swim_bladder",
        "subject_body",
    };

    for (int det_idx = static_cast<int>(mask_details.eye_masks.size()) - 1;
         det_idx >= 0;
         --det_idx) {
        const auto& mask =
            mask_details.eye_masks[static_cast<size_t>(det_idx)];
        if (!mask.valid || mask.roi_index < 0 || mask.rows <= 0 ||
            mask.cols <= 0 || mask.roi_width <= 0.0f ||
            mask.roi_height <= 0.0f || !std::isfinite(mask.offset_x) ||
            !std::isfinite(mask.offset_y)) {
            continue;
        }
        if (image_x < mask.offset_x ||
            image_x >= mask.offset_x + mask.roi_width ||
            image_y < mask.offset_y ||
            image_y >= mask.offset_y + mask.roi_height) {
            continue;
        }

        const double roi_x =
            (image_x - mask.offset_x) / mask.roi_width *
            static_cast<double>(mask.cols);
        const double roi_y =
            (image_y - mask.offset_y) / mask.roi_height *
            static_cast<double>(mask.rows);
        const int col = static_cast<int>(std::floor(roi_x));
        const int row = static_cast<int>(std::floor(roi_y));
        if (row < 0 || col < 0 || row >= mask.rows || col >= mask.cols) {
            continue;
        }
        const uint32_t linear_pixel =
            static_cast<uint32_t>(row * mask.cols + col);

        for (const char* label : kPriority) {
            if (!subjectMaskComponentVisible(label, options)) {
                continue;
            }
            const auto* component = findSubjectMaskComponent(mask, label);
            if (component == nullptr) {
                continue;
            }
            if (!componentContainsPixel(*component, linear_pixel)) {
                continue;
            }
            pick.valid = true;
            pick.detection_index = det_idx;
            pick.roi_index = mask.roi_index;
            pick.component_name = label;
            return pick;
        }
    }

    return pick;
}

struct SubjectMaskPaintPoint {
    bool valid = false;
    int detection_index = -1;
    int32_t roi_index = -1;
    int row = -1;
    int col = -1;
};

bool subjectMaskBrushInputEnabled(const CameraViewWindowContext& context) {
    return context.subject_mask_brush_input_enabled &&
           context.subject_mask_edit_session != nullptr &&
           context.subject_mask_edit_session->active() &&
           context.subject_mask_brush_state != nullptr &&
           context.subject_mask_brush_state->enabled;
}

SubjectMaskPaintPoint subjectMaskPaintPointAtPlotPoint(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    const SubjectMaskEditSession& edit_session,
    float image_height_px,
    const ImPlotPoint& plot_point) {
    SubjectMaskPaintPoint point;
    if (!mask_details.includes_eye_masks || !edit_session.active()) {
        return point;
    }

    const auto& target = edit_session.target();
    if (target.roi_index < 0 || target.rows == 0 || target.cols == 0) {
        return point;
    }

    const double image_x = plot_point.x;
    const double image_y = static_cast<double>(image_height_px) - plot_point.y;
    for (int det_idx = 0;
         det_idx < static_cast<int>(mask_details.eye_masks.size());
         ++det_idx) {
        const auto& mask =
            mask_details.eye_masks[static_cast<size_t>(det_idx)];
        if (!mask.valid || mask.roi_index != target.roi_index ||
            mask.rows <= 0 || mask.cols <= 0 || mask.roi_width <= 0.0f ||
            mask.roi_height <= 0.0f || !std::isfinite(mask.offset_x) ||
            !std::isfinite(mask.offset_y)) {
            continue;
        }
        if (image_x < mask.offset_x ||
            image_x >= mask.offset_x + mask.roi_width ||
            image_y < mask.offset_y ||
            image_y >= mask.offset_y + mask.roi_height) {
            continue;
        }

        const double roi_x =
            (image_x - mask.offset_x) / mask.roi_width *
            static_cast<double>(target.cols);
        const double roi_y =
            (image_y - mask.offset_y) / mask.roi_height *
            static_cast<double>(target.rows);
        const int col = static_cast<int>(std::floor(roi_x));
        const int row = static_cast<int>(std::floor(roi_y));
        if (row < 0 || col < 0 ||
            row >= static_cast<int>(target.rows) ||
            col >= static_cast<int>(target.cols)) {
            continue;
        }

        point.valid = true;
        point.detection_index = det_idx;
        point.roi_index = target.roi_index;
        point.row = row;
        point.col = col;
        return point;
    }
    return point;
}

void refreshSubjectMaskPreviewFromSession(
    const SubjectMaskEditSession* session,
    CameraViewSubjectMaskPreview& preview) {
    preview = CameraViewSubjectMaskPreview{};
    if (session == nullptr || !session->active() || !session->dirty()) {
        return;
    }
    const auto& target = session->target();
    preview.active = true;
    preview.dirty = session->dirty();
    preview.roi_index = target.roi_index;
    preview.component_name = target.component_name;
    preview.rows = static_cast<int>(target.rows);
    preview.cols = static_cast<int>(target.cols);
    preview.revision = session->previewRevision();
    preview.binary_mask = &session->previewMask();
}

SubjectMaskRoiPoint roiPoint(const SubjectMaskPaintPoint& point) {
    return SubjectMaskRoiPoint{point.row, point.col};
}

bool sameRoiPoint(const SubjectMaskRoiPoint& lhs,
                  const SubjectMaskRoiPoint& rhs) {
    return lhs.row == rhs.row && lhs.col == rhs.col;
}

bool appendRoiPointIfSeparated(std::vector<SubjectMaskRoiPoint>& points,
                               const SubjectMaskPaintPoint& point,
                               int min_distance_px) {
    if (!point.valid) {
        return false;
    }
    const SubjectMaskRoiPoint next = roiPoint(point);
    if (!points.empty()) {
        const SubjectMaskRoiPoint& prev = points.back();
        const int drow = next.row - prev.row;
        const int dcol = next.col - prev.col;
        if (drow * drow + dcol * dcol <
            min_distance_px * min_distance_px) {
            return false;
        }
    }
    points.push_back(next);
    return true;
}

void resetBrushStroke(SubjectMaskBrushState& brush) {
    brush.stroke_active = false;
    brush.last_row = -1;
    brush.last_col = -1;
}

void resetLassoDraft(SubjectMaskBrushState& brush) {
    brush.lasso_active = false;
    brush.lasso_points.clear();
}

void populatePaintResult(CameraViewSubjectMaskPaint& result,
                         const SubjectMaskEditSession& edit_session,
                         const SubjectMaskPaintPoint& point,
                         bool changed) {
    result.changed = result.changed || changed;
    result.roi_index = point.roi_index;
    result.component_name = edit_session.target().component_name;
    result.row = point.row;
    result.col = point.col;
}

bool applyShapeDraft(SubjectMaskEditSession& edit_session,
                     std::vector<SubjectMaskRoiPoint>& points,
                     bool erase) {
    if (points.size() < 3) {
        points.clear();
        return false;
    }
    const uint8_t value = erase ? 0 : 1;
    const bool changed = edit_session.fillPolygon(points, value);
    points.clear();
    return changed;
}

void appendDottedSegment(std::vector<double>& xs,
                         std::vector<double>& ys,
                         double x0,
                         double y0,
                         double x1,
                         double y1) {
    constexpr double kDotSpacingPx = 6.0;
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-6) {
        xs.push_back(x0);
        ys.push_back(y0);
        return;
    }
    const int dot_count =
        std::max(2, static_cast<int>(std::ceil(length / kDotSpacingPx)) + 1);
    for (int i = 0; i < dot_count; ++i) {
        const double t =
            static_cast<double>(i) / static_cast<double>(dot_count - 1);
        xs.push_back(x0 + dx * t);
        ys.push_back(y0 + dy * t);
    }
}

void drawSubjectMaskShapeDraft(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    const SubjectMaskEditSession& edit_session,
    const SubjectMaskBrushState& brush,
    float image_height_px) {
    if (!edit_session.active() || !mask_details.includes_eye_masks ||
        !brush.enabled) {
        return;
    }

    const auto& target = edit_session.target();
    const auto mask_it = std::find_if(
        mask_details.eye_masks.begin(),
        mask_details.eye_masks.end(),
        [&](const ZarrDetectionLoader::FrameDetections::EyeMask& mask) {
            return mask.valid && mask.roi_index == target.roi_index &&
                   mask.roi_width > 0.0f && mask.roi_height > 0.0f &&
                   std::isfinite(mask.offset_x) &&
                   std::isfinite(mask.offset_y);
        });
    if (mask_it == mask_details.eye_masks.end() || target.rows == 0 ||
        target.cols == 0) {
        return;
    }

    const auto& mask = *mask_it;
    const double cell_w = mask.roi_width / static_cast<double>(target.cols);
    const double cell_h = mask.roi_height / static_cast<double>(target.rows);
    auto toPlot = [&](const SubjectMaskRoiPoint& point) {
        const double x =
            mask.offset_x + (static_cast<double>(point.col) + 0.5) * cell_w;
        const double y_source =
            mask.offset_y + (static_cast<double>(point.row) + 0.5) * cell_h;
        return ImPlotPoint(x, static_cast<double>(image_height_px) - y_source);
    };

    const ImVec4 color = brush.erase ? ImVec4(1.0f, 0.35f, 0.25f, 0.95f)
                                     : ImVec4(1.0f, 0.95f, 0.18f, 0.95f);

    if (brush.tool == SubjectMaskPreviewTool::Brush) {
        if (!brush.brush_hover_valid) {
            return;
        }
        constexpr double kTwoPi = 6.28318530717958647692;
        const int sample_count =
            std::clamp(brush.radius_px * 8, 32, 192);
        const ImPlotPoint center = toPlot(brush.brush_hover);
        const double radius_x =
            static_cast<double>(std::max(1, brush.radius_px)) * cell_w;
        const double radius_y =
            static_cast<double>(std::max(1, brush.radius_px)) * cell_h;
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(static_cast<size_t>(sample_count));
        ys.reserve(static_cast<size_t>(sample_count));
        for (int i = 0; i < sample_count; ++i) {
            const double theta =
                kTwoPi * static_cast<double>(i) /
                static_cast<double>(sample_count);
            xs.push_back(center.x + std::cos(theta) * radius_x);
            ys.push_back(center.y + std::sin(theta) * radius_y);
        }
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                                   2.6f,
                                   color,
                                   1.0f,
                                   color);
        ImPlot::PlotScatter("##subject_mask_brush_footprint",
                            xs.data(),
                            ys.data(),
                            static_cast<int>(xs.size()));
        return;
    }

    std::vector<SubjectMaskRoiPoint> points;
    bool close_shape = false;
    if (brush.tool == SubjectMaskPreviewTool::Lasso && brush.lasso_active) {
        points = brush.lasso_points;
        close_shape = points.size() >= 3;
    } else if (brush.tool == SubjectMaskPreviewTool::Polygon) {
        points = brush.polygon_points;
        if (brush.polygon_hover_valid) {
            if (points.empty() || !sameRoiPoint(points.back(),
                                                brush.polygon_hover)) {
                points.push_back(brush.polygon_hover);
            }
        }
        close_shape = points.size() >= 3;
    }
    if (points.size() < 2) {
        return;
    }

    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(points.size() * 8);
    ys.reserve(points.size() * 8);
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const ImPlotPoint a = toPlot(points[i]);
        const ImPlotPoint b = toPlot(points[i + 1]);
        appendDottedSegment(xs, ys, a.x, a.y, b.x, b.y);
    }
    if (close_shape) {
        const ImPlotPoint a = toPlot(points.back());
        const ImPlotPoint b = toPlot(points.front());
        appendDottedSegment(xs, ys, a.x, a.y, b.x, b.y);
    }
    if (xs.empty()) {
        return;
    }

    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,
                               2.8f,
                               color,
                               1.0f,
                               color);
    ImPlot::PlotScatter("##subject_mask_shape_draft",
                        xs.data(),
                        ys.data(),
                        static_cast<int>(xs.size()));
}

void cameraTextureDrawTraceCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    if (cmd == nullptr || cmd->UserCallbackData == nullptr) {
        return;
    }
    auto* camera = static_cast<CameraResources*>(cmd->UserCallbackData);
    auto& trace = camera->texture_draw_trace;
    if (!trace.enabled || trace.queued_texture_id == 0) {
        return;
    }

    GLint active_texture = 0;
    GLint bound_texture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_texture);

    trace.callback_observed = true;
    trace.callback_count++;
    trace.callback_active_texture = active_texture;
    trace.callback_bound_texture_id = static_cast<GLuint>(bound_texture);
    trace.callback_bound_matches_queued =
        static_cast<GLuint>(bound_texture) == trace.queued_texture_id;
}

void queueCameraTextureDrawTrace(CameraResources& camera, int view_idx) {
    static uint64_t next_sequence = 1;
    auto& trace = camera.texture_draw_trace;
    trace.enabled = true;
    trace.queue_sequence = next_sequence++;
    trace.view_idx = view_idx;
    trace.queued_texture_id = camera.image_texture;
    trace.front_texture_id = camera.image_texture;
    trace.staging_texture_id = camera.playback_staging_texture;
    trace.front_pbo_id = camera.pbo_cuda.pbo;
    trace.staging_pbo_id = camera.playback_staging_pbo.pbo;

    trace.front_valid = camera.texture_has_valid_frame;
    trace.front_parent_frame = camera.last_uploaded_frame;
    trace.front_local_frame = camera.last_uploaded_local_frame;
    trace.front_pts = camera.last_uploaded_pts;

    trace.staging_valid = camera.playback_staging_valid;
    trace.staging_parent_frame = camera.playback_staging_frame;
    trace.staging_local_frame = camera.playback_staging_local_frame;
    trace.staging_pts = camera.playback_staging_pts;

    trace.callback_observed = false;
    trace.callback_count = 0;
    trace.callback_active_texture = 0;
    trace.callback_bound_texture_id = 0;
    trace.callback_bound_matches_queued = false;
}

}  // namespace

CameraViewWindowResult drawCameraViewWindowContents(
    const CameraViewWindowContext& context) {
    CameraViewWindowResult result;
    result.full_frame_edit_result.state = context.full_frame_edit_state;
    result.full_frame_keypoint_edit_state = context.full_frame_keypoint_edit_state;
    result.transport_result.slider_frame_number =
        context.transport_controls.slider_frame_number;
    CameraViewSubjectMaskPreview subject_mask_preview =
        context.subject_mask_preview;

    if (context.scene == nullptr || context.view_idx < 0 ||
        context.view_idx >= static_cast<int>(context.scene->num_cams)) {
        return result;
    }

    auto& camera = context.scene->cameras[context.view_idx];
    const auto scene_ui_build_start = std::chrono::steady_clock::now();
    ImGui::BeginGroup();
    std::string scene_name = "scene view" + std::to_string(context.view_idx);
    ImGui::BeginChild(scene_name.c_str(),
                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    ImVec2 avail_size = ImGui::GetContentRegionAvail();

    if (context.use_legacy_manual_keypoint_tools &&
        context.legacy_labeling_state != nullptr) {
        context.legacy_labeling_state->keypoints_find =
            context.legacy_labeling_state->hasFrameKeypoints(
                context.current_frame_num);
    }

    ImPlotInputMap& plot_input_map = ImPlot::GetInputMap();
    const int previous_plot_pan_mod = plot_input_map.PanMod;
    bool restore_plot_pan_mod = false;
    bool suppress_crosshairs = false;
    const bool subject_mask_brush_input_enabled =
        subjectMaskBrushInputEnabled(context);
    if (context.zarr_loaded && context.dataset_allows_bbox_edit &&
        context.bbox_edit_state != nullptr) {
        const bool draw_mode_active_for_current_frame =
            context.bbox_edit_state->draw_mode;
        const bool has_bbox_selection_for_current_frame =
            (context.bbox_edit_state->selected_frame ==
             context.current_frame_num) &&
            (context.bbox_edit_state->selected_box >= 0);
        if (context.bbox_edit_state->enabled &&
            (draw_mode_active_for_current_frame ||
             has_bbox_selection_for_current_frame)) {
            plot_input_map.PanMod = ImGuiMod_Shift;
            restore_plot_pan_mod = true;
            suppress_crosshairs = true;
        }
    }
    if (context.full_frame_keypoint_edit_enabled ||
        subject_mask_brush_input_enabled) {
        plot_input_map.PanMod = ImGuiMod_Shift;
        restore_plot_pan_mod = true;
        suppress_crosshairs = true;
    }

    const bool lightweight_playback_renderer_active =
        context.lightweight_playback_renderer_active;
    ImPlotFlags scene_plot_flags = ImPlotFlags_Equal;
    if (!suppress_crosshairs && !lightweight_playback_renderer_active) {
        scene_plot_flags |= ImPlotFlags_Crosshairs;
    }
    if (lightweight_playback_renderer_active) {
        scene_plot_flags |= ImPlotFlags_CanvasOnly | ImPlotFlags_NoFrame;
    } else {
        scene_plot_flags |= ImPlotAxisFlags_AutoFit;
    }

    int scene_plot_style_var_count = 0;
    int scene_plot_style_color_count = 0;
    if (lightweight_playback_renderer_active) {
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0.0f, 0.0f));
        scene_plot_style_var_count++;
        ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0.0f, 0.0f));
        scene_plot_style_var_count++;
        ImPlot::PushStyleColor(ImPlotCol_PlotBg,
                               ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        scene_plot_style_color_count++;
        ImPlot::PushStyleColor(ImPlotCol_PlotBorder,
                               ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        scene_plot_style_color_count++;
    } else {
        ImPlot::PushStyleVar(ImPlotStyleVar_LegendPadding,
                             ImVec2(12.0f, 12.0f));
        scene_plot_style_var_count++;
    }

    const auto camera_plot_image_ui_start = std::chrono::steady_clock::now();
    if (ImPlot::BeginPlot("##no_plot_name", avail_size, scene_plot_flags)) {
        if (lightweight_playback_renderer_active) {
            constexpr ImPlotAxisFlags kPlaybackAxisFlags =
                ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_NoMenus |
                ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoSideSwitch;
            ImPlot::SetupAxes(nullptr, nullptr, kPlaybackAxisFlags,
                              kPlaybackAxisFlags);
            ImPlot::SetupAxesLimits(
                0, static_cast<double>(camera.image_width), 0,
                static_cast<double>(camera.image_height), ImGuiCond_Once);
        } else {
            ImPlot::SetupLegend(ImPlotLocation_NorthWest,
                                ImPlotLegendFlags_None);
        }

        if (context.capture_texture_draw_trace) {
            queueCameraTextureDrawTrace(camera, context.view_idx);
        } else {
            camera.texture_draw_trace.enabled = false;
        }

        ImPlot::PlotImage("##no_image_name",
                          (ImTextureID)(intptr_t)camera.image_texture,
                          ImVec2(0, 0),
                          ImVec2(camera.image_width, camera.image_height));
        if (context.capture_texture_draw_trace) {
            ImPlot::GetPlotDrawList()->AddCallback(
                cameraTextureDrawTraceCallback, &camera);
        }

        const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
        const ImVec2 plot_pos = ImPlot::GetPlotPos();
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        const double image_width = static_cast<double>(camera.image_width);
        const double image_height = static_cast<double>(camera.image_height);
        const ImVec2 media_corner_a = ImPlot::PlotToPixels(0.0, 0.0);
        const ImVec2 media_corner_b =
            ImPlot::PlotToPixels(image_width, image_height);
        const double media_x_min = std::clamp(
            static_cast<double>(std::min(media_corner_a.x, media_corner_b.x)),
            static_cast<double>(plot_pos.x),
            static_cast<double>(plot_pos.x + plot_size.x));
        const double media_x_max = std::clamp(
            static_cast<double>(std::max(media_corner_a.x, media_corner_b.x)),
            static_cast<double>(plot_pos.x),
            static_cast<double>(plot_pos.x + plot_size.x));
        const double media_y_min = std::clamp(
            static_cast<double>(std::min(media_corner_a.y, media_corner_b.y)),
            static_cast<double>(plot_pos.y),
            static_cast<double>(plot_pos.y + plot_size.y));
        const double media_y_max = std::clamp(
            static_cast<double>(std::max(media_corner_a.y, media_corner_b.y)),
            static_cast<double>(plot_pos.y),
            static_cast<double>(plot_pos.y + plot_size.y));
        const double clamped_x_min =
            std::clamp(plot_limits.X.Min, 0.0, image_width);
        const double clamped_x_max =
            std::clamp(plot_limits.X.Max, 0.0, image_width);
        const double clamped_y_min =
            std::clamp(plot_limits.Y.Min, 0.0, image_height);
        const double clamped_y_max =
            std::clamp(plot_limits.Y.Max, 0.0, image_height);
        const double visible_width = std::max(0.0, clamped_x_max - clamped_x_min);
        const double visible_height =
            std::max(0.0, clamped_y_max - clamped_y_min);
        const double total_area = image_width * image_height;
        const double visible_area = visible_width * visible_height;
        const double visible_fraction =
            total_area > 0.0
                ? std::clamp(visible_area / total_area, 0.0, 1.0)
                : std::numeric_limits<double>::quiet_NaN();
        const bool zoomed_in = visible_width < (image_width - 1.0) ||
                               visible_height < (image_height - 1.0);
        result.perf.viewport_x_px = static_cast<double>(plot_pos.x);
        result.perf.viewport_y_px = static_cast<double>(plot_pos.y);
        result.perf.viewport_width_px = static_cast<double>(plot_size.x);
        result.perf.viewport_height_px = static_cast<double>(plot_size.y);
        result.perf.media_x_px = media_x_min;
        result.perf.media_y_px = media_y_min;
        result.perf.media_width_px = std::max(0.0, media_x_max - media_x_min);
        result.perf.media_height_px = std::max(0.0, media_y_max - media_y_min);
        result.perf.view_x_min = clamped_x_min;
        result.perf.view_x_max = clamped_x_max;
        result.perf.view_y_min = clamped_y_min;
        result.perf.view_y_max = clamped_y_max;
        result.perf.visible_fraction = visible_fraction;
        result.perf.zoomed_in = zoomed_in ? 1 : 0;

        result.perf.plot_image_ui_ms += durationMs(
            std::chrono::steady_clock::now() - camera_plot_image_ui_start);

        const auto camera_overlay_ui_start = std::chrono::steady_clock::now();

        if (context.has_yolo_detections && context.yolo_boxes != nullptr &&
            context.yolo_labels != nullptr &&
            context.yolo_class_ids != nullptr) {
            drawCvContours(*context.yolo_boxes, *context.yolo_labels,
                           *context.yolo_class_ids, camera.image_height);
        }

        if (context.zarr_loaded) {
            int valid_slots = 0;
            for (int slot_idx = 0; slot_idx < context.scene->size_of_buffer;
                 ++slot_idx) {
                const auto& slot = camera.display_buffer[slot_idx];
                if (!slot.available_to_write && slot.frame_number >= 0) {
                    ++valid_slots;
                }
            }
            const int total_slots =
                static_cast<int>(context.scene->size_of_buffer);
            const int empty_slots = std::max(0, total_slots - valid_slots);
            int recording_remaining = -1;
            if (context.total_recording_frames > 0) {
                if (context.latest_decoded_frame < 0) {
                    recording_remaining = context.total_recording_frames;
                } else {
                    const int64_t total_recording_frames =
                        static_cast<int64_t>(context.total_recording_frames);
                    const int64_t decoded_through_frame =
                        static_cast<int64_t>(context.latest_decoded_frame) + 1;
                    const int64_t remaining_frames =
                        total_recording_frames - decoded_through_frame;
                    recording_remaining = static_cast<int>(std::max<int64_t>(
                        0, remaining_frames));
                }
            }

            std::ostringstream sync_debug;
            sync_debug << "cam=" << context.camera_name
                       << " mode=" << (context.play_video ? "play" : "pause")
                       << " slot=" << context.presented_slot
                       << " displayed=" << context.presented_frame
                       << " current=" << context.current_frame_num
                       << " target="
                       << context.transport_controls.current_display_frame
                       << " slider="
                       << context.transport_controls.slider_frame_number
                       << " bbox_query=" << context.current_frame_num
                       << " empty_remaining=" << empty_slots
                       << " recording_remaining=" << recording_remaining
                       << " latest_decoded=" << context.latest_decoded_frame;

            result.frame_sync.valid_slots = valid_slots;
            result.frame_sync.empty_slots = empty_slots;
            result.frame_sync.latest_decoded = context.latest_decoded_frame;
            result.frame_sync.recording_remaining = recording_remaining;
            result.frame_sync.recording_total = context.total_recording_frames;
                    result.frame_sync.debug_line = sync_debug.str();

                    const float image_height_px = static_cast<float>(camera.image_height);
                    const bool plot_hovered = ImPlot::IsPlotHovered();
                    if (context.full_frame_keypoint_edit_enabled) {
                        result.full_frame_edit_result.state.draw_mode = false;
                        result.full_frame_edit_result.state.draw_active = false;
                        result.full_frame_edit_result.state.drag_active = false;
                        result.full_frame_edit_result.state.drag_mouse_button = -1;
                    }
                    const FullFrameKeypointEditContext keypoint_edit_context{
                        context.selected_keypoint_selection,
                        context.detection_details,
                        context.heading_spec,
                        static_cast<float>(camera.image_width),
                        image_height_px,
                        plot_hovered,
                        context.play_video,
                    };
                    if (context.full_frame_keypoint_edit_enabled) {
                        const auto keypoint_edit_result =
                            processFullFrameKeypointEditOverlay(
                                keypoint_edit_context,
                                context.full_frame_keypoint_edit_state);
                        result.full_frame_keypoint_edit_state =
                            keypoint_edit_result.state;
                    } else {
                        result.full_frame_keypoint_edit_state.active_handle = -1;
                    }
                    if (subject_mask_brush_input_enabled &&
                        context.mask_details != nullptr &&
                        !context.full_frame_keypoint_edit_enabled &&
                        !context.play_video &&
                        !ImGui::GetIO().KeyCtrl &&
                        !ImGui::GetIO().KeyShift &&
                        !ImGui::GetIO().KeyAlt) {
                        SubjectMaskBrushState& brush =
                            *context.subject_mask_brush_state;
                        SubjectMaskEditSession& edit_session =
                            *context.subject_mask_edit_session;
                        const SubjectMaskPaintPoint point =
                            plot_hovered
                                ? subjectMaskPaintPointAtPlotPoint(
                                      *context.mask_details,
                                      edit_session,
                                      image_height_px,
                                      ImPlot::GetPlotMousePos())
                                : SubjectMaskPaintPoint{};
                        brush.brush_hover_valid =
                            brush.tool == SubjectMaskPreviewTool::Brush &&
                            point.valid;
                        brush.brush_hover =
                            point.valid ? roiPoint(point) : SubjectMaskRoiPoint{};
                        brush.polygon_hover_valid =
                            brush.tool == SubjectMaskPreviewTool::Polygon &&
                            point.valid;
                        brush.polygon_hover =
                            point.valid ? roiPoint(point) : SubjectMaskRoiPoint{};
                        const bool mouse_down =
                            ImGui::IsMouseDown(ImGuiMouseButton_Left);
                        if (brush.tool == SubjectMaskPreviewTool::Brush &&
                            mouse_down) {
                            result.subject_mask_paint.attempted = true;
                            if (point.valid) {
                                const uint8_t value = brush.erase ? 0 : 1;
                                bool changed = false;
                                if (brush.stroke_active &&
                                    brush.last_row >= 0 &&
                                    brush.last_col >= 0) {
                                    changed = edit_session.stampLine(
                                        brush.last_row,
                                        brush.last_col,
                                        point.row,
                                        point.col,
                                        brush.radius_px,
                                        value);
                                } else {
                                    changed = edit_session.stampDisk(
                                        point.row,
                                        point.col,
                                        brush.radius_px,
                                        value);
                                }
                                brush.stroke_active = true;
                                brush.last_row = point.row;
                                brush.last_col = point.col;
                                result.subject_mask_paint.changed =
                                    result.subject_mask_paint.changed ||
                                    changed;
                                populatePaintResult(result.subject_mask_paint,
                                                    edit_session,
                                                    point,
                                                    changed);
                                refreshSubjectMaskPreviewFromSession(
                                    context.subject_mask_edit_session,
                                    subject_mask_preview);
                            } else {
                                brush.stroke_active = false;
                                brush.last_row = -1;
                                brush.last_col = -1;
                            }
                        } else if (brush.stroke_active) {
                            brush.stroke_active = false;
                            brush.last_row = -1;
                            brush.last_col = -1;
                            result.subject_mask_paint.stroke_finished = true;
                        }
                        if (brush.tool == SubjectMaskPreviewTool::Lasso) {
                            if (mouse_down) {
                                result.subject_mask_paint.attempted = true;
                                if (point.valid) {
                                    if (!brush.lasso_active) {
                                        brush.lasso_active = true;
                                        brush.lasso_points.clear();
                                    }
                                    appendRoiPointIfSeparated(
                                        brush.lasso_points,
                                        point,
                                        2);
                                    populatePaintResult(
                                        result.subject_mask_paint,
                                        edit_session,
                                        point,
                                        false);
                                }
                            } else if (brush.lasso_active) {
                                result.subject_mask_paint.attempted = true;
                                const bool changed = applyShapeDraft(
                                    edit_session,
                                    brush.lasso_points,
                                    brush.erase);
                                resetLassoDraft(brush);
                                result.subject_mask_paint.stroke_finished = true;
                                result.subject_mask_paint.changed =
                                    result.subject_mask_paint.changed ||
                                    changed;
                                result.subject_mask_paint.roi_index =
                                    edit_session.target().roi_index;
                                result.subject_mask_paint.component_name =
                                    edit_session.target().component_name;
                                refreshSubjectMaskPreviewFromSession(
                                    context.subject_mask_edit_session,
                                    subject_mask_preview);
                            }
                        } else {
                            resetLassoDraft(brush);
                        }
                        if (brush.tool == SubjectMaskPreviewTool::Polygon) {
                            if (ImGui::IsMouseClicked(
                                    ImGuiMouseButton_Right, false)) {
                                brush.polygon_points.clear();
                                brush.polygon_hover_valid = false;
                                result.subject_mask_paint.stroke_finished = true;
                            } else if (point.valid &&
                                       ImGui::IsMouseDoubleClicked(
                                           ImGuiMouseButton_Left)) {
                                appendRoiPointIfSeparated(
                                    brush.polygon_points,
                                    point,
                                    1);
                                const bool changed = applyShapeDraft(
                                    edit_session,
                                    brush.polygon_points,
                                    brush.erase);
                                brush.polygon_hover_valid = false;
                                populatePaintResult(result.subject_mask_paint,
                                                    edit_session,
                                                    point,
                                                    changed);
                                result.subject_mask_paint.attempted = true;
                                result.subject_mask_paint.stroke_finished = true;
                                refreshSubjectMaskPreviewFromSession(
                                    context.subject_mask_edit_session,
                                    subject_mask_preview);
                            } else if (point.valid &&
                                       ImGui::IsMouseClicked(
                                           ImGuiMouseButton_Left, false)) {
                                appendRoiPointIfSeparated(
                                    brush.polygon_points,
                                    point,
                                    1);
                                populatePaintResult(result.subject_mask_paint,
                                                    edit_session,
                                                    point,
                                                    false);
                                result.subject_mask_paint.attempted = true;
                            }
                        } else {
                            brush.polygon_hover_valid = false;
                        }
                    } else if (context.subject_mask_brush_state != nullptr &&
                               context.subject_mask_brush_state
                                   ->stroke_active) {
                        context.subject_mask_brush_state->stroke_active = false;
                        context.subject_mask_brush_state->last_row = -1;
                        context.subject_mask_brush_state->last_col = -1;
                        result.subject_mask_paint.stroke_finished = true;
                    } else if (context.subject_mask_brush_state != nullptr &&
                               context.subject_mask_brush_state->lasso_active) {
                        resetLassoDraft(*context.subject_mask_brush_state);
                        result.subject_mask_paint.stroke_finished = true;
                    } else if (context.subject_mask_brush_state != nullptr) {
                        context.subject_mask_brush_state->brush_hover_valid =
                            false;
                        context.subject_mask_brush_state->polygon_hover_valid =
                            false;
                    }
                    const bool can_modify_boxes =
                        context.dataset_allows_bbox_edit && context.bbox_edit_enabled &&
                        (context.bbox_allow_edit_while_playing || !context.play_video);

            std::vector<FullFrameRect> editable_rects;
            if (context.zarr_boxes != nullptr) {
                editable_rects.reserve(context.zarr_boxes->size());
                for (const auto& box : *context.zarr_boxes) {
                    editable_rects.push_back(
                        {box.x_min, box.y_min, box.width, box.height});
                }
            }

            const FullFrameRectEditContext full_frame_edit_context{
                context.current_frame_num,
                static_cast<float>(camera.image_width),
                image_height_px,
                plot_hovered,
                !context.full_frame_keypoint_edit_enabled &&
                    !context.subject_mask_pick_enabled &&
                    !subject_mask_brush_input_enabled,
                context.dataset_allows_bbox_edit,
                can_modify_boxes,
                &editable_rects,
                6.0f,
            };
            result.full_frame_edit_result = processFullFrameRectEditInput(
                full_frame_edit_context, context.full_frame_edit_state);

            crimson::overlay::ReadOnlyOverlayScene bounding_box_overlay_scene;
            if (context.zarr_boxes != nullptr &&
                context.detection_details != nullptr &&
                context.bbox_edit_state != nullptr &&
                !context.zarr_boxes->empty()) {
                const auto bbox_overlay_build_start =
                    std::chrono::steady_clock::now();
                const bool frame_has_bbox_edits =
                    context.bbox_edit_state->isFrameDirty(
                        context.current_frame_num);
                bounding_box_overlay_scene =
                    buildCameraViewBoundingBoxOverlayScene(
                        *context.zarr_boxes,
                        *context.detection_details,
                        *context.bbox_edit_state,
                        context.view_idx,
                        context.presented_frame,
                        context.current_frame_num,
                        static_cast<float>(camera.image_width),
                        image_height_px,
                        frame_has_bbox_edits,
                        context.active_dataset_has_synthetic_detections,
                        context.frame_is_interpolated);
                result.perf.bbox_overlay_build_ms += durationMs(
                    std::chrono::steady_clock::now() -
                    bbox_overlay_build_start);
                result.perf.bbox_overlay_item_count +=
                    static_cast<int>(bounding_box_overlay_scene.count(
                        CameraOverlayLayer::BoundingBoxes));
            }

            const std::string draft_label_suffix =
                std::to_string(context.view_idx);
            for (const CameraOverlayLayer layer :
                 crimson::overlay::kCameraOverlayLayerOrder) {
                switch (layer) {
                    case CameraOverlayLayer::BoundingBoxes:
                        if (bounding_box_overlay_scene.ready() &&
                            !bounding_box_overlay_scene.primitives.empty()) {
                            const auto bbox_overlay_draw_start =
                                std::chrono::steady_clock::now();
                            drawCameraViewReadOnlyOverlayScene(
                                bounding_box_overlay_scene, image_height_px);
                            result.perf.bbox_overlay_draw_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                bbox_overlay_draw_start);
                        }
                        break;
                    case CameraOverlayLayer::BoundingBoxDraft:
                        drawFullFrameRectDraftOverlay(
                            result.full_frame_edit_result.state,
                            context.current_frame_num,
                            image_height_px,
                            draft_label_suffix.c_str());
                        break;
                    case CameraOverlayLayer::Chaser:
                        if (context.chaser_bboxes != nullptr &&
                            context.chaser_states != nullptr &&
                            context.camera_params != nullptr) {
                            drawCameraViewChaserOverlay(
                                *context.chaser_bboxes,
                                *context.chaser_states,
                                *context.camera_params,
                                static_cast<int>(camera.image_width),
                                static_cast<int>(camera.image_height));
                        }
                        break;
                    case CameraOverlayLayer::MovementTrail:
                        if (context.movement_trail != nullptr) {
                            drawCameraViewMovementTrailOverlay(
                                *context.movement_trail,
                                image_height_px);
                        }
                        break;
                    case CameraOverlayLayer::KeypointHeading:
                        if (context.can_draw_headings &&
                            context.keypoint_descriptor != nullptr &&
                            context.keypoint_frame != nullptr) {
                            drawCameraViewHeadingOverlay(
                                *context.keypoint_descriptor,
                                *context.keypoint_frame,
                                static_cast<float>(camera.image_width),
                                image_height_px,
                                context.view_idx,
                                context.presented_frame);
                        }
                        break;
                    case CameraOverlayLayer::MovementLabel:
                        if (context.movement_sample != nullptr) {
                            drawCameraViewMovementOverlay(
                                *context.movement_sample,
                                context.detection_details,
                                image_height_px);
                        }
                        break;
                    case CameraOverlayLayer::SubjectMasks:
                        if (context.can_draw_eye_masks &&
                            context.subject_mask_scene != nullptr) {
                            drawCameraViewReadOnlyOverlayScene(
                                *context.subject_mask_scene, image_height_px,
                                &result.perf.mask_overlay);
                        }
                        if (context.can_draw_eye_masks &&
                            context.mask_details != nullptr) {
                            CameraViewMaskPerfMetrics mask_perf =
                                drawCameraViewEyeMaskOverlay(
                                    *context.mask_details,
                                    context.subject_shape_details,
                                    image_height_px,
                                    context.eye_mask_smoothing_run_id,
                                    context.mask_overlay_options,
                                    &subject_mask_preview,
                                    context.subject_mask_scene == nullptr);
                            accumulateCameraViewMaskPerfMetrics(
                                result.perf.mask_overlay, mask_perf);
                            if (context.subject_mask_brush_input_enabled &&
                                context.subject_mask_edit_session != nullptr &&
                                context.subject_mask_brush_state != nullptr) {
                                drawSubjectMaskShapeDraft(
                                    *context.mask_details,
                                    *context.subject_mask_edit_session,
                                    *context.subject_mask_brush_state,
                                    image_height_px);
                            }
                        }
                        break;
                    case CameraOverlayLayer::SubjectShape:
                        if (context.subject_shape_details != nullptr) {
                            const auto subject_shape_overlay_start =
                                std::chrono::steady_clock::now();
                            drawCameraViewSubjectShapeOverlay(
                                *context.subject_shape_details,
                                image_height_px,
                                context.subject_shape_overlay_options);
                            result.perf.subject_shape_overlay_ms +=
                                durationMs(
                                    std::chrono::steady_clock::now() -
                                    subject_shape_overlay_start);
                        }
                        break;
                    case CameraOverlayLayer::TailKinematics:
                        if (context.subject_shape_details != nullptr &&
                            context.tail_kinematics != nullptr) {
                            const auto tail_kinematics_overlay_start =
                                std::chrono::steady_clock::now();
                            drawCameraViewTailKinematicsOverlay(
                                *context.subject_shape_details,
                                *context.tail_kinematics,
                                image_height_px,
                                context.tail_kinematics_overlay_options);
                            result.perf.tail_kinematics_overlay_ms +=
                                durationMs(
                                    std::chrono::steady_clock::now() -
                                    tail_kinematics_overlay_start);
                        }
                        break;
                    case CameraOverlayLayer::SubjectMaskPicking:
                        if (context.subject_mask_pick_enabled &&
                            context.mask_details != nullptr && plot_hovered &&
                            !context.full_frame_keypoint_edit_enabled &&
                            !ImGui::GetIO().KeyCtrl &&
                            !ImGui::GetIO().KeyShift &&
                            !ImGui::GetIO().KeyAlt &&
                            ImGui::IsMouseClicked(
                                ImGuiMouseButton_Left, false)) {
                            const auto pick_start =
                                std::chrono::steady_clock::now();
                            result.subject_mask_pick =
                                pickSubjectMaskAtPlotPoint(
                                    *context.mask_details,
                                    context.mask_overlay_options,
                                    image_height_px,
                                    ImPlot::GetPlotMousePos());
                            result.perf.mask_overlay.pick_attempted = true;
                            result.perf.mask_overlay.pick_hit =
                                result.perf.mask_overlay.pick_hit ||
                                result.subject_mask_pick.valid;
                            result.perf.mask_overlay.pick_ms += durationMs(
                                std::chrono::steady_clock::now() -
                                pick_start);
                        }
                        break;
                    case CameraOverlayLayer::Keypoints:
                        if (context.keypoint_descriptor != nullptr &&
                            context.keypoint_frame != nullptr) {
                            const int keypoint_skip_detection =
                                context.full_frame_keypoint_edit_enabled &&
                                        context.selected_keypoint_selection !=
                                            nullptr &&
                                        context.selected_keypoint_selection
                                            ->valid
                                    ? static_cast<int>(
                                          context.selected_keypoint_selection
                                              ->detection_index)
                                    : -1;
                            drawCameraViewDetectionKeypointMarkers(
                                *context.keypoint_descriptor,
                                *context.keypoint_frame,
                                context.show_keypoint_markers,
                                static_cast<float>(camera.image_width),
                                image_height_px,
                                context.view_idx,
                                context.presented_frame,
                                keypoint_skip_detection);
                        }
                        break;
                }
            }
        }

        const ImVec2 stimulus_plot_size = ImPlot::GetPlotSize();
        const ImVec2 stimulus_plot_pos = ImPlot::GetPlotPos();
        const std::string stimulus_event_text =
            crimson::stimulus::stimulusCameraOverlayEventText(
                context.stimulus_camera_overlay_frame);
        ImVec2 stimulus_event_text_size{};
        if (!stimulus_event_text.empty()) {
            stimulus_event_text_size = ImGui::CalcTextSize(
                stimulus_event_text.c_str(), nullptr, false, -1.0f);
        }
        result.stimulus_camera_overlay_origin_x_px = stimulus_plot_pos.x;
        result.stimulus_camera_overlay_origin_y_px = stimulus_plot_pos.y;
        result.stimulus_camera_overlay_scene =
            crimson::stimulus::buildStimulusCameraOverlayScene(
                context.stimulus_camera_overlay_frame,
                {stimulus_plot_size.x, stimulus_plot_size.y},
                {stimulus_event_text_size.x, stimulus_event_text_size.y});
        drawCameraViewStimulusInsetOverlay(context.stimulus_player,
                                           context.target_stimulus_frame,
                                           context.stimulus_inset_options);
        const ImVec2 polar_plot_size = ImPlot::GetPlotSize();
        const ImVec2 polar_plot_pos = ImPlot::GetPlotPos();
        result.chaser_distance_polar_origin_x_px = polar_plot_pos.x;
        result.chaser_distance_polar_origin_y_px = polar_plot_pos.y;
        result.chaser_distance_polar_scene =
            crimson::polar::buildChaserDistancePolarScene(
                context.chaser_distance_polar_frame,
                {polar_plot_size.x, polar_plot_size.y},
                context.chaser_distance_polar_inset_options);
        drawCameraViewChaserDistancePolarInsetOverlay(
            result.chaser_distance_polar_scene);
        if (context.active_roi_inset_options.visible &&
            (context.mask_details != nullptr ||
             context.active_roi_inset_target.valid)) {
            drawCameraViewActiveRoiInsetOverlay(
                camera.image_texture,
                static_cast<int>(camera.image_width),
                static_cast<int>(camera.image_height),
                context.mask_details,
                context.detection_details,
                context.selected_keypoint_selection,
                &context.active_roi_inset_target,
                context.eye_mask_smoothing_run_id,
                context.mask_overlay_options,
                context.show_keypoint_markers,
                &subject_mask_preview,
                context.active_roi_inset_options);
        }

        if (context.use_legacy_manual_keypoint_tools &&
            context.legacy_labeling_state != nullptr) {
            const CameraViewManualKeypointInputContext keypoint_input_context{
                context.scene,
                context.legacy_labeling_state,
                context.current_frame_num,
                context.view_idx,
                ImPlot::IsPlotHovered(),
            };
            const CameraViewManualKeypointInputResult keypoint_input_result =
                processCameraViewManualKeypointInput(keypoint_input_context);
            result.legacy_manual_keypoints_find =
                keypoint_input_result.legacy_manual_keypoints_find;
            result.view_focused = keypoint_input_result.view_focused;
        }

        ImPlot::EndPlot();
        drawCameraViewStimulusCameraOverlay(
            result.stimulus_camera_overlay_scene,
            result.stimulus_camera_overlay_origin_x_px,
            result.stimulus_camera_overlay_origin_y_px);

        if (context.swap_playback_surface_after_draw) {
            const auto swap_start = std::chrono::steady_clock::now();
            std::swap(camera.image_texture, camera.playback_staging_texture);
            render_refresh_camera_presentation_texture(&camera);
            std::swap(camera.pbo_cuda, camera.playback_staging_pbo);
            std::swap(camera.applied_preview_sampling_mode,
                      camera.playback_staging_preview_sampling_mode);
            const int previous_front_frame = camera.last_uploaded_frame;
            const int previous_front_local_frame =
                camera.last_uploaded_local_frame;
            const int64_t previous_front_pts = camera.last_uploaded_pts;
            const bool previous_front_valid = camera.texture_has_valid_frame;
            camera.last_uploaded_frame = camera.playback_staging_frame;
            camera.last_uploaded_local_frame =
                camera.playback_staging_local_frame;
            camera.last_uploaded_pts = camera.playback_staging_pts;
            camera.texture_has_valid_frame = camera.playback_staging_valid;
            camera.playback_staging_frame = previous_front_frame;
            camera.playback_staging_local_frame = previous_front_local_frame;
            camera.playback_staging_pts = previous_front_pts;
            camera.playback_staging_valid = previous_front_valid;
            result.perf.playback_swap_ms += durationMs(
                std::chrono::steady_clock::now() - swap_start);
        }

        result.perf.overlay_ui_ms += durationMs(
            std::chrono::steady_clock::now() - camera_overlay_ui_start);
    }

    if (restore_plot_pan_mod) {
        plot_input_map.PanMod = previous_plot_pan_mod;
    }
    if (scene_plot_style_color_count > 0) {
        ImPlot::PopStyleColor(scene_plot_style_color_count);
    }
    if (scene_plot_style_var_count > 0) {
        ImPlot::PopStyleVar(scene_plot_style_var_count);
    }

    ImGui::EndChild();
    result.transport_result =
        drawCameraViewTransportControls(context.transport_controls);
    ImGui::EndGroup();
    result.perf.scene_ui_ms += durationMs(
        std::chrono::steady_clock::now() - scene_ui_build_start);
    return result;
}
