#include "gui/frame_inspect_detection_module.h"

#include "IconsForkAwesome.h"
#include "imgui.h"

#include <string>

namespace crimson::gui {

DetectionInspectModuleResult drawFrameInspectDetectionModule(
    const DetectionInspectPresentation &presentation,
    DetectionInspectModuleState &state) {
  DetectionInspectModuleResult result;
  if (!presentation.presentation_label.empty()) {
    ImGui::TextUnformatted(presentation.presentation_label.c_str());
  }
  if (!presentation.available) {
    ImGui::TextDisabled("%s", presentation.unavailable_message.c_str());
    return result;
  }

  if (!presentation.surface_label.empty()) {
    ImGui::Text("Surface: %s", presentation.surface_label.c_str());
  }
  if (!presentation.run_name.empty()) {
    ImGui::TextWrapped("Run: %s", presentation.run_name.c_str());
  }

  if (!presentation.frame_ready) {
    ImGui::TextDisabled("Loading detections for the presented frame...");
  } else {
    ImGui::Text("Frame %lld  |  %zu observations",
                static_cast<long long>(presentation.camera_frame),
                presentation.observations.size());
    if (presentation.observations.empty()) {
      ImGui::TextDisabled("No detections in this frame.");
    } else if (ImGui::BeginTable("##current-detections", 4,
                                 ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Observation");
      ImGui::TableSetupColumn("Confidence");
      ImGui::TableSetupColumn("Class");
      ImGui::TableSetupColumn("Source");
      ImGui::TableHeadersRow();
      for (size_t index = 0; index < presentation.observations.size();
           ++index) {
        const DetectionInspectObservation &observation =
            presentation.observations[index];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(static_cast<int>(index));
        const bool selected =
            observation.selectable && observation.instance_key != 0 &&
            state.selected_instance_key == observation.instance_key;
        const std::string label = "#" + std::to_string(index + 1);
        if (observation.selectable && observation.instance_key != 0) {
          if (ImGui::Selectable(label.c_str(), selected)) {
            state.selected_instance_key = observation.instance_key;
          }
        } else {
          ImGui::TextUnformatted(label.c_str());
        }
        ImGui::PopID();

        ImGui::TableSetColumnIndex(1);
        if (observation.confidence_valid) {
          ImGui::Text("%.3f", observation.confidence);
        } else {
          ImGui::TextDisabled("n/a");
        }
        ImGui::TableSetColumnIndex(2);
        if (observation.class_id_valid) {
          ImGui::Text("%d", observation.class_id);
        } else {
          ImGui::TextDisabled("n/a");
        }
        ImGui::TableSetColumnIndex(3);
        if (!observation.source_label.empty()) {
          ImGui::TextUnformatted(observation.source_label.c_str());
        } else {
          ImGui::TextDisabled("n/a");
        }
      }
      ImGui::EndTable();
    }
  }

  for (const std::string &line : presentation.detail_lines) {
    ImGui::TextDisabled("%s", line.c_str());
  }

  if (presentation.timeline_visible) {
    if (ImGui::Button(ICON_FK_LINE_CHART " Detection Timeline")) {
      result.request_open_timeline = true;
    }
    ImGui::SameLine();
    switch (presentation.timeline_state) {
    case DetectionInspectTimelineState::Closed:
      ImGui::TextDisabled("not loaded");
      break;
    case DetectionInspectTimelineState::Opening:
      ImGui::TextDisabled("opening...");
      break;
    case DetectionInspectTimelineState::Ready:
      ImGui::TextDisabled("ready");
      break;
    case DetectionInspectTimelineState::Failed:
      ImGui::TextDisabled("unavailable");
      if (!presentation.timeline_error.empty() &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", presentation.timeline_error.c_str());
      }
      break;
    }
  }
  return result;
}

} // namespace crimson::gui
