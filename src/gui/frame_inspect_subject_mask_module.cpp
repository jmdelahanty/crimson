#include "gui/frame_inspect_subject_mask_module.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace crimson::gui {
namespace {

size_t presentComponentCount(const SubjectMaskInspectObservation &observation) {
  return static_cast<size_t>(std::count_if(
      observation.components.begin(), observation.components.end(),
      [](const SubjectMaskInspectComponent &component) {
        return component.present;
      }));
}

size_t contourComponentCount(const SubjectMaskInspectObservation &observation) {
  return static_cast<size_t>(std::count_if(
      observation.components.begin(), observation.components.end(),
      [](const SubjectMaskInspectComponent &component) {
        return component.contour_available;
      }));
}

} // namespace

void drawFrameInspectSubjectMaskModule(
    const SubjectMaskInspectPresentation &presentation,
    SubjectMaskInspectModuleState &state) {
  if (!presentation.presentation_label.empty()) {
    ImGui::TextUnformatted(presentation.presentation_label.c_str());
  }
  if (!presentation.available) {
    ImGui::TextDisabled("%s", presentation.unavailable_message.c_str());
    return;
  }

  if (!presentation.surface_label.empty()) {
    ImGui::Text("Surface: %s", presentation.surface_label.c_str());
  }
  if (!presentation.run_name.empty()) {
    ImGui::TextWrapped("Run: %s", presentation.run_name.c_str());
  }

  if (!presentation.frame_ready) {
    ImGui::TextDisabled("Loading subject masks for the presented frame...");
  } else {
    ImGui::Text("Frame %lld  |  %zu observations",
                static_cast<long long>(presentation.camera_frame),
                presentation.observations.size());
    if (presentation.observations.empty()) {
      ImGui::TextDisabled("No subject-mask observations in this frame.");
    } else if (ImGui::BeginTable("##current-subject-masks", 4,
                                 ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Observation");
      ImGui::TableSetupColumn("ROI");
      ImGui::TableSetupColumn("Components");
      ImGui::TableSetupColumn("Contours");
      ImGui::TableHeadersRow();
      for (size_t index = 0; index < presentation.observations.size();
           ++index) {
        const SubjectMaskInspectObservation &observation =
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
        if (observation.roi_valid) {
          ImGui::Text("%.0f x %.0f", observation.roi_width,
                      observation.roi_height);
        } else {
          ImGui::TextDisabled("n/a");
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%zu / %zu", presentComponentCount(observation),
                    observation.components.size());
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%zu", contourComponentCount(observation));
      }
      ImGui::EndTable();
    }

    const auto selected = std::find_if(
        presentation.observations.begin(), presentation.observations.end(),
        [&](const SubjectMaskInspectObservation &candidate) {
          return candidate.selectable && candidate.instance_key != 0 &&
                 candidate.instance_key == state.selected_instance_key;
        });
    if (selected != presentation.observations.end()) {
      if (selected->source_crop_row_id_valid) {
        ImGui::TextDisabled(
            "Source crop row: %lld",
            static_cast<long long>(selected->source_crop_row_id));
      }
      if (!selected->components.empty() &&
          ImGui::BeginTable("##selected-subject-mask-components", 4,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Component");
        ImGui::TableSetupColumn("Channel");
        ImGui::TableSetupColumn("Pixel payload");
        ImGui::TableSetupColumn("Contour");
        ImGui::TableHeadersRow();
        for (const SubjectMaskInspectComponent &component :
             selected->components) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(component.label.c_str());
          ImGui::TableSetColumnIndex(1);
          if (component.channel_index_valid) {
            ImGui::Text("%zu", component.channel_index);
          } else {
            ImGui::TextDisabled("n/a");
          }
          ImGui::TableSetColumnIndex(2);
          if (component.pixel_payload_available) {
            ImGui::Text("%zu values", component.pixel_payload_value_count);
          } else {
            ImGui::TextDisabled("n/a");
          }
          ImGui::TableSetColumnIndex(3);
          if (component.contour_available) {
            ImGui::Text("%zu points", component.contour_point_count);
          } else {
            ImGui::TextDisabled("n/a");
          }
        }
        ImGui::EndTable();
      }
    }
  }

  for (const std::string &line : presentation.detail_lines) {
    ImGui::TextDisabled("%s", line.c_str());
  }
  if (!presentation.warning.empty()) {
    ImGui::TextWrapped("Warning: %s", presentation.warning.c_str());
  }
}

} // namespace crimson::gui
