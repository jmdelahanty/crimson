#include "zarr/analysis_crop_geometry_repository.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crimson::zarr {
namespace {

class VectorAnalysisCropGeometryRepository final
    : public AnalysisCropGeometryRepository {
public:
  VectorAnalysisCropGeometryRepository(
      AnalysisCropGeometryDescriptor descriptor,
      std::vector<AnalysisCropGeometryRow> rows)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)) {
    for (size_t index = 0; index < rows_.size(); ++index) {
      rows_by_frame_[rows_[index].camera_frame].push_back(index);
    }
  }

  const AnalysisCropGeometryDescriptor &descriptor() const override {
    return descriptor_;
  }

  crop::CropSourceCapabilities sourceCapabilities() const override {
    crop::CropSourceCapabilities capabilities;
    capabilities.live_geometry = !rows_.empty();
    return capabilities;
  }

  AnalysisCropGeometryResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    AnalysisCropGeometryResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
      result.status = AnalysisCropGeometryStatus::OutOfRange;
      return result;
    }

    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end() || found->second.empty()) {
      result.status = AnalysisCropGeometryStatus::Missing;
      return result;
    }

    // This matches the legacy unselected Crop Preview fallback, which uses the
    // first crop row associated with the visible camera frame.
    const auto &row = rows_[found->second.front()];
    result.status = AnalysisCropGeometryStatus::Mapped;
    result.roi_index = row.roi_index;
    result.instance_key = row.instance_key;
    result.frame_row_count = found->second.size();
    result.roi_bbox_xyxy = row.roi_bbox_xyxy;

    crop::CropFrameGeometry geometry;
    geometry.camera_frame = camera_frame;
    geometry.source_width = full_frame_width;
    geometry.source_height = full_frame_height;
    geometry.output_width = descriptor_.output_width;
    geometry.output_height = descriptor_.output_height;
    geometry.full_frame_crop = {row.offset_x, row.offset_y,
                                static_cast<double>(descriptor_.output_width),
                                static_cast<double>(descriptor_.output_height)};
    geometry.geometry_available = true;

    if (row.normalized_detection_cxcywh) {
      const auto &box = *row.normalized_detection_cxcywh;
      const double center_x = box[0] * full_frame_width;
      const double center_y = box[1] * full_frame_height;
      const double width = box[2] * full_frame_width;
      const double height = box[3] * full_frame_height;
      geometry.full_frame_detection = crop::CropRect{
          std::clamp(center_x - width * 0.5, 0.0,
                     static_cast<double>(full_frame_width)),
          std::clamp(center_y - height * 0.5, 0.0,
                     static_cast<double>(full_frame_height)),
          std::max(0.0, std::min(center_x + width * 0.5,
                                 static_cast<double>(full_frame_width)) -
                            std::max(center_x - width * 0.5, 0.0)),
          std::max(0.0, std::min(center_y + height * 0.5,
                                 static_cast<double>(full_frame_height)) -
                            std::max(center_y - height * 0.5, 0.0))};
      geometry.has_detection = geometry.full_frame_detection->valid();
      if (!geometry.has_detection) {
        geometry.full_frame_detection.reset();
      }
    }
    result.geometry = std::move(geometry);
    return result;
  }

private:
  AnalysisCropGeometryDescriptor descriptor_;
  std::vector<AnalysisCropGeometryRow> rows_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
};

} // namespace

std::unique_ptr<AnalysisCropGeometryRepository>
MakeAnalysisCropGeometryRepository(AnalysisCropGeometryDescriptor descriptor,
                                   std::vector<AnalysisCropGeometryRow> rows) {
  descriptor.row_count = rows.size();
  size_t camera_frame_count = 0;
  for (const auto &row : rows) {
    if (row.camera_frame >= 0 && static_cast<uint64_t>(row.camera_frame) <
                                     std::numeric_limits<size_t>::max()) {
      camera_frame_count = std::max(camera_frame_count,
                                    static_cast<size_t>(row.camera_frame) + 1);
    }
  }
  if (descriptor.camera_frame_count == 0) {
    descriptor.camera_frame_count = camera_frame_count;
  }
  return std::make_unique<VectorAnalysisCropGeometryRepository>(
      std::move(descriptor), std::move(rows));
}

} // namespace crimson::zarr
