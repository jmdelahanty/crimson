#pragma once

#include "camera.h"
#include "gui/camera_view_transport_controls.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "legacy_labeling_state.h"
#include "render.h"
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
    double scene_ui_ms = 0.0;
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
    bool active_dataset_has_synthetic_detections = false;
    bool frame_is_interpolated = false;
    int latest_decoded_frame = -1;
    int total_recording_frames = -1;

    bool has_yolo_detections = false;
    const std::vector<cv::Rect>* yolo_boxes = nullptr;
    const std::vector<std::string>* yolo_labels = nullptr;
    const std::vector<int>* yolo_class_ids = nullptr;

    bool show_keypoint_markers = false;
    bool can_draw_headings = false;
    bool can_draw_eye_masks = false;
    const ZarrDetectionLoader::FrameDetections* heading_details = nullptr;
    const ZarrDetectionLoader::FrameDetections* mask_details = nullptr;
    std::string eye_mask_smoothing_run_id;

    const std::vector<ZarrDetectionLoader::ChaserBoundingBox>* chaser_bboxes =
        nullptr;
    const std::vector<ZarrDetectionLoader::ChaserState>* chaser_states =
        nullptr;
    const CameraParams* camera_params = nullptr;
    const std::vector<std::string>* stimulus_events = nullptr;

    CameraViewTransportControlsContext transport_controls;
};

struct CameraViewWindowResult {
    FullFrameRectEditResult full_frame_edit_result;
    CameraViewTransportControlsResult transport_result;
    bool legacy_manual_keypoints_find = false;
    bool view_focused = false;
    CameraViewFrameSyncSummary frame_sync;
    CameraViewWindowPerfMetrics perf;
};

CameraViewWindowResult drawCameraViewWindowContents(
    const CameraViewWindowContext& context);
