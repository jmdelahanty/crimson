#include "gui/subject_mask_overlay_inspect_adapter.h"

#include <cmath>
#include <string>
#include <utility>

namespace crimson::gui {
namespace {

const char *storageName(zarr::SubjectMaskStorage storage) {
  switch (storage) {
  case zarr::SubjectMaskStorage::Dense:
    return "dense";
  case zarr::SubjectMaskStorage::Bitpacked:
    return "bitpacked";
  case zarr::SubjectMaskStorage::Rle:
    return "RLE";
  }
  return "unknown";
}

std::string statusWarning(const zarr::SubjectMaskOverlayResolution &frame) {
  if (!frame.error.empty()) {
    return frame.error;
  }
  switch (frame.status) {
  case zarr::SubjectMaskOverlayStatus::Mapped:
  case zarr::SubjectMaskOverlayStatus::Missing:
    return {};
  case zarr::SubjectMaskOverlayStatus::OutOfRange:
    return "Presented frame is outside the subject-mask frame domain.";
  case zarr::SubjectMaskOverlayStatus::InvalidDimensions:
    return "Source-camera dimensions are invalid.";
  case zarr::SubjectMaskOverlayStatus::ReadFailed:
    return "Subject-mask data could not be read.";
  }
  return "Subject-mask resolution failed.";
}

} // namespace

SubjectMaskInspectPresentation makeSubjectMaskOverlayInspectPresentation(
    const zarr::SubjectMaskOverlayDescriptor *descriptor,
    const zarr::SubjectMaskOverlayResolution *frame, int64_t presented_frame) {
  SubjectMaskInspectPresentation presentation;
  presentation.available =
      descriptor != nullptr && !descriptor->run_name.empty();
  if (!presentation.available) {
    return presentation;
  }

  presentation.surface_label = descriptor->contour_only
                                   ? "Sampled contour cache"
                               : descriptor->strict_v1 ? "Subject-mask v1"
                                                       : "Subject-mask overlay";
  presentation.run_name = descriptor->run_name;
  if (!descriptor->source_group.empty()) {
    presentation.detail_lines.push_back("Source group: " +
                                        descriptor->source_group);
  }
  if (!descriptor->source_crop_run.empty()) {
    presentation.detail_lines.push_back("Crop run: " +
                                        descriptor->source_crop_run);
  }
  presentation.detail_lines.push_back(
      "Storage: " + std::string(storageName(descriptor->storage)) + " | " +
      std::to_string(descriptor->mask_width) + " x " +
      std::to_string(descriptor->mask_height) + " | " +
      std::to_string(descriptor->component_labels.size()) + " components");
  if (descriptor->contour_only && !descriptor->presentation_cache_run.empty()) {
    presentation.detail_lines.push_back("Presentation cache: " +
                                        descriptor->presentation_cache_run);
  }

  presentation.frame_ready =
      frame != nullptr && frame->camera_frame == presented_frame;
  if (!presentation.frame_ready) {
    return presentation;
  }

  presentation.camera_frame = frame->camera_frame;
  presentation.warning = statusWarning(*frame);
  if (frame->status != zarr::SubjectMaskOverlayStatus::Mapped) {
    return presentation;
  }

  presentation.observations.reserve(frame->detections.size());
  for (const zarr::SubjectMaskOverlayDetection &detection : frame->detections) {
    SubjectMaskInspectObservation observation;
    observation.instance_key = detection.instance_key;
    observation.selectable = detection.instance_key != 0;
    observation.source_crop_row_id = detection.source_crop_row_id;
    observation.source_crop_row_id_valid = detection.source_crop_row_id >= 0;
    observation.roi_width = detection.roi_width;
    observation.roi_height = detection.roi_height;
    observation.roi_valid = std::isfinite(detection.roi_width) &&
                            std::isfinite(detection.roi_height) &&
                            detection.roi_width > 0.0 &&
                            detection.roi_height > 0.0;
    observation.components.reserve(detection.components.size());
    for (const zarr::SubjectMaskOverlayComponent &source_component :
         detection.components) {
      SubjectMaskInspectComponent component;
      component.label = source_component.label;
      component.channel_index = source_component.channel_index;
      component.channel_index_valid = true;
      component.present = source_component.present;
      component.pixel_payload_available = source_component.mask != nullptr;
      component.pixel_payload_value_count =
          component.pixel_payload_available ? source_component.mask->size() : 0;
      component.contour_available = !source_component.contour.empty();
      component.contour_point_count = source_component.contour.size();
      observation.valid = observation.valid || component.present;
      observation.components.push_back(std::move(component));
    }
    presentation.observations.push_back(std::move(observation));
  }
  return presentation;
}

} // namespace crimson::gui
