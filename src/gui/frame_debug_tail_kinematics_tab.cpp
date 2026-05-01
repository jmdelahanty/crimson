#include "gui/frame_debug_tail_kinematics_tab.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

std::string tailReasonAt(
    const ZarrDetectionData::TailKinematicsData& tail,
    size_t row) {
    if (row < tail.failure_reason.size()) {
        return tail.failure_reason[row];
    }
    return {};
}

void drawTailSeriesPlot(const char* title,
                        const std::vector<int32_t>& frame_index,
                        const std::vector<float>& values,
                        int current_frame_num) {
    if (values.empty()) {
        ImGui::TextDisabled("%s unavailable", title);
        return;
    }
    std::vector<double> xs;
    std::vector<double> ys;
    const size_t count = values.size();
    xs.reserve(count);
    ys.reserve(count);
    for (size_t row = 0; row < count; ++row) {
        const float value = values[row];
        if (!std::isfinite(value)) {
            continue;
        }
        const double x =
            (row < frame_index.size() && frame_index[row] >= 0)
                ? static_cast<double>(frame_index[row])
                : static_cast<double>(row);
        xs.push_back(x);
        ys.push_back(static_cast<double>(value));
    }
    if (xs.size() < 2) {
        ImGui::TextDisabled("%s has no finite values", title);
        return;
    }

    if (ImPlot::BeginPlot(title, ImVec2(-1.0f, 190.0f))) {
        ImPlot::SetupAxes("Frame", title, ImPlotAxisFlags_AutoFit,
                          ImPlotAxisFlags_AutoFit);
        ImPlot::PlotLine(title,
                         xs.data(),
                         ys.data(),
                         static_cast<int>(xs.size()));
        const double current_x[2] = {
            static_cast<double>(current_frame_num),
            static_cast<double>(current_frame_num)};
        const double current_y[2] = {
            *std::min_element(ys.begin(), ys.end()),
            *std::max_element(ys.begin(), ys.end())};
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 0.8f), 1.5f);
        ImPlot::PlotLine("Current Frame", current_x, current_y, 2);
        ImPlot::EndPlot();
    }
}

}  // namespace

void drawTailKinematicsTab(const FrameDebugWindowContext& context,
                           FrameDebugWindowState& state,
                           FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasTailKinematicsData()) {
        ImGui::TextDisabled("Tail kinematics data unavailable");
        return;
    }

    const auto& tail = context.zarr_loader.getTailKinematicsData();
    ImGui::Text("Run: %s", tail.run_name.c_str());
    ImGui::Text("Source subject shape: %s",
                tail.source_subject_shape_run.empty()
                    ? "<unknown>"
                    : tail.source_subject_shape_run.c_str());
    if (!tail.source_refined_subject_masks_run.empty()) {
        ImGui::Text("Source refined masks: %s",
                    tail.source_refined_subject_masks_run.c_str());
    }
    ImGui::Text("Rows: %zu | Samples: %zu", tail.row_count,
                tail.sample_count);
    if (!tail.warning.empty()) {
        ImGui::TextWrapped("Warning: %s", tail.warning.c_str());
    }

    auto overlay_options = context.tail_kinematics_overlay_options;
    ImGui::Separator();
    ImGui::Text("Tail Kinematics Overlay:");
    ImGui::Checkbox("Show k=10 tail-angle samples",
                    &overlay_options.show_overlay);
    ImGui::BeginDisabled(!overlay_options.show_overlay);
    ImGui::Checkbox("Tail-angle samples k=10 (analysis output)",
                    &overlay_options.show_samples);
    ImGui::Checkbox("Connect k=10 samples", &overlay_options.show_segments);
    ImGui::Checkbox("Tail angle vectors",
                    &overlay_options.show_angle_vectors);
    ImGui::Checkbox("Lateral deflection",
                    &overlay_options.show_lateral_deflection);
    ImGui::Checkbox("Color invalid frames",
                    &overlay_options.color_invalid_frames);
    ImGui::EndDisabled();
    result.tail_kinematics_overlay_options = overlay_options;

    if (auto current_row =
            context.zarr_loader.findTailKinematicsRowForFrame(
                context.current_frame_num)) {
        state.tail_kinematics_selected_row =
            static_cast<int>(*current_row);
        const bool current_valid =
            *current_row < tail.valid.size() && tail.valid[*current_row] != 0;
        ImGui::Text("Current frame tail row: %zu (%s)",
                    *current_row,
                    current_valid ? "valid" : "invalid");
        const std::string reason = tailReasonAt(tail, *current_row);
        if (!reason.empty()) {
            ImGui::TextWrapped("Current reason: %s", reason.c_str());
        }
    } else {
        ImGui::TextDisabled("No tail row mapped to current frame");
    }

    int selected_row = state.tail_kinematics_selected_row;
    if (ImGui::InputInt("Selected tail row", &selected_row)) {
        selected_row = std::clamp(
            selected_row,
            -1,
            tail.row_count == 0
                ? -1
                : static_cast<int>(tail.row_count - 1));
        state.tail_kinematics_selected_row = selected_row;
    }
    if (state.tail_kinematics_selected_row >= 0) {
        const size_t row =
            static_cast<size_t>(state.tail_kinematics_selected_row);
        if (row < tail.row_to_frame.size()) {
            ImGui::Text("Selected row frame: %d", tail.row_to_frame[row]);
        }
        const std::string reason = tailReasonAt(tail, row);
        if (!reason.empty()) {
            ImGui::TextWrapped("Selected reason: %s", reason.c_str());
        }
        if (ImGui::Button("Seek Selected Tail Row")) {
            result.request_seek_tail_kinematics_row = true;
            result.requested_tail_kinematics_row = row;
        }
        ImGui::SameLine();
        if (ImGui::Button("Show In Subject Shape")) {
            result.request_seek_tail_kinematics_row = true;
            result.requested_tail_kinematics_row = row;
            state.active_tab = FrameInspectTab::EyeMasks;
        }
    }

    ImGui::Separator();
    ImGui::Text("Tail Kinematics QC:");
    auto filters = state.tail_kinematics_qc_filters;
    bool changed = false;
    changed |= ImGui::Checkbox("Invalid rows", &filters.invalid_rows);
    changed |= ImGui::Checkbox("Non-finite tail tip angle",
                               &filters.nonfinite_tail_tip_angle);
    changed |= ImGui::Checkbox("Non-finite tail tip deflection",
                               &filters.nonfinite_tail_tip_lateral_deflection);
    if (ImGui::InputText("Tail reason contains",
                         state.tail_kinematics_reason_filter.data(),
                         state.tail_kinematics_reason_filter.size())) {
        changed = true;
    }
    if (changed) {
        filters.reason_substring =
            state.tail_kinematics_reason_filter.data();
        state.tail_kinematics_qc_filters = filters;
        state.tail_kinematics_qc_status.clear();
    } else {
        state.tail_kinematics_qc_filters.reason_substring =
            state.tail_kinematics_reason_filter.data();
    }
    if (ImGui::Button("Prev Tail QC Frame")) {
        result.request_prev_tail_kinematics_qc_frame = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Tail QC Frame")) {
        result.request_next_tail_kinematics_qc_frame = true;
    }
    if (!state.tail_kinematics_qc_status.empty()) {
        ImGui::TextWrapped("%s", state.tail_kinematics_qc_status.c_str());
    }

    ImGui::Separator();
    drawTailSeriesPlot("Tail Tip Angle (deg)",
                       tail.frame_index,
                       tail.tail_tip_angle_deg,
                       context.current_frame_num);
    drawTailSeriesPlot("Tail Tip Lateral Deflection (px)",
                       tail.frame_index,
                       tail.tail_tip_lateral_deflection_px,
                       context.current_frame_num);
    if (!tail.max_abs_tail_curvature_px_inv.empty()) {
        drawTailSeriesPlot("Max Abs Tail Curvature (px^-1)",
                           tail.frame_index,
                           tail.max_abs_tail_curvature_px_inv,
                           context.current_frame_num);
    }
}
