#include "zarr/eye_geometry_overlay_repository.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crimson::zarr {
namespace {

class Repository final : public EyeGeometryOverlayRepository {
public:
  Repository(EyeGeometryOverlayDescriptor descriptor,
             std::vector<EyeGeometryOverlayRow> rows)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)) {
    descriptor_.row_count = rows_.size();
    for (size_t index = 0; index < rows_.size(); ++index) {
      rows_[index].eye_row = index;
      if (rows_[index].camera_frame < 0) {
        continue;
      }
      rows_by_frame_[rows_[index].camera_frame].push_back(index);
      descriptor_.camera_frame_count =
          std::max(descriptor_.camera_frame_count,
                   static_cast<size_t>(rows_[index].camera_frame) + 1);
    }
  }

  const EyeGeometryOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  EyeGeometryOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    EyeGeometryOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 || (descriptor_.camera_frame_count > 0 &&
                             static_cast<uint64_t>(camera_frame) >=
                                 descriptor_.camera_frame_count)) {
      result.status = EyeGeometryOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = EyeGeometryOverlayStatus::InvalidDimensions;
      return result;
    }
    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end()) {
      result.status = EyeGeometryOverlayStatus::Missing;
      return result;
    }
    for (const size_t index : found->second) {
      const auto &row = rows_[index];
      if (!std::isfinite(row.roi_x) || !std::isfinite(row.roi_y) ||
          !std::isfinite(row.roi_width) || !std::isfinite(row.roi_height) ||
          row.roi_width <= 0.0 || row.roi_height <= 0.0) {
        continue;
      }
      EyeGeometryOverlayDetection detection;
      static_cast<EyeGeometryOverlayRow &>(detection) = row;
      result.detections.push_back(std::move(detection));
    }
    result.status = result.detections.empty()
                        ? EyeGeometryOverlayStatus::Missing
                        : EyeGeometryOverlayStatus::Mapped;
    return result;
  }

private:
  EyeGeometryOverlayDescriptor descriptor_;
  std::vector<EyeGeometryOverlayRow> rows_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
};

} // namespace

std::unique_ptr<EyeGeometryOverlayRepository>
MakeEyeGeometryOverlayRepository(EyeGeometryOverlayDescriptor descriptor,
                                 std::vector<EyeGeometryOverlayRow> rows) {
  return std::make_unique<Repository>(std::move(descriptor), std::move(rows));
}

} // namespace crimson::zarr
