#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
  std::string source_crop_run;
  KeypointCoordinateSpace coordinate_space = KeypointCoordinateSpace::Image;
  bool refined = false;
  std::vector<std::string> keypoint_labels;
  std::vector<std::array<size_t, 2>> skeleton_edges;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
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
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  std::vector<KeypointOverlayPoint> keypoints;
  std::optional<KeypointOverlayPoint> heading_origin;
  std::optional<double> heading_degrees;
  bool heading_valid = false;
  bool detection_interpolated = false;
  bool refined_keypoints = false;
  bool keypoint_usable = true;
  bool keypoint_detection_interpolated = false;
  bool keypoint_flip_corrected = false;
  std::optional<std::array<double, 4>> full_frame_box_xywh;
};

enum class KeypointOverlayStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidDimensions,
};

struct KeypointOverlayResolution {
  KeypointOverlayStatus status = KeypointOverlayStatus::Missing;
  int64_t camera_frame = -1;
  std::vector<KeypointOverlayDetection> detections;
};

class KeypointOverlayRepository {
 public:
  virtual ~KeypointOverlayRepository() = default;

  virtual const KeypointOverlayDescriptor& descriptor() const = 0;
  virtual KeypointOverlayResolution resolveCameraFrame(
      int64_t camera_frame,
      int full_frame_width,
      int full_frame_height) const = 0;
};

std::unique_ptr<KeypointOverlayRepository> MakeKeypointOverlayRepository(
    KeypointOverlayDescriptor descriptor,
    std::vector<KeypointOverlayRow> rows);

}  // namespace crimson::zarr
