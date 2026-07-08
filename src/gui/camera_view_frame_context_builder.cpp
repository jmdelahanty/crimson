#include "gui/camera_view_frame_context_builder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace {

double durationMs(std::chrono::steady_clock::duration delta) {
    return std::chrono::duration<double, std::milli>(delta).count();
}

bool hasActiveSubjectMaskEdit(const FrameDebugWindowState* state) {
    return state != nullptr && state->subject_mask_edit_session.active();
}

int activeSubjectMaskEditRoi(const FrameDebugWindowState* state) {
    if (!hasActiveSubjectMaskEdit(state)) {
        return -1;
    }
    return state->subject_mask_edit_session.target().roi_index;
}

std::string activeSubjectMaskEditComponent(const FrameDebugWindowState* state) {
    if (!hasActiveSubjectMaskEdit(state)) {
        return {};
    }
    return state->subject_mask_edit_session.target().component_name;
}

bool subjectMaskCanvasPickEnabled(const CameraViewFrameContextInput& input) {
    if (!(input.zarr_loaded && input.can_draw_eye_masks &&
          input.zarr_loader != nullptr &&
          input.frame_debug_state != nullptr)) {
        return false;
    }
    const bool brush_editing =
        input.frame_debug_state->subject_mask_brush.enabled &&
        input.frame_debug_state->subject_mask_edit_session.active();
    return !brush_editing &&
           input.frame_debug_state->subject_mask_canvas_pick_enabled &&
           input.frame_debug_state->active_tab == FrameInspectTab::EyeMasks &&
           input.zarr_loader->eyeMasksUseRefinedSubjectMasks();
}

bool subjectMaskBrushInputEnabled(const CameraViewFrameContextInput& input) {
    if (!(input.zarr_loaded && input.can_draw_eye_masks &&
          input.zarr_loader != nullptr &&
          input.frame_debug_state != nullptr)) {
        return false;
    }
    return !input.play_video &&
           input.frame_debug_state->subject_mask_brush.enabled &&
           input.frame_debug_state->subject_mask_edit_session.active() &&
           input.frame_debug_state->active_tab == FrameInspectTab::EyeMasks &&
           input.zarr_loader->eyeMasksUseRefinedSubjectMasks();
}

CameraViewSubjectMaskPreview buildSubjectMaskPreview(
    const SubjectMaskEditSession* session) {
    CameraViewSubjectMaskPreview preview;
    if (session == nullptr || !session->active() || !session->dirty()) {
        return preview;
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
    return preview;
}

CameraViewActiveRoiInsetOptions activeRoiInsetOptions(
    const FrameDebugWindowState* state) {
    CameraViewActiveRoiInsetOptions options;
    if (state == nullptr) {
        options.show_inset = false;
        return options;
    }
    options = state->active_roi_inset_options;
    return options;
}

const RefinedKeypointSelection* activeFullFrameKeypointSelection(
    const CameraViewFrameContextInput& input) {
    if (input.active_full_frame_keypoint_selection == nullptr ||
        !input.active_full_frame_keypoint_selection->has_value()) {
        return nullptr;
    }
    return &**input.active_full_frame_keypoint_selection;
}

bool finitePositiveRoi(float offset_x,
                       float offset_y,
                       float roi_width,
                       float roi_height) {
    return std::isfinite(offset_x) && std::isfinite(offset_y) &&
           std::isfinite(roi_width) && std::isfinite(roi_height) &&
           roi_width > 0.0f && roi_height > 0.0f;
}

CameraViewActiveRoiInsetTarget makeInsetTargetFromRoiMetadata(
    const ZarrDetectionLoader::KeypointRoiMetadata& metadata,
    size_t detection_index,
    const char* source_label) {
    CameraViewActiveRoiInsetTarget target;
    if (!metadata.valid || !metadata.has_crop_metadata ||
        !finitePositiveRoi(metadata.offset_x,
                           metadata.offset_y,
                           metadata.roi_width,
                           metadata.roi_height)) {
        return target;
    }
    target.valid = true;
    target.offset_x = metadata.offset_x;
    target.offset_y = metadata.offset_y;
    target.roi_width = metadata.roi_width;
    target.roi_height = metadata.roi_height;
    target.cols = std::max(1, static_cast<int>(std::round(metadata.roi_width)));
    target.rows = std::max(1, static_cast<int>(std::round(metadata.roi_height)));
    target.roi_index = metadata.roi_index;
    target.detection_index = detection_index;
    target.has_detection_index = true;
    target.source_label = source_label;
    return target;
}

CameraViewActiveRoiInsetTarget activeRoiInsetTarget(
    const CameraViewFrameContextInput& input) {
    const bool zarr_available = input.zarr_loaded && input.zarr_loader != nullptr;
    const RefinedKeypointSelection* selection =
        activeFullFrameKeypointSelection(input);
    if (selection != nullptr && selection->valid &&
        selection->frame_id == static_cast<size_t>(std::max(0, input.current_frame_num))) {
        CameraViewActiveRoiInsetTarget target =
            makeInsetTargetFromRoiMetadata(selection->roi_metadata,
                                           selection->detection_index,
                                           "keypoint ROI");
        if (target.valid) {
            return target;
        }
    }

    auto target_for_detection = [&](size_t detection_index,
                                    const char* source_label) {
        if (zarr_available) {
            CameraViewActiveRoiInsetTarget target =
                makeInsetTargetFromRoiMetadata(
                    input.zarr_loader->getKeypointRoiMetadataForFrameDetection(
                        static_cast<size_t>(std::max(0, input.current_frame_num)),
                        detection_index,
                        false),
                    detection_index,
                    source_label);
            if (target.valid) {
                return target;
            }
        }
        CameraViewActiveRoiInsetTarget target;
        return target;
    };

    if (input.bbox_edit_state != nullptr &&
        input.bbox_edit_state->selected_frame == input.current_frame_num &&
        input.bbox_edit_state->selected_box >= 0 && input.zarr_boxes != nullptr &&
        input.bbox_edit_state->selected_box <
            static_cast<int>(input.zarr_boxes->size())) {
        return target_for_detection(
            static_cast<size_t>(input.bbox_edit_state->selected_box),
            "selected bbox");
    }

    if (input.zarr_boxes != nullptr && input.zarr_boxes->size() == 1) {
        return target_for_detection(0, "single bbox");
    }

    if (input.detection_details != nullptr &&
        input.detection_details->boxes.size() == 1) {
        return target_for_detection(0, "single detection");
    }

    return CameraViewActiveRoiInsetTarget{};
}

}  // namespace

void prepareCameraViewFrameContext(
    const CameraViewFrameContextInput& input,
    PreparedCameraViewFrameContext& prepared) {
    prepared = PreparedCameraViewFrameContext{};

    const bool zarr_available = input.zarr_loaded && input.zarr_loader != nullptr;
    const auto* detection_details = input.detection_details;
    const ZarrDetectionLoader::FrameDetections* heading_details = nullptr;
    const ZarrDetectionLoader::FrameDetections* mask_details = nullptr;

    if (input.can_draw_headings) {
        heading_details = detection_details;
    }

    if (input.can_draw_eye_masks && zarr_available) {
        if (detection_details != nullptr &&
            detection_details->includes_eye_masks) {
            mask_details = detection_details;
        } else {
            const auto mask_load_start = std::chrono::steady_clock::now();
            prepared.mask_details = input.zarr_loader->getRawDetections(
                input.current_frame_num,
                /*use_interpolated=*/false,
                /*include_eye_masks=*/true);
            prepared.mask_data_load_ms += durationMs(
                std::chrono::steady_clock::now() - mask_load_start);
            mask_details = &*prepared.mask_details;
        }
    }

    if (zarr_available && input.zarr_loader->hasStimulusEvents()) {
        prepared.frame_events =
            input.zarr_loader->getStimulusEventsForFrame(
                input.current_frame_num);
    }

    if (zarr_available && input.zarr_loader->hasMovementData()) {
        prepared.movement_frame_sample =
            input.zarr_loader->getMovementSampleForFrame(
                input.current_frame_num);
        if (input.show_movement_trail) {
            prepared.movement_trail_points =
                input.zarr_loader->getMovementTrailForFrame(
                    input.current_frame_num,
                    input.movement_trail_seconds,
                    input.movement_trail_valid_samples_only);
        }
    }

    const bool full_frame_keypoint_edit_enabled =
        zarr_available && input.keypoint_tab_full_frame_edit_enabled;
    const FullFrameKeypointEditState full_frame_keypoint_edit_state =
        input.frame_debug_state != nullptr
            ? input.frame_debug_state->keypoint_review_panel.full_frame_edit
            : FullFrameKeypointEditState{};

    prepared.context = CameraViewWindowContext{
        input.scene,
        input.view_idx,
        input.camera_name,
        input.current_frame_num,
        input.presented_slot,
        input.presented_frame,
        input.swap_playback_surface_after_draw,
        input.play_video,
        input.lightweight_playback_renderer_active,
        input.use_legacy_manual_keypoint_tools,
        input.legacy_labeling_state,
        input.zarr_loaded,
        input.dataset_allows_bbox_edit,
        input.bbox_edit_enabled,
        input.bbox_allow_edit_while_playing,
        input.bbox_edit_state,
        input.full_frame_edit_state,
        zarr_available ? input.zarr_boxes : nullptr,
        zarr_available ? detection_details : nullptr,
        zarr_available ? &input.zarr_loader->getHeadingComputationSpec()
                       : nullptr,
        zarr_available &&
            input.zarr_loader->activeDatasetHasSyntheticDetections(),
        input.frame_is_interpolated,
        input.latest_decoded_frame,
        input.total_recording_frames,
        input.has_yolo_detections,
        input.has_yolo_detections ? input.yolo_boxes : nullptr,
        input.has_yolo_detections ? input.yolo_labels : nullptr,
        input.has_yolo_detections ? input.yolo_class_ids : nullptr,
        input.show_keypoint_markers,
        full_frame_keypoint_edit_enabled &&
            input.view_idx == input.visible_camera_index,
        activeFullFrameKeypointSelection(input),
        full_frame_keypoint_edit_state,
        input.can_draw_headings,
        input.can_draw_eye_masks,
        heading_details,
        mask_details,
        zarr_available ? detection_details : nullptr,
        zarr_available
            ? (input.zarr_loader->getEyeMaskSourcePath() + "|" +
               input.zarr_loader->getEyeAngleRunName())
            : std::string{},
        CameraViewMaskOverlayOptions{
            input.show_subject_body_mask,
            input.show_eye_left_mask,
            input.show_eye_right_mask,
            input.show_swim_bladder_mask,
            input.show_eye_direction_beams,
            input.show_eye_gaze_rays,
            input.show_eye_angle_arcs,
            input.show_eye_angle_labels,
            activeSubjectMaskEditRoi(input.frame_debug_state),
            activeSubjectMaskEditComponent(input.frame_debug_state),
            input.mask_overlay_mode},
        buildSubjectMaskPreview(input.subject_mask_edit_session),
        activeRoiInsetOptions(input.frame_debug_state),
        activeRoiInsetTarget(input),
        input.subject_mask_edit_session,
        input.subject_mask_brush_state,
        input.subject_shape_overlay_options,
        zarr_available && input.zarr_loader->hasTailKinematicsData()
            ? &input.zarr_loader->getTailKinematicsData()
            : nullptr,
        input.tail_kinematics_overlay_options,
        prepared.movement_frame_sample.has_value()
            ? &*prepared.movement_frame_sample
            : nullptr,
        !prepared.movement_trail_points.empty()
            ? &prepared.movement_trail_points
            : nullptr,
        subjectMaskCanvasPickEnabled(input),
        subjectMaskBrushInputEnabled(input),
        zarr_available ? input.chaser_bboxes : nullptr,
        zarr_available ? input.chaser_states : nullptr,
        input.camera_params,
        !prepared.frame_events.empty() ? &prepared.frame_events : nullptr,
        zarr_available
            ? input.zarr_loader->getStimulusStepForFrame(input.current_frame_num)
            : nullptr,
        input.stimulus_player,
        input.target_stimulus_frame,
        input.stimulus_inset_options,
        input.transport_controls,
        input.capture_texture_draw_trace,
    };
}

void applyCameraViewSubjectMaskPick(
    const CameraViewWindowResult& camera_view_result,
    ZarrDetectionLoader& zarr_loader,
    FrameDebugWindowState& frame_debug_state) {
    if (!camera_view_result.subject_mask_pick.valid) {
        return;
    }

    const auto& pick = camera_view_result.subject_mask_pick;
    std::string error;
    if (frame_debug_state.subject_mask_edit_session.startFromLoadedRow(
            zarr_loader,
            static_cast<size_t>(pick.roi_index),
            pick.component_name,
            &error)) {
        frame_debug_state.subject_mask_edit_detection_index =
            pick.detection_index;
        frame_debug_state.subject_mask_edit_component_name =
            pick.component_name;
        const auto& target =
            frame_debug_state.subject_mask_edit_session.target();
        std::ostringstream status;
        status << "Preview loaded from canvas: detection="
               << pick.detection_index << " roi=" << target.roi_index
               << " component=" << target.component_name
               << " shape=" << target.rows << "x" << target.cols;
        frame_debug_state.subject_mask_edit_status = status.str();
    } else {
        frame_debug_state.subject_mask_edit_status =
            "Canvas pick failed: " + error;
    }
}

void applyCameraViewSubjectMaskPaint(
    const CameraViewWindowResult& camera_view_result,
    FrameDebugWindowState& frame_debug_state) {
    const auto& paint = camera_view_result.subject_mask_paint;
    if (paint.stroke_finished) {
        frame_debug_state.subject_mask_brush.stroke_active = false;
        frame_debug_state.subject_mask_brush.last_row = -1;
        frame_debug_state.subject_mask_brush.last_col = -1;
    }
    if (!paint.changed) {
        return;
    }

    std::ostringstream status;
    status << "Preview dirty: roi=" << paint.roi_index
           << " component=" << paint.component_name
           << " tool=";
    switch (frame_debug_state.subject_mask_brush.tool) {
    case SubjectMaskPreviewTool::Brush:
        status << "brush";
        break;
    case SubjectMaskPreviewTool::Lasso:
        status << "lasso";
        break;
    case SubjectMaskPreviewTool::Polygon:
        status << "polygon";
        break;
    }
    status << " mode="
           << (frame_debug_state.subject_mask_brush.erase ? "erase" : "paint");
    if (paint.row >= 0 && paint.col >= 0) {
        status << " at row=" << paint.row << " col=" << paint.col;
    }
    frame_debug_state.subject_mask_edit_status = status.str();
}
