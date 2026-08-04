#include "gui/subject_shape_overlay_inspect_adapter.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace crimson::gui {
namespace {

uint64_t rowSelectionKey(size_t row) {
  if (static_cast<uint64_t>(row) == std::numeric_limits<uint64_t>::max()) {
    return 0;
  }
  return static_cast<uint64_t>(row) + 1;
}

SubjectShapeInspectFeature feature(std::string key, std::string label,
                                   bool valid, size_t point_count = 1) {
  SubjectShapeInspectFeature result;
  result.key = std::move(key);
  result.label = std::move(label);
  result.valid = valid;
  result.valid_known = true;
  result.point_count = point_count;
  return result;
}

SubjectShapeInspectFeature presentFeature(std::string key, std::string label,
                                          bool present,
                                          size_t point_count = 1) {
  SubjectShapeInspectFeature result;
  result.key = std::move(key);
  result.label = std::move(label);
  result.point_count = present ? point_count : 0;
  return result;
}

bool finitePoint(const zarr::SubjectShapeOverlayPoint &point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

std::string statusWarning(const zarr::SubjectShapeOverlayResolution &frame) {
  if (!frame.error.empty()) {
    return frame.error;
  }
  switch (frame.status) {
  case zarr::SubjectShapeOverlayStatus::Mapped:
  case zarr::SubjectShapeOverlayStatus::Missing:
    return {};
  case zarr::SubjectShapeOverlayStatus::OutOfRange:
    return "Presented frame is outside the subject-shape frame domain.";
  case zarr::SubjectShapeOverlayStatus::InvalidDimensions:
    return "Source-camera dimensions are invalid.";
  case zarr::SubjectShapeOverlayStatus::ReadFailed:
    return "Subject-shape data could not be read.";
  }
  return "Subject-shape resolution failed.";
}

} // namespace

SubjectShapeInspectPresentation makeSubjectShapeOverlayInspectPresentation(
    const zarr::SubjectShapeOverlayDescriptor *descriptor,
    const zarr::SubjectShapeOverlayResolution *frame, int64_t presented_frame) {
  SubjectShapeInspectPresentation presentation;
  presentation.available =
      descriptor != nullptr && !descriptor->run_name.empty();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label = "Subject-shape overlay";
  presentation.run_name = descriptor->run_name;
  if (!descriptor->schema_id.empty()) {
    presentation.detail_lines.push_back(
        "Schema: " + descriptor->schema_id + " v" +
        std::to_string(descriptor->schema_version));
  }
  if (!descriptor->method.empty()) {
    presentation.detail_lines.push_back(
        "Method: " + descriptor->method + " v" +
        std::to_string(descriptor->method_version));
  }
  if (!descriptor->source_refined_subject_masks_run.empty()) {
    presentation.detail_lines.push_back(
        "Refined masks: " + descriptor->source_refined_subject_masks_run);
  }
  if (!descriptor->source_crop_run.empty()) {
    presentation.detail_lines.push_back("Crop run: " +
                                        descriptor->source_crop_run);
  }
  if (!descriptor->head_endpoint_semantics.empty()) {
    presentation.detail_lines.push_back("Head endpoint: " +
                                        descriptor->head_endpoint_semantics);
  }

  presentation.frame_ready =
      frame != nullptr && frame->camera_frame == presented_frame;
  if (!presentation.frame_ready) {
    return presentation;
  }
  presentation.camera_frame = frame->camera_frame;
  presentation.warning = statusWarning(*frame);
  if (frame->status != zarr::SubjectShapeOverlayStatus::Mapped) {
    return presentation;
  }

  presentation.observations.reserve(frame->detections.size());
  for (const auto &detection : frame->detections) {
    const auto &geometry = detection.geometry;
    SubjectShapeInspectObservation observation;
    observation.row_selection_key = rowSelectionKey(detection.shape_row);
    observation.selectable = observation.row_selection_key != 0;
    observation.source_row = detection.shape_row;
    observation.source_row_valid = true;
    observation.detection_index = detection.detection_index;
    observation.detection_index_valid = detection.detection_index >= 0;
    observation.source_refined_row_id = detection.source_refined_row_id;
    observation.source_refined_row_id_valid =
        detection.source_refined_row_id >= 0;
    observation.source_crop_row_id = detection.source_crop_row_id;
    observation.source_crop_row_id_valid = detection.source_crop_row_id >= 0;
    observation.roi_width = detection.roi_width;
    observation.roi_height = detection.roi_height;
    observation.roi_valid = std::isfinite(detection.roi_width) &&
                            std::isfinite(detection.roi_height) &&
                            detection.roi_width > 0.0 &&
                            detection.roi_height > 0.0;
    observation.features = {
        feature("body_frame", "Body frame", geometry.body_frame_valid, 3),
        feature("snout_tip", "Snout tip", geometry.snout_tip_valid),
        feature("tail_base", "Tail base", geometry.tail_base_valid),
        presentFeature("tail_tip", "Tail tip", finitePoint(geometry.tail_tip)),
        feature("caudal_anchor", "Caudal anchor", geometry.caudal_anchor_valid),
        feature("centerline", "Centerline", geometry.centerline_valid,
                geometry.centerline.size()),
        feature("centerline_reaches_snout", "Centerline reaches snout",
                geometry.centerline_reaches_snout, 0),
        feature("bspline", "B-spline", geometry.bspline_valid,
                geometry.bspline_sample.size()),
        presentFeature("bspline_controls", "B-spline controls",
                       !geometry.bspline_control_points.empty(),
                       geometry.bspline_control_points.size()),
        feature("tail_samples", "Tail samples", geometry.tail_sample_valid,
                geometry.tail_samples.size()),
        presentFeature("tail_normals", "Tail normals",
                       !geometry.tail_normals.empty(),
                       geometry.tail_normals.size()),
    };
    presentation.observations.push_back(std::move(observation));
  }
  return presentation;
}

} // namespace crimson::gui
