#pragma once

#include "data_access.h"
#include "eye_angle_timeline.h"

#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository>
OpenEyeAngleTimelineRepository(const std::shared_ptr<ArchiveContext>& archive,
                               const std::string& requested_run = {},
                               std::string* error_message = nullptr,
                               crimson::data::SmallSeriesPreloadPolicy
                                   preload_policy = {});

}  // namespace crimson::zarr
