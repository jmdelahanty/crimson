#pragma once

#include "crop_image_provider.h"
#include "gui/crop_keypoint_editor.h"
#include "gui/crop_preview_perf.h"
#include "refined_keypoint_repository.h"

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

struct CropPreviewWindowState {
    unsigned int crop_texture = 0;
    std::vector<uint8_t> crop_rgba_buffer;
    int last_roi_index = -1;
    CropRect last_crop_rect;
    size_t last_width = 0;
    size_t last_height = 0;
    size_t last_channels = 0;
    int last_crop_preview_source_frame = -1;
    std::chrono::steady_clock::time_point last_crop_preview_refresh_time{};
    std::vector<std::array<float, 2>> crop_kp_positions;
    std::vector<std::string> crop_kp_labels;
    std::vector<std::array<size_t, 2>> crop_kp_edges;

    unsigned int rotated_crop_texture = 0;
    std::vector<uint8_t> rotated_rgba_buffer;
    unsigned int rotated_width = 0;
    unsigned int rotated_height = 0;
    bool rotated_valid = false;
    std::vector<std::array<float, 2>> rotated_kp_positions;
    std::vector<std::string> rotated_kp_labels;
    std::vector<std::array<size_t, 2>> rotated_kp_edges;
    float stored_heading_deg = 0.0f;
    bool stored_heading_valid = false;
    std::array<float, 2> arrow_origin_crop = {0.0f, 0.0f};
    std::array<float, 2> arrow_origin_rotated = {0.0f, 0.0f};
    bool arrow_origin_valid = false;
    int displayed_crop_roi_index = -1;
    int displayed_crop_source_frame = -1;
    std::string displayed_crop_source_label;

    CropKeypointPreviewUiState preview_ui_state;
    CropKeypointEditorState editor_state;
    std::string local_status_message;
};

struct CropPreviewWindowContext {
    const CropImageProvider& crop_image_provider;
    ZarrDetectionLoader& zarr_loader;
    RefinedKeypointRepository& refined_keypoint_repo;
    int current_frame_num = 0;
    int selected_frame = -1;
    int selected_box = -1;
    int selected_detection_index = -1;
    std::optional<CropSpec> selected_crop_spec;
    bool play_video = false;
};

struct CropPreviewWindowResult {
    CropKeypointEditorAction editor_action;
    std::optional<RefinedKeypointSelection> selected_keypoint_selection;
    CropPreviewPerfMetrics perf;
};

CropPreviewWindowResult drawCropPreviewWindow(const CropPreviewWindowContext& context,
                                             CropPreviewWindowState& state);
