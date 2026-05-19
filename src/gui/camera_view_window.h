#pragma once

#include "camera.h"
#include "gui/camera_view_transport_controls.h"
#include "gui/full_frame_keypoint_edit_overlay.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "gui/camera_view_overlay_renderer.h"
#include "legacy_labeling_state.h"
#include "render.h"
#include "refined_keypoint_repository.h"
#include "subject_mask_edit_session.h"
#include "zarr_bbox_edit.h"
#include "zarr_loader.h"

#include <opencv2/core.hpp>

#include <limits>
#include <string>
#include <vector>

struct CameraViewWindowPerfMetrics {
    double playback_swap_ms = 0.0;
    double plot_image_ui_ms = 0.0;
    double overlay_ui_ms = 0.0;
    double bbox_overlay_build_ms = 0.0;
    double bbox_overlay_draw_ms = 0.0;
    int bbox_overlay_item_count = 0;
    double subject_shape_overlay_ms = 0.0;
    double tail_kinematics_overlay_ms = 0.0;
    double scene_ui_ms = 0.0;
    CameraViewMaskPerfMetrics mask_overlay;
    double viewport_width_px = std::numeric_limits<double>::quiet_NaN();
    double viewport_height_px = std::numeric_limits<double>::quiet_NaN();
    double view_x_min = std::numeric_limits<double>::quiet_NaN();
    double view_x_max = std::numeric_limits<double>::quiet_NaN();
    double view_y_min = std::numeric_limits<double>::quiet_NaN();
    double view_y_max = std::numeric_limits<double>::quiet_NaN();
    double visible_fraction = std::numeric_limits<double>::quiet_NaN();
    int zoomed_in = -1;
};

struct CameraViewFrameSyncSummary {
    std::string debug_line;
    int valid_slots = -1;
    int empty_slots = -1;
    int latest_decoded = -1;
    int recording_remaining = -1;
    int recording_total = -1;
};

struct CameraViewWindowContext {
    render_scene* scene = nullptr;
    int view_idx = 0;
    std::string camera_name;
    int current_frame_num = 0;
    int presented_slot = -1;
    int presented_frame = -1;
    bool swap_playback_surface_after_draw = false;

    bool play_video = false;
    bool lightweight_playback_renderer_active = false;
    bool use_legacy_manual_keypoint_tools = false;
    LegacyLabelingState* legacy_labeling_state = nullptr;

    bool zarr_loaded = false;
    bool dataset_allows_bbox_edit = false;
    bool bbox_edit_enabled = false;
    bool bbox_allow_edit_while_playing = false;
    const ZarrBBoxEditState* bbox_edit_state = nullptr;
    FullFrameRectEditStateView full_frame_edit_state;
    const std::vector<LoggedBoundingBox>* zarr_boxes = nullptr;
    const ZarrDetectionLoader::FrameDetections* detection_details = nullptr;
    const KeypointHeadingComputationSpec* heading_spec = nullptr;
    bool active_dataset_has_synthetic_detections = false;
    bool frame_is_interpolated = false;
    int latest_decoded_frame = -1;
    int total_recording_frames = -1;

    bool has_yolo_detections = false;
    const std::vector<cv::Rect>* yolo_boxes = nullptr;
    const std::vector<std::string>* yolo_labels = nullptr;
    const std::vector<int>* yolo_class_ids = nullptr;

    bool show_keypoint_markers = false;
    bool full_frame_keypoint_edit_enabled = false;
    const RefinedKeypointSelection* selected_keypoint_selection = nullptr;
    FullFrameKeypointEditState full_frame_keypoint_edit_state;
    bool can_draw_headings = false;
    bool can_draw_eye_masks = false;
    const ZarrDetectionLoader::FrameDetections* heading_details = nullptr;
    const ZarrDetectionLoader::FrameDetections* mask_details = nullptr;
    const ZarrDetectionLoader::FrameDetections* subject_shape_details = nullptr;
    std::string eye_mask_smoothing_run_id;
    CameraViewMaskOverlayOptions mask_overlay_options;
    CameraViewSubjectMaskPreview subject_mask_preview;
    CameraViewActiveRoiInsetOptions active_roi_inset_options;
    SubjectMaskEditSession* subject_mask_edit_session = nullptr;
    SubjectMaskBrushState* subject_mask_brush_state = nullptr;
    CameraViewSubjectShapeOverlayOptions subject_shape_overlay_options;
    const ZarrDetectionData::TailKinematicsData* tail_kinematics = nullptr;
    CameraViewTailKinematicsOverlayOptions tail_kinematics_overlay_options;
    const ZarrDetectionLoader::MovementFrameSample* movement_sample = nullptr;
    const std::vector<ZarrDetectionLoader::MovementTrailPoint>* movement_trail =
        nullptr;
    bool subject_mask_pick_enabled = false;
    bool subject_mask_brush_input_enabled = false;

    const std::vector<ZarrDetectionLoader::ChaserBoundingBox>* chaser_bboxes =
        nullptr;
    const std::vector<ZarrDetectionLoader::ChaserState>* chaser_states =
        nullptr;
    const CameraParams* camera_params = nullptr;
    const std::vector<std::string>* stimulus_events = nullptr;
    const ZarrDetectionData::StimulusStep* stimulus_step = nullptr;
    const StimulusPlayback* stimulus_player = nullptr;
    int target_stimulus_frame = -1;
    CameraViewStimulusInsetOptions stimulus_inset_options;

    CameraViewTransportControlsContext transport_controls;
};

struct CameraViewSubjectMaskPick {
    bool valid = false;
    int detection_index = -1;
    int32_t roi_index = -1;
    std::string component_name;
};

struct CameraViewSubjectMaskPaint {
    bool attempted = false;
    bool changed = false;
    bool stroke_finished = false;
    int32_t roi_index = -1;
    std::string component_name;
    int row = -1;
    int col = -1;
};

struct CameraViewWindowResult {
    FullFrameRectEditResult full_frame_edit_result;
    FullFrameKeypointEditState full_frame_keypoint_edit_state;
    CameraViewTransportControlsResult transport_result;
    CameraViewSubjectMaskPick subject_mask_pick;
    CameraViewSubjectMaskPaint subject_mask_paint;
    bool legacy_manual_keypoints_find = false;
    bool view_focused = false;
    CameraViewFrameSyncSummary frame_sync;
    CameraViewWindowPerfMetrics perf;
};

CameraViewWindowResult drawCameraViewWindowContents(
    const CameraViewWindowContext& context);
