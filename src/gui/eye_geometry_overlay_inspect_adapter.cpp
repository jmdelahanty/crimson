#include "gui/eye_geometry_overlay_inspect_adapter.h"

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

std::string statusWarning(const zarr::EyeGeometryOverlayResolution &frame) {
  if (!frame.error.empty()) {
    return frame.error;
  }
  switch (frame.status) {
  case zarr::EyeGeometryOverlayStatus::Mapped:
  case zarr::EyeGeometryOverlayStatus::Missing:
    return {};
  case zarr::EyeGeometryOverlayStatus::OutOfRange:
    return "Presented frame is outside the eye-geometry frame domain.";
  case zarr::EyeGeometryOverlayStatus::InvalidDimensions:
    return "Source-camera dimensions are invalid.";
  case zarr::EyeGeometryOverlayStatus::ReadFailed:
    return "Eye-geometry data could not be read.";
  }
  return "Eye-geometry resolution failed.";
}

EyeAngleInspectField scalarField(std::string representation_key,
                                 std::string label, double value, bool valid) {
  EyeAngleInspectField field;
  field.representation_key = std::move(representation_key);
  field.label = std::move(label);
  field.units = "deg";
  field.value_x = value;
  field.valid = valid && std::isfinite(value);
  return field;
}

EyeAngleInspectField vectorField(std::string label,
                                 const zarr::EyeGeometryPoint &value,
                                 bool valid) {
  EyeAngleInspectField field;
  field.representation_key = "gaze";
  field.label = std::move(label);
  field.units = "unit vector";
  field.kind = EyeAngleInspectFieldKind::Vector2;
  field.value_x = value.x;
  field.value_y = value.y;
  field.valid = valid && std::isfinite(value.x) && std::isfinite(value.y);
  return field;
}

} // namespace

EyeAngleInspectPresentation makeEyeGeometryOverlayInspectPresentation(
    const zarr::EyeGeometryOverlayDescriptor *descriptor,
    const zarr::EyeGeometryOverlayResolution *frame, int64_t presented_frame) {
  EyeAngleInspectPresentation presentation;
  presentation.available =
      descriptor != nullptr && !descriptor->run_name.empty();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label = "Eye-geometry overlay";
  presentation.run_name = descriptor->run_name;
  presentation.default_representation_key = "eye_frame";
  presentation.representations = {
      {"eye_frame", "Eye frame", "body-relative eye angle", "eye frame"},
      {"signed", "Signed angle", "signed eye angle", "image/ROI"},
      {"gaze", "Gaze vector", "eye direction", "ROI x/y axes"},
  };
  if (!descriptor->schema_id.empty()) {
    presentation.detail_lines.push_back(
        "Schema: " + descriptor->schema_id + " v" +
        std::to_string(descriptor->schema_version));
  }
  if (!descriptor->method.empty()) {
    presentation.detail_lines.push_back(
        "Method: " + descriptor->method +
        (descriptor->method_version.empty()
             ? std::string{}
             : " " + descriptor->method_version));
  }
  if (!descriptor->source_refined_subject_masks_run.empty()) {
    presentation.detail_lines.push_back(
        "Refined masks: " + descriptor->source_refined_subject_masks_run);
  }
  if (!descriptor->source_crop_run.empty()) {
    presentation.detail_lines.push_back("Crop run: " +
                                        descriptor->source_crop_run);
  }

  presentation.frame_ready =
      frame != nullptr && frame->camera_frame == presented_frame;
  if (!presentation.frame_ready) {
    return presentation;
  }

  presentation.camera_frame = frame->camera_frame;
  presentation.warning = statusWarning(*frame);
  if (frame->status != zarr::EyeGeometryOverlayStatus::Mapped) {
    return presentation;
  }

  presentation.observations.reserve(frame->detections.size());
  for (const zarr::EyeGeometryOverlayDetection &detection : frame->detections) {
    EyeAngleInspectObservation observation;
    observation.row_selection_key = rowSelectionKey(detection.eye_row);
    observation.selectable = observation.row_selection_key != 0;
    observation.source_row = detection.eye_row;
    observation.source_row_valid = true;
    observation.detection_index = detection.detection_index;
    observation.detection_index_valid = detection.detection_index >= 0;
    observation.source_crop_row_id = detection.source_crop_row_id;
    observation.source_crop_row_id_valid = detection.source_crop_row_id >= 0;
    observation.frame_valid = detection.frame_valid;
    observation.frame_valid_known = true;
    observation.left_valid = detection.eyes[0].valid;
    observation.left_valid_known = zarr::EyeGeometryFieldsCover(
        frame->loaded_fields, zarr::EyeGeometryFields::LeftGeometry);
    observation.right_valid = detection.eyes[1].valid;
    observation.right_valid_known = zarr::EyeGeometryFieldsCover(
        frame->loaded_fields, zarr::EyeGeometryFields::RightGeometry);
    observation.fields = {
        scalarField("eye_frame", "Left eye-frame angle",
                    detection.eyes[0].eye_frame_angle_degrees,
                    detection.eyes[0].valid &&
                        detection.eyes[0].eye_frame_angle_valid),
        scalarField("eye_frame", "Right eye-frame angle",
                    detection.eyes[1].eye_frame_angle_degrees,
                    detection.eyes[1].valid &&
                        detection.eyes[1].eye_frame_angle_valid),
        scalarField("eye_frame", "Vergence", detection.vergence_degrees,
                    detection.vergence_valid),
        scalarField("signed", "Left signed angle",
                    detection.eyes[0].signed_angle_degrees,
                    detection.eyes[0].valid &&
                        detection.eyes[0].signed_angle_valid),
        scalarField("signed", "Right signed angle",
                    detection.eyes[1].signed_angle_degrees,
                    detection.eyes[1].valid &&
                        detection.eyes[1].signed_angle_valid),
        vectorField("Left gaze", detection.eyes[0].gaze,
                    detection.eyes[0].valid && detection.eyes[0].gaze_valid),
        vectorField("Right gaze", detection.eyes[1].gaze,
                    detection.eyes[1].valid && detection.eyes[1].gaze_valid),
    };
    const zarr::EyeGeometryFieldMask fields[] = {
        zarr::EyeGeometryFields::LeftAngle, zarr::EyeGeometryFields::RightAngle,
        zarr::EyeGeometryFields::Vergence, zarr::EyeGeometryFields::LeftSigned,
        zarr::EyeGeometryFields::RightSigned, zarr::EyeGeometryFields::LeftGaze,
        zarr::EyeGeometryFields::RightGaze};
    for (size_t i = 0; i < observation.fields.size(); ++i) {
      observation.fields[i].loaded =
          zarr::EyeGeometryFieldsCover(frame->loaded_fields, fields[i]);
      observation.fields[i].valid &= observation.fields[i].loaded;
    }
    presentation.observations.push_back(std::move(observation));
  }
  return presentation;
}

} // namespace crimson::gui
