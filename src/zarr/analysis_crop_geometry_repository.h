#pragma once

#include "crop_source_contract.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace crimson::zarr {

struct AnalysisCropGeometryDescriptor {
  std::string run_name;
  int output_width = 0;
  int output_height = 0;
  size_t row_count = 0;
  size_t camera_frame_count = 0;
};

struct AnalysisCropGeometryRow {
  int64_t camera_frame = -1;
  int64_t roi_index = -1;
  double offset_x = 0.0;
  double offset_y = 0.0;
  std::optional<std::array<double, 4>> normalized_detection_cxcywh;
};

enum class AnalysisCropGeometryStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
};

struct AnalysisCropGeometryResolution {
  AnalysisCropGeometryStatus status = AnalysisCropGeometryStatus::Missing;
  int64_t camera_frame = -1;
  std::optional<int64_t> roi_index;
  size_t frame_row_count = 0;
  std::optional<crop::CropFrameGeometry> geometry;
};

class AnalysisCropGeometryRepository {
 public:
  virtual ~AnalysisCropGeometryRepository() = default;

  virtual const AnalysisCropGeometryDescriptor& descriptor() const = 0;
  virtual crop::CropSourceCapabilities sourceCapabilities() const = 0;
  virtual AnalysisCropGeometryResolution resolveCameraFrame(
      int64_t camera_frame,
      int full_frame_width,
      int full_frame_height) const = 0;
};

std::unique_ptr<AnalysisCropGeometryRepository>
MakeAnalysisCropGeometryRepository(
    AnalysisCropGeometryDescriptor descriptor,
    std::vector<AnalysisCropGeometryRow> rows);

}  // namespace crimson::zarr
