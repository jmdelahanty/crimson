#include "gui/full_frame_keypoint_edit_overlay.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr size_t kManualFullFrameKeypointCount = 3;

bool isEditableSelection(const FullFrameKeypointEditContext& context) {
    return context.selection != nullptr && context.selection->valid &&
           context.selection->editable && context.detection_details != nullptr &&
           context.selection->detection_index <
               context.detection_details->keypoints_pixels.size() &&
           context.detection_details->has_keypoints;
}

bool sameSelection(const FullFrameKeypointEditState& state,
                   const RefinedKeypointSelection& selection) {
    return state.roi_index == selection.roi_index &&
           state.frame == static_cast<int>(selection.frame_id) &&
           state.detection_index ==
               static_cast<int>(selection.detection_index) &&
           state.run_name == selection.run_name;
}

std::array<float, 2> defaultFullFrameKeypointPosition(
    const RefinedKeypointSelection& selection,
    const std::string& label) {
    const float offset_x = std::isfinite(selection.roi_metadata.offset_x)
                               ? selection.roi_metadata.offset_x
                               : 0.0f;
    const float offset_y = std::isfinite(selection.roi_metadata.offset_y)
                               ? selection.roi_metadata.offset_y
                               : 0.0f;
    const float roi_width = std::max(selection.roi_metadata.roi_width, 1.0f);
    const float roi_height = std::max(selection.roi_metadata.roi_height, 1.0f);
    const bool is_left = label.find("left") != std::string::npos;
    const bool is_right = label.find("right") != std::string::npos;
    if (is_left) {
        return {offset_x + roi_width * 0.32f, offset_y + roi_height * 0.35f};
    }
    if (is_right) {
        return {offset_x + roi_width * 0.68f, offset_y + roi_height * 0.35f};
    }
    return {offset_x + roi_width * 0.50f, offset_y + roi_height * 0.68f};
}

ImU32 keypointColor(const std::string& label) {
    if (label.find("swim") != std::string::npos ||
        label.find("bladder") != std::string::npos) {
        return IM_COL32(255, 217, 38, 255);
    }
    if (label.find("left") != std::string::npos) {
        return IM_COL32(77, 242, 102, 255);
    }
    if (label.find("right") != std::string::npos) {
        return IM_COL32(191, 102, 242, 255);
    }
    return IM_COL32(242, 153, 51, 255);
}

void syncState(const FullFrameKeypointEditContext& context,
               FullFrameKeypointEditState& state) {
    if (!isEditableSelection(context) || context.selection == nullptr ||
        context.detection_details == nullptr) {
        resetFullFrameKeypointEditState(state);
        return;
    }

    if (sameSelection(state, *context.selection) && state.dirty) {
        return;
    }

    const auto& keypoints =
        context.detection_details
            ->keypoints_pixels[context.selection->detection_index];
    const size_t keypoint_count = std::min(
        keypoints.size(), context.detection_details->keypoint_labels.size());
    if (keypoint_count == 0) {
        resetFullFrameKeypointEditState(state);
        return;
    }

    state.positions_img.resize(keypoint_count);
    for (size_t i = 0; i < keypoint_count; ++i) {
        if (std::isfinite(keypoints[i][0]) && std::isfinite(keypoints[i][1])) {
            state.positions_img[i] = keypoints[i];
        } else {
            state.positions_img[i] = defaultFullFrameKeypointPosition(
                *context.selection,
                context.detection_details->keypoint_labels[i]);
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

void drawEditableKeypointOverlay(const FullFrameKeypointEditContext& context,
                                 const FullFrameKeypointEditState& state) {
    if (context.selection == nullptr || context.detection_details == nullptr ||
        state.positions_img.empty()) {
        return;
    }

    const auto& labels = context.detection_details->keypoint_labels;
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const float image_height = context.image_height_px;

    for (const auto& edge : context.detection_details->skeleton_edges) {
        const size_t a = edge[0];
        const size_t b = edge[1];
        if (a >= state.positions_img.size() || b >= state.positions_img.size()) {
            continue;
        }
        const auto& pa = state.positions_img[a];
        const auto& pb = state.positions_img[b];
        if (!std::isfinite(pa[0]) || !std::isfinite(pa[1]) ||
            !std::isfinite(pb[0]) || !std::isfinite(pb[1])) {
            continue;
        }
        const ImVec2 p0 = ImPlot::PlotToPixels(
            ImPlotPoint(pa[0], image_height - pa[1]));
        const ImVec2 p1 = ImPlot::PlotToPixels(
            ImPlotPoint(pb[0], image_height - pb[1]));
        draw_list->AddLine(p0, p1, IM_COL32(255, 255, 255, 210), 2.25f);
    }

    for (size_t i = 0; i < state.positions_img.size(); ++i) {
        const auto& point = state.positions_img[i];
        if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
            continue;
        }
        const std::string& label =
            i < labels.size() ? labels[i] : std::string{};
        const ImVec2 center = ImPlot::PlotToPixels(
            ImPlotPoint(point[0], image_height - point[1]));
        const float radius = state.active_handle == static_cast<int>(i) ? 8.0f
                                                                        : 6.5f;
        draw_list->AddCircleFilled(center, radius, keypointColor(label));
        draw_list->AddCircle(center, radius, IM_COL32(255, 255, 255, 255), 0,
                             2.0f);
    }
}

}  // namespace

void resetFullFrameKeypointEditState(FullFrameKeypointEditState& state) {
    state.positions_img.clear();
    state.active_handle = -1;
    state.roi_index = -1;
    state.frame = -1;
    state.detection_index = -1;
    state.run_name.clear();
    state.dirty = false;
}

FullFrameKeypointEditOverlayResult processFullFrameKeypointEditOverlay(
    const FullFrameKeypointEditContext& context,
    const FullFrameKeypointEditState& initial_state) {
    FullFrameKeypointEditOverlayResult result;
    result.state = initial_state;
    result.selection_editable = isEditableSelection(context);

    if (!result.state.enabled) {
        result.state.active_handle = -1;
        return result;
    }

    syncState(context, result.state);
    result.selection_editable = isEditableSelection(context);
    if (!result.selection_editable || context.selection == nullptr ||
        context.detection_details == nullptr) {
        return result;
    }

    drawEditableKeypointOverlay(context, result.state);

    if (!context.plot_hovered || context.play_video || result.state.positions_img.empty()) {
        result.state.active_handle = -1;
        return result;
    }

    const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
    int hovered_handle = -1;
    float best_distance_sq = 10.0f * 10.0f;
    for (size_t i = 0; i < result.state.positions_img.size(); ++i) {
        const auto& point = result.state.positions_img[i];
        if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
            continue;
        }
        const ImVec2 handle_px = ImPlot::PlotToPixels(
            ImPlotPoint(point[0], context.image_height_px - point[1]));
        const float dx = mouse_pos.x - handle_px.x;
        const float dy = mouse_pos.y - handle_px.y;
        const float distance_sq = dx * dx + dy * dy;
        if (distance_sq <= best_distance_sq) {
            hovered_handle = static_cast<int>(i);
            best_distance_sq = distance_sq;
        }
    }

    if (hovered_handle >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (result.state.active_handle < 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_handle >= 0) {
        result.state.active_handle = hovered_handle;
    }
    if (result.state.active_handle >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
            const float clamped_x = std::clamp(static_cast<float>(mouse_plot.x),
                                               0.0f,
                                               context.image_width_px);
            const float clamped_y = std::clamp(
                context.image_height_px - static_cast<float>(mouse_plot.y),
                0.0f,
                context.image_height_px);
            auto& point =
                result.state.positions_img[static_cast<size_t>(
                    result.state.active_handle)];
            if (!std::isfinite(point[0]) || !std::isfinite(point[1]) ||
                std::abs(point[0] - clamped_x) > 0.01f ||
                std::abs(point[1] - clamped_y) > 0.01f) {
                point = {clamped_x, clamped_y};
                result.state.dirty = true;
            }
        } else {
            result.state.active_handle = -1;
        }
    }

    return result;
}

bool buildFullFrameKeypointEditAction(
    const RefinedKeypointSelection& selection,
    const FullFrameKeypointEditState& state,
    CropKeypointEditorAction& action,
    std::string* error_message) {
    action = CropKeypointEditorAction{};
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (!selection.valid || !selection.editable) {
        if (error_message != nullptr) {
            *error_message =
                "Selected detection does not resolve to an editable refined keypoint ROI.";
        }
        return false;
    }
    if (!selection.roi_metadata.has_crop_metadata) {
        if (error_message != nullptr) {
            *error_message =
                "Selected ROI has no crop placement metadata; cannot save full-frame keypoint edits.";
        }
        return false;
    }
    if (!sameSelection(state, selection)) {
        if (error_message != nullptr) {
            *error_message =
                "Full-frame keypoint edit state does not match the selected ROI.";
        }
        return false;
    }
    if (state.positions_img.size() != kManualFullFrameKeypointCount) {
        if (error_message != nullptr) {
            *error_message =
                "Full-frame keypoint edit failed: expected 3 editable keypoints.";
        }
        return false;
    }

    action.type = CropKeypointEditorActionType::Save;
    for (size_t i = 0; i < kManualFullFrameKeypointCount; ++i) {
        if (!std::isfinite(state.positions_img[i][0]) ||
            !std::isfinite(state.positions_img[i][1])) {
            if (error_message != nullptr) {
                *error_message =
                    "Full-frame keypoint edit failed: all editable keypoints must be finite.";
            }
            action = CropKeypointEditorAction{};
            return false;
        }
        action.keypoints_roi[i][0] =
            static_cast<double>(state.positions_img[i][0] -
                                selection.roi_metadata.offset_x);
        action.keypoints_roi[i][1] =
            static_cast<double>(state.positions_img[i][1] -
                                selection.roi_metadata.offset_y);
    }
    return true;
}
