#pragma once

#include "stimulus_context_timeline.h"
#include "zarr/archive_context.h"

#include <cstddef>
#include <memory>
#include <string>

namespace crimson::zarr {

std::unique_ptr<crimson::timeline::StimulusContextTimelineRepository>
OpenStimulusContextTimelineRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    std::size_t frame_count_hint = 0,
    const std::string& requested_run = {},
    std::string* error_message = nullptr);

}  // namespace crimson::zarr
