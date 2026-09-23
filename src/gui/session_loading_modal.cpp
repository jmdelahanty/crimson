#include "gui/session_loading_modal.h"

#include "imgui.h"

#include <algorithm>

namespace crimson::ui {

void drawSessionLoadingModal(
    const session::SessionLoadingPresentation &presentation) {
  const char *popup_name = presentation.title.empty()
                               ? "Loading analysis"
                               : presentation.title.c_str();
  if (presentation.visible && !ImGui::IsPopupOpen(popup_name)) {
    ImGui::OpenPopup(popup_name);
  }
  if (!presentation.visible && !ImGui::IsPopupOpen(popup_name)) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(popup_name, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings)) {
    return;
  }
  if (!presentation.visible) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  ImGui::TextUnformatted(presentation.phase.c_str());
  ImGui::ProgressBar(
      std::clamp(static_cast<float>(presentation.fraction), 0.0f, 1.0f),
      ImVec2(-1.0f, 6.0f), "");
  ImGui::TextDisabled("%zu of %zu stages ready",
                      presentation.completed_products,
                      presentation.total_products);
  ImGui::EndPopup();
}

} // namespace crimson::ui
