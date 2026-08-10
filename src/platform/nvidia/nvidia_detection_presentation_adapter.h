#pragma once

#include "h5_loader.h"
#include "zarr/detection_repository.h"

#include <vector>

namespace crimson::platform::nvidia {

std::vector<LoggedBoundingBox> makeLegacyBoundingBoxes(
    const crimson::zarr::DetectionRepositoryDescriptor &descriptor,
    const crimson::zarr::DetectionFrame &frame);

} // namespace crimson::platform::nvidia
