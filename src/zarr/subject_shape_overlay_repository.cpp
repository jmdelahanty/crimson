#include "zarr/subject_shape_overlay_repository.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crimson::zarr {
namespace {

bool validPlacement(const SubjectShapeOverlayRow &row) {
  return std::isfinite(row.roi_x) && std::isfinite(row.roi_y) &&
         std::isfinite(row.roi_width) && std::isfinite(row.roi_height) &&
         row.roi_width > 0.0 && row.roi_height > 0.0;
}

class VectorSubjectShapeOverlayRepository final
    : public SubjectShapeOverlayRepository {
public:
  VectorSubjectShapeOverlayRepository(SubjectShapeOverlayDescriptor descriptor,
                                      std::vector<SubjectShapeOverlayRow> rows)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)) {
    for (size_t index = 0; index < rows_.size(); ++index) {
      rows_by_frame_[rows_[index].camera_frame].push_back(index);
    }
  }

  const SubjectShapeOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  SubjectShapeOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    SubjectShapeOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
      result.status = SubjectShapeOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = SubjectShapeOverlayStatus::InvalidDimensions;
      result.error = "Full-frame dimensions are invalid";
      return result;
    }
    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end()) {
      result.status = SubjectShapeOverlayStatus::Missing;
      return result;
    }

    result.status = SubjectShapeOverlayStatus::Mapped;
    result.detections.reserve(found->second.size());
    for (const size_t index : found->second) {
      const auto &row = rows_[index];
      if (!validPlacement(row)) {
        continue;
      }
      result.detections.push_back(
          {row.shape_row, row.detection_index, row.source_refined_row_id,
           row.source_crop_row_id, row.roi_x, row.roi_y, row.roi_width,
           row.roi_height, row.geometry});
    }
    if (result.detections.empty()) {
      result.status = SubjectShapeOverlayStatus::Missing;
    }
    return result;
  }

private:
  SubjectShapeOverlayDescriptor descriptor_;
  std::vector<SubjectShapeOverlayRow> rows_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
};

} // namespace

std::unique_ptr<SubjectShapeOverlayRepository>
MakeSubjectShapeOverlayRepository(SubjectShapeOverlayDescriptor descriptor,
                                  std::vector<SubjectShapeOverlayRow> rows) {
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
  return std::make_unique<VectorSubjectShapeOverlayRepository>(
      std::move(descriptor), std::move(rows));
}

} // namespace crimson::zarr
