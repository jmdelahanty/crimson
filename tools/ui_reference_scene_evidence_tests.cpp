#include "ui_reference_scene_evidence.h"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <set>
#include <string>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(double actual, double expected, double tolerance = 1e-9) {
  return std::abs(actual - expected) <= tolerance;
}

bool hasExactKeys(const nlohmann::json &value,
                  std::initializer_list<const char *> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  std::set<std::string> actual_keys;
  for (const auto &[key, ignored] : value.items()) {
    (void)ignored;
    actual_keys.insert(key);
  }
  std::set<std::string> expected_keys;
  for (const char *key : expected) {
    expected_keys.insert(key);
  }
  return actual_keys == expected_keys;
}

crimson::polar::ChaserDistancePolarScene polarScene() {
  using namespace crimson::polar;
  ChaserDistancePolarScene scene;
  scene.status = ChaserDistancePolarSceneStatus::Ready;
  scene.frame_availability = ChaserDistancePolarAvailability::Ready;
  scene.requested_camera_frame = 42;
  scene.source_camera_frame = 42;
  scene.viewport = {640.0, 480.0};
  scene.box = {10.0, 20.0, 100.0, 80.0};
  scene.graph = {18.0, 28.0, 84.0, 64.0};
  scene.center = {60.0, 55.0};
  scene.radius_px = 30.0;
  scene.display_max_distance_mm = 12.5;
  scene.point_count = 1;

  ChaserDistancePolarScenePrimitive marker;
  marker.type = ChaserDistancePolarScenePrimitiveType::Marker;
  marker.layer = ChaserDistancePolarSceneLayer::Points;
  marker.first = {25.0, 35.0};
  marker.radius_px = 4.0;
  marker.fill = {0.1, 0.2, 0.3, 0.4};
  marker.stroke = {0.5, 0.6, 0.7, 0.8};
  marker.chaser_index = 3;
  scene.primitives.push_back(marker);

  ChaserDistancePolarScenePrimitive grid;
  grid.type = ChaserDistancePolarScenePrimitiveType::Line;
  grid.layer = ChaserDistancePolarSceneLayer::Grid;
  scene.primitives.push_back(grid);

  ChaserDistancePolarSceneText text;
  text.layer = ChaserDistancePolarSceneLayer::Readout;
  text.anchor = {20.0, 30.0};
  text.color = {0.9, 0.8, 0.7, 1.0};
  text.centered = true;
  text.content = "3: 12.5 mm";
  scene.text.push_back(text);
  return scene;
}

crimson::polar::ChaserDistancePolarDescriptor polarDescriptor() {
  using namespace crimson::polar;
  ChaserDistancePolarDescriptor descriptor;
  descriptor.availability = ChaserDistancePolarAvailability::Ready;
  descriptor.provenance.source_group = "analysis/chaser_distance_runs";
  descriptor.provenance.run_name = "polar_run";
  descriptor.provenance.component_name = "chaser";
  descriptor.provenance.run_selection =
      ChaserDistancePolarSelectionProvenance::Requested;
  descriptor.provenance.component_selection =
      ChaserDistancePolarSelectionProvenance::LatestComplete;
  descriptor.row_count = 100;
  descriptor.chaser_count = 4;
  descriptor.distance_unit = ChaserDistancePolarDistanceUnit::Millimeters;
  descriptor.coordinate_frame = "arena_relative_canvas_px";
  descriptor.angle_convention = "test_convention";
  descriptor.normalized_coordinate_frame =
      ChaserDistancePolarCoordinateFrame::ArenaRelativeCanvasPixels;
  descriptor.normalized_angle_convention = ChaserDistancePolarAngleConvention::
      EgocentricDegreesZeroFrontPositiveAnatomicalLeft;
  descriptor.dataset_global_max_distance_mm = 12.0;
  descriptor.radial_scale.display_max_distance_mm = 12.5;
  return descriptor;
}

bool testPolarExactSchemaAndScaling() {
  const auto scene = polarScene();
  const auto descriptor = polarDescriptor();
  const auto unscaled = crimson::ui_reference::polarSceneEvidenceJson(
      scene, {100.0, 200.0, 1.0, 1.0}, &descriptor);
  CHECK(hasExactKeys(
      unscaled,
      {"ready", "status", "availability", "requested_frame", "source_frame",
       "point_count", "primitive_count", "text_count", "semantic_signature",
       "descriptor", "viewport", "box", "screen_box", "graph", "center",
       "radius_px", "display_max_distance_mm", "points", "text"}));
  CHECK(hasExactKeys(
      unscaled["descriptor"],
      {"availability", "source_group", "run_name", "component_name",
       "run_selection", "component_selection", "row_count", "chaser_count",
       "distance_unit", "coordinate_frame", "angle_convention",
       "normalized_coordinate_frame", "normalized_angle_convention",
       "dataset_global_max_distance_mm", "display_max_distance_mm"}));
  CHECK(unscaled["primitive_count"] == 2);
  CHECK(unscaled["points"].size() == 1);
  CHECK(hasExactKeys(unscaled["points"][0],
                     {"chaser_index", "center", "screen_center", "radius_px",
                      "fill", "stroke"}));
  CHECK(hasExactKeys(unscaled["text"][0],
                     {"layer", "anchor", "centered", "color", "content"}));
  CHECK(near(unscaled["screen_box"]["x"].get<double>(), 110.0));
  CHECK(near(unscaled["screen_box"]["y"].get<double>(), 220.0));
  CHECK(near(unscaled["points"][0]["screen_center"]["x"].get<double>(), 125.0));
  CHECK(near(unscaled["points"][0]["center"]["x"].get<double>(), 15.0));

  const auto retina = crimson::ui_reference::polarSceneEvidenceJson(
      scene, {100.0, 200.0, 2.0, 3.0}, &descriptor);
  CHECK(near(retina["screen_box"]["x"].get<double>(), 120.0));
  CHECK(near(retina["screen_box"]["y"].get<double>(), 260.0));
  CHECK(near(retina["screen_box"]["width"].get<double>(), 200.0));
  CHECK(near(retina["screen_box"]["height"].get<double>(), 240.0));
  CHECK(near(retina["points"][0]["screen_center"]["x"].get<double>(), 150.0));
  CHECK(near(retina["points"][0]["screen_center"]["y"].get<double>(), 305.0));
  CHECK(retina["semantic_signature"] == unscaled["semantic_signature"]);
  return true;
}

crimson::stimulus::StimulusCameraOverlayScene stimulusScene() {
  using namespace crimson::stimulus;
  StimulusCameraOverlayScene scene;
  scene.status = StimulusCameraOverlaySceneStatus::Ready;
  scene.frame_availability = StimulusCameraOverlayFrameAvailability::Ready;
  scene.requested_camera_frame = 42;
  scene.source_camera_frame = 42;
  scene.event_source_camera_frame = 40;
  scene.step_index = 2;
  scene.grating_direction_camera_deg = 90.0;
  scene.viewport = {640.0, 480.0};
  scene.event_box = {10.0, 20.0, 30.0, 40.0};
  scene.step_box = {50.0, 60.0, 70.0, 80.0};

  StimulusCameraOverlayPrimitive primitive;
  primitive.type = StimulusCameraOverlayPrimitiveType::Triangle;
  primitive.layer = StimulusCameraOverlaySceneLayer::StepArrow;
  primitive.first = {1.0, 2.0};
  primitive.second = {3.0, 4.0};
  primitive.third = {5.0, 6.0};
  primitive.corner_radius_px = 7.0;
  primitive.stroke_width_px = 8.0;
  primitive.fill = {0.1, 0.2, 0.3, 0.4};
  primitive.stroke = {0.5, 0.6, 0.7, 0.8};
  primitive.has_fill = true;
  primitive.has_stroke = true;
  scene.primitives.push_back(primitive);

  StimulusCameraOverlayText text;
  text.layer = StimulusCameraOverlaySceneLayer::StepText;
  text.anchor = {9.0, 10.0};
  text.color = {0.9, 0.8, 0.7, 1.0};
  text.content = "Moving grating";
  scene.text.push_back(text);
  return scene;
}

bool testStimulusExactSchemaAndScaling() {
  crimson::timeline::StimulusContextTimelineDescriptor descriptor;
  descriptor.run_name = "stimulus_run";
  descriptor.frame_count = 100;
  descriptor.event_count = 20;
  descriptor.step_count = 3;
  const auto scene = stimulusScene();
  const auto unscaled =
      crimson::ui_reference::stimulusCameraOverlaySceneEvidenceJson(
          scene, {100.0, 200.0, 1.0, 1.0}, &descriptor);
  CHECK(hasExactKeys(unscaled, {"ready",
                                "status",
                                "availability",
                                "requested_frame",
                                "source_frame",
                                "event_source_frame",
                                "step_index",
                                "grating_direction_camera_deg",
                                "primitive_count",
                                "text_count",
                                "event_box",
                                "step_box",
                                "viewport",
                                "display_origin",
                                "display_scale",
                                "semantic_signature",
                                "screen_event_box",
                                "screen_step_box",
                                "descriptor",
                                "primitives",
                                "text"}));
  CHECK(hasExactKeys(unscaled["descriptor"],
                     {"run_name", "frame_count", "event_count", "step_count"}));
  CHECK(hasExactKeys(unscaled["primitives"][0],
                     {"type", "layer", "first", "second", "third",
                      "corner_radius_px", "stroke_width_px", "fill", "stroke",
                      "has_fill", "has_stroke"}));
  CHECK(hasExactKeys(unscaled["text"][0],
                     {"layer", "anchor", "color", "content"}));
  CHECK(near(unscaled["display_scale"]["x"].get<double>(), 1.0));
  CHECK(near(unscaled["screen_event_box"]["x"].get<double>(), 110.0));
  CHECK(near(unscaled["screen_event_box"]["y"].get<double>(), 220.0));
  CHECK(near(unscaled["screen_step_box"]["x"].get<double>(), 150.0));

  const auto retina =
      crimson::ui_reference::stimulusCameraOverlaySceneEvidenceJson(
          scene, {100.0, 200.0, 2.0, 3.0}, &descriptor);
  CHECK(near(retina["display_scale"]["x"].get<double>(), 2.0));
  CHECK(near(retina["display_scale"]["y"].get<double>(), 3.0));
  CHECK(near(retina["screen_event_box"]["x"].get<double>(), 120.0));
  CHECK(near(retina["screen_event_box"]["y"].get<double>(), 260.0));
  CHECK(near(retina["screen_event_box"]["width"].get<double>(), 60.0));
  CHECK(near(retina["screen_event_box"]["height"].get<double>(), 120.0));
  CHECK(near(retina["screen_step_box"]["x"].get<double>(), 200.0));
  CHECK(near(retina["screen_step_box"]["y"].get<double>(), 380.0));
  CHECK(retina["semantic_signature"] == unscaled["semantic_signature"]);
  return true;
}

bool testInvalidPolarTransformWithholdsScreenGeometry() {
  const auto result = crimson::ui_reference::polarSceneEvidenceJson(
      polarScene(), {0.0, 0.0, 0.0, 1.0});
  CHECK(result["ready"] == true);
  CHECK(!result.contains("screen_box"));
  CHECK(!result.contains("points"));
  return true;
}

} // namespace

int main() {
  if (!testPolarExactSchemaAndScaling() ||
      !testStimulusExactSchemaAndScaling() ||
      !testInvalidPolarTransformWithholdsScreenGeometry()) {
    return 1;
  }
  std::cout << "ui_reference_scene_evidence_tests: PASS\n";
  return 0;
}
