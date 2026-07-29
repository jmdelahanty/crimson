#pragma once

#include "zarr/repository_memory.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

struct SubjectShapeOverlayPoint {
  double x = 0.0;
  double y = 0.0;
};

struct SubjectShapeOverlayDescriptor {
  std::string source_group;
  std::string run_name;
  std::string source_refined_subject_masks_run;
  std::string source_crop_run;
  std::string schema_id;
  int schema_version = 0;
  std::string method;
  int method_version = 0;
  std::string row_axis;
  std::string head_endpoint_semantics;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
  size_t coordinate_width = 0;
  size_t coordinate_height = 0;
  size_t centerline_point_count = 0;
  size_t bspline_sample_point_count = 0;
  size_t bspline_control_point_count = 0;
  size_t tail_sample_point_count = 0;
};

struct SubjectShapeOverlayGeometry {
  bool body_frame_valid = false;
  SubjectShapeOverlayPoint body_origin;
  SubjectShapeOverlayPoint body_forward_axis;
  SubjectShapeOverlayPoint body_left_axis;

  bool snout_tip_valid = false;
  SubjectShapeOverlayPoint snout_tip;
  bool tail_base_valid = false;
  SubjectShapeOverlayPoint tail_base;
  SubjectShapeOverlayPoint tail_tip;
  bool caudal_anchor_valid = false;
  SubjectShapeOverlayPoint caudal_anchor;

  bool centerline_valid = false;
  bool centerline_reaches_snout = false;
  std::vector<SubjectShapeOverlayPoint> centerline;
  bool bspline_valid = false;
  std::vector<SubjectShapeOverlayPoint> bspline_sample;
  std::vector<SubjectShapeOverlayPoint> bspline_control_points;
  bool tail_sample_valid = false;
  std::vector<SubjectShapeOverlayPoint> tail_samples;
  std::vector<SubjectShapeOverlayPoint> tail_normals;
};

struct SubjectShapeOverlayRow {
  size_t shape_row = 0;
  int64_t camera_frame = -1;
  int64_t detection_index = -1;
  int64_t source_refined_row_id = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
  SubjectShapeOverlayGeometry geometry;
};

struct SubjectShapeOverlayDetection {
  size_t shape_row = 0;
  int64_t detection_index = -1;
  int64_t source_refined_row_id = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
  SubjectShapeOverlayGeometry geometry;
};

enum class SubjectShapeOverlayStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
  InvalidDimensions,
  ReadFailed,
};

struct SubjectShapeOverlayResolution {
  SubjectShapeOverlayStatus status = SubjectShapeOverlayStatus::Missing;
  int64_t camera_frame = -1;
  std::vector<SubjectShapeOverlayDetection> detections;
  std::string error;
};

class SubjectShapeOverlayRepository {
public:
  virtual ~SubjectShapeOverlayRepository() = default;

  virtual const SubjectShapeOverlayDescriptor &descriptor() const = 0;
  virtual SubjectShapeOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const = 0;
  virtual RepositoryMemoryMetrics memoryMetrics() const { return {}; }
};

std::unique_ptr<SubjectShapeOverlayRepository>
MakeSubjectShapeOverlayRepository(SubjectShapeOverlayDescriptor descriptor,
                                  std::vector<SubjectShapeOverlayRow> rows);

} // namespace crimson::zarr
