#pragma once

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
    std::array<std::array<double, 2>, 3> keypoints_roi{};
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
    std::array<float, 2> base_arrow_origin = {0.0f, 0.0f};
    bool base_arrow_origin_valid = false;
    std::string* status_message = nullptr;
};

struct CropKeypointEditorDisplay {
    const std::vector<std::array<float, 2>>* positions = nullptr;
    std::array<float, 2> arrow_origin = {0.0f, 0.0f};
    bool arrow_origin_valid = false;
    bool selection_editable = false;
};

void resetCropKeypointEditorState(CropKeypointEditorState& state);

CropKeypointEditorDisplay drawCropKeypointEditorOverlay(
    const CropKeypointEditorContext& context,
    CropKeypointEditorState& state);

CropKeypointEditorAction drawCropKeypointEditorPanel(
    const CropKeypointEditorContext& context,
    CropKeypointEditorState& state);
