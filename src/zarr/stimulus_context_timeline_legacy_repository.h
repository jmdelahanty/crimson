#pragma once

#include "stimulus_context_timeline.h"

#include <memory>

class ZarrDetectionLoader;

namespace crimson::zarr {

// Materializes an owned, read-only portable snapshot from the maintained
// loader. Missing event camera IDs use corrected-first alignment resolution.
std::unique_ptr<timeline::StimulusContextTimelineRepository>
MakeLegacyStimulusContextTimelineRepository(
    const ZarrDetectionLoader& loader);

}  // namespace crimson::zarr
