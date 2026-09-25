#pragma once

#include "chaser_distance_polar.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crimson::polar {

enum class ChaserDistancePolarSceneStatus : uint8_t {
  Ready,
  Disabled,
  SampleUnavailable,
  NonExactFrame,
  UnsupportedConventions,
  InvalidViewport,
  TooSmall,
};

enum class ChaserDistancePolarSceneLayer : uint8_t {
  Background,
  Grid,
  Axes,
  Points,
  OrientationLabels,
  Readout,
};

inline constexpr std::array<ChaserDistancePolarSceneLayer, 6>
    kChaserDistancePolarSceneLayerOrder = {
        ChaserDistancePolarSceneLayer::Background,
        ChaserDistancePolarSceneLayer::Grid,
        ChaserDistancePolarSceneLayer::Axes,
        ChaserDistancePolarSceneLayer::Points,
        ChaserDistancePolarSceneLayer::OrientationLabels,
        ChaserDistancePolarSceneLayer::Readout,
};

struct ChaserDistancePolarScenePoint {
  double x = 0.0;
  double y = 0.0;
};

struct ChaserDistancePolarSceneRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;

  bool valid() const;
};

struct ChaserDistancePolarSceneViewport {
  double width_px = 0.0;
  double height_px = 0.0;

  bool valid() const;
};

struct ChaserDistancePolarSceneControls {
  bool show_inset = true;
  float width_px = 220.0f;
  float opacity = 0.86f;
  bool show_readout = true;
  bool show_labels = true;
};

enum class ChaserDistancePolarScenePrimitiveType : uint8_t {
  RoundedRectangle,
  Circle,
  Line,
  Marker,
};

struct ChaserDistancePolarScenePrimitive {
  ChaserDistancePolarScenePrimitiveType type =
      ChaserDistancePolarScenePrimitiveType::Line;
  ChaserDistancePolarSceneLayer layer =
      ChaserDistancePolarSceneLayer::Grid;
  ChaserDistancePolarScenePoint first;
  ChaserDistancePolarScenePoint second;
  double radius_px = 0.0;
  double corner_radius_px = 0.0;
  double stroke_width_px = 0.0;
  size_t segment_count = 0;
  ChaserDistancePolarRgba fill{0.0, 0.0, 0.0, 0.0};
  ChaserDistancePolarRgba stroke{0.0, 0.0, 0.0, 0.0};
  bool has_fill = false;
  bool has_stroke = false;
  int32_t chaser_index = -1;
};

struct ChaserDistancePolarSceneText {
  ChaserDistancePolarSceneLayer layer =
      ChaserDistancePolarSceneLayer::Readout;
  ChaserDistancePolarScenePoint anchor;
  ChaserDistancePolarRgba color;
  bool centered = false;
  std::string content;
};

struct ChaserDistancePolarScene {
  ChaserDistancePolarSceneStatus status =
      ChaserDistancePolarSceneStatus::SampleUnavailable;
  ChaserDistancePolarAvailability frame_availability =
      ChaserDistancePolarAvailability::DatasetUnavailable;
  int64_t requested_camera_frame = -1;
  std::optional<int64_t> source_camera_frame;
  ChaserDistancePolarSceneViewport viewport;
  ChaserDistancePolarSceneRect box;
  ChaserDistancePolarSceneRect graph;
  ChaserDistancePolarScenePoint center;
  double radius_px = 0.0;
  double display_max_distance_mm = 0.0;
  size_t point_count = 0;
  std::vector<ChaserDistancePolarScenePrimitive> primitives;
  std::vector<ChaserDistancePolarSceneText> text;

  bool ready() const {
    return status == ChaserDistancePolarSceneStatus::Ready;
  }
  size_t primitiveCount(ChaserDistancePolarSceneLayer layer) const;
  size_t textCount(ChaserDistancePolarSceneLayer layer) const;
};

struct ChaserDistancePolarSceneMeshVertex {
  float x = 0.0f;
  float y = 0.0f;
  ChaserDistancePolarRgba color;
};

struct ChaserDistancePolarSceneMesh {
  std::vector<ChaserDistancePolarSceneMeshVertex> triangle_vertices;
  size_t primitive_count = 0;

  size_t triangleCount() const { return triangle_vertices.size() / 3; }
};

std::string_view chaserDistancePolarSceneStatusName(
    ChaserDistancePolarSceneStatus status);
std::string_view chaserDistancePolarSceneLayerName(
    ChaserDistancePolarSceneLayer layer);

// Canonicalizes scene semantics relative to the inset box so equivalent
// presentations can be compared across differently sized camera viewports.
std::string chaserDistancePolarSceneSemanticSignature(
    const ChaserDistancePolarScene& scene);

ChaserDistancePolarScene buildChaserDistancePolarScene(
    const ChaserDistancePolarFrameSample& frame,
    const ChaserDistancePolarSceneViewport& viewport,
    const ChaserDistancePolarSceneControls& controls = {});

ChaserDistancePolarSceneMesh tessellateChaserDistancePolarScene(
    const ChaserDistancePolarScene& scene,
    ChaserDistancePolarScenePoint display_origin = {},
    double scale_x = 1.0,
    double scale_y = 1.0,
    size_t circle_segment_count = 48);

}  // namespace crimson::polar
