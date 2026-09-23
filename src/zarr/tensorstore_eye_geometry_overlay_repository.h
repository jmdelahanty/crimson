#pragma once

#include "zarr/eye_geometry_overlay_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

std::unique_ptr<EyeGeometryOverlayRepository>
OpenEyeGeometryOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 const std::string &requested_run = {},
                                 std::string *error_message = nullptr);

} // namespace crimson::zarr
