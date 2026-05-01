#include "gui/frame_debug_eye_angle_tab.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

bool stringEndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string eyeUnsmoothedBaseField(const std::string& field_name) {
    constexpr const char* kSmoothedSuffix = "_smoothed";
    if (!stringEndsWith(field_name, kSmoothedSuffix)) {
        return {};
    }
    return field_name.substr(0, field_name.size() - std::strlen(kSmoothedSuffix));
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool containsString(const std::vector<std::string>& values,
                    const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

void addUniqueString(std::vector<std::string>& values,
                     const std::string& value) {
    if (!value.empty() && !containsString(values, value)) {
        values.push_back(value);
    }
}

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

const ZarrDetectionData::EyeAngleScalarField* resolveEyeAnglePlotField(
    const ZarrDetectionLoader& loader,
    const std::string& requested_field,
    std::string& resolved_name) {
    resolved_name = requested_field;
    const auto* field = loader.findEyeAngleScalarField(requested_field);
    if (field != nullptr && (field->has_frame || field->has_roi)) {
        return field;
    }
    const std::string base = eyeUnsmoothedBaseField(requested_field);
    if (!base.empty()) {
        field = loader.findEyeAngleScalarField(base);
        if (field != nullptr && (field->has_frame || field->has_roi)) {
            resolved_name = base;
            return field;
        }
    }
    return nullptr;
}

const ZarrDetectionData::EyeAngleFieldInfo* findEyeAngleFieldInfo(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const std::string& field_name) {
    auto it = std::find_if(
        eye.fields.begin(),
        eye.fields.end(),
        [&](const auto& field) { return field.name == field_name; });
    if (it == eye.fields.end()) {
        return nullptr;
    }
    return &(*it);
}

std::string eyeAngleFieldDisplayName(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleScalarField* scalar_field,
    const std::string& field_name) {
    if (scalar_field != nullptr && !scalar_field->display_name.empty()) {
        return scalar_field->display_name;
    }
    if (const auto* info = findEyeAngleFieldInfo(eye, field_name);
        info != nullptr && !info->display_name.empty()) {
        return info->display_name;
    }
    return field_name;
}

std::string eyeAngleFieldAvailabilityLabel(
    const ZarrDetectionData::EyeAngleScalarField* field,
    const std::string& requested_name,
    const std::string& resolved_name) {
    if (field == nullptr) {
        return "unavailable";
    }
    std::string label;
    if (field->has_roi && field->has_frame) {
        label = "roi+frame";
    } else if (field->has_frame) {
        label = "frame";
    } else if (field->has_roi) {
        label = "roi";
    } else {
        label = "empty";
    }
    if (resolved_name != requested_name) {
        label += ", fallback: " + resolved_name;
    }
    return label;
}

std::vector<std::string> defaultEyeAnglePlotFields(
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep) {
    return rep.default_plot_fields.empty() ? rep.primary_roi_fields
                                           : rep.default_plot_fields;
}

std::vector<std::string> collectEyeAngleScalarCandidates(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep) {
    std::vector<std::string> candidates;
    for (const auto& field : rep.default_plot_fields) {
        addUniqueString(candidates, field);
    }
    for (const auto& field : rep.primary_roi_fields) {
        addUniqueString(candidates, field);
    }
    for (const auto& field : rep.aggregate_roi_fields) {
        addUniqueString(candidates, field);
    }
    for (const auto& field : rep.frame_fields) {
        addUniqueString(candidates, field);
        addUniqueString(candidates, field + "_smoothed");
    }
    for (const auto& field : eye.scalar_fields) {
        if (field.representation_key == rep.key) {
            addUniqueString(candidates, field.name);
        }
    }
    return candidates;
}

std::vector<std::string> collectEyeAngleVectorCandidates(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep) {
    std::vector<std::string> candidates;
    for (const auto& field : rep.vector_roi_fields) {
        addUniqueString(candidates, field);
    }
    for (const auto& field : eye.vector_fields) {
        if (field.representation_key == rep.key) {
            addUniqueString(candidates, field.name);
        }
    }
    return candidates;
}

void resetEyeAnglePlotSelection(
    const FrameDebugWindowContext& context,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep,
    FrameDebugWindowState& state) {
    state.eye_angle_selected_plot_fields.clear();
    for (const auto& field_name : defaultEyeAnglePlotFields(rep)) {
        std::string resolved_name;
        if (resolveEyeAnglePlotField(
                context.zarr_loader, field_name, resolved_name) != nullptr) {
            addUniqueString(state.eye_angle_selected_plot_fields, field_name);
        }
    }
}

const char* eyeAngleXAxisLabel(int mode) {
    switch (mode) {
        case 1:
            return "Time (s)";
        case 2:
            return "Row";
        default:
            return "Frame";
    }
}

std::optional<double> eyeAngleXForSample(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    bool use_frame_values,
    int x_axis_mode,
    size_t row) {
    if (x_axis_mode == 2) {
        return static_cast<double>(row);
    }
    if (x_axis_mode == 1) {
        const auto& times =
            use_frame_values ? eye.frame_time_seconds : eye.roi_time_seconds;
        if (row >= times.size() || !std::isfinite(times[row])) {
            return std::nullopt;
        }
        return static_cast<double>(times[row]);
    }
    if (use_frame_values) {
        return static_cast<double>(row);
    }
    if (row < eye.roi_frame_indices.size() &&
        eye.roi_frame_indices[row] >= 0) {
        return static_cast<double>(eye.roi_frame_indices[row]);
    }
    return static_cast<double>(row);
}

std::optional<double> currentEyeAnglePlotX(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    int x_axis_mode,
    int current_frame_num,
    std::optional<size_t> current_row) {
    if (x_axis_mode == 2) {
        if (current_row) {
            return static_cast<double>(*current_row);
        }
        return std::nullopt;
    }
    if (x_axis_mode == 1) {
        if (current_row && *current_row < eye.roi_time_seconds.size() &&
            std::isfinite(eye.roi_time_seconds[*current_row])) {
            return static_cast<double>(eye.roi_time_seconds[*current_row]);
        }
        if (current_frame_num >= 0 &&
            static_cast<size_t>(current_frame_num) <
                eye.frame_time_seconds.size() &&
            std::isfinite(eye.frame_time_seconds[current_frame_num])) {
            return static_cast<double>(
                eye.frame_time_seconds[current_frame_num]);
        }
        return std::nullopt;
    }
    return static_cast<double>(current_frame_num);
}

struct EyeAngleTraceBuffer {
    std::string requested_name;
    std::string resolved_name;
    std::string label;
    std::string units;
    bool use_frame_values = false;
    std::vector<double> xs;
    std::vector<double> ys;
};

bool buildEyeAngleTraceBuffer(
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleScalarField& field,
    const std::string& requested_name,
    const std::string& resolved_name,
    int x_axis_mode,
    EyeAngleTraceBuffer& out) {
    const bool use_frame = field.has_frame && !field.frame_values.empty();
    const auto& values = use_frame ? field.frame_values : field.roi_values;
    if (values.empty()) {
        return false;
    }

    out.requested_name = requested_name;
    out.resolved_name = resolved_name;
    out.label = field.display_name.empty() ? resolved_name : field.display_name;
    if (resolved_name != requested_name) {
        out.label += " (fallback)";
    }
    out.units = field.units.empty() ? "value" : field.units;
    out.use_frame_values = use_frame;
    out.xs.clear();
    out.ys.clear();
    out.xs.reserve(values.size());
    out.ys.reserve(values.size());
    for (size_t row = 0; row < values.size(); ++row) {
        const float value = values[row];
        if (!std::isfinite(value)) {
            continue;
        }
        auto x = eyeAngleXForSample(eye, use_frame, x_axis_mode, row);
        if (!x) {
            continue;
        }
        out.xs.push_back(*x);
        out.ys.push_back(static_cast<double>(value));
    }
    return out.xs.size() >= 2;
}

void drawEyeAngleTraceGroupPlot(
    const std::string& title,
    const std::vector<EyeAngleTraceBuffer>& traces,
    const char* x_axis_label,
    const std::string& units,
    std::optional<double> current_x) {
    if (traces.empty()) {
        return;
    }

    double y_min = std::numeric_limits<double>::infinity();
    double y_max = -std::numeric_limits<double>::infinity();
    for (const auto& trace : traces) {
        if (trace.ys.empty()) {
            continue;
        }
        const auto [trace_min, trace_max] =
            std::minmax_element(trace.ys.begin(), trace.ys.end());
        y_min = std::min(y_min, *trace_min);
        y_max = std::max(y_max, *trace_max);
    }
    if (!std::isfinite(y_min) || !std::isfinite(y_max)) {
        return;
    }
    if (y_min == y_max) {
        y_min -= 1.0;
        y_max += 1.0;
    }

    const std::string y_axis_label = units == "value" ? "Value" : units;
    if (ImPlot::BeginPlot(title.c_str(), ImVec2(-1.0f, 230.0f))) {
        ImPlot::SetupAxes(x_axis_label,
                          y_axis_label.c_str(),
                          ImPlotAxisFlags_AutoFit,
                          ImPlotAxisFlags_AutoFit);
        for (const auto& trace : traces) {
            ImPlot::PlotLine(trace.label.c_str(),
                             trace.xs.data(),
                             trace.ys.data(),
                             static_cast<int>(trace.xs.size()));
        }
        if (current_x) {
            const double current_line_x[2] = {*current_x, *current_x};
            const double current_line_y[2] = {y_min, y_max};
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 0.8f), 1.5f);
            ImPlot::PlotLine("Current", current_line_x, current_line_y, 2);
        }
        ImPlot::EndPlot();
    }
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

void drawEyeAngleFieldBrowser(
    const FrameDebugWindowContext& context,
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep,
    FrameDebugWindowState& state) {
    ImGui::Text("Plot Fields:");
    ImGui::InputText("Field filter",
                     state.eye_angle_field_filter.data(),
                     state.eye_angle_field_filter.size());

    if (ImGui::Button("Default")) {
        resetEyeAnglePlotSelection(context, rep, state);
    }
    ImGui::SameLine();
    if (ImGui::Button("All Available")) {
        state.eye_angle_selected_plot_fields.clear();
        for (const auto& field_name :
             collectEyeAngleScalarCandidates(eye, rep)) {
            std::string resolved_name;
            if (resolveEyeAnglePlotField(
                    context.zarr_loader, field_name, resolved_name) !=
                nullptr) {
                addUniqueString(state.eye_angle_selected_plot_fields,
                                field_name);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        state.eye_angle_selected_plot_fields.clear();
    }
    ImGui::TextDisabled("Selected scalar fields: %zu",
                        state.eye_angle_selected_plot_fields.size());

    const std::string filter = toLowerAscii(
        std::string(state.eye_angle_field_filter.data()));
    const auto candidates = collectEyeAngleScalarCandidates(eye, rep);
    int visible_scalar_count = 0;
    for (const auto& field_name : candidates) {
        std::string resolved_name;
        const auto* field = resolveEyeAnglePlotField(
            context.zarr_loader, field_name, resolved_name);
        const bool available = field != nullptr;
        const std::string display_name =
            eyeAngleFieldDisplayName(eye, field, field_name);
        const std::string search_text =
            toLowerAscii(field_name + " " + display_name);
        if (!filter.empty() &&
            search_text.find(filter) == std::string::npos) {
            continue;
        }

        ++visible_scalar_count;
        bool selected = containsString(state.eye_angle_selected_plot_fields,
                                       field_name);
        if (!available) {
            ImGui::BeginDisabled(true);
        }
        ImGui::PushID(field_name.c_str());
        if (ImGui::Checkbox(display_name.c_str(), &selected)) {
            if (selected) {
                addUniqueString(state.eye_angle_selected_plot_fields,
                                field_name);
            } else {
                auto& selected_fields = state.eye_angle_selected_plot_fields;
                selected_fields.erase(
                    std::remove(selected_fields.begin(),
                                selected_fields.end(),
                                field_name),
                    selected_fields.end());
            }
        }
        ImGui::PopID();
        if (!available) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]",
                            eyeAngleFieldAvailabilityLabel(
                                field, field_name, resolved_name)
                                .c_str());
        if (display_name != field_name) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", field_name.c_str());
        }
    }
    if (visible_scalar_count == 0) {
        ImGui::TextDisabled("No scalar fields match this filter");
    }

    const auto vector_candidates = collectEyeAngleVectorCandidates(eye, rep);
    if (!vector_candidates.empty() &&
        ImGui::CollapsingHeader("Vector Fields",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& field_name : vector_candidates) {
            const auto* field =
                context.zarr_loader.findEyeAngleVectorField(field_name);
            ImGui::TextDisabled("%s [%s]",
                                field_name.c_str(),
                                (field != nullptr && field->has_roi)
                                    ? "roi vector"
                                    : "unavailable");
        }
    }
}

void drawEyeAngleSelectedPlots(
    const FrameDebugWindowContext& context,
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const FrameDebugWindowState& state,
    std::optional<size_t> current_row) {
    if (state.eye_angle_selected_plot_fields.empty()) {
        ImGui::TextDisabled("No eye-angle scalar fields selected");
        return;
    }

    struct TraceGroup {
        std::string units;
        std::vector<EyeAngleTraceBuffer> traces;
    };
    std::vector<TraceGroup> groups;
    int unavailable_count = 0;
    int empty_count = 0;
    for (const auto& requested_field :
         state.eye_angle_selected_plot_fields) {
        std::string resolved_name;
        const auto* field = resolveEyeAnglePlotField(
            context.zarr_loader, requested_field, resolved_name);
        if (field == nullptr) {
            ++unavailable_count;
            continue;
        }

        EyeAngleTraceBuffer trace;
        if (!buildEyeAngleTraceBuffer(eye,
                                      *field,
                                      requested_field,
                                      resolved_name,
                                      state.eye_angle_plot_x_axis_mode,
                                      trace)) {
            ++empty_count;
            continue;
        }
        auto group_it = std::find_if(
            groups.begin(),
            groups.end(),
            [&](const auto& group) { return group.units == trace.units; });
        if (group_it == groups.end()) {
            groups.push_back({trace.units, {}});
            group_it = std::prev(groups.end());
        }
        group_it->traces.push_back(std::move(trace));
    }

    if (unavailable_count > 0) {
        ImGui::TextDisabled("%d selected fields unavailable",
                            unavailable_count);
    }
    if (empty_count > 0) {
        ImGui::TextDisabled(
            "%d selected fields have no finite samples for this x-axis",
            empty_count);
    }
    if (groups.empty()) {
        ImGui::TextDisabled("No selected fields can be plotted");
        return;
    }

    const char* x_axis_label =
        eyeAngleXAxisLabel(state.eye_angle_plot_x_axis_mode);
    const auto current_x = currentEyeAnglePlotX(
        eye,
        state.eye_angle_plot_x_axis_mode,
        context.current_frame_num,
        current_row);
    for (const auto& group : groups) {
        std::string title = "Eye Angles";
        if (groups.size() > 1) {
            title += " (" + group.units + ")";
        }
        drawEyeAngleTraceGroupPlot(
            title, group.traces, x_axis_label, group.units, current_x);
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

    const std::string selection_key = eye.run_name + "|" + selected_rep->key;
    if (state.eye_angle_plot_selection_key != selection_key) {
        state.eye_angle_plot_selection_key = selection_key;
        resetEyeAnglePlotSelection(context, *selected_rep, state);
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
    ImGui::Text("Eye-Angle Plots:");
    const char* x_axis_labels[] = {"Frame", "Time (s)", "Row"};
    int x_axis_mode = std::clamp(state.eye_angle_plot_x_axis_mode, 0, 2);
    if (ImGui::Combo("X axis", &x_axis_mode, x_axis_labels, 3)) {
        state.eye_angle_plot_x_axis_mode = x_axis_mode;
    }
    drawEyeAngleFieldBrowser(context, eye, *selected_rep, state);

    ImGui::Separator();
    drawEyeAngleSelectedPlots(context, eye, state, current_row);
}
