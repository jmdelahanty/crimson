#include "gui/camera_view_window.h"

#include "global.h"
#include "gui/camera_view_manual_keypoint_input.h"
#include "gui/camera_view_overlay_renderer.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

double durationMs(std::chrono::steady_clock::duration delta) {
    return std::chrono::duration<double, std::milli>(delta).count();
}

enum class CameraViewOverlayLayer {
    BoundingBoxes,
    BoundingBoxDraft,
    Chaser,
    MovementTrail,
    KeypointHeading,
    MovementLabel,
    SubjectMasks,
    SubjectShape,
    TailKinematics,
    SubjectMaskPicking,
    Keypoints,
};

constexpr std::array<CameraViewOverlayLayer, 11> kCameraViewOverlayOrder = {
    CameraViewOverlayLayer::BoundingBoxes,
    CameraViewOverlayLayer::BoundingBoxDraft,
    CameraViewOverlayLayer::Chaser,
    CameraViewOverlayLayer::MovementTrail,
    CameraViewOverlayLayer::KeypointHeading,
    CameraViewOverlayLayer::MovementLabel,
    CameraViewOverlayLayer::SubjectMasks,
    CameraViewOverlayLayer::SubjectShape,
    CameraViewOverlayLayer::TailKinematics,
    CameraViewOverlayLayer::SubjectMaskPicking,
    CameraViewOverlayLayer::Keypoints,
};

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

}  // namespace

CameraViewWindowResult drawCameraViewWindowContents(
    const CameraViewWindowContext& context) {
    CameraViewWindowResult result;
    result.full_frame_edit_result.state = context.full_frame_edit_state;
    result.full_frame_keypoint_edit_state = context.full_frame_keypoint_edit_state;
    result.transport_result.slider_frame_number =
        context.transport_controls.slider_frame_number;

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
    if (context.full_frame_keypoint_edit_enabled) {
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
            ImPlot::SetupLegend(ImPlotLocation_SouthWest,
                                ImPlotLegendFlags_None);
        }

        ImPlot::PlotImage("##no_image_name",
                          (ImTextureID)(intptr_t)camera.image_texture,
                          ImVec2(0, 0),
                          ImVec2(camera.image_width, camera.image_height));

        const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        const double image_width = static_cast<double>(camera.image_width);
        const double image_height = static_cast<double>(camera.image_height);
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
        result.perf.viewport_width_px = static_cast<double>(plot_size.x);
        result.perf.viewport_height_px = static_cast<double>(plot_size.y);
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
                    !context.subject_mask_pick_enabled,
                context.dataset_allows_bbox_edit,
                can_modify_boxes,
                &editable_rects,
                6.0f,
            };
            result.full_frame_edit_result = processFullFrameRectEditInput(
                full_frame_edit_context, context.full_frame_edit_state);

            std::vector<FullFrameRectOverlayItem> bounding_box_overlay_items;
            if (context.zarr_boxes != nullptr &&
                context.detection_details != nullptr &&
                context.bbox_edit_state != nullptr &&
                !context.zarr_boxes->empty()) {
                const bool frame_has_bbox_edits =
                    context.bbox_edit_state->isFrameDirty(
                        context.current_frame_num);
                bounding_box_overlay_items =
                    buildCameraViewBoundingBoxOverlayItems(
                        *context.zarr_boxes,
                        *context.detection_details,
                        *context.bbox_edit_state,
                        context.current_frame_num,
                        frame_has_bbox_edits,
                        context.active_dataset_has_synthetic_detections,
                        context.frame_is_interpolated);
            }

            const std::string draft_label_suffix =
                std::to_string(context.view_idx);
            for (const CameraViewOverlayLayer layer :
                 kCameraViewOverlayOrder) {
                switch (layer) {
                    case CameraViewOverlayLayer::BoundingBoxes:
                        if (!bounding_box_overlay_items.empty()) {
                            drawFullFrameRectOverlays(
                                bounding_box_overlay_items,
                                image_height_px);
                        }
                        break;
                    case CameraViewOverlayLayer::BoundingBoxDraft:
                        drawFullFrameRectDraftOverlay(
                            result.full_frame_edit_result.state,
                            context.current_frame_num,
                            image_height_px,
                            draft_label_suffix.c_str());
                        break;
                    case CameraViewOverlayLayer::Chaser:
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
                    case CameraViewOverlayLayer::MovementTrail:
                        if (context.movement_trail != nullptr) {
                            drawCameraViewMovementTrailOverlay(
                                *context.movement_trail,
                                image_height_px);
                        }
                        break;
                    case CameraViewOverlayLayer::KeypointHeading:
                        if (context.can_draw_headings &&
                            context.heading_details != nullptr) {
                            drawCameraViewHeadingOverlay(
                                *context.heading_details,
                                image_height_px);
                        }
                        break;
                    case CameraViewOverlayLayer::MovementLabel:
                        if (context.movement_sample != nullptr) {
                            drawCameraViewMovementOverlay(
                                *context.movement_sample,
                                context.detection_details,
                                image_height_px);
                        }
                        break;
                    case CameraViewOverlayLayer::SubjectMasks:
                        if (context.can_draw_eye_masks &&
                            context.mask_details != nullptr) {
                            CameraViewMaskPerfMetrics mask_perf =
                                drawCameraViewEyeMaskOverlay(
                                    *context.mask_details,
                                    context.subject_shape_details,
                                    image_height_px,
                                    context.eye_mask_smoothing_run_id,
                                    context.mask_overlay_options);
                            accumulateCameraViewMaskPerfMetrics(
                                result.perf.mask_overlay, mask_perf);
                        }
                        break;
                    case CameraViewOverlayLayer::SubjectShape:
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
                    case CameraViewOverlayLayer::TailKinematics:
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
                    case CameraViewOverlayLayer::SubjectMaskPicking:
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
                    case CameraViewOverlayLayer::Keypoints:
                        if (context.detection_details != nullptr) {
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
                                *context.detection_details,
                                context.show_keypoint_markers,
                                image_height_px,
                                keypoint_skip_detection);
                        }
                        break;
                }
            }
        }

        if (context.stimulus_events != nullptr) {
            drawCameraViewStimulusEventOverlay(context.view_idx,
                                               context.current_frame_num,
                                               *context.stimulus_events);
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

        if (context.swap_playback_surface_after_draw) {
            const auto swap_start = std::chrono::steady_clock::now();
            std::swap(camera.image_texture, camera.playback_staging_texture);
            std::swap(camera.pbo_cuda, camera.playback_staging_pbo);
            std::swap(camera.applied_preview_sampling_mode,
                      camera.playback_staging_preview_sampling_mode);
            const int previous_front_frame = camera.last_uploaded_frame;
            const bool previous_front_valid = camera.texture_has_valid_frame;
            camera.last_uploaded_frame = camera.playback_staging_frame;
            camera.texture_has_valid_frame = camera.playback_staging_valid;
            camera.playback_staging_frame = previous_front_frame;
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
