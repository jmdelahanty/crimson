#include "gui/camera_view_transport_controls.h"
#include "gui/camera_view_window_identity.h"
#include "imgui.h"
#include "imgui_semantic_snapshot.h"

#include <cmath>
#include <iostream>
#include <optional>
#include <string>

namespace {
#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "CHECK failed at " << __LINE__ << ": " << #condition << '\n'; \
  return false; } } while (false)

bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

struct Frame {
  ImGuiID identity = 0;
  ImVec2 position, size, timeline_position;
  bool collapsed = false;
  CameraViewTransportControlsResult transport;
  crimson::ui::SemanticSnapshot snapshot;
};

struct Fixture {
  int64_t frame = 0;
  Fixture() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60;
    unsigned char *pixels = nullptr;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  }
  ~Fixture() {
    crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
    ImGui::DestroyContext();
  }
  Frame draw(const std::string &label, bool move = false,
             std::optional<bool> collapse = std::nullopt) {
    Frame result;
    crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin(label.c_str());
    if (move) {
      ImGui::SetWindowPos(ImVec2(350, 600));
      ImGui::SetWindowSize(ImVec2(640, 420));
    }
    if (collapse) ImGui::SetWindowCollapsed(*collapse);
    result.identity = ImGui::GetID("##identity-probe");
    result.position = ImGui::GetWindowPos();
    result.size = ImGui::GetWindowSize();
    result.collapsed = ImGui::IsWindowCollapsed();
    if (visible) {
      CameraViewTransportControlsContext controls;
      controls.current_display_frame = controls.slider_frame_number = frame;
      controls.total_num_frames = 2937604;
      controls.video_fps = 30;
      result.transport = drawCameraViewTransportControls(controls);
      frame = result.transport.slider_frame_number;
    }
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(1000, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(900, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Analysis Timeline");
    if (move) ImGui::SetWindowPos(ImVec2(900, 70));
    result.timeline_position = ImGui::GetWindowPos();
    ImGui::End();
    ImGui::Render();
    result.snapshot = crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
    return result;
  }
};

std::string label(const std::string &clip, const std::string &recording = "recording:august93",
                  const std::string &camera = "2010093", size_t slot = 0) {
  return crimson::gui::cameraViewWindowLabel("Cam2010093_" + clip, recording, camera, slot);
}

std::string hiddenIdentity(const std::string &name) {
  const auto marker = name.rfind("###");
  return marker == std::string::npos ? name : name.substr(marker);
}

bool testLayoutAndScope() {
  Fixture fixture;
  const auto first = label("clip_000000");
  const auto next = label("clip_000001");
  fixture.draw(first);
  fixture.draw(first);
  const auto moved = fixture.draw(first, true);
  for (const auto &name : {first, next, label("clip_000029"), first}) {
    const auto result = fixture.draw(name);
    CHECK(result.identity == moved.identity);
    CHECK(near(result.position.x, 350) && near(result.position.y, 600));
    CHECK(near(result.size.x, 640) && near(result.size.y, 420));
    CHECK(near(result.timeline_position.x, 900) && near(result.timeline_position.y, 70));
    bool found_title = false;
    for (const auto &window : result.snapshot.windows) {
      if (hiddenIdentity(window.name) == hiddenIdentity(name)) {
        // ImGui renders Begin's current title but can retain the original
        // window->Name internally. The snapshot reads that cached name.
        CHECK(hiddenIdentity(window.name) == hiddenIdentity(first));
        found_title = true;
      }
    }
    CHECK(found_title);
  }
  fixture.draw(first, false, true);
  CHECK(fixture.draw(next).collapsed);
  fixture.draw(next, false, false);
  fixture.draw(first);
  CHECK(!fixture.draw(first).collapsed);

  for (const auto &different : {label("clip_000000", "recording:other"),
                                 label("clip_000000", "recording:august93", "2010094")}) {
    const auto result = fixture.draw(different);
    CHECK(result.identity != moved.identity);
    CHECK(near(result.position.x, 0) && near(result.position.y, 200));
  }
  CHECK(crimson::gui::cameraViewWindowLabel("ordinary.mp4", "", "") == "ordinary.mp4");
  CHECK(label("a", "r", "", 0) != label("a", "r", "", 1));
  CHECK(label("a", "r", "", 0) != label("a", "r", "0", 0));
  // Source text cannot inject a new ### ID or collide at component separators.
  CHECK(fixture.draw(label("a", "r###same", "c")).identity !=
        fixture.draw(label("a", "s###same", "c")).identity);
  CHECK(fixture.draw(label("a", "r/c", "d")).identity !=
        fixture.draw(label("a", "r", "c/d")).identity);
  const auto fallback = fixture.draw(label("a", "archive:legacy", "", 0));
  CHECK(fixture.draw(label("b", "archive:legacy", "", 0)).identity == fallback.identity);
  return true;
}

bool testSliderReleaseSurvivesClipChange() {
  Fixture fixture;
  const auto first = label("clip_000000");
  const auto next = label("clip_000029");
  fixture.draw(first);
  const auto warm = fixture.draw(first);
  std::optional<crimson::ui::SemanticItem> slider;
  for (const auto &item : warm.snapshot.items) {
    if (item.window_name == first && item.label == "##camera-transport-frame") slider = item;
  }
  CHECK(slider.has_value());
  auto &io = ImGui::GetIO();
  io.AddMousePosEvent(slider->bounds.x + slider->bounds.width * 0.7f,
                      slider->bounds.y + slider->bounds.height * 0.5f);
  fixture.draw(first); // Hover before the press, as a real pointer would.
  io.AddMouseButtonEvent(0, true);
  const auto pressed = fixture.draw(first);
  CHECK(pressed.transport.action == CameraViewTransportAction::SeekPreview);
  CHECK(pressed.transport.slider_active);
  const auto held = fixture.draw(next); // Decoder handoff changed visible title.
  CHECK(held.identity == warm.identity);
  CHECK(held.transport.slider_active);
  bool same_slider = false;
  for (const auto &item : held.snapshot.items) {
    if (hiddenIdentity(item.window_name) == hiddenIdentity(next) &&
        item.label == "##camera-transport-frame") {
      CHECK(item.id == slider->id);
      same_slider = true;
    }
  }
  CHECK(same_slider);
  io.AddMouseButtonEvent(0, false);
  const auto released = fixture.draw(next);
  CHECK(released.transport.slider_released);
  CHECK(released.transport.action == CameraViewTransportAction::SeekCommit);
  CHECK(released.transport.seek_request.has_value());
  // The time label can resize the track while held; release must preserve the
  // last drag value, not necessarily the initial mouse-down value.
  CHECK(released.transport.slider_frame_number == held.transport.slider_frame_number);
  CHECK(released.transport.seek_request->target_frame == held.transport.slider_frame_number);
  return true;
}
} // namespace

int main() {
  if (!testLayoutAndScope() || !testSliderReleaseSurvivesClipChange()) return 1;
  std::cout << "camera_view_window_identity_tests: PASS\n";
}
