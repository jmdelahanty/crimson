#pragma once

#include "zarr/archive_context.h"
#include "zarr/stimulus_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

std::unique_ptr<StimulusRepository> OpenStimulusRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run = {},
    std::string* error_message = nullptr);

}  // namespace crimson::zarr
