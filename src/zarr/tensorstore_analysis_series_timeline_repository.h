#pragma once

#include "analysis_series_timeline.h"

#include <cstddef>
#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenMotionSeriesTimelineRepository(
    const std::shared_ptr<ArchiveContext> &archive, size_t frame_count_hint,
    std::string *error_message = nullptr);

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenTailKinematicsTimelineRepository(
    const std::shared_ptr<ArchiveContext> &archive, size_t frame_count_hint,
    const std::string &requested_run = {},
    std::string *error_message = nullptr);

} // namespace crimson::zarr
