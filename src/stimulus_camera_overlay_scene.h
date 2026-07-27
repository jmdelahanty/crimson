#pragma once

#include "stimulus_context_timeline.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crimson::stimulus {

enum class StimulusCameraOverlayFrameAvailability : uint8_t {
  Ready,
  ValidFrameEmpty,
  TimelineUnavailable,
  FrameOutOfRange,
};

struct StimulusCameraOverlayFrameSample {
  StimulusCameraOverlayFrameAvailability availability =
      StimulusCameraOverlayFrameAvailability::TimelineUnavailable;
  int64_t requested_camera_frame = -1;
  std::optional<int64_t> source_camera_frame;
  std::vector<timeline::StimulusContextEvent> exact_events;
  std::vector<timeline::StimulusContextEvent> latest_events;
  std::optional<int64_t> latest_event_camera_frame;
  std::optional<timeline::StimulusContextStep> step;

  bool exactFrame() const {
    return source_camera_frame &&
           *source_camera_frame == requested_camera_frame;
  }
};

StimulusCameraOverlayFrameSample resolveStimulusCameraOverlayFrame(
    const timeline::StimulusContextTimelineSnapshot* snapshot,
    int64_t requested_camera_frame);

enum class StimulusCameraOverlaySceneStatus : uint8_t {
  Ready,
  Disabled,
  FrameUnavailable,
  NonExactFrame,
  InvalidViewport,
  InvalidTextMetrics,
};

enum class StimulusCameraOverlaySceneLayer : uint8_t {
  EventPanel,
  EventText,
  StepPanel,
  StepText,
  StepArrow,
};

enum class StimulusCameraOverlayPrimitiveType : uint8_t {
  RoundedRectangle,
  Line,
  Triangle,
};

struct StimulusCameraOverlayPoint {
  double x = 0.0;
  double y = 0.0;
};

struct StimulusCameraOverlayRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;

  bool valid() const;
};

struct StimulusCameraOverlayViewport {
  double width_px = 0.0;
  double height_px = 0.0;

  bool valid() const;
};

struct StimulusCameraOverlayColor {
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  double alpha = 0.0;

  bool valid() const;
};

struct StimulusCameraOverlayTextMetrics {
  double width_px = 0.0;
  double height_px = 0.0;

  bool valid() const;
};

struct StimulusCameraOverlayControls {
  bool show_events = true;
  bool persist_latest_event = true;
  bool show_step_direction = true;
};

struct StimulusCameraOverlayPrimitive {
  StimulusCameraOverlayPrimitiveType type =
      StimulusCameraOverlayPrimitiveType::Line;
  StimulusCameraOverlaySceneLayer layer =
      StimulusCameraOverlaySceneLayer::EventPanel;
  StimulusCameraOverlayPoint first;
  StimulusCameraOverlayPoint second;
  StimulusCameraOverlayPoint third;
  double corner_radius_px = 0.0;
  double stroke_width_px = 0.0;
  size_t segment_count = 0;
  StimulusCameraOverlayColor fill;
  StimulusCameraOverlayColor stroke;
  bool has_fill = false;
  bool has_stroke = false;
};

struct StimulusCameraOverlayText {
  StimulusCameraOverlaySceneLayer layer =
      StimulusCameraOverlaySceneLayer::EventText;
  StimulusCameraOverlayPoint anchor;
  StimulusCameraOverlayColor color;
  std::string content;
};

struct StimulusCameraOverlayScene {
  StimulusCameraOverlaySceneStatus status =
      StimulusCameraOverlaySceneStatus::FrameUnavailable;
  StimulusCameraOverlayFrameAvailability frame_availability =
      StimulusCameraOverlayFrameAvailability::TimelineUnavailable;
  int64_t requested_camera_frame = -1;
  std::optional<int64_t> source_camera_frame;
  std::optional<int64_t> event_source_camera_frame;
  std::optional<int32_t> step_index;
  std::optional<double> grating_direction_camera_deg;
  StimulusCameraOverlayViewport viewport;
  StimulusCameraOverlayRect event_box;
  StimulusCameraOverlayRect step_box;
  std::vector<StimulusCameraOverlayPrimitive> primitives;
  std::vector<StimulusCameraOverlayText> text;

  bool ready() const {
    return status == StimulusCameraOverlaySceneStatus::Ready;
  }
  size_t primitiveCount(StimulusCameraOverlaySceneLayer layer) const;
  size_t textCount(StimulusCameraOverlaySceneLayer layer) const;
};

struct StimulusCameraOverlayMeshVertex {
  float x = 0.0f;
  float y = 0.0f;
  StimulusCameraOverlayColor color;
};

struct StimulusCameraOverlayMesh {
  std::vector<StimulusCameraOverlayMeshVertex> triangle_vertices;
  size_t primitive_count = 0;

  size_t triangleCount() const { return triangle_vertices.size() / 3; }
};

std::string_view stimulusCameraOverlayFrameAvailabilityName(
    StimulusCameraOverlayFrameAvailability availability);
std::string_view stimulusCameraOverlaySceneStatusName(
    StimulusCameraOverlaySceneStatus status);
std::string_view stimulusCameraOverlaySceneLayerName(
    StimulusCameraOverlaySceneLayer layer);

std::string stimulusCameraOverlayEventText(
    const StimulusCameraOverlayFrameSample& frame,
    const StimulusCameraOverlayControls& controls = {});

StimulusCameraOverlayScene buildStimulusCameraOverlayScene(
    const StimulusCameraOverlayFrameSample& frame,
    const StimulusCameraOverlayViewport& viewport,
    const StimulusCameraOverlayTextMetrics& event_text_metrics,
    const StimulusCameraOverlayControls& controls = {});

// Canonicalizes panel-local semantics so equivalent scenes compare across
// differently shaped camera viewports.
std::string stimulusCameraOverlaySceneSemanticSignature(
    const StimulusCameraOverlayScene& scene);

StimulusCameraOverlayMesh tessellateStimulusCameraOverlayScene(
    const StimulusCameraOverlayScene& scene,
    StimulusCameraOverlayPoint display_origin = {},
    double scale_x = 1.0,
    double scale_y = 1.0,
    size_t rounded_segment_count = 24);

}  // namespace crimson::stimulus
