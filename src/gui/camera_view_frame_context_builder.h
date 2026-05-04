#pragma once

#include "gui/camera_view_window.h"
#include "gui/frame_debug_window.h"

#include <opencv2/core.hpp>

#include <optional>
#include <string>
#include <vector>

struct CameraViewFrameContextInput {
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
    ZarrDetectionLoader* zarr_loader = nullptr;
    bool dataset_allows_bbox_edit = false;
    bool bbox_edit_enabled = false;
    bool bbox_allow_edit_while_playing = false;
    const ZarrBBoxEditState* bbox_edit_state = nullptr;
    FullFrameRectEditStateView full_frame_edit_state;
    const std::vector<LoggedBoundingBox>* zarr_boxes = nullptr;
    const ZarrDetectionLoader::FrameDetections* detection_details = nullptr;
    bool frame_is_interpolated = false;
    int latest_decoded_frame = -1;
    int total_recording_frames = -1;

    bool has_yolo_detections = false;
    const std::vector<cv::Rect>* yolo_boxes = nullptr;
    const std::vector<std::string>* yolo_labels = nullptr;
    const std::vector<int>* yolo_class_ids = nullptr;

    bool show_keypoint_markers = false;
    bool keypoint_tab_full_frame_edit_enabled = false;
    int visible_camera_index = -1;
    const std::optional<RefinedKeypointSelection>*
        active_full_frame_keypoint_selection = nullptr;
    const FrameDebugWindowState* frame_debug_state = nullptr;

    bool can_draw_headings = false;
    bool can_draw_eye_masks = false;
    bool show_subject_body_mask = true;
    bool show_eye_left_mask = true;
    bool show_eye_right_mask = true;
    bool show_swim_bladder_mask = true;
    bool show_eye_direction_beams = true;
    bool show_eye_gaze_rays = true;
    bool show_eye_angle_arcs = true;
    bool show_eye_angle_labels = true;
    CameraViewMaskOverlayMode mask_overlay_mode =
        CameraViewMaskOverlayMode::Review;
    CameraViewSubjectShapeOverlayOptions subject_shape_overlay_options;
    CameraViewTailKinematicsOverlayOptions tail_kinematics_overlay_options;

    bool show_movement_trail = true;
    float movement_trail_seconds = 2.0f;
    bool movement_trail_valid_samples_only = true;
    CameraViewStimulusInsetOptions stimulus_inset_options;
    const StimulusPlayback* stimulus_player = nullptr;
    int target_stimulus_frame = -1;

    const std::vector<ZarrDetectionLoader::ChaserBoundingBox>* chaser_bboxes =
        nullptr;
    const std::vector<ZarrDetectionLoader::ChaserState>* chaser_states =
        nullptr;
    const CameraParams* camera_params = nullptr;

    CameraViewTransportControlsContext transport_controls;
};

struct PreparedCameraViewFrameContext {
    std::optional<ZarrDetectionLoader::FrameDetections> mask_details;
    std::vector<std::string> frame_events;
    std::optional<ZarrDetectionLoader::MovementFrameSample>
        movement_frame_sample;
    std::vector<ZarrDetectionLoader::MovementTrailPoint> movement_trail_points;
    CameraViewWindowContext context;
    double mask_data_load_ms = 0.0;
};

void prepareCameraViewFrameContext(
    const CameraViewFrameContextInput& input,
    PreparedCameraViewFrameContext& prepared);

void applyCameraViewSubjectMaskPick(
    const CameraViewWindowResult& camera_view_result,
    ZarrDetectionLoader& zarr_loader,
    FrameDebugWindowState& frame_debug_state);
