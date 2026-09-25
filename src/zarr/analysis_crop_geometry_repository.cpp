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
      AnalysisCropGeometryColumns columns)
      : descriptor_(std::move(descriptor)),
        mapped_row_count_(columns.offsets_xy.size()),
        frame_row_offsets_(std::move(columns.frame_row_offsets)),
        roi_indices_(std::move(columns.roi_indices)),
        offsets_xy_(std::move(columns.offsets_xy)),
        instance_keys_(std::move(columns.instance_keys)),
        instance_key_valid_(std::move(columns.instance_key_valid)),
        normalized_boxes_(std::move(columns.normalized_detection_cxcywh)),
        normalized_box_valid_(std::move(columns.normalized_detection_valid)),
        roi_boxes_(std::move(columns.roi_bbox_xyxy)),
        roi_box_valid_(std::move(columns.roi_bbox_valid)) {}

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
  std::vector<int64_t> frame_row_offsets_;
  std::vector<int64_t> roi_indices_;
  std::vector<std::array<double, 2>> offsets_xy_;
  std::vector<uint64_t> instance_keys_;
  std::vector<uint8_t> instance_key_valid_;
  std::vector<std::array<double, 4>> normalized_boxes_;
  std::vector<uint8_t> normalized_box_valid_;
  std::vector<std::array<double, 4>> roi_boxes_;
  std::vector<uint8_t> roi_box_valid_;
};

template <typename T>
bool OptionalColumnHasValidSize(const std::vector<T> &values,
                                const std::vector<uint8_t> &validity,
                                size_t row_count) {
  if (values.empty()) {
    return validity.empty();
  }
  return values.size() == row_count &&
         (validity.empty() || validity.size() == row_count);
}

template <typename T>
void ReorderColumn(std::vector<T> *values,
                   const std::vector<size_t> &target_by_source) {
  if (values->empty()) {
    return;
  }
  std::vector<T> reordered(values->size());
  for (size_t source = 0; source < values->size(); ++source) {
    reordered[target_by_source[source]] = std::move((*values)[source]);
  }
  *values = std::move(reordered);
}

bool ValidatePersistedOffsets(const std::vector<int64_t> &frame_indices,
                              const std::vector<int64_t> &frame_row_offsets,
                              size_t frame_count) {
  if (frame_row_offsets.size() != frame_count + 1 ||
      frame_row_offsets.empty() || frame_row_offsets.front() != 0 ||
      frame_row_offsets.back() != static_cast<int64_t>(frame_indices.size()) ||
      !std::is_sorted(frame_row_offsets.begin(), frame_row_offsets.end()) ||
      !std::is_sorted(frame_indices.begin(), frame_indices.end())) {
    return false;
  }
  for (size_t frame = 0; frame < frame_count; ++frame) {
    const int64_t first = frame_row_offsets[frame];
    const int64_t last = frame_row_offsets[frame + 1];
    if (first < 0 || last < first ||
        last > static_cast<int64_t>(frame_indices.size())) {
      return false;
    }
    for (int64_t row = first; row < last; ++row) {
      if (frame_indices[static_cast<size_t>(row)] !=
          static_cast<int64_t>(frame)) {
        return false;
      }
    }
  }
  return true;
}

std::unique_ptr<AnalysisCropGeometryRepository>
MakeCompactRepository(AnalysisCropGeometryDescriptor descriptor,
                      AnalysisCropGeometryColumns columns,
                      bool direct_compact_columns) {
  const size_t row_count = columns.frame_indices.size();
  if (columns.offsets_xy.size() != row_count ||
      (!columns.roi_indices.empty() &&
       columns.roi_indices.size() != row_count) ||
      !OptionalColumnHasValidSize(columns.instance_keys,
                                  columns.instance_key_valid, row_count) ||
      !OptionalColumnHasValidSize(columns.normalized_detection_cxcywh,
                                  columns.normalized_detection_valid,
                                  row_count) ||
      !OptionalColumnHasValidSize(columns.roi_bbox_xyxy, columns.roi_bbox_valid,
                                  row_count)) {
    return nullptr;
  }

  size_t inferred_frame_count = 0;
  for (const int64_t frame : columns.frame_indices) {
    if (frame < 0 ||
        static_cast<uint64_t>(frame) >= std::numeric_limits<size_t>::max()) {
      return nullptr;
    }
    inferred_frame_count =
        std::max(inferred_frame_count, static_cast<size_t>(frame) + 1);
  }
  if (descriptor.camera_frame_count == 0) {
    descriptor.camera_frame_count = columns.frame_row_offsets.empty()
                                        ? inferred_frame_count
                                        : columns.frame_row_offsets.size() - 1;
  }
  if (inferred_frame_count > descriptor.camera_frame_count) {
    return nullptr;
  }

  if (!columns.frame_row_offsets.empty()) {
    if (!ValidatePersistedOffsets(columns.frame_indices,
                                  columns.frame_row_offsets,
                                  descriptor.camera_frame_count)) {
      return nullptr;
    }
  } else {
    columns.frame_row_offsets.assign(descriptor.camera_frame_count + 1, 0);
    for (const int64_t frame : columns.frame_indices) {
      ++columns.frame_row_offsets[static_cast<size_t>(frame) + 1];
    }
    std::partial_sum(columns.frame_row_offsets.begin(),
                     columns.frame_row_offsets.end(),
                     columns.frame_row_offsets.begin());

    if (!std::is_sorted(columns.frame_indices.begin(),
                        columns.frame_indices.end())) {
      std::vector<int64_t> cursors(columns.frame_row_offsets.begin(),
                                   columns.frame_row_offsets.end() - 1);
      std::vector<size_t> target_by_source(row_count);
      for (size_t source = 0; source < row_count; ++source) {
        target_by_source[source] = static_cast<size_t>(
            cursors[static_cast<size_t>(columns.frame_indices[source])]++);
      }
      if (columns.roi_indices.empty()) {
        columns.roi_indices.resize(row_count);
        std::iota(columns.roi_indices.begin(), columns.roi_indices.end(),
                  int64_t{0});
      }
      ReorderColumn(&columns.roi_indices, target_by_source);
      ReorderColumn(&columns.offsets_xy, target_by_source);
      ReorderColumn(&columns.instance_keys, target_by_source);
      ReorderColumn(&columns.instance_key_valid, target_by_source);
      ReorderColumn(&columns.normalized_detection_cxcywh, target_by_source);
      ReorderColumn(&columns.normalized_detection_valid, target_by_source);
      ReorderColumn(&columns.roi_bbox_xyxy, target_by_source);
      ReorderColumn(&columns.roi_bbox_valid, target_by_source);
    }
  }

  if (!columns.roi_indices.empty()) {
    bool identity = true;
    for (size_t row = 0; row < columns.roi_indices.size(); ++row) {
      if (columns.roi_indices[row] != static_cast<int64_t>(row)) {
        identity = false;
        break;
      }
    }
    if (identity) {
      std::vector<int64_t>{}.swap(columns.roi_indices);
    }
  }

  columns.frame_indices = {};
  descriptor.row_count = row_count;
  descriptor.retained_frame_offsets = true;
  descriptor.pageable_payload = false;
  descriptor.direct_compact_columns = direct_compact_columns;
  return std::make_unique<CompactAnalysisCropGeometryRepository>(
      std::move(descriptor), std::move(columns));
}

} // namespace

std::unique_ptr<AnalysisCropGeometryRepository>
MakeAnalysisCropGeometryRepository(AnalysisCropGeometryDescriptor descriptor,
                                   std::vector<AnalysisCropGeometryRow> rows) {
  AnalysisCropGeometryColumns columns;
  const size_t row_count = rows.size();
  columns.frame_indices.resize(row_count);
  columns.roi_indices.resize(row_count);
  columns.offsets_xy.resize(row_count);
  size_t instance_key_count = 0;
  size_t normalized_box_count = 0;
  size_t roi_box_count = 0;
  for (const auto &row : rows) {
    instance_key_count += row.instance_key.has_value();
    normalized_box_count += row.normalized_detection_cxcywh.has_value();
    roi_box_count += row.roi_bbox_xyxy.has_value();
  }
  if (instance_key_count > 0) {
    columns.instance_keys.resize(row_count);
    if (instance_key_count != row_count) {
      columns.instance_key_valid.assign(row_count, 0);
    }
  }
  if (normalized_box_count > 0) {
    columns.normalized_detection_cxcywh.resize(row_count);
    if (normalized_box_count != row_count) {
      columns.normalized_detection_valid.assign(row_count, 0);
    }
  }
  if (roi_box_count > 0) {
    columns.roi_bbox_xyxy.resize(row_count);
    if (roi_box_count != row_count) {
      columns.roi_bbox_valid.assign(row_count, 0);
    }
  }

  for (size_t index = 0; index < row_count; ++index) {
    const auto &row = rows[index];
    columns.frame_indices[index] = row.camera_frame;
    columns.roi_indices[index] = row.roi_index;
    columns.offsets_xy[index] = {row.offset_x, row.offset_y};
    if (row.instance_key) {
      columns.instance_keys[index] = *row.instance_key;
      if (!columns.instance_key_valid.empty()) {
        columns.instance_key_valid[index] = 1;
      }
    }
    if (row.normalized_detection_cxcywh) {
      columns.normalized_detection_cxcywh[index] =
          *row.normalized_detection_cxcywh;
      if (!columns.normalized_detection_valid.empty()) {
        columns.normalized_detection_valid[index] = 1;
      }
    }
    if (row.roi_bbox_xyxy) {
      columns.roi_bbox_xyxy[index] = *row.roi_bbox_xyxy;
      if (!columns.roi_bbox_valid.empty()) {
        columns.roi_bbox_valid[index] = 1;
      }
    }
  }
  return MakeCompactRepository(std::move(descriptor), std::move(columns),
                               false);
}

std::unique_ptr<AnalysisCropGeometryRepository>
MakeAnalysisCropGeometryRepository(AnalysisCropGeometryDescriptor descriptor,
                                   AnalysisCropGeometryColumns columns) {
  return MakeCompactRepository(std::move(descriptor), std::move(columns), true);
}

} // namespace crimson::zarr
