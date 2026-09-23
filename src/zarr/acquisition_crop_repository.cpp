#include "zarr/acquisition_crop_repository.h"

#include <utility>

namespace crimson::zarr {
namespace {

crop::CropFrameGeometry BuildGeometry(
    const AcquisitionCropStreamDescriptor& descriptor,
    int64_t camera_frame,
    const AcquisitionCropFrameRow& row,
    int full_frame_width,
    int full_frame_height) {
  crop::CropFrameGeometry geometry;
  geometry.camera_frame = camera_frame;
  geometry.recording_frame_id = row.recording_frame_id;
  geometry.source_width = full_frame_width;
  geometry.source_height = full_frame_height;
  geometry.output_width = descriptor.output_width;
  geometry.output_height = descriptor.output_height;
  geometry.full_frame_crop = row.full_frame_crop;
  geometry.full_frame_detection = row.full_frame_detection;
  geometry.geometry_available = row.has_detection && !row.blank_frame;
  geometry.has_detection = row.has_detection;
  geometry.blank_frame = row.blank_frame;
  return geometry;
}

class VectorAcquisitionCropRepository final
    : public AcquisitionCropRepository {
 public:
  VectorAcquisitionCropRepository(
      AcquisitionCropStreamDescriptor descriptor,
      std::vector<AcquisitionCropFrameRow> rows)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)) {}

  const AcquisitionCropStreamDescriptor& descriptor() const override {
    return descriptor_;
  }

  size_t cameraFrameCount() const override { return rows_.size(); }

  AcquisitionCropFrameResolution resolveCameraFrame(
      int64_t camera_frame) const override {
    AcquisitionCropFrameResolution resolution;
    resolution.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= rows_.size()) {
      return resolution;
    }
    resolution.status = AcquisitionCropMappingStatus::Mapped;
    resolution.video_frame = camera_frame;
    resolution.metadata_row = camera_frame;
    resolution.row = rows_[static_cast<size_t>(camera_frame)];
    return resolution;
  }

  crop::CropSourceCapabilities sourceCapabilities() const override {
    crop::CropSourceCapabilities capabilities;
    capabilities.live_geometry = !rows_.empty();
    capabilities.acquisition_video = !rows_.empty();
    return capabilities;
  }

  crop::AcquisitionCropFrameState acquisitionFrameState(
      int64_t camera_frame,
      std::optional<int64_t> decoded_video_frame,
      int full_frame_width,
      int full_frame_height) const override {
    crop::AcquisitionCropFrameState state;
    const auto resolution = resolveCameraFrame(camera_frame);
    if (resolution.status != AcquisitionCropMappingStatus::Mapped ||
        !resolution.video_frame || !resolution.row) {
      return state;
    }
    state.resolved_camera_frame = camera_frame;
    state.mapped_video_frame = resolution.video_frame;
    state.decoded_video_frame = decoded_video_frame;
    state.blank_frame = resolution.row->blank_frame;
    state.geometry = BuildGeometry(descriptor_, camera_frame,
                                   *resolution.row, full_frame_width,
                                   full_frame_height);
    return state;
  }

  std::optional<crop::CropFrameGeometry> liveGeometry(
      int64_t camera_frame,
      int full_frame_width,
      int full_frame_height) const override {
    const auto resolution = resolveCameraFrame(camera_frame);
    if (resolution.status != AcquisitionCropMappingStatus::Mapped ||
        !resolution.row) {
      return std::nullopt;
    }
    return BuildGeometry(descriptor_, camera_frame, *resolution.row,
                         full_frame_width, full_frame_height);
  }

 private:
  AcquisitionCropStreamDescriptor descriptor_;
  std::vector<AcquisitionCropFrameRow> rows_;
};

}  // namespace

std::unique_ptr<AcquisitionCropRepository> MakeAcquisitionCropRepository(
    AcquisitionCropStreamDescriptor descriptor,
    std::vector<AcquisitionCropFrameRow> rows) {
  return std::make_unique<VectorAcquisitionCropRepository>(
      std::move(descriptor), std::move(rows));
}

}  // namespace crimson::zarr
