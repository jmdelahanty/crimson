#include "stimulus_camera_overlay_scene.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

std::shared_ptr<const crimson::timeline::StimulusContextTimelineSnapshot>
snapshot() {
  using namespace crimson::timeline;
  StimulusContextTimelineDescriptor descriptor;
  descriptor.run_name = "stimulus_camera_overlay_fixture";
  descriptor.frame_count = 40;
  descriptor.event_types = {{3, "Trial", 0}, {5, "Pulse", 0}};
  std::vector<StimulusContextEvent> events = {
      {0, 100, 8, 1000, 3, {}, "warmup", "{}", {}},
      {1, 101, 8, 1100, 5, {}, "target", "{}", {}},
      {2, 200, 20, 2000, 3, {}, "finish", "{}", {}},
      {3, 300, -1, 3000, 5, {}, "unresolved", "{}", {}},
  };
  StimulusContextStep moving;
  moving.step_index = 0;
  moving.step_name = "Moving";
  moving.stimulus_mode = "MOVING_GRATING";
  moving.start_camera_frame = 5;
  moving.end_camera_frame = 15;
  moving.moving_grating.present = true;
  moving.moving_grating.grating_direction_camera_deg = 90.0;
  StimulusContextStep chaser;
  chaser.step_index = 1;
  chaser.step_name = "Chaser";
  chaser.stimulus_mode = "CHASER";
  chaser.start_camera_frame = 15;
  chaser.end_camera_frame = 30;
  auto repository = MakeStimulusContextTimelineRepository(
      std::move(descriptor), std::move(events),
      {std::move(chaser), std::move(moving)});
  return repository->snapshot();
}

bool near(double left, double right, double tolerance = 1e-9) {
  return std::fabs(left - right) <= tolerance;
}

bool testExactFrameResolutionAndPersistence() {
  using namespace crimson::stimulus;
  const auto data = snapshot();
  auto sample = resolveStimulusCameraOverlayFrame(nullptr, 8);
  CHECK(sample.availability ==
        StimulusCameraOverlayFrameAvailability::TimelineUnavailable);
  sample = resolveStimulusCameraOverlayFrame(data.get(), -1);
  CHECK(sample.availability ==
        StimulusCameraOverlayFrameAvailability::FrameOutOfRange);
  sample = resolveStimulusCameraOverlayFrame(data.get(), 4);
  CHECK(sample.availability ==
        StimulusCameraOverlayFrameAvailability::ValidFrameEmpty);
  CHECK(sample.exactFrame());
  CHECK(sample.latest_events.empty());
  CHECK(!sample.step);

  sample = resolveStimulusCameraOverlayFrame(data.get(), 8);
  CHECK(sample.availability == StimulusCameraOverlayFrameAvailability::Ready);
  CHECK(sample.exact_events.size() == 2);
  CHECK(sample.latest_events.size() == 2);
  CHECK(sample.latest_event_camera_frame == 8);
  CHECK(sample.step && sample.step->step_index == 0);

  sample = resolveStimulusCameraOverlayFrame(data.get(), 14);
  CHECK(sample.exact_events.empty());
  CHECK(sample.latest_events.size() == 2);
  CHECK(sample.latest_event_camera_frame == 8);
  CHECK(sample.step && sample.step->step_index == 0);

  sample = resolveStimulusCameraOverlayFrame(data.get(), 15);
  CHECK(sample.step && sample.step->step_index == 1);
  sample = resolveStimulusCameraOverlayFrame(data.get(), 20);
  CHECK(sample.exact_events.size() == 1);
  CHECK(sample.latest_event_camera_frame == 20);
  CHECK(sample.step && sample.step->step_index == 1);
  sample = resolveStimulusCameraOverlayFrame(data.get(), 40);
  CHECK(sample.availability ==
        StimulusCameraOverlayFrameAvailability::FrameOutOfRange);
  CHECK(!sample.source_camera_frame);
  return true;
}

bool testMaintainedGeometryTextColorsAndOrder() {
  using namespace crimson::stimulus;
  const auto data = snapshot();
  const auto sample = resolveStimulusCameraOverlayFrame(data.get(), 8);
  const auto scene = buildStimulusCameraOverlayScene(
      sample, {430.0, 298.0}, {120.0, 26.0});
  CHECK(scene.ready());
  CHECK(scene.requested_camera_frame == 8);
  CHECK(scene.source_camera_frame == 8);
  CHECK(scene.event_source_camera_frame == 8);
  CHECK(scene.step_index == 0);
  CHECK(scene.grating_direction_camera_deg == 90.0);
  CHECK(near(scene.event_box.x, 12.0));
  CHECK(near(scene.event_box.y, 12.0));
  CHECK(near(scene.event_box.width, 132.0));
  CHECK(near(scene.event_box.height, 34.0));
  CHECK(near(scene.step_box.x, 214.0));
  CHECK(near(scene.step_box.y, 12.0));
  CHECK(near(scene.step_box.width, 204.0));
  CHECK(near(scene.step_box.height, 76.0));
  CHECK(scene.primitives.size() == 4);
  CHECK(scene.primitives[0].layer ==
        StimulusCameraOverlaySceneLayer::EventPanel);
  CHECK(scene.primitives[1].layer ==
        StimulusCameraOverlaySceneLayer::StepPanel);
  CHECK(scene.primitives[2].type ==
        StimulusCameraOverlayPrimitiveType::Line);
  CHECK(scene.primitives[3].type ==
        StimulusCameraOverlayPrimitiveType::Triangle);
  CHECK(scene.text.size() == 2);
  CHECK(scene.text[0].content == "Trial - warmup\nPulse - target");
  CHECK(scene.text[1].content == "Grating motion 90 deg");
  CHECK(scene.text[0].color.valid());
  CHECK(scene.text[1].color.valid());

  const auto& shaft = scene.primitives[2];
  CHECK(near(shaft.first.x, shaft.second.x));
  CHECK(shaft.first.y > shaft.second.y);
  CHECK(scene.primitiveCount(StimulusCameraOverlaySceneLayer::StepArrow) == 2);
  CHECK(scene.textCount(StimulusCameraOverlaySceneLayer::EventText) == 1);
  return true;
}

bool testControlsInvalidInputsAndEmptyScene() {
  using namespace crimson::stimulus;
  const auto data = snapshot();
  auto sample = resolveStimulusCameraOverlayFrame(data.get(), 14);
  StimulusCameraOverlayControls controls;
  controls.persist_latest_event = false;
  auto scene = buildStimulusCameraOverlayScene(sample, {430.0, 298.0},
                                                {0.0, 13.0}, controls);
  CHECK(scene.ready());
  CHECK(!scene.event_box.valid());
  CHECK(scene.step_box.valid());

  controls.show_events = false;
  controls.show_step_direction = false;
  scene = buildStimulusCameraOverlayScene(sample, {430.0, 298.0},
                                          {0.0, 13.0}, controls);
  CHECK(scene.status == StimulusCameraOverlaySceneStatus::Disabled);

  controls = {};
  scene = buildStimulusCameraOverlayScene(sample, {}, {100.0, 13.0}, controls);
  CHECK(scene.status == StimulusCameraOverlaySceneStatus::InvalidViewport);
  scene = buildStimulusCameraOverlayScene(sample, {430.0, 298.0}, {}, controls);
  CHECK(scene.status ==
        StimulusCameraOverlaySceneStatus::InvalidTextMetrics);

  sample = resolveStimulusCameraOverlayFrame(data.get(), 4);
  scene = buildStimulusCameraOverlayScene(sample, {430.0, 298.0},
                                          {0.0, 13.0}, controls);
  CHECK(scene.ready());
  CHECK(scene.primitives.empty());
  CHECK(scene.text.empty());

  sample.source_camera_frame = 3;
  scene = buildStimulusCameraOverlayScene(sample, {430.0, 298.0},
                                          {0.0, 13.0}, controls);
  CHECK(scene.status == StimulusCameraOverlaySceneStatus::NonExactFrame);
  return true;
}

bool testViewportIndependentSignatureAndTessellation() {
  using namespace crimson::stimulus;
  const auto data = snapshot();
  const auto sample = resolveStimulusCameraOverlayFrame(data.get(), 8);
  const auto linux = buildStimulusCameraOverlayScene(
      sample, {430.0, 298.0}, {120.0, 26.0});
  const auto mac = buildStimulusCameraOverlayScene(
      sample, {338.0, 338.0}, {120.0, 26.0});
  CHECK(linux.ready());
  CHECK(mac.ready());
  CHECK(!near(linux.step_box.x, mac.step_box.x));
  const auto linux_signature =
      stimulusCameraOverlaySceneSemanticSignature(linux);
  const auto mac_signature = stimulusCameraOverlaySceneSemanticSignature(mac);
  if (linux_signature != mac_signature) {
    std::cerr << "linux signature: " << linux_signature << '\n'
              << "mac signature: " << mac_signature << '\n';
  }
  CHECK(linux_signature == mac_signature);

  const auto mesh = tessellateStimulusCameraOverlayScene(
      linux, {10.0, 20.0}, 2.0, 2.0);
  CHECK(mesh.primitive_count == 4);
  CHECK(mesh.triangleCount() > 20);
  CHECK(!mesh.triangle_vertices.empty());
  CHECK(mesh.triangle_vertices.front().x >= 10.0f);
  CHECK(mesh.triangle_vertices.front().y >= 20.0f);
  CHECK(tessellateStimulusCameraOverlayScene(linux, {}, 0.0, 1.0)
            .triangle_vertices.empty());
  return true;
}

}  // namespace

int main() {
  if (!testExactFrameResolutionAndPersistence() ||
      !testMaintainedGeometryTextColorsAndOrder() ||
      !testControlsInvalidInputsAndEmptyScene() ||
      !testViewportIndependentSignatureAndTessellation()) {
    return 1;
  }
  std::cout << "stimulus_camera_overlay_scene_tests: PASS\n";
  return 0;
}
