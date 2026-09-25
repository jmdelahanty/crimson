#include "gui/frame_debug_subject_mask_adapter.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace {

bool hasAngleLabels(const ZarrDetectionLoader::FrameDetections::EyeMask &mask) {
  return (mask.has_eye_frame_angles && (mask.eye_frame_angle_valid[0] != 0 ||
                                        mask.eye_frame_angle_valid[1] != 0 ||
                                        mask.eye_frame_vergence_valid != 0)) ||
         (mask.has_eye_angles &&
          (mask.feret_angle_valid[0] != 0 || mask.feret_angle_valid[1] != 0));
}

crimson::gui::SubjectMaskInspectComponent
makeLegacyEyeComponent(const std::string &label, size_t channel_index,
                       const std::vector<uint32_t> &pixels, bool mask_valid) {
  crimson::gui::SubjectMaskInspectComponent component;
  component.label = label;
  component.channel_index = channel_index;
  component.channel_index_valid =
      channel_index != std::numeric_limits<size_t>::max();
  component.present = mask_valid && !pixels.empty();
  component.pixel_payload_available = component.present;
  component.pixel_payload_value_count = pixels.size();
  return component;
}

} // namespace

crimson::gui::SubjectMaskInspectPresentation
makeFrameDebugSubjectMaskInspectPresentation(
    const FrameDebugWindowContext &context) {
  crimson::gui::SubjectMaskInspectPresentation presentation;
  presentation.presentation_label = "Current-frame presentation";
  presentation.unavailable_message =
      "Subject-mask arrays unavailable for current dataset.";
  presentation.available = context.zarr_loader.hasEyeMasks();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label =
      !context.zarr_loader.getEyeMaskSourceLabel().empty()
          ? context.zarr_loader.getEyeMaskSourceLabel()
      : context.zarr_loader.eyeMasksUseRefinedSubjectMasks()
          ? "Legacy refined subject masks"
          : "Legacy eye masks";
  presentation.run_name = context.zarr_loader.getEyeMaskRunName();
  if (!context.zarr_loader.getEyeMaskSourcePath().empty()) {
    presentation.detail_lines.push_back(
        "Dataset: " + context.zarr_loader.getEyeMaskSourcePath());
  }
  if (context.zarr_loader.eyeMasksUseRefinedSubjectMasks()) {
    const auto &labels = context.zarr_loader.getEyeMaskChannelLabels();
    const auto &channels = context.zarr_loader.getEyeMaskChannelIndices();
    const auto channel_name = [](size_t channel) {
      return channel == std::numeric_limits<size_t>::max()
                 ? std::string("unavailable")
                 : std::to_string(channel);
    };
    presentation.detail_lines.push_back(
        "Channels: " + labels[0] + "=" + channel_name(channels[0]) + ", " +
        labels[1] + "=" + channel_name(channels[1]));
  }
  if (context.zarr_loader.hasEyeAngleData()) {
    presentation.detail_lines.push_back(
        "Angle run: " + context.zarr_loader.getEyeAngleRunName());
  }
  presentation.warning = context.zarr_loader.getEyeMaskWarning();

  presentation.frame_ready = context.detection_details != nullptr &&
                             context.detection_details->includes_eye_masks;
  if (!presentation.frame_ready) {
    return presentation;
  }

  presentation.camera_frame = context.current_frame_num;
  const auto &masks = context.detection_details->eye_masks;
  presentation.observations.reserve(masks.size());
  size_t valid_masks = 0;
  size_t masks_with_axes = 0;
  size_t masks_with_angle_labels = 0;
  size_t masks_with_subject_body = 0;
  size_t masks_with_swim_bladder = 0;
  size_t masks_with_component_contours = 0;
  for (const auto &mask : masks) {
    crimson::gui::SubjectMaskInspectObservation observation;
    observation.valid = mask.valid;
    observation.source_crop_row_id = mask.roi_index;
    observation.source_crop_row_id_valid = mask.roi_index >= 0;
    observation.roi_width = mask.roi_width;
    observation.roi_height = mask.roi_height;
    observation.roi_valid = std::isfinite(mask.roi_width) &&
                            std::isfinite(mask.roi_height) &&
                            mask.roi_width > 0.0f && mask.roi_height > 0.0f;
    observation.axes_available = mask.has_feret_axes;
    observation.angle_labels_available = hasAngleLabels(mask);
    valid_masks += observation.valid ? 1 : 0;
    masks_with_axes += observation.axes_available ? 1 : 0;
    masks_with_angle_labels += observation.angle_labels_available ? 1 : 0;
    observation.components.reserve(mask.subject_mask_components.size());
    for (const auto &source_component : mask.subject_mask_components) {
      crimson::gui::SubjectMaskInspectComponent component;
      component.label = source_component.label;
      component.channel_index = source_component.channel_index;
      component.channel_index_valid =
          source_component.channel_index != std::numeric_limits<size_t>::max();
      component.present = source_component.valid;
      component.pixel_payload_available = source_component.valid;
      component.pixel_payload_value_count =
          source_component.pixel_indices.size();
      component.contour_available = source_component.has_contour;
      component.contour_point_count = source_component.contour_xy.size();
      if (component.present && component.label == "subject_body") {
        ++masks_with_subject_body;
      } else if (component.present && component.label == "swim_bladder") {
        ++masks_with_swim_bladder;
      }
      masks_with_component_contours += component.contour_available ? 1 : 0;
      observation.components.push_back(std::move(component));
    }
    if (observation.components.empty()) {
      const auto &labels = context.zarr_loader.getEyeMaskChannelLabels();
      const auto &channels = context.zarr_loader.getEyeMaskChannelIndices();
      observation.components.push_back(makeLegacyEyeComponent(
          labels[0], channels[0], mask.pixel_indices[0], mask.valid));
      observation.components.push_back(makeLegacyEyeComponent(
          labels[1], channels[1], mask.pixel_indices[1], mask.valid));
    }
    presentation.observations.push_back(std::move(observation));
  }
  presentation.detail_lines.push_back(
      "Valid masks: " + std::to_string(valid_masks) +
      " | axes: " + std::to_string(masks_with_axes) +
      " | angle labels: " + std::to_string(masks_with_angle_labels));
  if (context.zarr_loader.eyeMasksUseRefinedSubjectMasks()) {
    presentation.detail_lines.push_back(
        "Body/swim bladder masks: " + std::to_string(masks_with_subject_body) +
        " / " + std::to_string(masks_with_swim_bladder) +
        " | component contours: " +
        std::to_string(masks_with_component_contours));
  }
  return presentation;
}
