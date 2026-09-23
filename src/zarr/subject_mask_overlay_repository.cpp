#include "zarr/subject_mask_overlay_repository.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crimson::zarr {
namespace {

class VectorSubjectMaskOverlayRepository final
    : public SubjectMaskOverlayRepository {
public:
  VectorSubjectMaskOverlayRepository(SubjectMaskOverlayDescriptor descriptor,
                                     std::vector<SubjectMaskOverlayRow> rows)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)) {
    for (size_t index = 0; index < rows_.size(); ++index) {
      rows_by_frame_[rows_[index].camera_frame].push_back(index);
    }
  }

  const SubjectMaskOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  SubjectMaskOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    SubjectMaskOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
      result.status = SubjectMaskOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = SubjectMaskOverlayStatus::InvalidDimensions;
      return result;
    }
    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end()) {
      result.status = SubjectMaskOverlayStatus::Missing;
      return result;
    }

    result.status = SubjectMaskOverlayStatus::Mapped;
    result.detections.reserve(found->second.size());
    for (const size_t index : found->second) {
      const auto &row = rows_[index];
      if (!std::isfinite(row.roi_x) || !std::isfinite(row.roi_y) ||
          !std::isfinite(row.roi_width) || !std::isfinite(row.roi_height) ||
          row.roi_width <= 0.0 || row.roi_height <= 0.0) {
        continue;
      }
      SubjectMaskOverlayDetection detection;
      detection.instance_key = row.instance_key;
      detection.detection_index = row.detection_index;
      detection.source_crop_row_id = row.source_crop_row_id;
      detection.roi_x = row.roi_x;
      detection.roi_y = row.roi_y;
      detection.roi_width = row.roi_width;
      detection.roi_height = row.roi_height;
      detection.components = row.components;
      for (auto &component : detection.components) {
        component.contour.erase(
            std::remove_if(component.contour.begin(), component.contour.end(),
                           [](const auto &point) {
                             return !std::isfinite(point.x) ||
                                    !std::isfinite(point.y);
                           }),
            component.contour.end());
        for (auto &point : component.contour) {
          if (component.mask_width > 0 && component.mask_height > 0) {
            point.x = row.roi_x +
                      point.x * row.roi_width / component.mask_width;
            point.y = row.roi_y +
                      point.y * row.roi_height / component.mask_height;
          } else {
            point.x += row.roi_x;
            point.y += row.roi_y;
          }
        }
      }
      result.detections.push_back(std::move(detection));
    }
    if (result.detections.empty()) {
      result.status = SubjectMaskOverlayStatus::Missing;
    }
    return result;
  }

private:
  SubjectMaskOverlayDescriptor descriptor_;
  std::vector<SubjectMaskOverlayRow> rows_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
};

} // namespace

std::unique_ptr<SubjectMaskOverlayRepository>
MakeSubjectMaskOverlayRepository(SubjectMaskOverlayDescriptor descriptor,
                                 std::vector<SubjectMaskOverlayRow> rows) {
  descriptor.row_count = rows.size();
  size_t camera_frame_count = 0;
  for (const auto &row : rows) {
    if (row.camera_frame >= 0 && static_cast<uint64_t>(row.camera_frame) <
                                     std::numeric_limits<size_t>::max()) {
      camera_frame_count = std::max(camera_frame_count,
                                    static_cast<size_t>(row.camera_frame) + 1);
    }
  }
  descriptor.camera_frame_count =
      std::max(descriptor.camera_frame_count, camera_frame_count);
  return std::make_unique<VectorSubjectMaskOverlayRepository>(
      std::move(descriptor), std::move(rows));
}

} // namespace crimson::zarr
