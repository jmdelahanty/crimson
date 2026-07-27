#pragma once

#include "chaser_distance_polar.h"

#include <memory>

class ZarrDetectionLoader;

namespace crimson::zarr {

// The loader must outlive the returned compatibility repository.
std::unique_ptr<polar::ChaserDistancePolarRepository>
MakeLegacyChaserDistancePolarRepository(
    const ZarrDetectionLoader& loader);

}  // namespace crimson::zarr
