#include "gui/frame_debug_eye_angle_tab.h"

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <string>

namespace {

const ZarrDetectionData::EyeAngleRepresentationInfo*
findEyeAngleRepresentation(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    int index) {
    if (index < 0 ||
        static_cast<size_t>(index) >= eye.representations.size()) {
        return nullptr;
    }
    return &eye.representations[static_cast<size_t>(index)];
}

std::string eyeReasonAt(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    size_t row) {
    if (row < eye.roi_reason_labels.size()) {
        return eye.roi_reason_labels[row];
    }
    return {};
}

void drawEyeAngleCurrentValues(
    const FrameDebugWindowContext& context,
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep,
    size_t row) {
    ImGui::Text("Current eye-angle row: %zu", row);
    if (row < eye.row_to_frame.size()) {
        ImGui::Text("Row frame: %d", eye.row_to_frame[row]);
    }
    const bool left_valid =
        eye.roi_valid_left.empty() ||
        (row < eye.roi_valid_left.size() && eye.roi_valid_left[row] != 0);
    const bool right_valid =
        eye.roi_valid_right.empty() ||
        (row < eye.roi_valid_right.size() && eye.roi_valid_right[row] != 0);
    const bool frame_valid =
        eye.roi_valid_frame.empty() ||
        (row < eye.roi_valid_frame.size() && eye.roi_valid_frame[row] != 0);
    ImGui::Text("QA: left=%s right=%s frame=%s",
                left_valid ? "valid" : "invalid",
                right_valid ? "valid" : "invalid",
                frame_valid ? "valid" : "invalid");
    const bool marginal =
        (row < eye.roi_major_axis_marginal.size() &&
         eye.roi_major_axis_marginal[row] != 0) ||
        (row < eye.roi_left_major_axis_marginal.size() &&
         eye.roi_left_major_axis_marginal[row] != 0) ||
        (row < eye.roi_right_major_axis_marginal.size() &&
         eye.roi_right_major_axis_marginal[row] != 0);
    if (marginal) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "Major-axis marginal");
    }
    const std::string reason = eyeReasonAt(eye, row);
    if (!reason.empty()) {
        ImGui::TextWrapped("Reason: %s", reason.c_str());
    }

    auto draw_scalar_value = [&](const std::string& field_name) {
        const auto* field =
            context.zarr_loader.findEyeAngleScalarField(field_name);
        if (field == nullptr || !field->has_roi ||
            row >= field->roi_values.size()) {
            return;
        }
        const float value = field->roi_values[row];
        if (!std::isfinite(value)) {
            ImGui::Text("%s: NaN", field_name.c_str());
        } else {
            ImGui::Text("%s: %.2f %s",
                        field_name.c_str(),
                        value,
                        field->units.empty() ? "deg" : field->units.c_str());
        }
    };
    for (const auto& field : rep.primary_roi_fields) {
        draw_scalar_value(field);
    }
    for (const auto& field : rep.aggregate_roi_fields) {
        draw_scalar_value(field);
    }
    for (const auto& field_name : rep.vector_roi_fields) {
        const auto* field =
            context.zarr_loader.findEyeAngleVectorField(field_name);
        if (field == nullptr || !field->has_roi ||
            row >= field->roi_values.size()) {
            continue;
        }
        const auto value = field->roi_values[row];
        ImGui::Text("%s: [%.3f, %.3f]",
                    field_name.c_str(),
                    value[0],
                    value[1]);
    }
}

}  // namespace

void drawEyeAngleTab(const FrameDebugWindowContext& context,
                     FrameDebugWindowState& state,
                     FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasEyeAngleAnalysisData()) {
        ImGui::TextDisabled("Eye-angle analysis data unavailable");
        return;
    }

    const auto& eye = context.zarr_loader.getEyeAngleAnalysisData();
    ImGui::Text("Run: %s", eye.run_name.c_str());
    ImGui::Text("Schema: %s v%d",
                eye.schema_id.empty() ? "<unknown>" : eye.schema_id.c_str(),
                eye.schema_version);
    ImGui::Text("Method: %s %s",
                eye.method.empty() ? "<unknown>" : eye.method.c_str(),
                eye.method_version.c_str());
    ImGui::Text("Source geometry: %s",
                eye.source_geometry_kind.empty()
                    ? "<unknown>"
                    : eye.source_geometry_kind.c_str());
    if (!eye.source_eye_geometry_run.empty()) {
        ImGui::Text("Source eye geometry run: %s",
                    eye.source_eye_geometry_run.c_str());
    }
    if (!eye.source_subject_shape_run.empty()) {
        ImGui::Text("Source subject shape: %s",
                    eye.source_subject_shape_run.c_str());
    }
    if (eye.variant_schema_inferred) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "Representation metadata inferred");
    }
    if (!eye.warning.empty()) {
        ImGui::TextWrapped("Warning: %s", eye.warning.c_str());
    }
    ImGui::Text("Rows: %zu | Frame rows: %zu | Scalars: %zu | Vectors: %zu",
                eye.row_count,
                eye.frame_count,
                eye.scalar_fields.size(),
                eye.vector_fields.size());

    if (eye.representations.empty()) {
        ImGui::TextDisabled("No eye-angle representations available");
        return;
    }
    if (state.eye_angle_representation_index < 0 ||
        static_cast<size_t>(state.eye_angle_representation_index) >=
            eye.representations.size()) {
        state.eye_angle_representation_index = 0;
        for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
            if (eye.representations[idx].key == eye.default_representation) {
                state.eye_angle_representation_index =
                    static_cast<int>(idx);
                break;
            }
        }
    }
    const auto* selected_rep = findEyeAngleRepresentation(
        eye, state.eye_angle_representation_index);
    const char* preview =
        selected_rep == nullptr
            ? "<none>"
            : (selected_rep->display_name.empty()
                   ? selected_rep->key.c_str()
                   : selected_rep->display_name.c_str());
    if (ImGui::BeginCombo("Representation", preview)) {
        for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
            const auto& rep = eye.representations[idx];
            const bool selected =
                static_cast<int>(idx) == state.eye_angle_representation_index;
            const char* label =
                rep.display_name.empty() ? rep.key.c_str()
                                         : rep.display_name.c_str();
            if (ImGui::Selectable(label, selected)) {
                state.eye_angle_representation_index =
                    static_cast<int>(idx);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    selected_rep = findEyeAngleRepresentation(
        eye, state.eye_angle_representation_index);
    if (selected_rep == nullptr) {
        return;
    }
    if (!selected_rep->role.empty() || !selected_rep->axis.empty()) {
        ImGui::Text("Role: %s | Axis: %s",
                    selected_rep->role.c_str(),
                    selected_rep->axis.c_str());
    }

    const auto current_row = context.zarr_loader.findEyeAngleRowForFrame(
        context.current_frame_num);
    if (current_row) {
        state.eye_angle_selected_row = static_cast<int>(*current_row);
        drawEyeAngleCurrentValues(context, eye, *selected_rep, *current_row);
    } else {
        ImGui::TextDisabled("No eye-angle row mapped to current frame");
    }

    int selected_row = state.eye_angle_selected_row;
    if (ImGui::InputInt("Selected eye-angle row", &selected_row)) {
        selected_row = std::clamp(
            selected_row,
            -1,
            eye.row_count == 0
                ? -1
                : static_cast<int>(eye.row_count - 1));
        state.eye_angle_selected_row = selected_row;
    }
    if (state.eye_angle_selected_row >= 0) {
        const size_t row = static_cast<size_t>(state.eye_angle_selected_row);
        if (row < eye.row_to_frame.size()) {
            ImGui::Text("Selected row frame: %d", eye.row_to_frame[row]);
        }
        const std::string reason = eyeReasonAt(eye, row);
        if (!reason.empty()) {
            ImGui::TextWrapped("Selected reason: %s", reason.c_str());
        }
        if (ImGui::Button("Seek Selected Eye Row")) {
            result.request_seek_eye_angle_row = true;
            result.requested_eye_angle_row = row;
        }
    }

    ImGui::Separator();
    ImGui::Text("Eye-Angle QC:");
    auto filters = state.eye_angle_qc_filters;
    bool changed = false;
    changed |= ImGui::Checkbox("Invalid rows", &filters.invalid_rows);
    changed |= ImGui::Checkbox("Major-axis marginal",
                               &filters.major_axis_marginal);
    if (ImGui::InputText("Eye reason contains",
                         state.eye_angle_reason_filter.data(),
                         state.eye_angle_reason_filter.size())) {
        changed = true;
    }
    if (changed) {
        filters.reason_substring = state.eye_angle_reason_filter.data();
        state.eye_angle_qc_filters = filters;
        state.eye_angle_qc_status.clear();
    } else {
        state.eye_angle_qc_filters.reason_substring =
            state.eye_angle_reason_filter.data();
    }
    if (ImGui::Button("Prev Eye QC Frame")) {
        result.request_prev_eye_angle_qc_frame = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Eye QC Frame")) {
        result.request_next_eye_angle_qc_frame = true;
    }
    if (!state.eye_angle_qc_status.empty()) {
        ImGui::TextWrapped("%s", state.eye_angle_qc_status.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled(
        "Eye-angle time-series traces are shown in Analysis Timeline.");
}
