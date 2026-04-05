#include "gui/crop_keypoint_editor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr size_t kManualCropKeypointCount = 3;

bool isEditableSelection(const CropKeypointEditorContext& context) {
    return context.selection != nullptr && context.selection->valid &&
           context.selection->editable &&
           context.displayed_crop_roi_index == context.selection->roi_index;
}

std::array<float, 2> defaultCropKeypointPosition(const std::string& label,
                                                 float width,
                                                 float height) {
    const float safe_width = std::max(width, 1.0f);
    const float safe_height = std::max(height, 1.0f);
    const bool is_left = label.find("left") != std::string::npos;
    const bool is_right = label.find("right") != std::string::npos;
    if (is_left) {
        return {safe_width * 0.32f, safe_height * 0.35f};
    }
    if (is_right) {
        return {safe_width * 0.68f, safe_height * 0.35f};
    }
    return {safe_width * 0.50f, safe_height * 0.68f};
}

bool sameSelection(const CropKeypointEditorState& state,
                   const RefinedKeypointSelection& selection) {
    return state.roi_index == selection.roi_index &&
           state.frame == static_cast<int>(selection.frame_id) &&
           state.detection_index ==
               static_cast<int>(selection.detection_index) &&
           state.run_name == selection.run_name;
}

void syncCropKeypointEditorState(const CropKeypointEditorContext& context,
                                 CropKeypointEditorState& state) {
    if (!isEditableSelection(context) || context.source_positions == nullptr ||
        context.labels == nullptr ||
        context.source_positions->size() != kManualCropKeypointCount ||
        context.labels->size() != kManualCropKeypointCount) {
        resetCropKeypointEditorState(state);
        return;
    }

    if (sameSelection(state, *context.selection) && state.dirty) {
        return;
    }

    state.positions.resize(kManualCropKeypointCount);
    for (size_t i = 0; i < kManualCropKeypointCount; ++i) {
        const auto& source = (*context.source_positions)[i];
        if (std::isfinite(source[0]) && std::isfinite(source[1])) {
            state.positions[i] = source;
        } else {
            state.positions[i] = defaultCropKeypointPosition(
                (*context.labels)[i], context.crop_width, context.crop_height);
        }
    }
    state.active_handle = -1;
    state.roi_index = context.selection->roi_index;
    state.frame = static_cast<int>(context.selection->frame_id);
    state.detection_index =
        static_cast<int>(context.selection->detection_index);
    state.run_name = context.selection->run_name;
    state.dirty = false;
}

bool computeArrowOriginForPositions(
    const std::vector<std::array<float, 2>>& positions,
    const std::vector<std::string>& labels,
    std::array<float, 2>& out_origin) {
    float left_x = NAN;
    float left_y = NAN;
    float right_x = NAN;
    float right_y = NAN;
    for (size_t i = 0; i < positions.size() && i < labels.size(); ++i) {
        const bool is_left = labels[i].find("left") != std::string::npos;
        const bool is_right = labels[i].find("right") != std::string::npos;
        if ((!is_left && !is_right) || !std::isfinite(positions[i][0]) ||
            !std::isfinite(positions[i][1])) {
            continue;
        }
        if (is_left) {
            left_x = positions[i][0];
            left_y = positions[i][1];
        }
        if (is_right) {
            right_x = positions[i][0];
            right_y = positions[i][1];
        }
    }
    if (!std::isfinite(left_x) || !std::isfinite(right_x)) {
        return false;
    }
    out_origin = {(left_x + right_x) * 0.5f, (left_y + right_y) * 0.5f};
    return true;
}

ImU32 cropKeypointColor(const std::string& label) {
    if (label.find("swim") != std::string::npos ||
        label.find("bladder") != std::string::npos) {
        return IM_COL32(255, 217, 38, 220);
    }
    if (label.find("left") != std::string::npos) {
        return IM_COL32(77, 242, 102, 220);
    }
    if (label.find("right") != std::string::npos) {
        return IM_COL32(191, 102, 242, 220);
    }
    return IM_COL32(242, 153, 51, 220);
}

void drawKeypointOverlay(const CropKeypointEditorContext& context,
                         const std::vector<std::array<float, 2>>& positions) {
    if (!context.show_keypoints || context.labels == nullptr ||
        context.edges == nullptr || positions.empty()) {
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    for (const auto& edge : *context.edges) {
        const size_t a = edge[0];
        const size_t b = edge[1];
        if (a >= positions.size() || b >= positions.size()) {
            continue;
        }
        const float ax = positions[a][0];
        const float ay = positions[a][1];
        const float bx = positions[b][0];
        const float by = positions[b][1];
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) ||
            !std::isfinite(by)) {
            continue;
        }
        draw_list->AddLine(
            ImVec2(context.image_top_left.x + ax * context.image_scale,
                   context.image_top_left.y + ay * context.image_scale),
            ImVec2(context.image_top_left.x + bx * context.image_scale,
                   context.image_top_left.y + by * context.image_scale),
            IM_COL32(255, 255, 255, 160),
            1.5f);
    }

    for (size_t i = 0; i < positions.size(); ++i) {
        const float x = positions[i][0];
        const float y = positions[i][1];
        if (!std::isfinite(x) || !std::isfinite(y)) {
            continue;
        }
        const std::string& label =
            i < context.labels->size() ? (*context.labels)[i] : "";
        ImVec2 center(context.image_top_left.x + x * context.image_scale,
                      context.image_top_left.y + y * context.image_scale);
        draw_list->AddCircleFilled(center,
                                   4.0f * context.image_scale,
                                   cropKeypointColor(label));
        draw_list->AddCircle(center,
                             4.0f * context.image_scale,
                             IM_COL32(255, 255, 255, 180),
                             0,
                             1.5f);
    }
}

void drawHeadingArrow(const CropKeypointEditorContext& context,
                      const std::array<float, 2>& arrow_origin,
                      bool arrow_origin_valid) {
    if (!context.show_heading_arrow || !context.stored_heading_valid ||
        !arrow_origin_valid) {
        return;
    }

    const float rad = context.stored_heading_deg *
                      (3.14159265f / 180.0f);
    const float arrow_len =
        std::min(context.image_size.x, context.image_size.y) * 0.2f;
    ImVec2 center(context.image_top_left.x + arrow_origin[0] * context.image_scale,
                  context.image_top_left.y + arrow_origin[1] * context.image_scale);
    const float dx = std::cos(rad) * arrow_len;
    const float dy = -std::sin(rad) * arrow_len;
    ImVec2 tip(center.x + dx, center.y + dy);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(center, tip, IM_COL32(255, 50, 50, 220), 2.5f);
    const float head_len = 8.0f * context.image_scale;
    const float head_angle = 2.6f;
    ImVec2 h1(tip.x + head_len * std::cos(rad + head_angle),
              tip.y - head_len * std::sin(rad + head_angle));
    ImVec2 h2(tip.x + head_len * std::cos(rad - head_angle),
              tip.y - head_len * std::sin(rad - head_angle));
    draw_list->AddTriangleFilled(tip, h1, h2, IM_COL32(255, 50, 50, 220));
}

}  // namespace

void resetCropKeypointEditorState(CropKeypointEditorState& state) {
    state.positions.clear();
    state.active_handle = -1;
    state.roi_index = -1;
    state.frame = -1;
    state.detection_index = -1;
    state.run_name.clear();
    state.dirty = false;
}

CropKeypointEditorDisplay drawCropKeypointEditorOverlay(
    const CropKeypointEditorContext& context,
    CropKeypointEditorState& state) {
    CropKeypointEditorDisplay display;

    syncCropKeypointEditorState(context, state);
    display.selection_editable = isEditableSelection(context);

    if (display.selection_editable &&
        state.positions.size() == kManualCropKeypointCount) {
        display.positions = &state.positions;
    } else if (context.source_positions != nullptr) {
        display.positions = context.source_positions;
    }

    if (display.positions != nullptr && context.labels != nullptr &&
        computeArrowOriginForPositions(*display.positions,
                                       *context.labels,
                                       display.arrow_origin)) {
        display.arrow_origin_valid = true;
    } else {
        display.arrow_origin = context.base_arrow_origin;
        display.arrow_origin_valid = context.base_arrow_origin_valid;
    }

    if (display.selection_editable && context.show_keypoints &&
        !context.play_video && display.positions == &state.positions) {
        const bool crop_image_hovered = ImGui::IsItemHovered();
        const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
        const float hit_radius =
            std::max(8.0f, 8.0f * context.image_scale);
        const float hit_radius_sq = hit_radius * hit_radius;
        int hovered_handle = -1;
        float best_distance_sq = hit_radius_sq;
        for (size_t i = 0; i < state.positions.size(); ++i) {
            const float x = state.positions[i][0];
            const float y = state.positions[i][1];
            if (!std::isfinite(x) || !std::isfinite(y)) {
                continue;
            }
            const float sx = context.image_top_left.x + x * context.image_scale;
            const float sy = context.image_top_left.y + y * context.image_scale;
            const float dx = mouse_pos.x - sx;
            const float dy = mouse_pos.y - sy;
            const float distance_sq = dx * dx + dy * dy;
            if (distance_sq <= best_distance_sq) {
                hovered_handle = static_cast<int>(i);
                best_distance_sq = distance_sq;
            }
        }

        if (hovered_handle >= 0 && crop_image_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (state.active_handle < 0 && crop_image_hovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            hovered_handle >= 0) {
            state.active_handle = hovered_handle;
        }
        if (state.active_handle >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float local_x =
                    (mouse_pos.x - context.image_top_left.x) / context.image_scale;
                float local_y =
                    (mouse_pos.y - context.image_top_left.y) / context.image_scale;
                local_x = std::clamp(local_x, 0.0f, context.crop_width);
                local_y = std::clamp(local_y, 0.0f, context.crop_height);
                auto& point =
                    state.positions[static_cast<size_t>(state.active_handle)];
                if (!std::isfinite(point[0]) || !std::isfinite(point[1]) ||
                    std::abs(point[0] - local_x) > 0.01f ||
                    std::abs(point[1] - local_y) > 0.01f) {
                    point = {local_x, local_y};
                    state.dirty = true;
                    display.arrow_origin_valid = computeArrowOriginForPositions(
                        state.positions, *context.labels, display.arrow_origin);
                }
            } else {
                state.active_handle = -1;
            }
        }
    } else {
        state.active_handle = -1;
    }

    if (display.positions != nullptr) {
        drawKeypointOverlay(context, *display.positions);
    }
    drawHeadingArrow(context, display.arrow_origin, display.arrow_origin_valid);
    return display;
}

CropKeypointEditorAction drawCropKeypointEditorPanel(
    const CropKeypointEditorContext& context,
    CropKeypointEditorState& state) {
    CropKeypointEditorAction action;

    if (context.show_rotated_crop && context.rotated_valid && state.dirty) {
        ImGui::TextDisabled("Rotated keypoint overlay refreshes after save.");
    }

    ImGui::Separator();
    ImGui::Text("Refined Keypoint Edit");

    if (context.selection == nullptr) {
        ImGui::TextDisabled(
            "Select a detection with refined keypoints to edit this crop.");
    } else if (!context.selection->valid) {
        if (!context.selection->message.empty()) {
            ImGui::TextWrapped("%s", context.selection->message.c_str());
        } else {
            ImGui::TextDisabled(
                "Selected detection does not resolve to a writable keypoint ROI.");
        }
    } else if (!context.selection->editable) {
        ImGui::TextWrapped(
            "Loaded keypoints are raw; in-place edits require a refined keypoint run.");
    } else if (context.selection->roi_index != context.displayed_crop_roi_index) {
        ImGui::TextDisabled(
            "Selected detection does not match the displayed crop.");
    } else {
        ImGui::Text("Run: %s | ROI: %d | detection %zu",
                    context.selection->run_name.c_str(),
                    context.selection->roi_index,
                    context.selection->detection_index);
        ImGui::TextDisabled(
            "Drag the crop keypoints, then save or mark the row state explicitly.");
        if (state.dirty) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                               "Unsaved keypoint changes.");
        }
        if (context.play_video) {
            ImGui::TextDisabled("Pause playback to edit or save keypoints.");
        }

        ImGui::BeginDisabled(context.play_video);
        if (ImGui::Button("Save Keypoint Edit")) {
            if (state.positions.size() != kManualCropKeypointCount) {
                if (context.status_message != nullptr) {
                    *context.status_message =
                        "Keypoint edit failed: expected 3 editable crop keypoints.";
                }
            } else {
                action.type = CropKeypointEditorActionType::Save;
                for (size_t i = 0; i < kManualCropKeypointCount; ++i) {
                    action.keypoints_roi[i][0] = state.positions[i][0];
                    action.keypoints_roi[i][1] = state.positions[i][1];
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Mark No Keypoints")) {
            action.type = CropKeypointEditorActionType::MarkNoKeypoints;
        }
        ImGui::SameLine();
        if (ImGui::Button("Mark Detection Issue")) {
            action.type = CropKeypointEditorActionType::MarkDetectionIssue;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Keypoint Edit")) {
            resetCropKeypointEditorState(state);
            syncCropKeypointEditorState(context, state);
            action.type = CropKeypointEditorActionType::Reset;
            if (context.status_message != nullptr) {
                *context.status_message = "Reset unsaved crop keypoint edits.";
            }
        }
        ImGui::EndDisabled();
    }

    if (context.status_message != nullptr && !context.status_message->empty()) {
        ImGui::TextWrapped("%s", context.status_message->c_str());
    }
    return action;
}
