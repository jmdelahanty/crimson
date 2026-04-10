#pragma once

#include "camera.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "zarr_bbox_edit.h"

#include <vector>

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

void drawCameraViewEyeMaskOverlay(
    const ZarrDetectionLoader::FrameDetections& mask_details,
    float image_height_px,
    const std::string& smoothing_run_id);

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
