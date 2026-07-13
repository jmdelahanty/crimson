#pragma once

#include "crop_source_contract.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace crimson::zarr {

struct AcquisitionCropStreamDescriptor {
  std::string schema_id;
  int schema_version = 0;
  std::string source_schema_id;
  std::string stream_key;
  std::string stream_id;
  std::string camera_id;
  std::string frame_clock;
  std::string video_pixel_coordinate_space;
  std::string source_geometry_coordinate_space;
  std::string blank_frame_policy;
  std::string selection_policy;
  int output_width = 0;
  int output_height = 0;
  int64_t frame_count = 0;
  double frame_rate = 0.0;
  std::string codec;
  std::string container;
  std::string encoded_format;
  std::string pixel_source_format;
  std::filesystem::path stored_video_path;
  std::filesystem::path resolved_video_path;
  std::filesystem::path stored_metadata_path;
  std::filesystem::path resolved_metadata_path;
  std::filesystem::path stored_keyframes_path;
  std::filesystem::path resolved_keyframes_path;
  std::filesystem::path stored_summary_path;
  std::filesystem::path resolved_summary_path;
  std::filesystem::path stored_status_path;
  std::filesystem::path resolved_status_path;
  int64_t keyframe_count = 0;
  int64_t first_keyframe = -1;
  int64_t last_keyframe = -1;
};

struct AcquisitionCropFrameRow {
  int64_t recording_frame_id = -1;
  int64_t local_frame_id = -1;
  int64_t camera_frame_id = -1;
  int64_t timestamp = 0;
  int64_t timestamp_sys = 0;
  bool has_detection = false;
  bool blank_frame = false;
  double detection_confidence = 0.0;
  crop::CropRect full_frame_crop;
  std::optional<crop::CropRect> full_frame_detection;
};

enum class AcquisitionCropMappingStatus : uint8_t {
  Mapped,
  OutOfRange,
};

struct AcquisitionCropFrameResolution {
  AcquisitionCropMappingStatus status =
      AcquisitionCropMappingStatus::OutOfRange;
  int64_t camera_frame = -1;
  std::optional<int64_t> video_frame;
  std::optional<int64_t> metadata_row;
  std::optional<AcquisitionCropFrameRow> row;
};

class AcquisitionCropRepository {
 public:
  virtual ~AcquisitionCropRepository() = default;

  virtual const AcquisitionCropStreamDescriptor& descriptor() const = 0;
  virtual size_t cameraFrameCount() const = 0;
  virtual AcquisitionCropFrameResolution resolveCameraFrame(
      int64_t camera_frame) const = 0;
  virtual crop::CropSourceCapabilities sourceCapabilities() const = 0;
  virtual crop::AcquisitionCropFrameState acquisitionFrameState(
      int64_t camera_frame,
      std::optional<int64_t> decoded_video_frame,
      int full_frame_width,
      int full_frame_height) const = 0;
  virtual std::optional<crop::CropFrameGeometry> liveGeometry(
      int64_t camera_frame,
      int full_frame_width,
      int full_frame_height) const = 0;
};

std::unique_ptr<AcquisitionCropRepository> MakeAcquisitionCropRepository(
    AcquisitionCropStreamDescriptor descriptor,
    std::vector<AcquisitionCropFrameRow> rows);

}  // namespace crimson::zarr
