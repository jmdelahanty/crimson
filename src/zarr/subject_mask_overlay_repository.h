#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

enum class SubjectMaskStorage : uint8_t {
  Dense,
  Bitpacked,
  Rle,
};

struct SubjectMaskOverlayPoint {
  double x = 0.0;
  double y = 0.0;
};

struct SubjectMaskOverlayDescriptor {
  std::string source_group;
  std::string run_name;
  std::string source_crop_run;
  std::string label_schema_id;
  SubjectMaskStorage storage = SubjectMaskStorage::Dense;
  std::vector<std::string> component_labels;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
  size_t mask_width = 0;
  size_t mask_height = 0;
};

struct SubjectMaskOverlayComponent {
  std::string label;
  size_t channel_index = 0;
  bool present = false;
  size_t mask_width = 0;
  size_t mask_height = 0;
  std::shared_ptr<const std::vector<uint8_t>> mask;
  std::vector<SubjectMaskOverlayPoint> contour;
};

struct SubjectMaskOverlayRow {
  int64_t camera_frame = -1;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
  std::vector<SubjectMaskOverlayComponent> components;
};

struct SubjectMaskOverlayDetection {
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
  std::vector<SubjectMaskOverlayComponent> components;
};

enum class SubjectMaskOverlayStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidDimensions,
  ReadFailed,
};

struct SubjectMaskOverlayResolution {
  SubjectMaskOverlayStatus status = SubjectMaskOverlayStatus::Missing;
  int64_t camera_frame = -1;
  std::vector<SubjectMaskOverlayDetection> detections;
  std::string error;
};

class SubjectMaskOverlayRepository {
public:
  virtual ~SubjectMaskOverlayRepository() = default;

  virtual const SubjectMaskOverlayDescriptor &descriptor() const = 0;
  virtual SubjectMaskOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const = 0;
};

std::unique_ptr<SubjectMaskOverlayRepository>
MakeSubjectMaskOverlayRepository(SubjectMaskOverlayDescriptor descriptor,
                                 std::vector<SubjectMaskOverlayRow> rows);

} // namespace crimson::zarr
