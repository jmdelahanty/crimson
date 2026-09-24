#pragma once

#include "zarr/repository_memory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

using EyeGeometryFieldMask = uint32_t;
namespace EyeGeometryFields {
constexpr EyeGeometryFieldMask LeftGeometry = 1u << 0;
constexpr EyeGeometryFieldMask RightGeometry = 1u << 1;
constexpr EyeGeometryFieldMask BodyFrame = 1u << 2;
constexpr EyeGeometryFieldMask LeftGaze = 1u << 3;
constexpr EyeGeometryFieldMask RightGaze = 1u << 4;
constexpr EyeGeometryFieldMask LeftSigned = 1u << 5;
constexpr EyeGeometryFieldMask RightSigned = 1u << 6;
constexpr EyeGeometryFieldMask LeftAngle = 1u << 7;
constexpr EyeGeometryFieldMask RightAngle = 1u << 8;
constexpr EyeGeometryFieldMask Vergence = 1u << 9;
constexpr EyeGeometryFieldMask All = (1u << 10) - 1;
}

inline EyeGeometryFieldMask NormalizeEyeGeometryFields(EyeGeometryFieldMask fields) {
  fields &= EyeGeometryFields::All;
  if (fields & (EyeGeometryFields::LeftGaze | EyeGeometryFields::LeftSigned |
                EyeGeometryFields::LeftAngle | EyeGeometryFields::Vergence))
    fields |= EyeGeometryFields::LeftGeometry | EyeGeometryFields::BodyFrame;
  if (fields & (EyeGeometryFields::RightGaze | EyeGeometryFields::RightSigned |
                EyeGeometryFields::RightAngle | EyeGeometryFields::Vergence))
    fields |= EyeGeometryFields::RightGeometry | EyeGeometryFields::BodyFrame;
  return fields;
}

inline bool EyeGeometryFieldsCover(EyeGeometryFieldMask loaded,
                                   EyeGeometryFieldMask requested) {
  requested = NormalizeEyeGeometryFields(requested);
  return (loaded & requested) == requested;
}

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
  std::string source_subject_shape_run;
  std::string publication_identity_digest;
  bool validated_instance_keys = false;
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
  uint64_t instance_key = 0;
  bool instance_key_valid = false;
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
  EyeGeometryFieldMask loaded_fields = EyeGeometryFields::All;
};

class EyeGeometryOverlayRepository {
public:
  virtual ~EyeGeometryOverlayRepository() = default;
  virtual const EyeGeometryOverlayDescriptor &descriptor() const = 0;
  virtual EyeGeometryOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const = 0;
  virtual EyeGeometryOverlayResolution resolveCameraFrameFields(
      int64_t camera_frame, int full_frame_width, int full_frame_height,
      EyeGeometryFieldMask fields,
      const std::function<bool()> &cancelled = {}) const {
    if (cancelled && cancelled()) return {};
    auto result = resolveCameraFrame(camera_frame, full_frame_width,
                                     full_frame_height);
    if (result.status == EyeGeometryOverlayStatus::Mapped)
      result.loaded_fields = EyeGeometryFields::All;
    return result;
  }
  virtual RepositoryMemoryMetrics memoryMetrics() const { return {}; }
  struct AccessMetrics {
    struct ArrayReadMetrics {
      std::string array;
      uint64_t calls = 0;
      uint64_t logical_bytes = 0;
      uint64_t failures = 0;
      // Dispatch-to-consumption latency includes batch queueing and earlier
      // future joins; it is not isolated disk, transfer, or decode time.
      uint64_t future_elapsed_count = 0;
      uint64_t future_elapsed_ns_sum = 0;
      uint64_t future_elapsed_ns_max = 0;
    };
    uint64_t payload_read_calls = 0;
    uint64_t logical_payload_bytes_read = 0;
    uint64_t retained_decoded_cache_bytes = 0;
    uint64_t cache_hits = 0;
    uint64_t peak_inflight_payload_reads = 0;
    std::vector<ArrayReadMetrics> per_array;
  };
  virtual AccessMetrics accessMetrics() const { return {}; }
};

std::unique_ptr<EyeGeometryOverlayRepository>
MakeEyeGeometryOverlayRepository(EyeGeometryOverlayDescriptor descriptor,
                                 std::vector<EyeGeometryOverlayRow> rows);

} // namespace crimson::zarr
