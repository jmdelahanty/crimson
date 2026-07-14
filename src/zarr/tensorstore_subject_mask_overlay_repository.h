#pragma once

#include "zarr/archive_context.h"
#include "zarr/subject_mask_overlay_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

std::unique_ptr<SubjectMaskOverlayRepository>
OpenSubjectMaskOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 const std::string &requested_run = {},
                                 std::string *error_message = nullptr);

} // namespace crimson::zarr
