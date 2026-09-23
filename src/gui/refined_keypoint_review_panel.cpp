#include "gui/refined_keypoint_review_panel.h"

#include "gui/full_frame_keypoint_edit_overlay.h"
#include "gui/refined_keypoint_style.h"
#include "gui/review_metadata_editor.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {

bool sameSelection(const FullFrameKeypointEditState& state,
                   const RefinedKeypointSelection& selection) {
    return state.roi_index == selection.roi_index &&
           state.frame == static_cast<int>(selection.frame_id) &&
           state.detection_index ==
               static_cast<int>(selection.detection_index) &&
           state.run_name == selection.run_name;
}

bool hasFinitePosition(const std::array<float, 2>& point) {
    return std::isfinite(point[0]) && std::isfinite(point[1]);
}

void drawCurrentFrameKeypointStatusMatrix(
    const RefinedKeypointReviewPanelContext& context,
    const RefinedKeypointReviewPanelState& state,
    const std::optional<RefinedKeypointSelection>& selection) {
    if (context.detection_details == nullptr ||
        !context.detection_details->has_keypoints ||
        context.detection_details->keypoint_labels.empty() ||
        context.detection_details->keypoints_pixels.empty()) {
        ImGui::TextDisabled("Keypoint status unavailable for current frame.");
        return;
    }
    const auto& detections = *context.detection_details;

    size_t keypoint_count = detections.keypoint_labels.size();
    for (const auto& positions : detections.keypoints_pixels) {
        keypoint_count = std::min(keypoint_count, positions.size());
    }
    if (keypoint_count == 0) {
        ImGui::TextDisabled("Keypoint status unavailable for current frame.");
        return;
    }

    std::unordered_set<std::string> heading_labels;
    const auto& heading_spec = context.zarr_loader.getHeadingComputationSpec();
    if (heading_spec.available && heading_spec.enabled) {
        heading_labels.insert(heading_spec.dependent_labels.begin(),
                              heading_spec.dependent_labels.end());
    }

    ImGui::Separator();
    ImGui::Text("Keypoint Status:");

    static ImGuiTableFlags table_flags =
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_HighlightHoveredColumn;

    const int columns_count = static_cast<int>(keypoint_count) + 1;
    const float text_base_height = ImGui::GetTextLineHeightWithSpacing();
    const float data_row_count =
        static_cast<float>(detections.keypoints_pixels.size()) +
        (heading_labels.empty() ? 0.0f : 1.0f);
    const float table_height = std::max(
        text_base_height * 9.0f,
        ImGui::GetFontSize() * 4.5f + text_base_height * (data_row_count + 2.0f));
    if (!ImGui::BeginTable("##selected_refined_keypoint_status",
                           columns_count,
                           table_flags,
                           ImVec2(0.0f, table_height))) {
        return;
    }

    ImGui::TableSetupColumn("Status",
                            ImGuiTableColumnFlags_NoHide |
                                ImGuiTableColumnFlags_NoReorder);
    for (size_t column = 0; column < keypoint_count; ++column) {
        ImGui::TableSetupColumn(
            detections.keypoint_labels[column].c_str(),
            ImGuiTableColumnFlags_AngledHeader |
                ImGuiTableColumnFlags_WidthFixed);
    }
    ImGui::TableSetupScrollFreeze(1, 2);
    ImGui::TableAngledHeadersRow();
    ImGui::TableHeadersRow();

    auto draw_detection_row = [&](size_t det_idx) {
        const auto& positions = detections.keypoints_pixels[det_idx];
        const bool row_selected =
            selection.has_value() && selection->valid &&
            selection->detection_index == det_idx;
        const std::vector<std::array<float, 2>>* displayed_positions =
            &positions;
        int active_handle = -1;
        if (row_selected && (state.full_frame_edit.enabled || state.full_frame_edit.dirty) &&
            sameSelection(state.full_frame_edit, *selection) &&
            state.full_frame_edit.positions_img.size() == keypoint_count) {
            displayed_positions = &state.full_frame_edit.positions_img;
            active_handle = state.full_frame_edit.active_handle;
        }

        ImGui::TableNextRow();
        if (row_selected) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_RowBg0,
                ImGui::GetColorU32(ImVec4(0.22f, 0.28f, 0.4f, 0.28f)));
        }
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("det %zu%s", det_idx, row_selected ? " *" : "");

        for (size_t kp_idx = 0; kp_idx < keypoint_count; ++kp_idx) {
            if (!ImGui::TableSetColumnIndex(static_cast<int>(kp_idx + 1))) {
                continue;
            }

            const bool has_position =
                kp_idx < displayed_positions->size() &&
                hasFinitePosition((*displayed_positions)[kp_idx]);
            const bool is_active = active_handle == static_cast<int>(kp_idx);

            ImVec4 cell_color(0.0f, 0.0f, 0.0f, 0.0f);
            if (has_position) {
                cell_color = chooseRefinedKeypointColor(
                    detections.keypoint_labels[kp_idx], kp_idx);
                cell_color.w = is_active ? 1.0f : 0.85f;
            }
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                   ImGui::GetColorU32(cell_color));

            if (is_active) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "A");
            }
        }
    };

    for (size_t det_idx = 0; det_idx < detections.keypoints_pixels.size();
         ++det_idx) {
        draw_detection_row(det_idx);
    }

    if (!heading_labels.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Heading deps");

        for (size_t kp_idx = 0; kp_idx < keypoint_count; ++kp_idx) {
            if (!ImGui::TableSetColumnIndex(static_cast<int>(kp_idx + 1))) {
                continue;
            }
            const bool is_heading_keypoint =
                heading_labels.find(detections.keypoint_labels[kp_idx]) !=
                heading_labels.end();
            ImVec4 cell_color(0.0f, 0.0f, 0.0f, 0.0f);
            if (is_heading_keypoint) {
                cell_color = chooseRefinedKeypointColor(
                    detections.keypoint_labels[kp_idx], kp_idx);
                cell_color.w = 0.35f;
            }
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                   ImGui::GetColorU32(cell_color));
            if (is_heading_keypoint) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.95f), "H");
            }
        }
    }

    ImGui::EndTable();
}

}  // namespace

RefinedKeypointReviewPanelResult drawRefinedKeypointReviewPanel(
    const RefinedKeypointReviewPanelContext& context,
    RefinedKeypointReviewPanelState& state) {
    RefinedKeypointReviewPanelResult result;

    if (!context.zarr_loader.hasKeypointData()) {
        return result;
    }

    ImGui::Separator();
    ImGui::Text("Keypoint Review Write:");

    RefinedKeypointRepository refined_keypoint_repo(context.zarr_loader);
    std::string keypoint_edit_reason;
    const bool can_write_kp_review =
        refined_keypoint_repo.canEditActiveRun(&keypoint_edit_reason);

    if (context.selected_frame == context.current_frame_num &&
        context.selected_box >= 0) {
        result.selected_selection =
            refined_keypoint_repo.resolveFrameDetectionSelection(
                static_cast<size_t>(context.current_frame_num),
                static_cast<size_t>(context.selected_box),
                false);
    }

    if (result.selected_selection.has_value()) {
        const auto& selection = *result.selected_selection;
        if (selection.valid) {
            ImGui::Text("Selected ROI: %d | detection %zu | run: %s",
                        selection.roi_index,
                        selection.detection_index,
                        selection.run_name.c_str());
        } else if (!selection.message.empty()) {
            ImGui::TextWrapped("%s", selection.message.c_str());
        }
    } else {
        ImGui::TextDisabled("Select a detection to edit refined keypoints.");
    }
    drawCurrentFrameKeypointStatusMatrix(context, state, result.selected_selection);

    ImGui::Separator();
    ImGui::Text("Keypoint Edit:");

    const bool has_editable_selection =
        result.selected_selection.has_value() && result.selected_selection->valid &&
        result.selected_selection->editable;
    if (!has_editable_selection) {
        ImGui::TextDisabled(
            "Select a refined keypoint detection to edit in the main video.");
    } else {
        ImGui::TextDisabled(
            "Drag selected points in the main video. Use the advanced crop preview only when you need a larger precision view.");
    }
    ImGui::Checkbox("Show advanced crop preview",
                    &state.show_advanced_crop_preview);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Opens the legacy crop/keypoint precision window. The ROI inset is the normal review view.");
    }

    ImGui::BeginDisabled(!has_editable_selection);
    ImGui::Checkbox("Enable full-frame keypoint edit",
                    &state.full_frame_edit.enabled);
    if (state.full_frame_edit.enabled) {
        ImGui::Checkbox("Show keypoint names",
                        &state.full_frame_edit.show_labels);
    }
    ImGui::EndDisabled();
    if (state.full_frame_edit.enabled && context.play_video) {
        ImGui::TextDisabled("Pause playback to drag or save keypoints.");
    }
    if (state.full_frame_edit.enabled && state.full_frame_edit.dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                           "Unsaved full-frame keypoint changes.");
    }

    ImGui::BeginDisabled(!has_editable_selection || context.play_video);
    if (ImGui::Button("Save Keypoint Edit")) {
        std::string edit_error;
        if (!buildFullFrameKeypointEditAction(*result.selected_selection,
                                              state.full_frame_edit,
                                              result.edit_action,
                                              &edit_error)) {
            state.manual_write_status = edit_error;
        } else {
            state.manual_write_status.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Mark No Keypoints")) {
        state.manual_write_status.clear();
        result.edit_action.type = CropKeypointEditorActionType::MarkNoKeypoints;
    }
    ImGui::SameLine();
    if (ImGui::Button("Mark Detection Issue")) {
        state.manual_write_status.clear();
        result.edit_action.type =
            CropKeypointEditorActionType::MarkDetectionIssue;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Keypoint Edit")) {
        resetFullFrameKeypointEditState(state.full_frame_edit);
        state.manual_write_status = "Reset unsaved full-frame keypoint edits.";
    }
    ImGui::EndDisabled();

    if (!state.manual_write_status.empty()) {
        ImGui::TextColored(ImVec4(0.8f, 0.95f, 0.6f, 1.0f),
                           "%s",
                           state.manual_write_status.c_str());
    }

    if (!can_write_kp_review) {
        ImGui::TextWrapped("%s", keypoint_edit_reason.c_str());
    }

    ImGui::BeginDisabled(!can_write_kp_review);
    drawReviewMetadataEditor("kp_review_write", state.review_metadata);
    if (ImGui::Button("Write Keypoint Review Status")) {
        const auto metadata =
            resolveReviewMetadataValues(state.review_metadata);
        result.request_review_write = true;
        result.review_options.intended_use = metadata.intended_use;
        result.review_options.state = metadata.review_state;
        result.review_options.method = metadata.method;
        result.review_options.reviewer = metadata.reviewer;
        result.review_options.notes = metadata.notes;
    }
    ImGui::EndDisabled();

    ImGui::TextWrapped(
        "  Writes keypoint_review_status on refined_keypoints_runs/<active> and updates the latest status pointer.");
    if (!state.review_write_status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                           "%s",
                           state.review_write_status.c_str());
    }

    return result;
}
