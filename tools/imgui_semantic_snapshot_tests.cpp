#include "gui/session_loading_modal.h"
#include "imgui.h"
#include "imgui_semantic_snapshot.h"
#include "loading_progress.h"
#include "session_readiness.h"

#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
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

bool near(float lhs, float rhs) { return std::abs(lhs - rhs) < 0.01f; }

bool testSemanticSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.DisplayFramebufferScale = ImVec2(2.0f, 2.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(10.0f, 20.0f));
  ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
  bool checked = true;
  ImGui::Begin("Fixture###fixture-window");
  ImGui::Button("Open...");
  ImGui::Checkbox("Enabled", &checked);
  ImGui::SliderInt("Width", &width, 1, 1024);
  ImGui::End();
  ImGui::Render();

  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
  CHECK(snapshot.frame >= 1);
  CHECK(near(snapshot.display_width, 640.0f));
  CHECK(near(snapshot.framebuffer_scale_x, 2.0f));
  const crimson::ui::SemanticWindow *fixture_window = nullptr;
  for (const auto &window : snapshot.windows) {
    if (window.name == "Fixture###fixture-window") {
      fixture_window = &window;
      break;
    }
  }
  if (fixture_window == nullptr) {
    for (const auto &window : snapshot.windows) {
      std::cerr << "captured window: " << window.name << '\n';
    }
  }
  CHECK(fixture_window != nullptr);
  CHECK(fixture_window->visible_name == "Fixture");
  CHECK(near(fixture_window->bounds.x, 10.0f));
  CHECK(near(fixture_window->bounds.y, 20.0f));

  bool found_open = false;
  bool found_enabled = false;
  bool found_width = false;
  int previous_order = -1;
  for (const auto &item : snapshot.items) {
    CHECK(item.submission_order > previous_order);
    previous_order = item.submission_order;
    if (item.window_name != "Fixture###fixture-window") {
      continue;
    }
    CHECK(item.window_relative_bounds.x >= 0.0f);
    CHECK(item.window_relative_bounds.y >= 0.0f);
    found_open |= item.visible_label == "Open...";
    found_enabled |= item.visible_label == "Enabled";
    found_width |= item.visible_label == "Width";
  }
  CHECK(found_open);
  CHECK(found_enabled);
  CHECK(found_width);

  const auto json = crimson::ui::semanticSnapshotJson(snapshot);
  CHECK(json.at("schema") == "crimson.imgui_semantic_snapshot.v1");
  CHECK(json.at("windows").size() >= 1);
  CHECK(json.at("items").size() >= 3);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testSessionLoadingModalSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::loading::LoadingProgressTracker progress;
  progress.start(2, "Loading keypoints");
  progress.completeProduct("archive", "Archive ready", true, 1.0);
  progress.startProduct("keypoints", "Loading keypoints");
  const std::vector<crimson::session::SessionReadinessProductRule> rules = {
      {"archive", crimson::session::ProductAvailabilityRequirement::Required}};
  const auto readiness =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  const auto presentation = crimson::session::makeSessionLoadingPresentation(
      progress.snapshot(), readiness);

  ImGui::NewFrame();
  ImGui::Begin("Host");
  crimson::ui::drawSessionLoadingModal(presentation);
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Host");
  crimson::ui::drawSessionLoadingModal(presentation);
  ImGui::End();
  ImGui::Render();

  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
  bool found_modal = false;
  for (const auto &window : snapshot.windows) {
    found_modal |= window.visible_name == "Loading analysis";
  }
  if (!found_modal) {
    for (const auto &window : snapshot.windows) {
      std::cerr << "captured loading window: " << window.name << '\n';
    }
  }
  CHECK(found_modal);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

} // namespace

int main() {
  if (!testSemanticSnapshot() || !testSessionLoadingModalSnapshot()) {
    return 1;
  }
  std::cout << "imgui_semantic_snapshot_tests: PASS\n";
  return 0;
}
