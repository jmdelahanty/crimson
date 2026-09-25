#pragma once

#include "zarr/subject_shape_overlay_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

std::unique_ptr<SubjectShapeOverlayRepository>
OpenSubjectShapeOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                  const std::string &requested_run = {},
                                  std::string *error_message = nullptr);

} // namespace crimson::zarr
