#pragma once

#include "detection_quality_timeline.h"

#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

struct DetectionQualityTimelineOpenRequest {
  DetectionSurfaceKind surface_kind = DetectionSurfaceKind::CanonicalRawV1;
  std::string run_name;
  bool allow_selector_ineligible_refined_run = false;
};

struct DetectionQualityTimelineOpenMetrics {
  double total_ms = 0.0;
  double metadata_ms = 0.0;
  double exact_handle_open_ms = 0.0;
  double offset_read_ms = 0.0;
  size_t root_metadata_reads = 0;
  size_t direct_group_metadata_reads = 0;
  size_t exact_handle_opens = 0;
  size_t offset_read_calls = 0;
  size_t retained_offset_bytes = 0;
};

std::unique_ptr<timeline::DetectionQualityTimelineRepository>
OpenDetectionQualityTimelineRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const DetectionQualityTimelineOpenRequest &request,
    std::string *error_message = nullptr,
    DetectionQualityTimelineOpenMetrics *open_metrics = nullptr);

} // namespace crimson::zarr
