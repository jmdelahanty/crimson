#include "zarr/analysis_crop_geometry_repository.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace crimson::zarr {
namespace {

class CompactAnalysisCropGeometryRepository final
    : public AnalysisCropGeometryRepository {
public:
  CompactAnalysisCropGeometryRepository(
      AnalysisCropGeometryDescriptor descriptor,
      std::vector<AnalysisCropGeometryRow> rows)
      : descriptor_(std::move(descriptor)) {
    const size_t frame_count = descriptor_.camera_frame_count;
    std::vector<uint64_t> frame_counts(frame_count, 0);
    size_t mapped_rows = 0;
    size_t instance_key_count = 0;
    size_t normalized_box_count = 0;
    size_t roi_box_count = 0;
    for (const auto &row : rows) {
      if (row.camera_frame < 0 ||
          static_cast<uint64_t>(row.camera_frame) >= frame_count) {
        continue;
      }
      ++frame_counts[static_cast<size_t>(row.camera_frame)];
      ++mapped_rows;
      instance_key_count += row.instance_key.has_value();
      normalized_box_count += row.normalized_detection_cxcywh.has_value();
      roi_box_count += row.roi_bbox_xyxy.has_value();
    }

    frame_row_offsets_.resize(frame_count + 1, 0);
    for (size_t frame = 0; frame < frame_count; ++frame) {
      frame_row_offsets_[frame + 1] =
          frame_row_offsets_[frame] + frame_counts[frame];
    }
    mapped_row_count_ = mapped_rows;
    offsets_xy_.resize(mapped_rows);
    if (instance_key_count > 0) {
      instance_keys_.resize(mapped_rows);
      if (instance_key_count != mapped_rows) {
        instance_key_valid_.assign(mapped_rows, 0);
      }
    }
    if (normalized_box_count > 0) {
      normalized_boxes_.resize(mapped_rows);
      if (normalized_box_count != mapped_rows) {
        normalized_box_valid_.assign(mapped_rows, 0);
      }
    }
    if (roi_box_count > 0) {
      roi_boxes_.resize(mapped_rows);
      if (roi_box_count != mapped_rows) {
        roi_box_valid_.assign(mapped_rows, 0);
      }
    }

    std::vector<uint64_t> cursors(frame_row_offsets_.begin(),
                                  frame_row_offsets_.end() - 1);
    for (size_t source_index = 0; source_index < rows.size(); ++source_index) {
      const auto &row = rows[source_index];
      if (row.camera_frame < 0 ||
          static_cast<uint64_t>(row.camera_frame) >= frame_count) {
        continue;
      }
      const size_t target =
          static_cast<size_t>(cursors[static_cast<size_t>(row.camera_frame)]++);
      if (row.roi_index != static_cast<int64_t>(target)) {
        if (roi_indices_.empty()) {
          roi_indices_.resize(mapped_rows);
          std::iota(roi_indices_.begin(), roi_indices_.end(), int64_t{0});
        }
        roi_indices_[target] = row.roi_index;
      }
      offsets_xy_[target] = {row.offset_x, row.offset_y};
      if (row.instance_key) {
        instance_keys_[target] = *row.instance_key;
        if (!instance_key_valid_.empty()) {
          instance_key_valid_[target] = 1;
        }
      }
      if (row.normalized_detection_cxcywh) {
        normalized_boxes_[target] = *row.normalized_detection_cxcywh;
        if (!normalized_box_valid_.empty()) {
          normalized_box_valid_[target] = 1;
        }
      }
      if (row.roi_bbox_xyxy) {
        roi_boxes_[target] = *row.roi_bbox_xyxy;
        if (!roi_box_valid_.empty()) {
          roi_box_valid_[target] = 1;
        }
      }
    }
    descriptor_.retained_frame_offsets = true;
    descriptor_.pageable_payload = false;
  }

  const AnalysisCropGeometryDescriptor &descriptor() const override {
    return descriptor_;
  }

  crop::CropSourceCapabilities sourceCapabilities() const override {
    crop::CropSourceCapabilities capabilities;
    capabilities.live_geometry = mapped_row_count_ > 0;
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

    const size_t frame = static_cast<size_t>(camera_frame);
    const size_t first = static_cast<size_t>(frame_row_offsets_[frame]);
    const size_t last = static_cast<size_t>(frame_row_offsets_[frame + 1]);
    if (first == last) {
      result.status = AnalysisCropGeometryStatus::Missing;
      return result;
    }

    // This matches the legacy unselected Crop Preview fallback, which uses the
    // first crop row associated with the visible camera frame.
    result.status = AnalysisCropGeometryStatus::Mapped;
    result.roi_index = roi_indices_.empty() ? static_cast<int64_t>(first)
                                            : roi_indices_[first];
    if (!instance_keys_.empty() &&
        (instance_key_valid_.empty() || instance_key_valid_[first] != 0)) {
      result.instance_key = instance_keys_[first];
    }
    result.frame_row_count = last - first;
    if (!roi_boxes_.empty() &&
        (roi_box_valid_.empty() || roi_box_valid_[first] != 0)) {
      result.roi_bbox_xyxy = roi_boxes_[first];
    }

    crop::CropFrameGeometry geometry;
    geometry.camera_frame = camera_frame;
    geometry.source_width = full_frame_width;
    geometry.source_height = full_frame_height;
    geometry.output_width = descriptor_.output_width;
    geometry.output_height = descriptor_.output_height;
    geometry.full_frame_crop = {offsets_xy_[first][0], offsets_xy_[first][1],
                                static_cast<double>(descriptor_.output_width),
                                static_cast<double>(descriptor_.output_height)};
    geometry.geometry_available = true;

    if (!normalized_boxes_.empty() &&
        (normalized_box_valid_.empty() || normalized_box_valid_[first] != 0)) {
      const auto &box = normalized_boxes_[first];
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

  RepositoryMemoryMetrics memoryMetrics() const override {
    RepositoryMemoryMetrics metrics;
    metrics.retained_index_bytes =
        memory::vectorAllocationBytes(frame_row_offsets_) +
        memory::vectorAllocationBytes(roi_indices_);
    metrics.retained_payload_bytes =
        memory::vectorAllocationBytes(offsets_xy_) +
        memory::vectorAllocationBytes(instance_keys_) +
        memory::vectorAllocationBytes(instance_key_valid_) +
        memory::vectorAllocationBytes(normalized_boxes_) +
        memory::vectorAllocationBytes(normalized_box_valid_) +
        memory::vectorAllocationBytes(roi_boxes_) +
        memory::vectorAllocationBytes(roi_box_valid_);
    return metrics;
  }

private:
  AnalysisCropGeometryDescriptor descriptor_;
  size_t mapped_row_count_ = 0;
  std::vector<uint64_t> frame_row_offsets_;
  std::vector<int64_t> roi_indices_;
  std::vector<std::array<double, 2>> offsets_xy_;
  std::vector<uint64_t> instance_keys_;
  std::vector<uint8_t> instance_key_valid_;
  std::vector<std::array<double, 4>> normalized_boxes_;
  std::vector<uint8_t> normalized_box_valid_;
  std::vector<std::array<double, 4>> roi_boxes_;
  std::vector<uint8_t> roi_box_valid_;
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
  return std::make_unique<CompactAnalysisCropGeometryRepository>(
      std::move(descriptor), std::move(rows));
}

} // namespace crimson::zarr
