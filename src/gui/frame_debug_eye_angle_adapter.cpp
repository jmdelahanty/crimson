#include "gui/frame_debug_eye_angle_adapter.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace {

uint64_t rowSelectionKey(size_t row) {
  if (static_cast<uint64_t>(row) == std::numeric_limits<uint64_t>::max()) {
    return 0;
  }
  return static_cast<uint64_t>(row) + 1;
}

std::string fieldLabel(const std::string &display_name,
                       const std::string &name) {
  return display_name.empty() ? name : display_name;
}

} // namespace

crimson::gui::EyeAngleInspectPresentation
makeFrameDebugEyeAngleInspectPresentation(
    const FrameDebugWindowContext &context) {
  crimson::gui::EyeAngleInspectPresentation presentation;
  presentation.presentation_label = "Current-frame presentation";
  presentation.unavailable_message =
      "Eye-angle analysis data unavailable for current dataset.";
  presentation.available = context.zarr_loader.hasEyeAngleAnalysisData();
  if (!presentation.available) {
    return presentation;
  }

  const auto &eye = context.zarr_loader.getEyeAngleAnalysisData();
  presentation.surface_label = "Legacy eye-angle analysis";
  presentation.run_name = eye.run_name;
  presentation.default_representation_key = eye.default_representation;
  presentation.representations.reserve(eye.representations.size());
  for (const auto &source : eye.representations) {
    presentation.representations.push_back(
        {source.key,
         source.display_name.empty() ? source.key : source.display_name,
         source.role, source.axis});
  }
  if (!eye.schema_id.empty()) {
    presentation.detail_lines.push_back("Schema: " + eye.schema_id + " v" +
                                        std::to_string(eye.schema_version));
  }
  if (!eye.method.empty()) {
    presentation.detail_lines.push_back("Method: " + eye.method +
                                        (eye.method_version.empty()
                                             ? std::string{}
                                             : " " + eye.method_version));
  }
  if (!eye.source_geometry_kind.empty()) {
    presentation.detail_lines.push_back("Source geometry: " +
                                        eye.source_geometry_kind);
  }
  if (!eye.source_eye_geometry_run.empty()) {
    presentation.detail_lines.push_back("Eye geometry run: " +
                                        eye.source_eye_geometry_run);
  } else if (!eye.source_subject_shape_run.empty()) {
    presentation.detail_lines.push_back("Subject shape run: " +
                                        eye.source_subject_shape_run);
  }
  presentation.detail_lines.push_back(
      "Rows: " + std::to_string(eye.row_count) +
      " | scalar fields: " + std::to_string(eye.scalar_fields.size()) +
      " | vector fields: " + std::to_string(eye.vector_fields.size()));
  if (eye.variant_schema_inferred) {
    presentation.detail_lines.push_back("Representation metadata inferred");
  }
  presentation.warning = eye.warning;
  presentation.frame_ready = true;
  presentation.camera_frame = context.current_frame_num;

  auto append_observation = [&](size_t row) {
    crimson::gui::EyeAngleInspectObservation observation;
    observation.row_selection_key = rowSelectionKey(row);
    observation.selectable = observation.row_selection_key != 0;
    observation.source_row = row;
    observation.source_row_valid = true;
    observation.left_valid_known = !eye.roi_valid_left.empty();
    observation.left_valid =
        eye.roi_valid_left.empty() ||
        (row < eye.roi_valid_left.size() && eye.roi_valid_left[row] != 0);
    observation.right_valid_known = !eye.roi_valid_right.empty();
    observation.right_valid =
        eye.roi_valid_right.empty() ||
        (row < eye.roi_valid_right.size() && eye.roi_valid_right[row] != 0);
    observation.frame_valid_known = !eye.roi_valid_frame.empty();
    observation.frame_valid =
        eye.roi_valid_frame.empty() ||
        (row < eye.roi_valid_frame.size() && eye.roi_valid_frame[row] != 0);
    observation.marginal_known = !eye.roi_major_axis_marginal.empty() ||
                                 !eye.roi_left_major_axis_marginal.empty() ||
                                 !eye.roi_right_major_axis_marginal.empty();
    observation.marginal = (row < eye.roi_major_axis_marginal.size() &&
                            eye.roi_major_axis_marginal[row] != 0) ||
                           (row < eye.roi_left_major_axis_marginal.size() &&
                            eye.roi_left_major_axis_marginal[row] != 0) ||
                           (row < eye.roi_right_major_axis_marginal.size() &&
                            eye.roi_right_major_axis_marginal[row] != 0);
    if (row < eye.roi_reason_labels.size()) {
      observation.reason = eye.roi_reason_labels[row];
    }

    observation.fields.reserve(eye.scalar_fields.size() +
                               eye.vector_fields.size());
    for (const auto &source : eye.scalar_fields) {
      if (!source.has_roi) {
        continue;
      }
      crimson::gui::EyeAngleInspectField field;
      field.representation_key = source.representation_key;
      field.label = fieldLabel(source.display_name, source.name);
      field.units = source.units;
      if (row < source.roi_values.size()) {
        field.value_x = source.roi_values[row];
        field.valid = std::isfinite(field.value_x);
      }
      observation.fields.push_back(std::move(field));
    }
    for (const auto &source : eye.vector_fields) {
      if (!source.has_roi) {
        continue;
      }
      crimson::gui::EyeAngleInspectField field;
      field.representation_key = source.representation_key;
      field.label = fieldLabel(source.display_name, source.name);
      field.units = source.units;
      field.kind = crimson::gui::EyeAngleInspectFieldKind::Vector2;
      if (row < source.roi_values.size()) {
        field.value_x = source.roi_values[row][0];
        field.value_y = source.roi_values[row][1];
        field.valid =
            std::isfinite(field.value_x) && std::isfinite(field.value_y);
      }
      observation.fields.push_back(std::move(field));
    }
    presentation.observations.push_back(std::move(observation));
  };

  if (eye.row_to_frame_nondecreasing) {
    const auto range =
        std::equal_range(eye.row_to_frame.begin(), eye.row_to_frame.end(),
                         context.current_frame_num);
    for (auto current = range.first; current != range.second; ++current) {
      append_observation(
          static_cast<size_t>(current - eye.row_to_frame.begin()));
    }
  } else {
    for (size_t row = 0; row < eye.row_to_frame.size(); ++row) {
      if (eye.row_to_frame[row] == context.current_frame_num) {
        append_observation(row);
      }
    }
  }
  return presentation;
}
