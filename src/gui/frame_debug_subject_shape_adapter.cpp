#include "gui/frame_debug_subject_shape_adapter.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace {

uint64_t rowSelectionKey(int32_t row) {
  if (row < 0) {
    return 0;
  }
  return static_cast<uint64_t>(row) + 1;
}

crimson::gui::SubjectShapeInspectFeature feature(std::string key,
                                                 std::string label, bool valid,
                                                 size_t point_count = 1) {
  crimson::gui::SubjectShapeInspectFeature result;
  result.key = std::move(key);
  result.label = std::move(label);
  result.valid = valid;
  result.valid_known = true;
  result.point_count = point_count;
  return result;
}

crimson::gui::SubjectShapeInspectFeature
presentFeature(std::string key, std::string label, bool present,
               size_t point_count = 1) {
  crimson::gui::SubjectShapeInspectFeature result;
  result.key = std::move(key);
  result.label = std::move(label);
  result.point_count = present ? point_count : 0;
  return result;
}

bool finitePoint(const std::array<float, 2> &point) {
  return std::isfinite(point[0]) && std::isfinite(point[1]);
}

void appendReason(std::vector<std::string> *reasons,
                  const std::string &reason) {
  if (!reason.empty() &&
      std::find(reasons->begin(), reasons->end(), reason) == reasons->end()) {
    reasons->push_back(reason);
  }
}

} // namespace

crimson::gui::SubjectShapeInspectPresentation
makeFrameDebugSubjectShapeInspectPresentation(
    const FrameDebugWindowContext &context) {
  crimson::gui::SubjectShapeInspectPresentation presentation;
  presentation.presentation_label = "Subject shape presentation";
  presentation.unavailable_message =
      "Subject-shape arrays unavailable for current dataset.";
  presentation.available = context.zarr_loader.hasSubjectShapeData();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label = "Legacy subject-shape analysis";
  presentation.run_name = context.zarr_loader.getSubjectShapeRunName();
  presentation.detail_lines.push_back(
      "Schema version: " +
      std::to_string(context.zarr_loader.getSubjectShapeSchemaVersion()));
  if (!context.zarr_loader.getSubjectShapeMethod().empty()) {
    presentation.detail_lines.push_back(
        "Method: " + context.zarr_loader.getSubjectShapeMethod() + " v" +
        std::to_string(context.zarr_loader.getSubjectShapeMethodVersion()));
  }
  if (!context.zarr_loader.getSubjectShapeSourceRefinedSubjectMasksRun()
           .empty()) {
    presentation.detail_lines.push_back(
        "Refined masks: " +
        context.zarr_loader.getSubjectShapeSourceRefinedSubjectMasksRun());
  }
  if (!context.zarr_loader.getSubjectShapeHeadEndpointSemantics().empty()) {
    presentation.detail_lines.push_back(
        "Head endpoint: " +
        context.zarr_loader.getSubjectShapeHeadEndpointSemantics());
  }
  presentation.warning = context.zarr_loader.getSubjectShapeWarning();
  presentation.frame_ready = context.detection_details != nullptr &&
                             context.detection_details->includes_subject_shapes;
  if (!presentation.frame_ready) {
    return presentation;
  }

  presentation.camera_frame = context.current_frame_num;
  const auto &source_shapes = context.detection_details->subject_shapes;
  presentation.observations.reserve(source_shapes.size());
  for (size_t index = 0; index < source_shapes.size(); ++index) {
    const auto &source = source_shapes[index];
    crimson::gui::SubjectShapeInspectObservation observation;
    observation.row_selection_key = rowSelectionKey(source.roi_index);
    observation.selectable = observation.row_selection_key != 0;
    if (source.roi_index >= 0) {
      observation.source_row = static_cast<size_t>(source.roi_index);
      observation.source_row_valid = true;
    }
    observation.detection_index = static_cast<int64_t>(index);
    observation.detection_index_valid = true;
    observation.roi_width = source.roi_width;
    observation.roi_height = source.roi_height;
    observation.roi_valid = std::isfinite(source.roi_width) &&
                            std::isfinite(source.roi_height) &&
                            source.roi_width > 0.0f && source.roi_height > 0.0f;
    observation.features = {
        feature("body_frame", "Body frame", source.body_frame_valid, 3),
        feature("snout_tip", "Snout tip", source.snout_tip_valid),
        feature("tail_base", "Tail base", source.tail_base_valid),
        presentFeature("tail_tip", "Tail tip", finitePoint(source.tail_tip_xy)),
        feature("caudal_anchor", "Caudal anchor", source.caudal_contour_valid),
        feature("centerline", "Centerline", source.centerline_valid,
                source.centerline_xy.size()),
        feature("centerline_reaches_snout", "Centerline reaches snout",
                source.centerline_reaches_snout, 0),
        feature("bspline", "B-spline", source.bspline_valid,
                source.bspline_sample_xy.size()),
        presentFeature("bspline_controls", "B-spline controls",
                       !source.bspline_control_points_xy.empty(),
                       source.bspline_control_points_xy.size()),
        feature("tail_samples", "Tail samples", source.tail_sample_valid,
                source.tail_sample_xy.size()),
        presentFeature("tail_normals", "Tail normals",
                       !source.tail_normal_xy.empty(),
                       source.tail_normal_xy.size()),
    };
    appendReason(&observation.reasons, source.body_frame_failure_reason);
    appendReason(&observation.reasons, source.snout_tip_failure_reason);
    appendReason(&observation.reasons, source.centerline_failure_reason);
    appendReason(&observation.reasons, source.bspline_failure_reason);
    appendReason(&observation.reasons, source.tail_sample_failure_reason);
    presentation.observations.push_back(std::move(observation));
  }
  return presentation;
}
