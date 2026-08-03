#include "gui/camera_view_transport_controls.h"
#include "gui/frame_inspect_window.h"
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

bool testCameraTransportUses64BitFrameState() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1000.0f, 240.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  constexpr int64_t kFrameCount = 5'000'000'001LL;
  constexpr int64_t kCurrentFrame = 3'500'000'000LL;
  ImGui::NewFrame();
  ImGui::SetNextWindowSize(ImVec2(960.0f, 180.0f));
  ImGui::Begin("Transport fixture");
  const auto result =
      drawCameraViewTransportControls(CameraViewTransportControlsContext{
          kCurrentFrame,
          kFrameCount,
          kFrameCount - 1,
          700.0,
          false,
          kCurrentFrame,
          true,
      });
  ImGui::End();
  ImGui::Render();

  CHECK(result.slider_frame_number == kCurrentFrame);
  CHECK(result.action == CameraViewTransportAction::None);
  CHECK(!result.intent.has_value());
  ImGui::DestroyContext();
  return true;
}

bool testFrameInspectWindowComposition() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  using crimson::workspace::FrameInspectView;
  FrameInspectView selected = FrameInspectView::Keypoints;
  crimson::workspace::FrameInspectViewSyncState sync;
  int header_draws = 0;
  int detect_draws = 0;
  int keypoint_draws = 0;
  int hidden_draws = 0;
  int footer_draws = 0;
  crimson::gui::FrameInspectWindowComposition composition;
  composition.draw_header = [&]() {
    ++header_draws;
    ImGui::TextUnformatted("Inspection header");
  };
  composition.modules = {
      {FrameInspectView::Detect, "Detect", true,
       [&]() {
         ++detect_draws;
         ImGui::TextUnformatted("Detection module");
       }},
      {FrameInspectView::Keypoints, "Keypoints", true,
       [&]() {
         ++keypoint_draws;
         ImGui::TextUnformatted("Keypoint module");
       }},
      {FrameInspectView::EyeMasks, "Subject Masks", false,
       [&]() {
         ++hidden_draws;
         ImGui::TextUnformatted("Hidden module");
       }},
  };
  composition.draw_footer = [&]() {
    ++footer_draws;
    ImGui::TextUnformatted("Inspection footer");
  };

  ImGui::NewFrame();
  crimson::gui::drawFrameInspectWindow({}, selected, sync, composition);
  ImGui::Render();

  header_draws = 0;
  detect_draws = 0;
  keypoint_draws = 0;
  hidden_draws = 0;
  footer_draws = 0;
  ImGui::NewFrame();
  const auto result =
      crimson::gui::drawFrameInspectWindow({}, selected, sync, composition);
  ImGui::Render();

  CHECK(result.window_visible);
  CHECK(result.rendered_module);
  CHECK(result.active_module == FrameInspectView::Keypoints);
  CHECK(selected == FrameInspectView::Keypoints);
  CHECK(header_draws == 1);
  CHECK(detect_draws == 0);
  CHECK(keypoint_draws == 1);
  CHECK(hidden_draws == 0);
  CHECK(footer_draws == 1);
  CHECK(!sync.shouldApply(FrameInspectView::Keypoints));

  ImGui::DestroyContext();
  return true;
}

} // namespace

int main() {
  if (!testSemanticSnapshot() || !testSessionLoadingModalSnapshot() ||
      !testCameraTransportUses64BitFrameState() ||
      !testFrameInspectWindowComposition()) {
    return 1;
  }
  std::cout << "imgui_semantic_snapshot_tests: PASS\n";
  return 0;
}
