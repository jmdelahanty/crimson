#include "platform/macos/apple_workspace_layout.h"

#include <cmath>
#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "  \
                << #condition << '\n';                                         \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(double lhs, double rhs) { return std::abs(lhs - rhs) < 1e-6; }

bool testReferenceLayout() {
  using namespace crimson::macos::workspace;
  const auto layout = makeMaintainedWorkspaceLayout(1920.0, 1080.0);
  CHECK(near(layout.scale, 1.0));
  CHECK(near(layout.file_browser.x, 10.0));
  CHECK(near(layout.file_browser.width, 560.0));
  CHECK(near(layout.frame_inspect.y, 270.0));
  CHECK(near(layout.frame_inspect.height, 800.0));
  CHECK(near(layout.diagnostics.x, 580.0));
  CHECK(near(layout.frames_in_buffer.x, 895.0));
  CHECK(near(layout.camera.y, 320.0));
  CHECK(near(layout.stimulus_event_timeline.x, 1405.0));
  CHECK(near(layout.analysis_timeline.y, 500.0));
  CHECK(near(layout.analysis_timeline.bottom(), 1060.0));
  return true;
}

bool testScaledFirstUseLayout() {
  using namespace crimson::macos::workspace;
  const auto layout = makeMaintainedWorkspaceLayout(1280.0, 800.0);
  CHECK(near(layout.scale, 2.0 / 3.0));
  CHECK(layout.frame_inspect.right() <= 1280.0);
  CHECK(layout.frame_inspect.bottom() <= 800.0);
  CHECK(layout.camera.right() <= 1280.0);
  CHECK(layout.analysis_timeline.right() <= 1280.0);
  CHECK(layout.analysis_timeline.bottom() <= 800.0);
  return true;
}

bool testOptionalReferenceLayouts() {
  using namespace crimson::macos::workspace;
  const auto crop = makeMaintainedWorkspaceLayout(
      1920.0, 1080.0, LayoutProfile::CropReference);
  CHECK(near(crop.camera.x, 780.0));
  CHECK(near(crop.camera.width, 620.0));
  CHECK(near(crop.advanced_crop_preview.x, 780.0));
  CHECK(near(crop.advanced_crop_preview.width, 420.0));
  CHECK(near(crop.stimulus_event_timeline.y, 580.0));
  CHECK(near(crop.analysis_timeline.y, 820.0));

  const auto stimulus = makeMaintainedWorkspaceLayout(
      1920.0, 1080.0, LayoutProfile::StimulusReference);
  CHECK(near(stimulus.camera.x, 780.0));
  CHECK(near(stimulus.frames_in_buffer.height, 200.0));
  CHECK(near(stimulus.stimulus.x, 780.0));
  CHECK(near(stimulus.stimulus_frames_in_buffer.x, 1270.0));
  CHECK(near(stimulus.stimulus_event_timeline.y, 930.0));
  CHECK(near(stimulus.analysis_timeline.x, 780.0));
  return true;
}

bool testMediaFitAndInsets() {
  using namespace crimson::macos::workspace;
  const Rect square = fitMedia({100.0, 50.0, 500.0, 300.0}, 4512.0, 4512.0);
  CHECK(near(square.x, 200.0));
  CHECK(near(square.y, 50.0));
  CHECK(near(square.width, 300.0));
  CHECK(near(square.height, 300.0));

  const auto insets = makeCameraInsetLayout(square, true, true);
  CHECK(insets.crop.valid());
  CHECK(insets.stimulus.valid());
  CHECK(insets.stimulus.x >= square.x);
  CHECK(insets.crop.right() <= square.right());
  CHECK(insets.crop.bottom() <= square.bottom());
  CHECK(insets.stimulus.right() < insets.crop.x);
  CHECK(!fitMedia({}, 10.0, 10.0).valid());
  CHECK(!makeMaintainedWorkspaceLayout(0.0, 800.0).camera.valid());
  return true;
}

bool testSavedWindowConstraint() {
  using namespace crimson::macos::workspace;
  const Rect work_area{0.0, 0.0, 1280.0, 800.0};

  const Rect shifted =
      constrainToBounds({562.5, 252.0, 650.0, 605.0}, work_area);
  CHECK(near(shifted.x, 562.5));
  CHECK(near(shifted.y, 195.0));
  CHECK(near(shifted.width, 650.0));
  CHECK(near(shifted.height, 605.0));

  const Rect resized =
      constrainToBounds({-50.0, -40.0, 1600.0, 900.0}, work_area);
  CHECK(near(resized.x, 0.0));
  CHECK(near(resized.y, 0.0));
  CHECK(near(resized.width, 1280.0));
  CHECK(near(resized.height, 800.0));
  CHECK(!constrainToBounds({}, work_area).valid());
  return true;
}

} // namespace

int main() {
  if (!testReferenceLayout() || !testScaledFirstUseLayout() ||
      !testOptionalReferenceLayouts() || !testMediaFitAndInsets() ||
      !testSavedWindowConstraint()) {
    return 1;
  }
  std::cout << "apple_workspace_layout_tests: PASS\n";
  return 0;
}
