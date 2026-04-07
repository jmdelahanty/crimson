#pragma once

#include "review_frame_state.h"
#include "zarr_bbox_edit.h"

#include <string>
#include <vector>

struct FrameDebugWindowState {
    int manual_write_intended_use = 0;
    int manual_write_review_state = 0;
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
};

struct FrameDebugWindowResult {
    int requested_detection_dataset_index = -1;
    bool review_filters_changed = false;
    ReviewFrameFilters review_frame_filters;
    bool request_prev_review_frame = false;
    bool request_next_review_frame = false;
    bool request_dump_decode_buffers = false;
    bool request_random_seek_dump = false;
    bool request_reset_frame_bbox_edits = false;
    bool request_clear_bbox_selection = false;
    bool request_build_manual_payload_preview = false;
    bool request_write_manual_payload = false;
    bool show_keypoint_markers = false;
    bool show_heading_arrows = false;
    bool show_eye_masks = false;
};

FrameDebugWindowResult drawFrameDebugWindow(const FrameDebugWindowContext& context,
                                            FrameDebugWindowState& state);
