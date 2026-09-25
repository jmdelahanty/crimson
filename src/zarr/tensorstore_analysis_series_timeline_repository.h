#pragma once

#include "analysis_series_timeline.h"
#include "data_access.h"

#include <cstddef>
#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

struct MotionSeriesTimelineOpenRequest {
  size_t frame_count_hint = 0;
  std::string scope = "offline";
  std::string run_name;
  int64_t track_id = -1;
  bool require_source_identity = false;
};

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenMotionSeriesTimelineRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const MotionSeriesTimelineOpenRequest &request,
    std::string *error_message = nullptr,
    crimson::data::SmallSeriesPreloadPolicy preload_policy = {});

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenMotionSeriesTimelineRepository(
    const std::shared_ptr<ArchiveContext> &archive, size_t frame_count_hint,
    std::string *error_message = nullptr,
    crimson::data::SmallSeriesPreloadPolicy preload_policy = {});

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenTailKinematicsTimelineRepository(
    const std::shared_ptr<ArchiveContext> &archive, size_t frame_count_hint,
    const std::string &requested_run = {},
    std::string *error_message = nullptr,
    crimson::data::SmallSeriesPreloadPolicy preload_policy = {});

} // namespace crimson::zarr
