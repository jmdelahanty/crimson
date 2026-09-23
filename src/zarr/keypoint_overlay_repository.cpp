#include "zarr/keypoint_overlay_repository.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crimson::zarr {
namespace {

class VectorKeypointOverlayRepository final
    : public KeypointOverlayRepository {
 public:
  VectorKeypointOverlayRepository(KeypointOverlayDescriptor descriptor,
                                  std::vector<KeypointOverlayRow> rows)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)) {
    for (size_t index = 0; index < rows_.size(); ++index) {
      rows_by_frame_[rows_[index].camera_frame].push_back(index);
    }
  }

  const KeypointOverlayDescriptor& descriptor() const override {
    return descriptor_;
  }

  KeypointOverlayResolution resolveCameraFrame(
      int64_t camera_frame,
      int full_frame_width,
      int full_frame_height) const override {
    KeypointOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >=
            descriptor_.camera_frame_count) {
      result.status = KeypointOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = KeypointOverlayStatus::InvalidDimensions;
      return result;
    }

    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end()) {
      result.status = KeypointOverlayStatus::Missing;
      return result;
    }

    result.status = KeypointOverlayStatus::Mapped;
    result.detections.reserve(found->second.size());
    for (const size_t row_index : found->second) {
      const auto& row = rows_[row_index];
      KeypointOverlayDetection detection;
      detection.detection_index = row.detection_index;
      detection.source_crop_row_id = row.source_crop_row_id;
      detection.heading_degrees = row.heading_degrees;
      detection.heading_valid = row.heading_valid;
      detection.detection_interpolated = row.detection_interpolated;
      detection.refined_keypoints = row.refined_keypoints;
      detection.keypoint_usable = row.keypoint_usable;
      detection.keypoint_detection_interpolated =
          row.keypoint_detection_interpolated;
      detection.keypoint_flip_corrected = row.keypoint_flip_corrected;

      if (row.normalized_detection_cxcywh) {
        const auto& normalized = *row.normalized_detection_cxcywh;
        const double center_x = normalized[0] * full_frame_width;
        const double center_y = normalized[1] * full_frame_height;
        const double raw_width = normalized[2] * full_frame_width;
        const double raw_height = normalized[3] * full_frame_height;
        const double left = std::clamp(center_x - raw_width * 0.5, 0.0,
                                       static_cast<double>(full_frame_width));
        const double top = std::clamp(center_y - raw_height * 0.5, 0.0,
                                      static_cast<double>(full_frame_height));
        const double right = std::clamp(center_x + raw_width * 0.5, 0.0,
                                        static_cast<double>(full_frame_width));
        const double bottom = std::clamp(center_y + raw_height * 0.5, 0.0,
                                         static_cast<double>(full_frame_height));
        if (std::isfinite(left) && std::isfinite(top) &&
            std::isfinite(right) && std::isfinite(bottom) && right > left &&
            bottom > top) {
          detection.full_frame_box_xywh =
              std::array<double, 4>{left, top, right - left, bottom - top};
          detection.heading_origin = KeypointOverlayPoint{
              (left + right) * 0.5, (top + bottom) * 0.5};
        }
      }

      const double nan = std::numeric_limits<double>::quiet_NaN();
      detection.keypoints.reserve(row.keypoints.size());
      for (const auto point : row.keypoints) {
        KeypointOverlayPoint converted{nan, nan};
        if (std::isfinite(point.x) && std::isfinite(point.y)) {
          if (descriptor_.coordinate_space == KeypointCoordinateSpace::Image) {
            converted = point;
          } else {
            double local_x = point.x;
            double local_y = point.y;
            if (descriptor_.coordinate_space ==
                KeypointCoordinateSpace::NormalizedRoi) {
              double width = row.roi_width;
              double height = row.roi_height;
              if ((width <= 0.0 || height <= 0.0) &&
                  detection.full_frame_box_xywh) {
                width = (*detection.full_frame_box_xywh)[2];
                height = (*detection.full_frame_box_xywh)[3];
              }
              if (width > 0.0 && height > 0.0) {
                local_x *= width;
                local_y *= height;
              } else {
                detection.keypoints.push_back(converted);
                continue;
              }
            }
            std::optional<KeypointOverlayPoint> offset = row.roi_offset;
            if (!offset && detection.full_frame_box_xywh) {
              offset = KeypointOverlayPoint{
                  (*detection.full_frame_box_xywh)[0],
                  (*detection.full_frame_box_xywh)[1]};
            }
            if (offset && std::isfinite(offset->x) &&
                std::isfinite(offset->y)) {
              converted = {offset->x + local_x, offset->y + local_y};
            }
          }
        }
        detection.keypoints.push_back(converted);
      }
      result.detections.push_back(std::move(detection));
    }
    return result;
  }

 private:
  KeypointOverlayDescriptor descriptor_;
  std::vector<KeypointOverlayRow> rows_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
};

}  // namespace

std::unique_ptr<KeypointOverlayRepository> MakeKeypointOverlayRepository(
    KeypointOverlayDescriptor descriptor,
    std::vector<KeypointOverlayRow> rows) {
  descriptor.row_count = rows.size();
  size_t camera_frame_count = 0;
  for (const auto& row : rows) {
    if (row.camera_frame >= 0 &&
        static_cast<uint64_t>(row.camera_frame) <
            std::numeric_limits<size_t>::max()) {
      camera_frame_count =
          std::max(camera_frame_count,
                   static_cast<size_t>(row.camera_frame) + 1);
    }
  }
  descriptor.camera_frame_count = camera_frame_count;
  return std::make_unique<VectorKeypointOverlayRepository>(
      std::move(descriptor), std::move(rows));
}

}  // namespace crimson::zarr
