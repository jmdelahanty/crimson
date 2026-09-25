#include "gui/session_loading_modal.h"

#include "imgui.h"

#include <algorithm>

namespace crimson::ui {

void drawSessionLoadingModal(
    const session::SessionLoadingPresentation &presentation) {
  const std::string popup_name =
      (presentation.title.empty() ? "Loading analysis" : presentation.title) +
      "###session_loading_modal";
  if (presentation.visible && !ImGui::IsPopupOpen(popup_name.c_str())) {
    ImGui::OpenPopup(popup_name.c_str());
  }
  if (!presentation.visible && !ImGui::IsPopupOpen(popup_name.c_str())) {
    return;
  }

  const float font_size = ImGui::GetFontSize();
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImVec2 display_size = viewport->WorkSize;
  const float width = std::max(
      1.0f, std::min(420.0f * font_size / 13.0f,
                     display_size.x - 32.0f * font_size / 13.0f));
  const float height = std::max(
      1.0f, std::min(8.5f * font_size,
                     display_size.y - 32.0f * font_size / 13.0f));
  ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  if (!ImGui::BeginPopupModal(popup_name.c_str(), nullptr,
                              ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove)) {
    return;
  }
  if (!presentation.visible) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  ImGui::BeginChild("phase", ImVec2(0.0f, 3.0f * font_size), false);
  ImGui::TextWrapped("%s", presentation.phase.c_str());
  ImGui::EndChild();
  ImGui::ProgressBar(
      std::clamp(static_cast<float>(presentation.fraction), 0.0f, 1.0f),
      ImVec2(ImGui::GetContentRegionAvail().x, 6.0f * font_size / 13.0f), "");
  ImGui::TextDisabled("%zu of %zu stages ready",
                      presentation.completed_products,
                      presentation.total_products);
  ImGui::EndPopup();
}

} // namespace crimson::ui
