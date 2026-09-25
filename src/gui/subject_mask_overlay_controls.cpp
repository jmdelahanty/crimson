#include "gui/subject_mask_overlay_controls.h"

#include "imgui.h"

namespace crimson::gui {
namespace {

bool drawAvailableCheckbox(const char *label, bool *value, bool available) {
  ImGui::BeginDisabled(!available);
  const bool changed = ImGui::Checkbox(label, value);
  ImGui::EndDisabled();
  return changed;
}

} // namespace

SubjectMaskOverlayControlResult drawSubjectMaskOverlayControls(
    SubjectMaskOverlayControlState &state,
    const SubjectMaskOverlayControlCapabilities &capabilities,
    const SubjectMaskOverlayControlOptions &options) {
  SubjectMaskOverlayControlResult result;
  if (capabilities.show_mode) {
    ImGui::BeginDisabled(!capabilities.mode_available);
    int mode = static_cast<int>(state.mode);
    const char *mode_labels[] = {"Realtime", "Review", "Debug"};
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::Combo("Mode", &mode, mode_labels, IM_ARRAYSIZE(mode_labels))) {
      state.mode = static_cast<overlay::ReadOnlyMaskOverlayMode>(mode);
      result.changed = true;
    }
    ImGui::EndDisabled();
    if (options.mode_tooltip != nullptr && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", options.mode_tooltip);
    }
  }

  if (capabilities.show_components) {
    result.changed |=
        drawAvailableCheckbox("Subject body", &state.show_subject_body,
                              capabilities.subject_body_available);
    ImGui::SameLine();
    result.changed |=
        drawAvailableCheckbox("Swim bladder", &state.show_swim_bladder,
                              capabilities.swim_bladder_available);
    result.changed |= drawAvailableCheckbox("Left eye", &state.show_left_eye,
                                            capabilities.left_eye_available);
    ImGui::SameLine();
    result.changed |= drawAvailableCheckbox("Right eye", &state.show_right_eye,
                                            capabilities.right_eye_available);
  }
  return result;
}

} // namespace crimson::gui
