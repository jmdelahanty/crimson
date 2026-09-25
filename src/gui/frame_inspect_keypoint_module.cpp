#include "gui/frame_inspect_keypoint_module.h"

#include "IconsForkAwesome.h"
#include "imgui.h"

#include <algorithm>
#include <string>

namespace crimson::gui {

KeypointInspectModuleResult
drawFrameInspectKeypointModule(const KeypointInspectPresentation &presentation,
                               KeypointInspectModuleState &state) {
  KeypointInspectModuleResult result;
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
    ImGui::TextDisabled("Loading keypoints for the presented frame...");
  } else {
    ImGui::Text("Frame %lld  |  %zu observations",
                static_cast<long long>(presentation.camera_frame),
                presentation.observations.size());
    if (presentation.observations.empty()) {
      ImGui::TextDisabled("No keypoint observations in this frame.");
    } else if (ImGui::BeginTable("##current-keypoints", 4,
                                 ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Observation");
      ImGui::TableSetupColumn("Pose confidence");
      ImGui::TableSetupColumn("Valid landmarks");
      ImGui::TableSetupColumn("State");
      ImGui::TableHeadersRow();
      for (size_t index = 0; index < presentation.observations.size();
           ++index) {
        const KeypointInspectObservation &observation =
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
        if (observation.pose_confidence_valid) {
          ImGui::Text("%.3f", observation.pose_confidence);
        } else {
          ImGui::TextDisabled("n/a");
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%zu / %zu", observation.valid_landmark_count,
                    observation.landmark_count);
        ImGui::TableSetColumnIndex(3);
        if (!observation.state_label.empty()) {
          ImGui::TextUnformatted(observation.state_label.c_str());
        } else {
          ImGui::TextDisabled("n/a");
        }
      }
      ImGui::EndTable();
    }

    const auto selected = std::find_if(
        presentation.observations.begin(), presentation.observations.end(),
        [&](const KeypointInspectObservation &candidate) {
          return candidate.selectable && candidate.instance_key != 0 &&
                 candidate.instance_key == state.selected_instance_key;
        });
    if (selected != presentation.observations.end() &&
        !selected->landmarks.empty() &&
        ImGui::BeginTable("##selected-keypoint-confidence", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Landmark");
      ImGui::TableSetupColumn("Confidence");
      ImGui::TableSetupColumn("Valid");
      ImGui::TableSetupColumn("Edited");
      ImGui::TableHeadersRow();
      for (const KeypointInspectLandmark &landmark : selected->landmarks) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(landmark.label.c_str());
        ImGui::TableSetColumnIndex(1);
        if (landmark.confidence_valid) {
          ImGui::Text("%.3f", landmark.confidence);
        } else {
          ImGui::TextDisabled("n/a");
        }
        ImGui::TableSetColumnIndex(2);
        if (landmark.valid_known) {
          ImGui::TextUnformatted(landmark.valid ? "Yes" : "No");
        } else {
          ImGui::TextDisabled("n/a");
        }
        ImGui::TableSetColumnIndex(3);
        if (landmark.edited_known) {
          ImGui::TextUnformatted(landmark.edited ? "Yes" : "No");
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
    if (ImGui::Button(ICON_FK_LINE_CHART " Keypoint Quality Timeline")) {
      result.request_open_timeline = true;
    }
    ImGui::SameLine();
    switch (presentation.timeline_state) {
    case KeypointInspectTimelineState::Closed:
      ImGui::TextDisabled("not loaded");
      break;
    case KeypointInspectTimelineState::Opening:
      ImGui::TextDisabled("opening...");
      break;
    case KeypointInspectTimelineState::Ready:
      ImGui::TextDisabled("ready");
      break;
    case KeypointInspectTimelineState::Failed:
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
