#include "zarr/legacy_subject_mask_overlay_repository.h"

#include "zarr_loader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace crimson::zarr {
namespace {

std::shared_ptr<const std::vector<uint8_t>>
denseMask(int rows, int cols, const std::vector<uint32_t> &pixels) {
  if (rows <= 0 || cols <= 0 || pixels.empty()) {
    return {};
  }
  const size_t height = static_cast<size_t>(rows);
  const size_t width = static_cast<size_t>(cols);
  if (width > std::numeric_limits<size_t>::max() / height) {
    return {};
  }
  auto mask = std::make_shared<std::vector<uint8_t>>(width * height, 0);
  for (const uint32_t pixel : pixels) {
    if (static_cast<size_t>(pixel) < mask->size()) {
      (*mask)[pixel] = 1;
    }
  }
  return mask;
}

std::vector<SubjectMaskOverlayPoint>
sourceContour(const ZarrDetectionLoader::FrameDetections::EyeMask &mask,
              const std::vector<std::array<float, 2>> &points) {
  std::vector<SubjectMaskOverlayPoint> result;
  if (mask.rows <= 0 || mask.cols <= 0 || mask.roi_width <= 0.0f ||
      mask.roi_height <= 0.0f || !std::isfinite(mask.offset_x) ||
      !std::isfinite(mask.offset_y)) {
    return result;
  }
  const double scale_x =
      static_cast<double>(mask.roi_width) / static_cast<double>(mask.cols);
  const double scale_y =
      static_cast<double>(mask.roi_height) / static_cast<double>(mask.rows);
  result.reserve(points.size());
  for (const auto &point : points) {
    if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
      continue;
    }
    result.push_back({static_cast<double>(mask.offset_x) + point[0] * scale_x,
                      static_cast<double>(mask.offset_y) + point[1] * scale_y});
  }
  return result;
}

} // namespace

LegacySubjectMaskOverlayRepository::LegacySubjectMaskOverlayRepository(
    const ZarrDetectionLoader &loader, AllowBlockingLoad allow_blocking_load)
    : loader_(loader), allow_blocking_load_(std::move(allow_blocking_load)) {
  refreshDescriptor();
}

void LegacySubjectMaskOverlayRepository::refreshDescriptor() const {
  descriptor_.source_group = loader_.eyeMasksUseRefinedSubjectMasks()
                                 ? "refined_subject_masks_runs"
                                 : "legacy_eye_masks";
  descriptor_.cache_namespace =
      loader_.getArchivePath() + ":" + loader_.getEyeMaskSourcePath();
  descriptor_.run_name = loader_.getEyeMaskRunName();
  descriptor_.source_crop_run.clear();
  descriptor_.label_schema_id = "legacy_subject_mask_overlay";
  descriptor_.component_labels = loader_.getRefinedSubjectMaskLabels();
  if (descriptor_.component_labels.empty()) {
    for (const auto &component :
         loader_.getRefinedSubjectMaskOverlayComponents()) {
      descriptor_.component_labels.push_back(component.label);
    }
  }
  if (descriptor_.component_labels.empty()) {
    const auto &eye_labels = loader_.getEyeMaskChannelLabels();
    descriptor_.component_labels.assign(eye_labels.begin(), eye_labels.end());
  }
  descriptor_.camera_frame_count = loader_.getTotalFrames();
  descriptor_.row_count = 0;
  descriptor_.strict_v1 = false;
}

const SubjectMaskOverlayDescriptor &
LegacySubjectMaskOverlayRepository::descriptor() const {
  refreshDescriptor();
  return descriptor_;
}

SubjectMaskOverlayResolution
LegacySubjectMaskOverlayRepository::resolveCameraFrame(
    int64_t camera_frame, int full_frame_width, int full_frame_height) const {
  refreshDescriptor();
  SubjectMaskOverlayResolution result;
  result.camera_frame = camera_frame;
  if (!loader_.hasEyeMasks() || descriptor_.run_name.empty()) {
    result.status = SubjectMaskOverlayStatus::Missing;
    return result;
  }
  if (camera_frame < 0 ||
      static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
    result.status = SubjectMaskOverlayStatus::OutOfRange;
    return result;
  }
  if (full_frame_width <= 0 || full_frame_height <= 0) {
    result.status = SubjectMaskOverlayStatus::InvalidDimensions;
    return result;
  }

  const bool allow_blocking = !allow_blocking_load_ || allow_blocking_load_();
  const auto legacy =
      loader_.getRawDetections(static_cast<size_t>(camera_frame), false, true,
                               false, true, allow_blocking);
  if (!legacy.includes_eye_masks) {
    result.status = SubjectMaskOverlayStatus::Missing;
    return result;
  }

  result.detections.reserve(legacy.eye_masks.size());
  for (size_t detection_index = 0; detection_index < legacy.eye_masks.size();
       ++detection_index) {
    const auto &mask = legacy.eye_masks[detection_index];
    if (!mask.valid || mask.rows <= 0 || mask.cols <= 0 || mask.roi_index < 0 ||
        mask.roi_width <= 0.0f || mask.roi_height <= 0.0f ||
        !std::isfinite(mask.offset_x) || !std::isfinite(mask.offset_y)) {
      continue;
    }
    SubjectMaskOverlayDetection detection;
    detection.detection_index = static_cast<int64_t>(detection_index);
    detection.source_crop_row_id = mask.roi_index;
    detection.roi_x = mask.offset_x;
    detection.roi_y = mask.offset_y;
    detection.roi_width = mask.roi_width;
    detection.roi_height = mask.roi_height;

    for (const auto &component : mask.subject_mask_components) {
      if (!component.valid) {
        continue;
      }
      SubjectMaskOverlayComponent converted;
      converted.label = component.label;
      converted.channel_index = component.channel_index;
      converted.mask_width = static_cast<size_t>(mask.cols);
      converted.mask_height = static_cast<size_t>(mask.rows);
      converted.mask = denseMask(mask.rows, mask.cols, component.pixel_indices);
      converted.present = converted.mask != nullptr;
      if (component.has_contour) {
        converted.contour = sourceContour(mask, component.contour_xy);
      }
      detection.components.push_back(std::move(converted));
    }

    if (detection.components.empty()) {
      const auto &labels = loader_.getEyeMaskChannelLabels();
      const auto &channels = loader_.getEyeMaskChannelIndices();
      for (size_t eye = 0; eye < mask.pixel_indices.size(); ++eye) {
        if (mask.pixel_indices[eye].empty()) {
          continue;
        }
        SubjectMaskOverlayComponent converted;
        converted.label = labels[eye];
        converted.channel_index = channels[eye];
        converted.mask_width = static_cast<size_t>(mask.cols);
        converted.mask_height = static_cast<size_t>(mask.rows);
        converted.mask =
            denseMask(mask.rows, mask.cols, mask.pixel_indices[eye]);
        converted.present = converted.mask != nullptr;
        detection.components.push_back(std::move(converted));
      }
    }
    if (!detection.components.empty()) {
      result.detections.push_back(std::move(detection));
    }
  }

  result.status = result.detections.empty() ? SubjectMaskOverlayStatus::Missing
                                            : SubjectMaskOverlayStatus::Mapped;
  return result;
}

} // namespace crimson::zarr
