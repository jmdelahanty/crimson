#include "chaser_distance_polar_scene.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace crimson::polar;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__         \
                << ": " #condition << '\n';                                 \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(double actual, double expected, double tolerance = 1e-6) {
  return std::abs(actual - expected) <= tolerance;
}

ChaserDistancePolarDescriptor descriptor() {
  ChaserDistancePolarDescriptor value;
  value.availability = ChaserDistancePolarAvailability::Ready;
  value.provenance.run_name = "run";
  value.provenance.component_name = "component";
  value.provenance.run_selection =
      ChaserDistancePolarSelectionProvenance::LatestComplete;
  value.provenance.component_selection =
      ChaserDistancePolarSelectionProvenance::LatestCompleted;
  value.row_count = 100;
  value.chaser_count = 4;
  value.distance_unit = ChaserDistancePolarDistanceUnit::Millimeters;
  value.coordinate_frame = std::string(kArenaRelativeCanvasPixelFrame);
  value.angle_convention =
      std::string(kPositiveAnatomicalLeftAngleConvention);
  value.dataset_global_max_distance_mm = 100.0;
  return normalizeChaserDistancePolarDescriptor(std::move(value));
}

ChaserDistancePolarPoint point(int32_t chaser,
                               double distance,
                               double bearing) {
  ChaserDistancePolarPoint value;
  value.chaser_index = chaser;
  value.distance_mm = distance;
  value.bearing_degrees = bearing;
  value.valid = true;
  value.color =
      resolveChaserDistancePolarColor(chaser, std::nullopt, std::nullopt);
  return value;
}

ChaserDistancePolarFrameSample readyFrame() {
  return makeChaserDistancePolarFrameSample(
      descriptor(), 56, 56,
      {point(0, 105.0, 0.0), point(1, 105.0, 90.0),
       point(2, 105.0, -90.0), point(7, 200.0, -180.0)});
}

const ChaserDistancePolarScenePrimitive* marker(
    const ChaserDistancePolarScene& scene,
    int32_t chaser) {
  for (const auto& primitive : scene.primitives) {
    if (primitive.type == ChaserDistancePolarScenePrimitiveType::Marker &&
        primitive.chaser_index == chaser) {
      return &primitive;
    }
  }
  return nullptr;
}

bool hasText(const ChaserDistancePolarScene& scene,
             ChaserDistancePolarSceneLayer layer,
             const std::string& content) {
  for (const auto& text : scene.text) {
    if (text.layer == layer && text.content == content) {
      return true;
    }
  }
  return false;
}

bool testNamesAndLayerOrder() {
  CHECK(chaserDistancePolarSceneStatusName(
            ChaserDistancePolarSceneStatus::Ready) == "ready");
  CHECK(chaserDistancePolarSceneStatusName(
            ChaserDistancePolarSceneStatus::NonExactFrame) ==
        "non_exact_frame");
  const std::vector<std::string> expected = {
      "background", "grid", "axes", "points", "orientation_labels",
      "readout"};
  CHECK(kChaserDistancePolarSceneLayerOrder.size() == expected.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    CHECK(chaserDistancePolarSceneLayerName(
              kChaserDistancePolarSceneLayerOrder[index]) == expected[index]);
  }
  return true;
}

bool testWithholdingStates() {
  const ChaserDistancePolarSceneViewport viewport{1000.0, 800.0};
  auto frame = readyFrame();
  ChaserDistancePolarSceneControls controls;
  controls.show_inset = false;
  CHECK(buildChaserDistancePolarScene(frame, viewport, controls).status ==
        ChaserDistancePolarSceneStatus::Disabled);

  controls.show_inset = true;
  auto unavailable = frame;
  unavailable.availability =
      ChaserDistancePolarAvailability::DatasetUnavailable;
  CHECK(buildChaserDistancePolarScene(unavailable, viewport, controls).status ==
        ChaserDistancePolarSceneStatus::SampleUnavailable);

  auto non_exact = frame;
  non_exact.source_camera_frame = 55;
  CHECK(buildChaserDistancePolarScene(non_exact, viewport, controls).status ==
        ChaserDistancePolarSceneStatus::NonExactFrame);

  auto unsupported = frame;
  unsupported.normalized_angle_convention =
      ChaserDistancePolarAngleConvention::Unsupported;
  CHECK(buildChaserDistancePolarScene(unsupported, viewport, controls).status ==
        ChaserDistancePolarSceneStatus::UnsupportedConventions);

  CHECK(buildChaserDistancePolarScene(frame, {0.0, 800.0}, controls).status ==
        ChaserDistancePolarSceneStatus::InvalidViewport);
  CHECK(buildChaserDistancePolarScene(frame, {159.0, 800.0}, controls).status ==
        ChaserDistancePolarSceneStatus::TooSmall);
  controls.width_px = std::numeric_limits<float>::quiet_NaN();
  CHECK(buildChaserDistancePolarScene(frame, viewport, controls).status ==
        ChaserDistancePolarSceneStatus::TooSmall);
  return true;
}

bool testMaintainedLayoutAndCardinalOrientation() {
  const auto scene = buildChaserDistancePolarScene(
      readyFrame(), {1000.0, 800.0}, {});
  CHECK(scene.ready());
  CHECK(scene.frame_availability == ChaserDistancePolarAvailability::Ready);
  CHECK(scene.requested_camera_frame == 56);
  CHECK(scene.source_camera_frame == 56);
  CHECK(near(scene.box.x, 752.0));
  CHECK(near(scene.box.y, 12.0));
  CHECK(near(scene.box.width, 236.0));
  CHECK(near(scene.box.height, 308.0));
  CHECK(near(scene.graph.x, 760.0));
  CHECK(near(scene.graph.y, 20.0));
  CHECK(near(scene.graph.width, 220.0));
  CHECK(near(scene.center.x, 870.0));
  CHECK(near(scene.center.y, 130.0));
  CHECK(near(scene.radius_px, 86.0));
  CHECK(near(scene.display_max_distance_mm, 105.0));

  CHECK(scene.primitiveCount(
            ChaserDistancePolarSceneLayer::Background) == 1);
  CHECK(scene.primitiveCount(ChaserDistancePolarSceneLayer::Grid) == 2);
  CHECK(scene.primitiveCount(ChaserDistancePolarSceneLayer::Axes) == 2);
  CHECK(scene.primitiveCount(ChaserDistancePolarSceneLayer::Points) == 4);
  CHECK(scene.textCount(
            ChaserDistancePolarSceneLayer::OrientationLabels) == 4);
  CHECK(scene.textCount(ChaserDistancePolarSceneLayer::Readout) == 5);
  CHECK(scene.point_count == 4);

  const auto* front = marker(scene, 0);
  const auto* left = marker(scene, 1);
  const auto* right = marker(scene, 2);
  const auto* behind = marker(scene, 7);
  CHECK(front != nullptr && left != nullptr && right != nullptr &&
        behind != nullptr);
  CHECK(near(front->first.x, scene.center.x));
  CHECK(near(front->first.y, scene.center.y - scene.radius_px));
  CHECK(near(left->first.x, scene.center.x - scene.radius_px));
  CHECK(near(left->first.y, scene.center.y));
  CHECK(near(right->first.x, scene.center.x + scene.radius_px));
  CHECK(near(right->first.y, scene.center.y));
  CHECK(near(behind->first.x, scene.center.x));
  CHECK(near(behind->first.y, scene.center.y + scene.radius_px));
  CHECK(near(behind->radius_px, 5.0));
  CHECK(near(behind->stroke_width_px, 1.3));

  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::OrientationLabels,
                "front"));
  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::OrientationLabels,
                "left"));
  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::OrientationLabels,
                "right"));
  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::OrientationLabels,
                "behind"));
  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::Readout,
                "Chaser bearing f=56"));
  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::Readout,
                "c0 105.0 mm  +0.0 deg"));
  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::Readout,
                "c7 200.0 mm  -180.0 deg"));
  return true;
}

bool testControlsAndEmptyFrame() {
  ChaserDistancePolarSceneControls controls;
  controls.width_px = 360.0f;
  controls.opacity = 0.01f;
  controls.show_labels = false;
  controls.show_readout = false;
  auto scene = buildChaserDistancePolarScene(
      readyFrame(), {1000.0, 800.0}, controls);
  CHECK(scene.ready());
  CHECK(near(scene.graph.width, 320.0));
  CHECK(near(scene.box.height, 336.0));
  CHECK(near(scene.radius_px, 148.0));
  CHECK(scene.text.empty());
  CHECK(!scene.primitives.empty());
  CHECK(near(scene.primitives.front().fill.alpha,
             0.15 * 225.0 / 255.0));

  auto empty = makeChaserDistancePolarFrameSample(
      descriptor(), 56, 56, {});
  CHECK(empty.availability ==
        ChaserDistancePolarAvailability::ValidFrameEmpty);
  scene = buildChaserDistancePolarScene(empty, {1000.0, 800.0}, {});
  CHECK(scene.ready());
  CHECK(scene.point_count == 0);
  CHECK(hasText(scene, ChaserDistancePolarSceneLayer::Readout,
                "No valid chaser sample"));
  return true;
}

bool testPortableTessellation() {
  const auto scene = buildChaserDistancePolarScene(
      readyFrame(), {1000.0, 800.0}, {});
  const auto mesh = tessellateChaserDistancePolarScene(
      scene, {100.0, 50.0}, 2.0, 2.0, 48);
  CHECK(mesh.primitive_count == scene.primitives.size());
  CHECK(mesh.triangleCount() > scene.primitives.size());
  CHECK(mesh.triangle_vertices.size() % 3 == 0);
  for (const auto& vertex : mesh.triangle_vertices) {
    CHECK(std::isfinite(vertex.x));
    CHECK(std::isfinite(vertex.y));
    CHECK(vertex.x >= 100.0f);
    CHECK(vertex.y >= 50.0f);
    CHECK(vertex.color.valid());
  }
  CHECK(tessellateChaserDistancePolarScene(
            scene, {}, 0.0, 1.0, 48)
            .triangle_vertices.empty());
  return true;
}

bool testViewportIndependentSemanticSignature() {
  const auto linux_scene = buildChaserDistancePolarScene(
      readyFrame(), {430.0, 298.0}, {});
  const auto mac_scene = buildChaserDistancePolarScene(
      readyFrame(), {338.0, 338.0}, {});
  CHECK(linux_scene.ready());
  CHECK(mac_scene.ready());
  CHECK(near(linux_scene.box.width, mac_scene.box.width));
  CHECK(near(linux_scene.box.height, mac_scene.box.height));
  CHECK(!near(linux_scene.box.x, mac_scene.box.x));
  const auto linux_signature =
      chaserDistancePolarSceneSemanticSignature(linux_scene);
  const auto mac_signature =
      chaserDistancePolarSceneSemanticSignature(mac_scene);
  CHECK(linux_signature.rfind("crimson.polar-scene-semantics.v1", 0) == 0);
  if (linux_signature != mac_signature) {
    const auto mismatch = std::mismatch(
        linux_signature.begin(), linux_signature.end(), mac_signature.begin(),
        mac_signature.end());
    const size_t index =
        static_cast<size_t>(mismatch.first - linux_signature.begin());
    std::cerr << "semantic signature mismatch at " << index << "\nlinux: "
              << linux_signature.substr(index, 120) << "\nmac:   "
              << mac_signature.substr(index, 120) << '\n';
  }
  CHECK(linux_signature == mac_signature);

  ChaserDistancePolarSceneControls changed;
  changed.opacity = 0.5f;
  CHECK(linux_signature != chaserDistancePolarSceneSemanticSignature(
                               buildChaserDistancePolarScene(
                                   readyFrame(), {430.0, 298.0}, changed)));
  return true;
}

}  // namespace

int main() {
  if (!testNamesAndLayerOrder() || !testWithholdingStates() ||
      !testMaintainedLayoutAndCardinalOrientation() ||
      !testControlsAndEmptyFrame() || !testPortableTessellation() ||
      !testViewportIndependentSemanticSignature()) {
    return 1;
  }
  std::cout << "chaser_distance_polar_scene_tests: PASS\n";
  return 0;
}
