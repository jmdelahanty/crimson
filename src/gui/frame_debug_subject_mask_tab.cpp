#include "gui/frame_debug_subject_mask_tab.h"

#include "gui/frame_debug_subject_mask_adapter.h"
#include "gui/frame_inspect_subject_mask_module.h"

#include "imgui.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace {

void drawReviewArtifactBlock(
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    if (artifact == nullptr) {
        ImGui::TextDisabled("Review metadata unavailable");
        return;
    }
    if (!artifact->run_name.empty()) {
        ImGui::Text("Run: %s", artifact->run_name.c_str());
    }
    if (!artifact->has_review_status) {
        ImGui::TextDisabled("Review metadata unavailable");
        return;
    }

    const auto& review_state = artifact->review_state;
    ImVec4 status_color = (review_state == "approved")
                              ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                          : (review_state == "rejected")
                              ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                              : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
    ImGui::TextColored(status_color, "Review: %s", review_state.c_str());
    if (!artifact->review_intended_use.empty() ||
        !artifact->review_method.empty()) {
        ImGui::Text("Use: %s | Method: %s",
                    artifact->review_intended_use.c_str(),
                    artifact->review_method.c_str());
    }
    if (!artifact->review_timestamp.empty()) {
        ImGui::Text("Reviewed: %s", artifact->review_timestamp.c_str());
    }
    if (!artifact->review_reviewer.empty()) {
        ImGui::Text("Reviewer: %s", artifact->review_reviewer.c_str());
    }
    if (!artifact->review_notes.empty()) {
        ImGui::Text("Notes: %s", artifact->review_notes.c_str());
    }
}

const char* maskReviewTitle(const FrameDebugWindowContext& context) {
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks() ||
                   context.zarr_loader.hasSubjectShapeData()
               ? "Subject Mask Review:"
               : "Eye Mask Review:";
}

std::string shortSubjectMaskComponentLabel(const std::string& label) {
    if (label == "subject_body") {
        return "Body";
    }
    if (label == "eye_left") {
        return "Left eye";
    }
    if (label == "eye_right") {
        return "Right eye";
    }
    if (label == "swim_bladder") {
        return "Swim bladder";
    }
    return label;
}

void clearSubjectMaskShapeDrafts(SubjectMaskBrushState& brush) {
    brush.brush_hover_valid = false;
    brush.brush_hover = SubjectMaskRoiPoint{};
    brush.stroke_active = false;
    brush.last_row = -1;
    brush.last_col = -1;
    brush.lasso_active = false;
    brush.lasso_points.clear();
    brush.polygon_points.clear();
    brush.polygon_hover_valid = false;
    brush.polygon_hover = SubjectMaskRoiPoint{};
}

bool maskHasComponent(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask,
    const std::string& component_name) {
    return std::any_of(
        mask.subject_mask_components.begin(),
        mask.subject_mask_components.end(),
        [&](const ZarrDetectionLoader::FrameDetections::EyeMask::
                SubjectMaskComponent& component) {
            return component.label == component_name;
        });
}

std::string componentSummary(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask) {
    std::ostringstream oss;
    bool first = true;
    for (const char* label :
         {"subject_body", "eye_left", "eye_right", "swim_bladder"}) {
        if (!maskHasComponent(mask, label)) {
            continue;
        }
        if (!first) {
            oss << ", ";
        }
        oss << shortSubjectMaskComponentLabel(label);
        first = false;
    }
    return first ? "<none>" : oss.str();
}

std::string firstEditableComponent(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask) {
    for (const char* label :
         {"subject_body", "eye_left", "eye_right", "swim_bladder"}) {
        if (maskHasComponent(mask, label)) {
            return label;
        }
    }
    if (!mask.subject_mask_components.empty()) {
        return mask.subject_mask_components.front().label;
    }
    return {};
}

bool loadSubjectMaskEditTarget(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state,
    int detection_index,
    int32_t roi_index,
    const std::string& component_name) {
    if (roi_index < 0 || component_name.empty()) {
        state.subject_mask_edit_status =
            "Preview load failed: no editable subject-mask target.";
        return false;
    }

    std::string error;
    if (state.subject_mask_edit_session.startFromLoadedRow(
            context.zarr_loader,
            static_cast<size_t>(roi_index),
            component_name,
            &error)) {
        state.subject_mask_edit_detection_index = detection_index;
        state.subject_mask_edit_component_name = component_name;
        clearSubjectMaskShapeDrafts(state.subject_mask_brush);
        const auto& target = state.subject_mask_edit_session.target();
        std::ostringstream oss;
        oss << "Preview loaded: detection=" << detection_index
            << " roi=" << target.roi_index
            << " component=" << target.component_name
            << " shape=" << target.rows << "x" << target.cols;
        state.subject_mask_edit_status = oss.str();
        return true;
    }

    state.subject_mask_edit_status = "Preview load failed: " + error;
    return false;
}

void drawSubjectMaskEditPreviewSection(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state) {
    if (!context.zarr_loader.eyeMasksUseRefinedSubjectMasks()) {
        return;
    }

    if (context.zarr_loader.getRefinedSubjectMaskOverlayComponents().empty()) {
        return;
    }

    if (state.subject_mask_edit_session.active() &&
        state.subject_mask_edit_session.target().zarr_path !=
            context.zarr_loader.getArchivePath()) {
        state.subject_mask_edit_session.clear();
        state.subject_mask_edit_status.clear();
    }

    ImGui::Separator();
    ImGui::Text("Subject Mask Editor:");
    ImGui::Checkbox("Enable canvas mask selection",
                    &state.subject_mask_canvas_pick_enabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When enabled, left-clicking visible subject masks in the camera view selects an edit target. Turn this off to leave canvas clicks available for keypoint editing or navigation while masks stay visible.");
    }

    const auto* masks =
        (context.detection_details != nullptr &&
         context.detection_details->includes_eye_masks)
            ? &context.detection_details->eye_masks
            : nullptr;
    std::vector<int> editable_target_indices;
    if (masks != nullptr) {
        for (int idx = 0; idx < static_cast<int>(masks->size()); ++idx) {
            if ((*masks)[static_cast<size_t>(idx)].roi_index >= 0) {
                editable_target_indices.push_back(idx);
            }
        }
    }

    if (masks == nullptr || editable_target_indices.empty()) {
        ImGui::TextDisabled("No editable subject-mask rows on this frame.");
    } else {
        const bool selected_target_on_frame = std::find(
            editable_target_indices.begin(),
            editable_target_indices.end(),
            state.subject_mask_edit_detection_index) !=
            editable_target_indices.end();
        if (!selected_target_on_frame) {
            state.subject_mask_edit_detection_index =
                editable_target_indices.front();
        }

        ImGuiTableFlags table_flags =
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##subject_mask_target_table",
                              4,
                              table_flags)) {
            ImGui::TableSetupColumn("Detection");
            ImGui::TableSetupColumn("ROI row");
            ImGui::TableSetupColumn("Components");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();

            for (int idx : editable_target_indices) {
                const auto& mask = (*masks)[static_cast<size_t>(idx)];
                const bool active_target =
                    state.subject_mask_edit_session.active() &&
                    state.subject_mask_edit_session.target().roi_index ==
                        mask.roi_index;
                const bool selected_row =
                    idx == state.subject_mask_edit_detection_index ||
                    active_target;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string label = "Detection " + std::to_string(idx);
                if (ImGui::Selectable(label.c_str(), selected_row)) {
                    std::string component_name =
                        state.subject_mask_edit_component_name;
                    if (!maskHasComponent(mask, component_name)) {
                        component_name = firstEditableComponent(mask);
                    }
                    loadSubjectMaskEditTarget(context,
                                              state,
                                              idx,
                                              mask.roi_index,
                                              component_name);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", mask.roi_index);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", componentSummary(mask).c_str());
                ImGui::TableSetColumnIndex(3);
                if (active_target &&
                    state.subject_mask_edit_session.dirty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f),
                                       "dirty");
                } else if (active_target) {
                    ImGui::Text("preview");
                } else {
                    ImGui::TextDisabled("clean");
                }
            }
            ImGui::EndTable();
        }

        const auto& selected_mask =
            (*masks)[static_cast<size_t>(
                state.subject_mask_edit_detection_index)];
        ImGui::Text("Component:");
        bool first_button = true;
        for (const char* label :
             {"subject_body", "eye_left", "eye_right", "swim_bladder"}) {
            const bool available = maskHasComponent(selected_mask, label);
            const bool selected =
                state.subject_mask_edit_component_name == label &&
                state.subject_mask_edit_session.active() &&
                state.subject_mask_edit_session.target().roi_index ==
                    selected_mask.roi_index;
            if (!first_button) {
                ImGui::SameLine();
            }
            first_button = false;
            ImGui::BeginDisabled(!available);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.34f, 0.34f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(0.42f, 0.42f, 0.24f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(0.48f, 0.48f, 0.28f, 1.0f));
            }
            if (ImGui::Button(shortSubjectMaskComponentLabel(label).c_str())) {
                loadSubjectMaskEditTarget(
                    context,
                    state,
                    state.subject_mask_edit_detection_index,
                    selected_mask.roi_index,
                    label);
            }
            if (selected) {
                ImGui::PopStyleColor(3);
            }
            ImGui::EndDisabled();
        }
    }

    if (state.subject_mask_edit_session.active()) {
        ImGui::Separator();
        ImGui::Text("Preview Tools:");
        ImGui::Checkbox("Enable preview tools",
                        &state.subject_mask_brush.enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Paints only the in-memory preview mask. Persisted Zarr data is unchanged because save is still disabled.");
        }
        ImGui::BeginDisabled(!state.subject_mask_brush.enabled);
        ImGui::Text("Tool:");
        bool tool_changed = false;
        if (ImGui::RadioButton(
                "Brush",
                state.subject_mask_brush.tool ==
                    SubjectMaskPreviewTool::Brush)) {
            state.subject_mask_brush.tool = SubjectMaskPreviewTool::Brush;
            tool_changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(
                "Lasso",
                state.subject_mask_brush.tool ==
                    SubjectMaskPreviewTool::Lasso)) {
            state.subject_mask_brush.tool = SubjectMaskPreviewTool::Lasso;
            tool_changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(
                "Polygon",
                state.subject_mask_brush.tool ==
                    SubjectMaskPreviewTool::Polygon)) {
            state.subject_mask_brush.tool = SubjectMaskPreviewTool::Polygon;
            tool_changed = true;
        }
        if (tool_changed) {
            clearSubjectMaskShapeDrafts(state.subject_mask_brush);
        }
        if (state.subject_mask_brush.tool == SubjectMaskPreviewTool::Brush) {
            ImGui::SliderInt("Radius (ROI px)",
                             &state.subject_mask_brush.radius_px,
                             1,
                             64);
        }
        bool erase_mode = state.subject_mask_brush.erase;
        if (ImGui::RadioButton("Paint", !erase_mode)) {
            state.subject_mask_brush.erase = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Erase", erase_mode)) {
            state.subject_mask_brush.erase = true;
        }
        if (state.subject_mask_brush.tool == SubjectMaskPreviewTool::Lasso) {
            ImGui::TextDisabled("Drag in the selected ROI; release fills the lasso.");
        } else if (state.subject_mask_brush.tool ==
                   SubjectMaskPreviewTool::Polygon) {
            ImGui::TextDisabled("Left-click vertices in the selected ROI; double-click or Apply fills.");
            ImGui::BeginDisabled(
                state.subject_mask_brush.polygon_points.size() < 3);
            if (ImGui::Button("Apply Polygon")) {
                const uint8_t value =
                    state.subject_mask_brush.erase ? 0 : 1;
                if (state.subject_mask_edit_session.fillPolygon(
                        state.subject_mask_brush.polygon_points, value)) {
                    state.subject_mask_edit_status =
                        "Preview dirty: polygon fill applied.";
                } else {
                    state.subject_mask_edit_status =
                        "Polygon fill made no preview change.";
                }
                state.subject_mask_brush.polygon_points.clear();
                state.subject_mask_brush.polygon_hover_valid = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(
                state.subject_mask_brush.polygon_points.empty());
            if (ImGui::Button("Cancel Polygon")) {
                state.subject_mask_brush.polygon_points.clear();
                state.subject_mask_brush.polygon_hover_valid = false;
                state.subject_mask_edit_status = "Polygon draft canceled.";
            }
            ImGui::EndDisabled();
            if (!state.subject_mask_brush.polygon_points.empty()) {
                ImGui::TextDisabled("Vertices: %zu",
                                    state.subject_mask_brush
                                        .polygon_points.size());
            }
        }
        ImGui::EndDisabled();
        if (!state.subject_mask_brush.enabled) {
            clearSubjectMaskShapeDrafts(state.subject_mask_brush);
        }

        if (ImGui::Button("Reset Preview")) {
            state.subject_mask_edit_session.resetPreview();
            clearSubjectMaskShapeDrafts(state.subject_mask_brush);
            state.subject_mask_edit_status =
                "Preview reset to loaded mask row.";
        }

        const auto& target = state.subject_mask_edit_session.target();
        ImGui::Text("Active: %s row %d",
                    target.component_name.c_str(),
                    target.roi_index);
        ImGui::Text("Shape: %zux%zu | Dirty: %s",
                    target.rows,
                    target.cols,
                    state.subject_mask_edit_session.dirty() ? "yes" : "no");
        ImGui::TextDisabled("Save backend: PreviewOnly");
        ImGui::BeginDisabled(true);
        ImGui::Button("Save");
        ImGui::EndDisabled();
    }

    if (!state.subject_mask_edit_status.empty()) {
        ImGui::TextWrapped("%s", state.subject_mask_edit_status.c_str());
    }
}

void drawSubjectShapeQcSection(const FrameDebugWindowContext& context,
                               FrameDebugWindowState& state,
                               FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasSubjectShapeData()) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Subject Shape QC:");
    ImGui::Text("Rows: %zu | Run: %s",
                context.zarr_loader.getSubjectShapeRowCount(),
                context.zarr_loader.getSubjectShapeRunName().c_str());

    auto filters = state.subject_shape_qc_filters;
    bool changed = false;
    changed |= ImGui::Checkbox("Any invalid", &filters.any_invalid);
    changed |= ImGui::Checkbox("Source mask QC failure",
                               &filters.source_mask_qc_failure);
    changed |= ImGui::Checkbox("Body frame invalid",
                               &filters.body_frame_invalid);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Snout invalid", &filters.snout_invalid);
    changed |= ImGui::Checkbox("Centerline invalid",
                               &filters.centerline_invalid);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Centerline misses snout",
                               &filters.centerline_misses_snout);
    changed |= ImGui::Checkbox("B-spline invalid",
                               &filters.bspline_invalid);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Tail base invalid",
                               &filters.tail_base_invalid);
    changed |= ImGui::Checkbox("Tail samples invalid",
                               &filters.tail_sample_invalid);
    if (ImGui::InputText("Reason contains",
                         state.subject_shape_reason_filter.data(),
                         state.subject_shape_reason_filter.size())) {
        changed = true;
    }
    if (changed) {
        filters.reason_substring =
            state.subject_shape_reason_filter.data();
        state.subject_shape_qc_filters = filters;
        state.subject_shape_qc_status.clear();
    } else {
        state.subject_shape_qc_filters.reason_substring =
            state.subject_shape_reason_filter.data();
    }

    if (ImGui::Button("Prev Shape QC Frame")) {
        result.request_prev_subject_shape_qc_frame = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Shape QC Frame")) {
        result.request_next_subject_shape_qc_frame = true;
    }
    if (!state.subject_shape_qc_status.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                           "%s",
                           state.subject_shape_qc_status.c_str());
    }
}

}  // namespace

const char* frameDebugSubjectMaskTabTitle(
    const FrameDebugWindowContext& context) {
    return context.zarr_loader.eyeMasksUseRefinedSubjectMasks() ||
                   context.zarr_loader.hasSubjectShapeData()
               ? "Subject Masks"
               : "Eye Masks";
}

void drawSubjectMaskTab(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state,
    FrameDebugWindowResult& result,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact) {
    if (!context.zarr_loader.hasEyeMasks() &&
        !context.zarr_loader.hasSubjectShapeData() &&
        artifact == nullptr) {
        ImGui::TextDisabled("Subject mask data unavailable");
        return;
    }

    const auto presentation =
        makeFrameDebugSubjectMaskInspectPresentation(context);
    crimson::gui::drawFrameInspectSubjectMaskModule(
        presentation, state.subject_mask_inspect);

    if (context.zarr_loader.hasEyeMasks()) {
        drawSubjectMaskEditPreviewSection(context, state);
    }
    drawSubjectShapeQcSection(context, state, result);

    ImGui::Separator();
    ImGui::Text("%s", maskReviewTitle(context));
    drawReviewArtifactBlock(artifact);
}
