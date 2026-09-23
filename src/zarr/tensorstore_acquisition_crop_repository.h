#pragma once

#include "zarr/acquisition_crop_repository.h"
#include "zarr/archive_context.h"

#include <memory>
#include <string>

namespace crimson::zarr {

std::unique_ptr<AcquisitionCropRepository>
OpenAcquisitionCropRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    std::string* error_message = nullptr);

}  // namespace crimson::zarr
