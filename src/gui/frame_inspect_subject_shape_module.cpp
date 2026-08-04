#include "gui/frame_inspect_subject_shape_module.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace crimson::gui {
namespace {

const SubjectShapeInspectFeature *
findFeature(const SubjectShapeInspectObservation &observation,
            const char *key) {
  const auto found =
      std::find_if(observation.features.begin(), observation.features.end(),
                   [&](const SubjectShapeInspectFeature &feature) {
                     return feature.key == key;
                   });
  return found == observation.features.end() ? nullptr : &*found;
}

const char *featureState(const SubjectShapeInspectFeature *feature) {
  if (feature == nullptr) {
    return "n/a";
  }
  if (!feature->valid_known) {
    return feature->point_count > 0 ? "present" : "n/a";
  }
  return feature->valid ? "valid" : "invalid";
}

std::string curveSummary(const SubjectShapeInspectObservation &observation) {
  const auto *centerline = findFeature(observation, "centerline");
  const auto *bspline = findFeature(observation, "bspline");
  return std::to_string(centerline == nullptr ? 0 : centerline->point_count) +
         " / " + std::to_string(bspline == nullptr ? 0 : bspline->point_count);
}

} // namespace

void drawFrameInspectSubjectShapeModule(
    const SubjectShapeInspectPresentation &presentation,
    SubjectShapeInspectModuleState &state) {
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
    ImGui::TextDisabled("Loading subject shape for the presented frame...");
  } else {
    ImGui::Text("Frame %lld  |  %zu observations",
                static_cast<long long>(presentation.camera_frame),
                presentation.observations.size());
    if (presentation.observations.empty()) {
      ImGui::TextDisabled("No subject-shape observations in this frame.");
    } else if (ImGui::BeginTable("##current-subject-shapes", 4,
                                 ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Observation");
      ImGui::TableSetupColumn("Body frame");
      ImGui::TableSetupColumn("Snout / tail base");
      ImGui::TableSetupColumn("Centerline / spline");
      ImGui::TableHeadersRow();
      for (size_t index = 0; index < presentation.observations.size();
           ++index) {
        const auto &observation = presentation.observations[index];
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
        ImGui::TextUnformatted(
            featureState(findFeature(observation, "body_frame")));
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s / %s",
                    featureState(findFeature(observation, "snout_tip")),
                    featureState(findFeature(observation, "tail_base")));
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(curveSummary(observation).c_str());
      }
      ImGui::EndTable();
    }

    const auto selected = std::find_if(
        presentation.observations.begin(), presentation.observations.end(),
        [&](const SubjectShapeInspectObservation &candidate) {
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
      if (selected->source_refined_row_id_valid) {
        ImGui::TextDisabled(
            "Refined row ID: %lld",
            static_cast<long long>(selected->source_refined_row_id));
      }
      if (selected->source_crop_row_id_valid) {
        ImGui::SameLine();
        ImGui::TextDisabled(
            "Crop row: %lld",
            static_cast<long long>(selected->source_crop_row_id));
      }
      if (selected->roi_valid) {
        ImGui::TextDisabled("ROI: %.0f x %.0f px", selected->roi_width,
                            selected->roi_height);
      }
      if (ImGui::BeginTable("##selected-subject-shape-features", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Geometry");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Points");
        ImGui::TableHeadersRow();
        for (const auto &feature : selected->features) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(feature.label.c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(featureState(&feature));
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%zu", feature.point_count);
        }
        ImGui::EndTable();
      }
      for (const auto &reason : selected->reasons) {
        if (!reason.empty()) {
          ImGui::TextWrapped("Reason: %s", reason.c_str());
        }
      }
    }
  }

  for (const auto &line : presentation.detail_lines) {
    ImGui::TextDisabled("%s", line.c_str());
  }
  if (!presentation.warning.empty()) {
    ImGui::TextWrapped("Warning: %s", presentation.warning.c_str());
  }
}

} // namespace crimson::gui
