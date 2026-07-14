#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

struct EyeGeometryPoint {
  double x = 0.0;
  double y = 0.0;
};

struct EyeGeometryAxis {
  bool valid = false;
  EyeGeometryPoint start;
  EyeGeometryPoint end;
};

struct EyeGeometryEye {
  bool valid = false;
  EyeGeometryAxis major_axis;
  EyeGeometryAxis minor_axis;
  bool gaze_valid = false;
  EyeGeometryPoint gaze;
  bool signed_angle_valid = false;
  double signed_angle_degrees = 0.0;
  bool eye_frame_angle_valid = false;
  double eye_frame_angle_degrees = 0.0;
};

struct EyeGeometryOverlayDescriptor {
  std::string source_group;
  std::string run_name;
  std::string source_refined_subject_masks_run;
  std::string source_crop_run;
  std::string schema_id;
  int schema_version = 0;
  std::string method;
  std::string method_version;
  std::string row_axis;
  std::string layout;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
  size_t coordinate_width = 0;
  size_t coordinate_height = 0;
};

struct EyeGeometryOverlayRow {
  size_t eye_row = 0;
  int64_t camera_frame = -1;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
  bool frame_valid = false;
  bool body_frame_valid = false;
  EyeGeometryPoint body_origin;
  EyeGeometryPoint body_forward_axis;
  EyeGeometryPoint body_left_axis;
  std::array<EyeGeometryEye, 2> eyes;
  bool vergence_valid = false;
  double vergence_degrees = 0.0;
};

struct EyeGeometryOverlayDetection : EyeGeometryOverlayRow {};

enum class EyeGeometryOverlayStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidDimensions,
  ReadFailed,
};

struct EyeGeometryOverlayResolution {
  EyeGeometryOverlayStatus status = EyeGeometryOverlayStatus::Missing;
  int64_t camera_frame = -1;
  std::vector<EyeGeometryOverlayDetection> detections;
  std::string error;
};

class EyeGeometryOverlayRepository {
public:
  virtual ~EyeGeometryOverlayRepository() = default;
  virtual const EyeGeometryOverlayDescriptor &descriptor() const = 0;
  virtual EyeGeometryOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const = 0;
};

std::unique_ptr<EyeGeometryOverlayRepository>
MakeEyeGeometryOverlayRepository(EyeGeometryOverlayDescriptor descriptor,
                                 std::vector<EyeGeometryOverlayRow> rows);

} // namespace crimson::zarr
