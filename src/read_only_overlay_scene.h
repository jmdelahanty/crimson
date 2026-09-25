#pragma once

#include "overlay_scene_contract.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace crimson::overlay {

struct Color {
  float red = 1.0f;
  float green = 1.0f;
  float blue = 1.0f;
  float alpha = 1.0f;

  bool valid() const;
};

enum class BoxProvenance : uint8_t {
  Clean,
  Interpolated,
  Manual,
};

struct DetectionBoxInput {
  Rect rect;
  int class_id = 0;
  BoxProvenance provenance = BoxProvenance::Clean;
  bool selected = false;
  bool added = false;
  bool frame_modified = false;
};

struct DetectionOverlayInput {
  uint64_t instance_key = 0;
  int64_t refined_row_id = -1;
  uint8_t source_kind_code = 0;
  std::optional<DetectionBoxInput> box;
  std::vector<Point> keypoints;
  std::optional<Point> heading_origin;
  std::optional<double> heading_degrees;
  bool heading_valid = false;
  bool detection_interpolated = false;
  bool suppress_keypoint_markers = false;
  bool refined_keypoints = false;
  bool keypoint_usable = true;
  bool keypoint_detection_interpolated = false;
  bool keypoint_flip_corrected = false;
  bool source_success = false;
  bool refined_success = false;
  bool confidence_valid = false;
  bool geometry_valid = false;
  uint8_t review_state_code = 0;
  uint16_t reason_code = 0;
  std::vector<uint8_t> keypoint_edit_flags;
  bool heading_from_body_frame = false;
  bool instance_key_valid = false;
};

struct SubjectMaskComponentInput {
  std::string cache_namespace;
  std::string label;
  int64_t source_crop_row_id = -1;
  size_t channel_index = 0;
  Rect source_rect;
  size_t mask_width = 0;
  size_t mask_height = 0;
  std::shared_ptr<const std::vector<uint8_t>> mask;
  std::vector<Point> contour;
  uint64_t instance_key = 0;
  bool instance_key_valid = false;
};

struct SubjectShapeInput {
  size_t shape_row = 0;
  int64_t detection_index = -1;
  int64_t source_refined_row_id = -1;
  int64_t source_crop_row_id = -1;
  Rect source_rect;
  double coordinate_width = 0.0;
  double coordinate_height = 0.0;

  bool body_frame_valid = false;
  Point body_origin;
  Point body_forward_axis;
  Point body_left_axis;
  bool snout_tip_valid = false;
  Point snout_tip;
  bool tail_base_valid = false;
  Point tail_base;
  Point tail_tip;
  bool caudal_anchor_valid = false;
  Point caudal_anchor;
  bool centerline_valid = false;
  bool centerline_reaches_snout = false;
  std::vector<Point> centerline;
  bool bspline_valid = false;
  std::vector<Point> bspline_sample;
  std::vector<Point> bspline_control_points;
  bool tail_sample_valid = false;
  std::vector<Point> tail_samples;
  std::vector<Point> tail_normals;
  uint64_t instance_key = 0;
  bool instance_key_valid = false;
};

struct EyeAxisInput {
  bool valid = false;
  Point start;
  Point end;
};

struct EyeInput {
  bool valid = false;
  EyeAxisInput major_axis;
  EyeAxisInput minor_axis;
  bool gaze_valid = false;
  Point gaze;
  bool signed_angle_valid = false;
  double signed_angle_degrees = 0.0;
  bool eye_frame_angle_valid = false;
  double eye_frame_angle_degrees = 0.0;
};

struct EyeGeometryInput {
  size_t eye_row = 0;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  Rect source_rect;
  double coordinate_width = 0.0;
  double coordinate_height = 0.0;
  bool frame_valid = false;
  bool body_frame_valid = false;
  Point body_origin;
  Point body_forward_axis;
  Point body_left_axis;
  std::array<EyeInput, 2> eyes;
  bool vergence_valid = false;
  double vergence_degrees = 0.0;
};

struct ReadOnlyOverlayInput {
  FrameIdentity identity;
  coordinates::CoordinateSpace presentation_space =
      coordinates::kSourceCameraContinuousPixels;
  double source_width = 0.0;
  double source_height = 0.0;
  std::vector<std::string> keypoint_labels;
  std::vector<std::array<size_t, 2>> skeleton_edges;
  std::vector<DetectionOverlayInput> detections;
  std::vector<SubjectMaskComponentInput> subject_masks;
  std::vector<EyeGeometryInput> eye_geometry;
  std::vector<SubjectShapeInput> subject_shapes;
  bool show_boxes = true;
  bool show_headings = true;
  bool show_subject_mask_fills = true;
  bool show_subject_mask_contours = true;
  bool show_subject_body_mask = true;
  bool show_eye_left_mask = true;
  bool show_eye_right_mask = true;
  bool show_swim_bladder_mask = true;
  bool independent_mask_contours = false;
  bool show_subject_body_contour = false;
  bool show_eye_left_contour = false;
  bool show_eye_right_contour = false;
  bool show_swim_bladder_contour = false;
  bool show_eye_geometry = true;
  bool show_eye_direction_beams = true;
  bool show_eye_gaze_rays = true;
  bool show_eye_angle_arcs = true;
  bool show_eye_angle_labels = true;
  bool show_subject_shape = true;
  bool show_subject_shape_body_axes = false;
  bool show_subject_shape_snout_tip = true;
  bool show_subject_shape_caudal_anchor = true;
  bool show_subject_shape_tail_base = true;
  bool show_subject_shape_tail_tip = true;
  bool show_subject_shape_centerline = true;
  bool show_subject_shape_bspline = true;
  bool show_subject_shape_bspline_debug_points = false;
  bool show_subject_shape_bspline_control_points = false;
  bool show_subject_shape_tail_samples = false;
  bool show_subject_shape_tail_normals = false;
  bool show_keypoints = true;
};

enum class MarkerShape : uint8_t {
  Circle,
  Square,
  Diamond,
  Cross,
  Plus,
  TriangleUp,
  TriangleDown,
};

enum class PrimitiveType : uint8_t {
  Polyline,
  Marker,
  Arrow,
  Polygon,
};

struct Primitive {
  PrimitiveType type = PrimitiveType::Polyline;
  CameraOverlayLayer layer = CameraOverlayLayer::BoundingBoxes;
  std::vector<Point> points;
  Color stroke;
  Color fill;
  Color outline;
  double stroke_width_px = 1.0;
  double marker_size_px = 0.0;
  double outline_width_px = 0.0;
  double arrow_head_size_px = 0.0;
  MarkerShape marker_shape = MarkerShape::Circle;
  uint64_t instance_key = 0;
  int64_t refined_row_id = -1;
  uint8_t source_kind_code = 0;
  std::string label;
};

struct RasterMask {
  CameraOverlayLayer layer = CameraOverlayLayer::SubjectMasks;
  Rect source_rect;
  size_t width = 0;
  size_t height = 0;
  std::shared_ptr<const std::vector<uint8_t>> alpha;
  Color color;
  std::string cache_key;
  std::string label;
};

struct TextAnnotation {
  CameraOverlayLayer layer = CameraOverlayLayer::SubjectMasks;
  Point source_anchor;
  Point offset_px;
  Color text;
  Color background;
  Color border;
  double font_scale = 1.0;
  bool centered = true;
  std::string content;
  std::string label;
};

struct ScreenTextAnnotation {
  CameraOverlayLayer layer = CameraOverlayLayer::SubjectMasks;
  Point anchor;
  Rect clip_rect;
  Color text;
  Color background;
  Color border;
  double font_scale = 1.0;
  bool centered = true;
  std::string content;
  std::string label;
};

enum class ReadOnlyOverlayBuildStatus : uint8_t {
  Ready,
  InvalidIdentity,
  InvalidCoordinateSpace,
  InvalidDimensions,
};

struct ReadOnlyOverlayScene {
  ReadOnlyOverlayBuildStatus status =
      ReadOnlyOverlayBuildStatus::InvalidIdentity;
  FrameIdentity identity;
  coordinates::CoordinateSpace presentation_space =
      coordinates::kSourceCameraContinuousPixels;
  double source_width = 0.0;
  double source_height = 0.0;
  std::vector<RasterMask> raster_masks;
  std::vector<Primitive> primitives;
  std::vector<TextAnnotation> text_annotations;

  bool ready() const;
  size_t count(PrimitiveType type) const;
  size_t count(CameraOverlayLayer layer) const;
  size_t rasterCount(CameraOverlayLayer layer) const;
  size_t textCount(CameraOverlayLayer layer) const;
};

ReadOnlyOverlayScene
buildReadOnlyOverlayScene(const ReadOnlyOverlayInput &input);

Color keypointColor(const std::string &label, size_t keypoint_index);
MarkerShape keypointMarkerShape(const std::string &label,
                                size_t keypoint_index);
double keypointMarkerSizePx(const std::string &label);
Color subjectMaskColor(const std::string &label);
int subjectMaskComponentRank(const std::string &label);

struct ScreenVertex {
  float x = 0.0f;
  float y = 0.0f;
  Color color;
};

struct ScreenMesh {
  std::vector<ScreenVertex> triangle_vertices;
  size_t primitive_count = 0;

  size_t triangleCount() const { return triangle_vertices.size() / 3; }
};

ScreenMesh
tessellateReadOnlyOverlayScene(const ReadOnlyOverlayScene &scene,
                               const SourceViewportTransform &transform,
                               size_t circle_segment_count = 24);

ScreenMesh tessellateReadOnlyOverlaySceneLayer(
    const ReadOnlyOverlayScene &scene, const SourceViewportTransform &transform,
    CameraOverlayLayer layer, size_t circle_segment_count = 24);

std::vector<ScreenTextAnnotation>
layoutReadOnlyOverlayText(const ReadOnlyOverlayScene &scene,
                          const SourceViewportTransform &transform);

} // namespace crimson::overlay
