#pragma once

#include "gui/camera_view_overlay_renderer.h"
#include "gui/crop_keypoint_editor.h"
#include "gui/refined_keypoint_review_panel.h"
#include "gui/review_metadata_editor.h"
#include "review_frame_state.h"
#include "subject_mask_edit_session.h"
#include "zarr_bbox_edit.h"

#include <array>
#include <string>
#include <vector>

enum class FrameInspectTab {
    Detect = 0,
    Keypoints,
    EyeMasks,
    TailKinematics,
    EyeAngles,
};

struct FrameDebugWindowState {
    ReviewMetadataEditorState manual_write_review;
    RefinedKeypointReviewPanelState keypoint_review_panel;
    SubjectMaskEditSession subject_mask_edit_session;
    SubjectMaskBrushState subject_mask_brush;
    CameraViewActiveRoiInsetOptions active_roi_inset_options;
    int subject_mask_edit_detection_index = -1;
    std::string subject_mask_edit_component_name = "subject_body";
    std::string subject_mask_edit_status;
    bool subject_mask_canvas_pick_enabled = true;
    ZarrDetectionLoader::SubjectShapeQcFilterOptions subject_shape_qc_filters;
    std::array<char, 128> subject_shape_reason_filter{};
    std::string subject_shape_qc_status;
    ZarrDetectionLoader::TailKinematicsQcFilterOptions tail_kinematics_qc_filters;
    std::array<char, 128> tail_kinematics_reason_filter{};
    std::string tail_kinematics_qc_status;
    int tail_kinematics_selected_row = -1;
    ZarrDetectionLoader::EyeAngleQcFilterOptions eye_angle_qc_filters;
    std::array<char, 128> eye_angle_reason_filter{};
    std::string eye_angle_qc_status;
    int eye_angle_selected_row = -1;
    int eye_angle_representation_index = -1;
    FrameInspectTab active_tab = FrameInspectTab::Detect;
};

struct FrameDebugWindowContext {
    int current_frame_num = 0;
    int display_target_frame = 0;
    int slider_frame = 0;
    int frame_sync_valid_slots = -1;
    int frame_sync_empty_slots = -1;
    unsigned int scene_buffer_size = 0;
    int frame_sync_recording_remaining = -1;
    int frame_sync_recording_total = 0;
    int frame_sync_latest_decoded = -1;
    const std::string& frame_sync_debug_line;

    bool zarr_loaded = false;
    ZarrDetectionLoader& zarr_loader;
    const std::vector<std::string>& detection_dataset_labels;
    int detection_dataset_choice = 0;
    bool dataset_has_synthetic_boxes = false;
    bool frame_is_interpolated = false;
    bool frame_has_bbox_edits = false;
    bool dataset_allows_bbox_edit = false;
    const std::vector<LoggedBoundingBox>& zarr_boxes;
    const ZarrDetectionLoader::FrameDetections* detection_details = nullptr;

    ReviewFrameFilters review_frame_filters;
    bool review_frame_cache_valid = false;
    size_t review_frame_count = 0;
    const std::string& review_frame_status;

    const std::string& decode_debug_status;
    const std::string& default_buffer_dump_root;

    ZarrBBoxEditState& bbox_edit_state;
    bool play_video = false;
    const std::string& bbox_payload_status;

    bool show_keypoint_markers = false;
    bool show_heading_arrows = false;
    bool show_eye_masks = false;
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
    bool show_stimulus_debug_windows = false;
};

struct FrameDebugWindowResult {
    int requested_detection_dataset_index = -1;
    bool review_filters_changed = false;
    ReviewFrameFilters review_frame_filters;
    bool request_prev_review_frame = false;
    bool request_next_review_frame = false;
    bool request_reset_frame_bbox_edits = false;
    bool request_clear_bbox_selection = false;
    bool request_build_manual_payload_preview = false;
    bool request_write_manual_payload = false;
    std::optional<RefinedKeypointSelection> selected_keypoint_selection;
    CropKeypointEditorAction keypoint_edit_action;
    bool request_keypoint_review_write = false;
    RefinedKeypointReviewStatusWriteOptions keypoint_review_options;
    bool show_keypoint_markers = false;
    bool show_heading_arrows = false;
    bool show_eye_masks = false;
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
    bool show_stimulus_debug_windows = false;
    bool request_prev_subject_shape_qc_frame = false;
    bool request_next_subject_shape_qc_frame = false;
    bool request_prev_tail_kinematics_qc_frame = false;
    bool request_next_tail_kinematics_qc_frame = false;
    bool request_seek_tail_kinematics_row = false;
    size_t requested_tail_kinematics_row = 0;
    bool request_prev_eye_angle_qc_frame = false;
    bool request_next_eye_angle_qc_frame = false;
    bool request_seek_eye_angle_row = false;
    size_t requested_eye_angle_row = 0;
};

FrameDebugWindowResult drawFrameDebugWindow(const FrameDebugWindowContext& context,
                                            FrameDebugWindowState& state);
