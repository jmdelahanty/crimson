#include "gui/frame_debug_eye_angle_tab.h"

#include "gui/frame_debug_eye_angle_adapter.h"
#include "gui/frame_inspect_eye_angle_module.h"
#include "imgui.h"

#include <algorithm>
#include <cstddef>

void drawEyeAngleTab(const FrameDebugWindowContext &context,
                     FrameDebugWindowState &state,
                     FrameDebugWindowResult &result) {
  auto presentation = makeFrameDebugEyeAngleInspectPresentation(context);
  const auto selected_in_frame = std::find_if(
      presentation.observations.begin(), presentation.observations.end(),
      [&](const crimson::gui::EyeAngleInspectObservation &observation) {
        return observation.selectable &&
               observation.row_selection_key ==
                   state.eye_angle_inspect.selected_row_key;
      });
  if (!presentation.observations.empty() &&
      selected_in_frame == presentation.observations.end()) {
    state.eye_angle_inspect.selected_row_key =
        presentation.observations.front().row_selection_key;
  }
  crimson::gui::drawFrameInspectEyeAngleModule(presentation,
                                               state.eye_angle_inspect);
  if (!presentation.available) {
    return;
  }

  const auto selected = std::find_if(
      presentation.observations.begin(), presentation.observations.end(),
      [&](const crimson::gui::EyeAngleInspectObservation &observation) {
        return observation.selectable &&
               observation.row_selection_key ==
                   state.eye_angle_inspect.selected_row_key;
      });
  if (selected != presentation.observations.end() &&
      selected->source_row_valid) {
    state.eye_angle_selected_row = static_cast<int>(selected->source_row);
  }

  const auto &eye = context.zarr_loader.getEyeAngleAnalysisData();
  ImGui::Separator();
  int selected_row = state.eye_angle_selected_row;
  if (ImGui::InputInt("Selected eye-angle row", &selected_row)) {
    selected_row = std::clamp(
        selected_row, -1,
        eye.row_count == 0 ? -1 : static_cast<int>(eye.row_count - 1));
    state.eye_angle_selected_row = selected_row;
    state.eye_angle_inspect.selected_row_key =
        selected_row < 0 ? 0 : static_cast<uint64_t>(selected_row) + 1;
  }
  if (state.eye_angle_selected_row >= 0) {
    const size_t row = static_cast<size_t>(state.eye_angle_selected_row);
    if (row < eye.row_to_frame.size()) {
      ImGui::Text("Selected row frame: %d", eye.row_to_frame[row]);
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
  changed |=
      ImGui::Checkbox("Major-axis marginal", &filters.major_axis_marginal);
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
