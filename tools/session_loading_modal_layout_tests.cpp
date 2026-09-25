#include "gui/session_loading_modal.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "   \
                << #condition << '\n';                                         \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

bool testModalLayout() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1600.0f, 1000.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::session::SessionLoadingPresentation state;
  state.visible = true;
  state.phase = "Preparing session";
  state.total_products = 12;

  auto frame = [&]() {
    ImGui::NewFrame();
    crimson::ui::drawSessionLoadingModal(state);
    ImGui::Render();
    ImGuiWindow *window =
        ImGui::FindWindowByName("Loading analysis###session_loading_modal");
    return window != nullptr ? window : ImGui::FindWindowByName("Loading analysis");
  };

  const ImGuiWindow *modal = frame();
  CHECK(modal != nullptr && modal->Active);
  const ImVec2 initial_size = modal->Size;
  const ImGuiID initial_id = modal->ID;
  CHECK(near(initial_size.x, 420.0f));
  for (int i = 0; i < 300; ++i) {
    modal = frame();
    CHECK(modal != nullptr && modal->Active);
    CHECK(near(modal->Size.x, initial_size.x));
    CHECK(near(modal->Size.y, initial_size.y));
  }
  CHECK(initial_size.y > 90.0f && initial_size.y < 140.0f);
  CHECK(!modal->ScrollbarY);
  CHECK((modal->Flags & ImGuiWindowFlags_NoResize) != 0);
  CHECK((modal->Flags & ImGuiWindowFlags_NoMove) != 0);

  for (int i = 0; i <= 100; ++i) {
    state.fraction = i / 100.0;
    state.completed_products = static_cast<size_t>(i * 1000);
    state.total_products = 100000;
    modal = frame();
    CHECK(modal != nullptr && modal->Active);
    CHECK(near(modal->Size.x, initial_size.x));
    CHECK(near(modal->Size.y, initial_size.y));
  }

  state.phase = "Loading canonical keypoints and subject mask manifest metadata";
  modal = frame();
  CHECK(modal != nullptr && near(modal->Size.x, initial_size.x));
  CHECK(near(modal->Size.y, initial_size.y));
  state.phase = "Ready";
  modal = frame();
  CHECK(modal != nullptr && near(modal->Size.x, initial_size.x));
  CHECK(near(modal->Size.y, initial_size.y));

  state.title = "Opening recording";
  modal = frame();
  CHECK(modal != nullptr && modal->Active);
  CHECK(modal->ID == initial_id);
  CHECK(near(modal->Size.x, initial_size.x));
  CHECK(near(modal->Size.y, initial_size.y));
  CHECK(!modal->ScrollbarY);

  ImGui::StyleColorsClassic();
  ImGui::GetStyle().FontScaleMain = 15.0f / 13.0f;
  modal = frame();
  CHECK(modal != nullptr && modal->Active);
  CHECK(modal->Size.x > initial_size.x);
  CHECK(!modal->ScrollbarY);

  io.DisplaySize = ImVec2(320.0f, 240.0f);
  modal = frame();
  CHECK(modal != nullptr && modal->Active);
  CHECK(modal->Size.x < io.DisplaySize.x && modal->Size.y < io.DisplaySize.y);
  CHECK(modal->Size.x > 250.0f);
  CHECK(modal->Pos.x >= 0.0f && modal->Pos.y >= 0.0f);
  CHECK(modal->Pos.x + modal->Size.x <= io.DisplaySize.x);
  CHECK(modal->Pos.y + modal->Size.y <= io.DisplaySize.y);

  io.DisplaySize = ImVec2(320.0f, 240.0f);
  ImGui::GetStyle().FontScaleMain = 1.5f;
  modal = frame();
  CHECK(modal != nullptr && modal->Active);
  CHECK(modal->Size.x < io.DisplaySize.x && modal->Size.y < io.DisplaySize.y);
  CHECK(modal->Size.x > 240.0f);
  CHECK(modal->Pos.x >= 0.0f && modal->Pos.y >= 0.0f);
  CHECK(modal->Pos.x + modal->Size.x <= io.DisplaySize.x);
  CHECK(modal->Pos.y + modal->Size.y <= io.DisplaySize.y);

  state.visible = false;
  frame();
  modal = frame();
  CHECK(modal != nullptr && !modal->Active);
  state.visible = true;
  modal = frame();
  CHECK(modal != nullptr && modal->Active);

  ImGui::DestroyContext();
  return true;
}

} // namespace

int main() {
  if (!testModalLayout()) {
    return 1;
  }
  std::cout << "session_loading_modal_layout_tests: PASS\n";
  return 0;
}
