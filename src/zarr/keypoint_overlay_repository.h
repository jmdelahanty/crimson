#pragma once

#include "keypoint_quality_timeline.h"
#include "zarr/repository_memory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace crimson::zarr {

enum class KeypointCoordinateSpace : uint8_t {
  Image,
  Roi,
  NormalizedRoi,
};

struct KeypointOverlayPoint {
  double x = 0.0;
  double y = 0.0;
};

struct KeypointOverlayDescriptor {
  std::string source_group;
  std::string run_name;
  std::string recording_id;
  std::string manifest_digest;
  std::string manifest_payload_digest;
  std::string coordinate_authority;
  std::string source_crop_run;
  KeypointCoordinateSpace coordinate_space = KeypointCoordinateSpace::Image;
  bool refined = false;
  std::vector<std::string> keypoint_labels;
  std::vector<std::array<size_t, 2>> skeleton_edges;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
  bool stable_instance_keys = false;
};

struct KeypointOverlayRow {
  int64_t camera_frame = -1;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  std::vector<KeypointOverlayPoint> keypoints;
  std::optional<double> heading_degrees;
  bool heading_valid = true;
  bool detection_interpolated = false;
  bool refined_keypoints = false;
  bool keypoint_usable = true;
  bool keypoint_detection_interpolated = false;
  bool keypoint_flip_corrected = false;
  std::optional<std::array<double, 4>> normalized_detection_cxcywh;
  std::optional<KeypointOverlayPoint> roi_offset;
  double roi_width = 0.0;
  double roi_height = 0.0;
};

struct KeypointOverlayDetection {
  uint64_t instance_key = 0;
  bool instance_key_valid = false;
  int64_t acquisition_frame = -1;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  std::vector<KeypointOverlayPoint> keypoints;
  std::vector<double> keypoint_confidences;
  std::vector<uint8_t> keypoint_valid;
  double pose_confidence = std::numeric_limits<double>::quiet_NaN();
  std::optional<KeypointOverlayPoint> heading_origin;
  std::optional<double> heading_degrees;
  bool heading_valid = false;
  bool detection_interpolated = false;
  bool refined_keypoints = false;
  bool keypoint_usable = true;
  bool keypoint_detection_interpolated = false;
  bool keypoint_flip_corrected = false;
  bool source_success = false;
  bool refined_success = false;
  bool confidence_valid = false;
  bool geometry_valid = false;
  uint8_t review_state_code = 0;
  uint16_t reason_code = 0;
  std::vector<uint8_t> keypoint_edit_flags;
  bool heading_from_body_frame = false;
  std::optional<std::array<double, 4>> full_frame_box_xywh;
};

enum class KeypointOverlayStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidDimensions,
  ReadFailed,
};

struct KeypointOverlayResolution {
  KeypointOverlayStatus status = KeypointOverlayStatus::Missing;
  int64_t camera_frame = -1;
  std::vector<KeypointOverlayDetection> detections;
  std::string error;
};

class KeypointOverlayRepository {
public:
  virtual ~KeypointOverlayRepository() = default;

  virtual const KeypointOverlayDescriptor &descriptor() const = 0;
  virtual KeypointOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const = 0;
  virtual RepositoryMemoryMetrics memoryMetrics() const { return {}; }
  virtual std::unique_ptr<timeline::KeypointQualityTimelineRepository>
  createQualityTimelineRepository(std::string *error = nullptr) {
    if (error) {
      *error = "Keypoint repository has no v2 quality timeline";
    }
    return nullptr;
  }
};

std::unique_ptr<KeypointOverlayRepository>
MakeKeypointOverlayRepository(KeypointOverlayDescriptor descriptor,
                              std::vector<KeypointOverlayRow> rows);

} // namespace crimson::zarr
