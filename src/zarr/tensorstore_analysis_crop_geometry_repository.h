#pragma once

#include "zarr/analysis_crop_geometry_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

std::unique_ptr<AnalysisCropGeometryRepository>
OpenAnalysisCropGeometryRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run = {},
    std::string* error_message = nullptr);

}  // namespace crimson::zarr
