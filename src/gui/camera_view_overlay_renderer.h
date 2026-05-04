#pragma once

#include "camera.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "zarr_bbox_edit.h"

#include <cstdint>
#include <string>
#include <vector>

struct StimulusPlayback;

enum class CameraViewMaskOverlayMode {
    Realtime = 0,
    Review = 1,
    Debug = 2,
};

const char* cameraViewMaskOverlayModeLabel(CameraViewMaskOverlayMode mode);

struct CameraViewMaskPerfMetrics {
    bool attempted = false;
    std::string mode;
    int roi_count = 0;
    int visible_roi_count = 0;
    int component_fill_count = 0;
    int fallback_scatter_count = 0;
    int texture_cache_hits = 0;
    int texture_cache_misses = 0;
    int texture_uploads = 0;
    int contours_drawn = 0;
    int selected_contours_drawn = 0;
    int contour_points = 0;
    int axes_drawn = 0;
    int visual_cones_drawn = 0;
    int visual_cone_overlaps_drawn = 0;
    int gaze_rays_drawn = 0;
    int angle_labels_drawn = 0;
    bool selected_highlight_drawn = false;
    bool pick_attempted = false;
    bool pick_hit = false;
    double texture_lookup_ms = 0.0;
    double texture_upload_ms = 0.0;
    double fill_draw_ms = 0.0;
    double contour_build_ms = 0.0;
    double contour_draw_ms = 0.0;
    double axis_draw_ms = 0.0;
    double pick_ms = 0.0;
    double total_draw_ms = 0.0;
};

void accumulateCameraViewMaskPerfMetrics(CameraViewMaskPerfMetrics& dst,
                                         const CameraViewMaskPerfMetrics& src);

struct CameraViewMaskOverlayOptions {
    bool show_subject_body = true;
    bool show_eye_left = true;
    bool show_eye_right = true;
    bool show_swim_bladder = true;
    bool show_eye_direction_beams = true;
    bool show_eye_gaze_rays = true;
    bool show_eye_angle_arcs = true;
    bool show_eye_angle_labels = true;
    int32_t highlighted_roi_index = -1;
    std::string highlighted_component_name;
    CameraViewMaskOverlayMode mode = CameraViewMaskOverlayMode::Review;
};

struct CameraViewSubjectMaskPreview {
    bool active = false;
    bool dirty = false;
    int32_t roi_index = -1;
    std::string component_name;
    int rows = 0;
    int cols = 0;
    uint64_t revision = 0;
    const std::vector<uint8_t>* binary_mask = nullptr;
};

struct CameraViewSubjectShapeOverlayOptions {
    bool show_overlay = true;
    bool show_body_contour = false;
    bool show_swim_bladder_contour = false;
    bool show_eye_contours = false;
    bool show_body_frame_axes = false;
    bool show_snout_tip = true;
    bool show_caudal_anchor = true;
    bool show_tail_base = true;
    bool show_tail_tip = true;
    bool show_centerline = true;
    bool show_bspline_sample = true;
    bool show_bspline_debug_points = false;
    bool show_bspline_control_points = false;
    bool show_tail_samples = false;
    bool show_tail_normals = false;
};

struct CameraViewTailKinematicsOverlayOptions {
    bool show_overlay = true;
    bool show_samples = true;
    bool show_segments = true;
    bool show_angle_vectors = false;
    bool show_lateral_deflection = false;
    bool color_invalid_frames = true;
};

struct CameraViewStimulusInsetOptions {
    bool show_inset = true;
    float width_px = 220.0f;
    float opacity = 0.82f;
    bool show_frame_label = true;
};

std::vector<FullFrameRectOverlayItem> buildCameraViewBoundingBoxOverlayItems(
    const std::vector<LoggedBoundingBox>& zarr_boxes,
    const ZarrDetectionLoader::FrameDetections& detection_details,
    const ZarrBBoxEditState& bbox_edit_state,
    int current_frame_num,
    bool frame_has_bbox_edits,
    bool active_dataset_has_synthetic_detections,
    bool is_zarr_interpolated);

void drawCameraViewDetectionKeypointMarkers(
    const ZarrDetectionLoader::FrameDetections& detection_details,
    bool show_keypoint_markers,
    float image_height_px,
    int skip_detection_index = -1);

void drawCameraViewHeadingOverlay(
    const ZarrDetectionLoader::FrameDetections& heading_details,
    float image_height_px);

void drawCameraViewMovementOverlay(
    const ZarrDetectionLoader::MovementFrameSample& movement_sample,
    const ZarrDetectionLoader::FrameDetections* detection_details,
    float image_height_px);

void drawCameraViewMovementTrailOverlay(
    const std::vector<ZarrDetectionLoader::MovementTrailPoint>& trail_points,
    float image_height_px);

CameraViewMaskPerfMetrics drawCameraViewEyeMaskOverlay(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    const ZarrDetectionLoader::FrameDetections* subject_shape_details,
    float image_height_px,
    const std::string& smoothing_run_id,
    const CameraViewMaskOverlayOptions& options,
    const CameraViewSubjectMaskPreview* edit_preview = nullptr);

void drawCameraViewSubjectShapeOverlay(
    const ZarrDetectionLoader::FrameDetections& detection_details,
    float image_height_px,
    const CameraViewSubjectShapeOverlayOptions& options);

void drawCameraViewTailKinematicsOverlay(
    const ZarrDetectionLoader::FrameDetections& detection_details,
    const ZarrDetectionData::TailKinematicsData& tail_kinematics,
    float image_height_px,
    const CameraViewTailKinematicsOverlayOptions& options);

void drawCameraViewChaserOverlay(
    std::vector<ZarrDetectionLoader::ChaserBoundingBox> chaser_bboxes,
    const std::vector<ZarrDetectionLoader::ChaserState>& chaser_states,
    const CameraParams& camera_params,
    int image_width_px,
    int image_height_px);

void drawCameraViewStimulusEventOverlay(
    int view_idx,
    int current_frame_num,
    const std::vector<std::string>& frame_events);

void drawCameraViewStimulusStepDirectionOverlay(
    const ZarrDetectionData::StimulusStep* stimulus_step);

void drawCameraViewStimulusInsetOverlay(
    const StimulusPlayback* stimulus_player,
    int target_stimulus_frame,
    const CameraViewStimulusInsetOptions& options);
