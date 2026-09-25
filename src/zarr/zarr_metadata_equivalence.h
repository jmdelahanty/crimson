#pragma once

#include <nlohmann/json_fwd.hpp>

namespace crimson::zarr::internal {

bool EquivalentDirectAndConsolidatedZarrNode(
    const nlohmann::json &direct, const nlohmann::json &consolidated);

} // namespace crimson::zarr::internal
