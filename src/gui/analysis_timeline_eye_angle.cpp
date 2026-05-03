#include "gui/analysis_timeline_eye_angle.h"

#include "gui/analysis_timeline_trace_plot.h"
#include "gui/camera_view_overlay_style.h"
#include "imgui.h"
#include "zarr_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

enum class EyeAngleTraceRole {
    Other,
    Left,
    Right,
    Vergence,
};

std::string toLowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

EyeAngleTraceRole eyeAngleTraceRoleForField(const std::string& field_name) {
    const std::string lower = toLowerAsciiCopy(field_name);
    if (lower.find("vergence") != std::string::npos) {
        return EyeAngleTraceRole::Vergence;
    }
    if (lower.find("left") != std::string::npos) {
        return EyeAngleTraceRole::Left;
    }
    if (lower.find("right") != std::string::npos) {
        return EyeAngleTraceRole::Right;
    }
    return EyeAngleTraceRole::Other;
}

std::optional<ImVec4> eyeAngleTimelineColorForRole(EyeAngleTraceRole role) {
    if (role == EyeAngleTraceRole::Vergence) {
        return ImVec4(0.34f, 1.0f, 0.42f, 0.95f);
    }
    if (role == EyeAngleTraceRole::Left) {
        return camera_view_overlay::subjectMaskContourColor("eye_left");
    }
    if (role == EyeAngleTraceRole::Right) {
        return camera_view_overlay::subjectMaskContourColor("eye_right");
    }
    return std::nullopt;
}

bool eyeAngleTraceRoleEnabled(EyeAngleTraceRole role,
                              bool show_left,
                              bool show_right,
                              bool show_vergence) {
    if (role == EyeAngleTraceRole::Left) {
        return show_left;
    }
    if (role == EyeAngleTraceRole::Right) {
        return show_right;
    }
    if (role == EyeAngleTraceRole::Vergence) {
        return show_vergence;
    }
    return true;
}

bool stringEndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

std::string unsmoothedEyeAngleFieldName(const std::string& field_name) {
    constexpr const char* kSmoothedSuffix = "_smoothed";
    if (!stringEndsWith(field_name, kSmoothedSuffix)) {
        return {};
    }
    return field_name.substr(0, field_name.size() -
                                      std::string(kSmoothedSuffix).size());
}

void addUniqueField(std::vector<std::string>& fields,
                    const std::string& field_name) {
    if (field_name.empty() ||
        std::find(fields.begin(), fields.end(), field_name) != fields.end()) {
        return;
    }
    fields.push_back(field_name);
}

const ZarrDetectionData::EyeAngleScalarField* resolveEyeAngleTimelineField(
    const ZarrDetectionLoader& loader,
    const std::string& requested_field,
    std::string& resolved_name) {
    resolved_name = requested_field;
    const auto* field = loader.findEyeAngleScalarField(requested_field);
    if (field != nullptr && (field->has_frame || field->has_roi)) {
        return field;
    }
    const std::string base = unsmoothedEyeAngleFieldName(requested_field);
    if (!base.empty()) {
        field = loader.findEyeAngleScalarField(base);
        if (field != nullptr && (field->has_frame || field->has_roi)) {
            resolved_name = base;
            return field;
        }
    }
    return nullptr;
}

std::vector<std::string> defaultEyeAngleTimelineFields(
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep) {
    std::vector<std::string> fields;
    for (const auto& field : rep.default_plot_fields) {
        addUniqueField(fields, field);
    }
    if (fields.empty()) {
        for (const auto& field : rep.primary_roi_fields) {
            addUniqueField(fields, field + "_smoothed");
            addUniqueField(fields, field);
        }
        for (const auto& field : rep.aggregate_roi_fields) {
            addUniqueField(fields, field + "_smoothed");
            addUniqueField(fields, field);
        }
    }
    return fields;
}

std::vector<AnalysisTimelineTrace> buildEyeAngleTimelineTraces(
    const ZarrDetectionLoader& loader,
    const ZarrDetectionData::EyeAngleAnalysisData& eye,
    const ZarrDetectionData::EyeAngleRepresentationInfo& rep,
    double video_fps,
    bool show_left,
    bool show_right,
    bool show_vergence) {
    std::vector<AnalysisTimelineTrace> traces;
    for (const auto& requested_field : defaultEyeAngleTimelineFields(rep)) {
        std::string resolved_name;
        const auto* field =
            resolveEyeAngleTimelineField(loader, requested_field, resolved_name);
        if (field == nullptr) {
            continue;
        }
        const EyeAngleTraceRole trace_role =
            eyeAngleTraceRoleForField(resolved_name);
        if (!eyeAngleTraceRoleEnabled(trace_role,
                                      show_left,
                                      show_right,
                                      show_vergence)) {
            continue;
        }
        const bool use_frame_values =
            field->has_frame && !field->frame_values.empty();
        const auto& values =
            use_frame_values ? field->frame_values : field->roi_values;
        if (values.empty()) {
            continue;
        }

        AnalysisTimelineTrace trace;
        trace.label = field->display_name.empty() ? resolved_name
                                                  : field->display_name;
        if (resolved_name != requested_field) {
            trace.label += " (fallback)";
        }
        trace.units = field->units.empty() ? "deg" : field->units;
        if (auto color = eyeAngleTimelineColorForRole(trace_role)) {
            trace.has_color = true;
            trace.color = *color;
        }
        trace.xs.reserve(values.size());
        trace.ys.reserve(values.size());
        for (size_t row = 0; row < values.size(); ++row) {
            const float value = values[row];
            if (!std::isfinite(value)) {
                continue;
            }
            std::optional<double> t;
            if (use_frame_values) {
                if (row < eye.frame_time_seconds.size() &&
                    std::isfinite(eye.frame_time_seconds[row])) {
                    t = static_cast<double>(eye.frame_time_seconds[row]);
                } else {
                    t = sampleTimeFromFrame(static_cast<int32_t>(row),
                                            video_fps);
                }
            } else if (row < eye.roi_time_seconds.size() &&
                       std::isfinite(eye.roi_time_seconds[row])) {
                t = static_cast<double>(eye.roi_time_seconds[row]);
            } else if (row < eye.roi_frame_indices.size()) {
                t = sampleTimeFromFrame(eye.roi_frame_indices[row], video_fps);
            }
            if (!t.has_value()) {
                continue;
            }
            trace.xs.push_back(*t);
            trace.ys.push_back(static_cast<double>(value));
        }
        if (trace.xs.size() >= 2) {
            traces.push_back(std::move(trace));
        }
    }
    return traces;
}

}  // namespace

void drawAnalysisTimelineEyeAngleSection(
    const AnalysisTimelineEyeAngleContext& context,
    AnalysisTimelineWindowState& state) {
    ImGui::SeparatorText("Eye-Angle Traces");
    const auto& eye = context.zarr_loader.getEyeAngleAnalysisData();
    ImGui::Text("Run: %s", eye.run_name.c_str());
    if (eye.representations.empty()) {
        ImGui::TextDisabled("No eye-angle representations available.");
        return;
    }

    if (state.eye_angle_representation_index < 0 ||
        static_cast<size_t>(state.eye_angle_representation_index) >=
            eye.representations.size()) {
        state.eye_angle_representation_index = 0;
        for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
            if (eye.representations[idx].key == eye.default_representation) {
                state.eye_angle_representation_index = static_cast<int>(idx);
                break;
            }
        }
    }

    const auto& selected_rep =
        eye.representations[static_cast<size_t>(
            state.eye_angle_representation_index)];
    const char* rep_preview = selected_rep.display_name.empty()
                                  ? selected_rep.key.c_str()
                                  : selected_rep.display_name.c_str();
    if (ImGui::BeginCombo("Representation##analysis_eye_angle_rep",
                          rep_preview)) {
        for (size_t idx = 0; idx < eye.representations.size(); ++idx) {
            const auto& rep = eye.representations[idx];
            const bool selected =
                static_cast<int>(idx) == state.eye_angle_representation_index;
            const char* label =
                rep.display_name.empty() ? rep.key.c_str()
                                         : rep.display_name.c_str();
            if (ImGui::Selectable(label, selected)) {
                state.eye_angle_representation_index = static_cast<int>(idx);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Checkbox("Show selected eye-angle representation",
                    &state.show_eye_angle_traces);
    ImGui::BeginDisabled(!state.show_eye_angle_traces);
    ImGui::SameLine();
    ImGui::Checkbox("Left##eye_angle_trace_left",
                    &state.show_eye_left_trace);
    ImGui::SameLine();
    ImGui::Checkbox("Right##eye_angle_trace_right",
                    &state.show_eye_right_trace);
    ImGui::SameLine();
    ImGui::Checkbox("Vergence##eye_angle_trace_vergence",
                    &state.show_eye_vergence_trace);
    ImGui::EndDisabled();

    if (!state.show_eye_angle_traces) {
        return;
    }

    double current_eye_time = context.fallback_current_time;
    if (auto current_row =
            context.zarr_loader.findEyeAngleRowForFrame(
                context.current_frame_num)) {
        if (*current_row < eye.roi_time_seconds.size() &&
            std::isfinite(eye.roi_time_seconds[*current_row])) {
            current_eye_time =
                static_cast<double>(eye.roi_time_seconds[*current_row]);
        }
    } else if (context.current_frame_num >= 0 &&
               static_cast<size_t>(context.current_frame_num) <
                   eye.frame_time_seconds.size() &&
               std::isfinite(eye.frame_time_seconds[context.current_frame_num])) {
        current_eye_time =
            static_cast<double>(eye.frame_time_seconds[context.current_frame_num]);
    }

    const auto traces = buildEyeAngleTimelineTraces(
        context.zarr_loader,
        eye,
        selected_rep,
        context.video_fps,
        state.show_eye_left_trace,
        state.show_eye_right_trace,
        state.show_eye_vergence_trace);
    std::string y_axis = "deg";
    if (!traces.empty() && !traces.front().units.empty()) {
        y_axis = traces.front().units;
    }
    drawAnalysisTracePlot("Eye Angles",
                          y_axis.c_str(),
                          traces,
                          context.scroll_state,
                          current_eye_time,
                          "##current_time_eye_angles");
}
