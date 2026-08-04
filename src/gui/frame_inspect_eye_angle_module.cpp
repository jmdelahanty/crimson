#include "gui/frame_inspect_eye_angle_module.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace crimson::gui {
namespace {

const EyeAngleInspectRepresentation *
findRepresentation(const EyeAngleInspectPresentation &presentation,
                   const std::string &key) {
  const auto found = std::find_if(
      presentation.representations.begin(), presentation.representations.end(),
      [&](const EyeAngleInspectRepresentation &candidate) {
        return candidate.key == key;
      });
  return found == presentation.representations.end() ? nullptr : &*found;
}

size_t
fieldCountForRepresentation(const EyeAngleInspectObservation &observation,
                            const std::string &representation_key) {
  return static_cast<size_t>(
      std::count_if(observation.fields.begin(), observation.fields.end(),
                    [&](const EyeAngleInspectField &field) {
                      return representation_key.empty() ||
                             field.representation_key == representation_key;
                    }));
}

const char *validityLabel(const EyeAngleInspectObservation &observation) {
  if (observation.marginal_known && observation.marginal) {
    return "Marginal";
  }
  if (!observation.frame_valid_known) {
    return "Unknown";
  }
  return observation.frame_valid ? "Valid" : "Invalid";
}

void drawEyeValidity(const EyeAngleInspectObservation &observation) {
  if (!observation.left_valid_known && !observation.right_valid_known) {
    ImGui::TextDisabled("n/a");
    return;
  }
  const char *left = observation.left_valid_known
                         ? observation.left_valid ? "valid" : "invalid"
                         : "unknown";
  const char *right = observation.right_valid_known
                          ? observation.right_valid ? "valid" : "invalid"
                          : "unknown";
  ImGui::Text("L %s / R %s", left, right);
}

} // namespace

void drawFrameInspectEyeAngleModule(
    const EyeAngleInspectPresentation &presentation,
    EyeAngleInspectModuleState &state) {
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

  const EyeAngleInspectRepresentation *selected_representation =
      findRepresentation(presentation, state.selected_representation_key);
  if (selected_representation == nullptr &&
      !presentation.default_representation_key.empty()) {
    selected_representation = findRepresentation(
        presentation, presentation.default_representation_key);
  }
  if (selected_representation == nullptr &&
      !presentation.representations.empty()) {
    selected_representation = &presentation.representations.front();
  }
  if (selected_representation != nullptr) {
    state.selected_representation_key = selected_representation->key;
    if (ImGui::BeginCombo("Representation",
                          selected_representation->label.c_str())) {
      for (const EyeAngleInspectRepresentation &representation :
           presentation.representations) {
        const bool selected =
            representation.key == state.selected_representation_key;
        if (ImGui::Selectable(representation.label.c_str(), selected)) {
          state.selected_representation_key = representation.key;
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    selected_representation =
        findRepresentation(presentation, state.selected_representation_key);
    if (selected_representation != nullptr &&
        (!selected_representation->role.empty() ||
         !selected_representation->axis.empty())) {
      ImGui::TextDisabled("Role: %s | Axis: %s",
                          selected_representation->role.empty()
                              ? "unspecified"
                              : selected_representation->role.c_str(),
                          selected_representation->axis.empty()
                              ? "unspecified"
                              : selected_representation->axis.c_str());
    }
  }
  const std::string active_representation = selected_representation == nullptr
                                                ? std::string{}
                                                : selected_representation->key;

  if (!presentation.frame_ready) {
    ImGui::TextDisabled("Loading eye angles for the presented frame...");
  } else {
    ImGui::Text("Frame %lld  |  %zu observations",
                static_cast<long long>(presentation.camera_frame),
                presentation.observations.size());
    if (presentation.observations.empty()) {
      ImGui::TextDisabled("No eye-angle observations in this frame.");
    } else if (ImGui::BeginTable("##current-eye-angles", 4,
                                 ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Observation");
      ImGui::TableSetupColumn("Eyes");
      ImGui::TableSetupColumn("Fields");
      ImGui::TableSetupColumn("State");
      ImGui::TableHeadersRow();
      for (size_t index = 0; index < presentation.observations.size();
           ++index) {
        const EyeAngleInspectObservation &observation =
            presentation.observations[index];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(static_cast<int>(index));
        const bool selected =
            observation.selectable && observation.row_selection_key != 0 &&
            state.selected_row_key == observation.row_selection_key;
        const std::string label = "#" + std::to_string(index + 1);
        if (observation.selectable && observation.row_selection_key != 0) {
          if (ImGui::Selectable(label.c_str(), selected)) {
            state.selected_row_key = observation.row_selection_key;
          }
        } else {
          ImGui::TextUnformatted(label.c_str());
        }
        ImGui::PopID();

        ImGui::TableSetColumnIndex(1);
        drawEyeValidity(observation);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%zu", fieldCountForRepresentation(observation,
                                                       active_representation));
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(validityLabel(observation));
      }
      ImGui::EndTable();
    }

    const auto selected = std::find_if(
        presentation.observations.begin(), presentation.observations.end(),
        [&](const EyeAngleInspectObservation &candidate) {
          return candidate.selectable && candidate.row_selection_key != 0 &&
                 candidate.row_selection_key == state.selected_row_key;
        });
    if (selected != presentation.observations.end()) {
      if (selected->source_row_valid) {
        ImGui::TextDisabled("Source row: %zu", selected->source_row);
      }
      if (selected->detection_index_valid) {
        ImGui::SameLine();
        ImGui::TextDisabled("Detection: %lld",
                            static_cast<long long>(selected->detection_index));
      }
      if (selected->source_crop_row_id_valid) {
        ImGui::TextDisabled(
            "Source crop row: %lld",
            static_cast<long long>(selected->source_crop_row_id));
      }
      if (!selected->fields.empty() &&
          ImGui::BeginTable("##selected-eye-angle-fields", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Field");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Units");
        ImGui::TableHeadersRow();
        for (const EyeAngleInspectField &field : selected->fields) {
          if (!active_representation.empty() &&
              field.representation_key != active_representation) {
            continue;
          }
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(field.label.c_str());
          ImGui::TableSetColumnIndex(1);
          if (!field.valid) {
            ImGui::TextDisabled("n/a");
          } else if (field.kind == EyeAngleInspectFieldKind::Vector2) {
            ImGui::Text("[%.3f, %.3f]", field.value_x, field.value_y);
          } else {
            ImGui::Text("%.3f", field.value_x);
          }
          ImGui::TableSetColumnIndex(2);
          if (field.units.empty()) {
            ImGui::TextDisabled("n/a");
          } else {
            ImGui::TextUnformatted(field.units.c_str());
          }
        }
        ImGui::EndTable();
      }
      if (!selected->reason.empty()) {
        ImGui::TextWrapped("Reason: %s", selected->reason.c_str());
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
