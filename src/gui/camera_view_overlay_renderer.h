#pragma once

#include "camera.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "zarr_bbox_edit.h"

#include <cstdint>
#include <string>
#include <vector>

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
    int32_t highlighted_roi_index = -1;
    std::string highlighted_component_name;
    CameraViewMaskOverlayMode mode = CameraViewMaskOverlayMode::Review;
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

CameraViewMaskPerfMetrics drawCameraViewEyeMaskOverlay(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    float image_height_px,
    const std::string& smoothing_run_id,
    const CameraViewMaskOverlayOptions& options);

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
