#pragma once

#include "keypoint_heading_utils.h"
#include "refined_keypoint_repository.h"

#include <array>
#include <string>
#include <vector>

#include "imgui.h"

enum class CropKeypointEditorActionType {
    None = 0,
    Save,
    MarkNoKeypoints,
    MarkDetectionIssue,
    Reset,
};

struct CropKeypointEditorAction {
    CropKeypointEditorActionType type = CropKeypointEditorActionType::None;
    std::vector<std::array<double, 2>> keypoints_roi;
};

struct CropKeypointEditorState {
    std::vector<std::array<float, 2>> positions;
    int active_handle = -1;
    int roi_index = -1;
    int frame = -1;
    int detection_index = -1;
    std::string run_name;
    bool dirty = false;
};

struct CropKeypointEditorContext {
    const RefinedKeypointSelection* selection = nullptr;
    int displayed_crop_roi_index = -1;
    int displayed_crop_source_frame = -1;
    const std::string* displayed_crop_source_label = nullptr;
    bool play_video = false;
    float crop_width = 0.0f;
    float crop_height = 0.0f;
    ImVec2 image_top_left{};
    ImVec2 image_size{};
    float image_scale = 1.0f;
    bool show_keypoints = true;
    bool show_heading_arrow = false;
    bool show_rotated_crop = false;
    bool rotated_valid = false;
    bool stored_heading_valid = false;
    float stored_heading_deg = 0.0f;
    const std::vector<std::array<float, 2>>* source_positions = nullptr;
    const std::vector<std::string>* labels = nullptr;
    const std::vector<std::array<size_t, 2>>* edges = nullptr;
    const KeypointHeadingComputationSpec* heading_spec = nullptr;
    std::array<float, 2> base_arrow_origin = {0.0f, 0.0f};
    bool base_arrow_origin_valid = false;
    std::string* status_message = nullptr;
};

struct CropKeypointEditorDisplay {
    const std::vector<std::array<float, 2>>* positions = nullptr;
    std::array<float, 2> arrow_origin = {0.0f, 0.0f};
    bool arrow_origin_valid = false;
    bool selection_editable = false;
    bool candidate_heading_valid = false;
    float candidate_heading_deg = 0.0f;
};

struct CropKeypointPreviewUiState {
    bool show_keypoints = true;
    bool show_rotated_crop = true;
    bool show_heading_arrow = false;
};

struct CropKeypointRotatedPreviewContext {
    bool valid = false;
    unsigned int texture_id = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    const std::vector<std::array<float, 2>>* positions = nullptr;
    const std::vector<std::string>* labels = nullptr;
    const std::vector<std::array<size_t, 2>>* edges = nullptr;
    std::array<float, 2> arrow_origin = {0.0f, 0.0f};
    bool arrow_origin_valid = false;
};

struct CropKeypointPreviewPanelContext {
    unsigned int crop_texture_id = 0;
    size_t crop_width = 0;
    size_t crop_height = 0;
    int displayed_crop_roi_index = -1;
    int displayed_crop_source_frame = -1;
    int current_frame_num = 0;
    const std::string* displayed_crop_source_label = nullptr;
    bool play_video = false;
    CropKeypointEditorContext editor_context;
    CropKeypointRotatedPreviewContext rotated;
};

struct CropKeypointPreviewPanelResult {
    CropKeypointEditorDisplay editor_display;
    CropKeypointEditorAction editor_action;
};

void resetCropKeypointEditorState(CropKeypointEditorState& state);

CropKeypointEditorDisplay drawCropKeypointEditorOverlay(
    const CropKeypointEditorContext& context,
    CropKeypointEditorState& state);

CropKeypointEditorAction drawCropKeypointEditorPanel(
    const CropKeypointEditorContext& context,
    CropKeypointEditorState& state);

CropKeypointPreviewPanelResult drawCropKeypointPreviewPanel(
    const CropKeypointPreviewPanelContext& context,
    CropKeypointPreviewUiState& ui_state,
    CropKeypointEditorState& editor_state);
