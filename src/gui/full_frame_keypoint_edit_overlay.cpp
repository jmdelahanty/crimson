#include "gui/full_frame_keypoint_edit_overlay.h"
#include "gui/refined_keypoint_style.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

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

ImU32 keypointColor(const std::string& label, size_t kp_idx) {
    return chooseRefinedKeypointColorU32(label, kp_idx);
}

std::string keypointDisplayLabel(const std::vector<std::string>& labels,
                                 size_t kp_idx) {
    if (kp_idx < labels.size() && !labels[kp_idx].empty()) {
        return labels[kp_idx];
    }
    return "kp_" + std::to_string(kp_idx);
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
        const std::string label = keypointDisplayLabel(labels, i);
        const ImVec2 center = ImPlot::PlotToPixels(
            ImPlotPoint(point[0], image_height - point[1]));
        const float radius = state.active_handle == static_cast<int>(i) ? 8.0f
                                                                        : 6.5f;
        draw_list->AddCircleFilled(center, radius, keypointColor(label, i));
        draw_list->AddCircle(center, radius, IM_COL32(255, 255, 255, 255), 0,
                             2.0f);
        if (state.show_labels) {
            const ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
            const ImVec2 text_pos(center.x + radius + 5.0f,
                                  center.y - text_size.y * 0.5f);
            draw_list->AddRectFilled(
                ImVec2(text_pos.x - 3.0f, text_pos.y - 2.0f),
                ImVec2(text_pos.x + text_size.x + 3.0f,
                       text_pos.y + text_size.y + 2.0f),
                IM_COL32(0, 0, 0, 150),
                3.0f);
            draw_list->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
                               IM_COL32(0, 0, 0, 230),
                               label.c_str());
            draw_list->AddText(text_pos,
                               IM_COL32(255, 255, 255, 245),
                               label.c_str());
        }
    }
}

void drawCandidateHeadingOverlay(const FullFrameKeypointEditContext& context,
                                 const FullFrameKeypointEditState& state) {
    if (!(context.heading_spec != nullptr && context.heading_spec->available &&
          context.heading_spec->enabled && context.selection != nullptr &&
          context.detection_details != nullptr && state.dirty &&
          context.selection->detection_index <
              context.detection_details->keypoints_pixels.size())) {
        return;
    }

    const auto& baseline_positions =
        context.detection_details
            ->keypoints_pixels[context.selection->detection_index];
    if (baseline_positions.size() != state.positions_img.size()) {
        return;
    }
    std::vector<int> edited_indices;
    edited_indices.reserve(state.positions_img.size());
    for (size_t i = 0; i < state.positions_img.size(); ++i) {
        const auto& baseline = baseline_positions[i];
        const auto& current = state.positions_img[i];
        const bool baseline_finite =
            std::isfinite(baseline[0]) && std::isfinite(baseline[1]);
        const bool current_finite =
            std::isfinite(current[0]) && std::isfinite(current[1]);
        if (baseline_finite != current_finite) {
            edited_indices.push_back(static_cast<int>(i));
            continue;
        }
        if (!baseline_finite) {
            continue;
        }
        if (std::abs(baseline[0] - current[0]) > 0.01f ||
            std::abs(baseline[1] - current[1]) > 0.01f) {
            edited_indices.push_back(static_cast<int>(i));
        }
    }
    if (!headingComputationDependsOnEditedIndices(*context.heading_spec,
                                                  edited_indices)) {
        return;
    }

    const auto positions_d = convertKeypointPositionsToDouble(state.positions_img);
    double heading_deg = std::numeric_limits<double>::quiet_NaN();
    std::array<double, 2> origin{};
    if (!evaluateKeypointHeadingDegrees(*context.heading_spec,
                                        positions_d,
                                        heading_deg,
                                        &origin)) {
        return;
    }

    const float image_height = context.image_height_px;
    const float rad = static_cast<float>(heading_deg * kPi / 180.0);
    const float arrow_len =
        std::max(60.0f, std::max(context.image_width_px, context.image_height_px) * 0.07f);
    const float base_x = static_cast<float>(origin[0]);
    const float base_y = static_cast<float>(origin[1]);
    const float end_x = base_x + std::cos(rad) * arrow_len;
    const float end_y = base_y - std::sin(rad) * arrow_len;

    ImDrawList* plot_draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 p0 =
        ImPlot::PlotToPixels(ImPlotPoint(base_x, image_height - base_y));
    const ImVec2 p1 =
        ImPlot::PlotToPixels(ImPlotPoint(end_x, image_height - end_y));
    const ImU32 dashed_color = IM_COL32(255, 210, 80, 240);
    const int dash_count = 10;
    for (int dash_idx = 0; dash_idx < dash_count; ++dash_idx) {
        const float start_t = static_cast<float>(dash_idx) / dash_count;
        const float end_t = std::min(1.0f, start_t + 0.06f);
        const ImVec2 seg_a(p0.x + (p1.x - p0.x) * start_t,
                           p0.y + (p1.y - p0.y) * start_t);
        const ImVec2 seg_b(p0.x + (p1.x - p0.x) * end_t,
                           p0.y + (p1.y - p0.y) * end_t);
        plot_draw_list->AddLine(seg_a, seg_b, dashed_color, 2.0f);
    }

    ImVec2 dir(p0.x - p1.x, p0.y - p1.y);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len > 1e-3f) {
        dir.x /= len;
        dir.y /= len;
        constexpr float head_size = 8.0f;
        const ImVec2 left(
            p1.x + dir.x * head_size + dir.y * head_size * 0.5f,
            p1.y + dir.y * head_size - dir.x * head_size * 0.5f);
        const ImVec2 right(
            p1.x + dir.x * head_size - dir.y * head_size * 0.5f,
            p1.y + dir.y * head_size + dir.x * head_size * 0.5f);
        plot_draw_list->AddLine(p1, left, dashed_color, 2.0f);
        plot_draw_list->AddLine(p1, right, dashed_color, 2.0f);
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
    drawCandidateHeadingOverlay(context, result.state);

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
    if (state.positions_img.empty()) {
        if (error_message != nullptr) {
            *error_message =
                "Full-frame keypoint edit failed: no editable keypoints are loaded.";
        }
        return false;
    }

    action.type = CropKeypointEditorActionType::Save;
    action.keypoints_roi.resize(state.positions_img.size());
    for (size_t i = 0; i < state.positions_img.size(); ++i) {
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
