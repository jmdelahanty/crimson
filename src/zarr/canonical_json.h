#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace crimson::zarr {

bool IsLowerSha256(const std::string &value);
std::string CanonicalJsonSha256(const nlohmann::json &value);

} // namespace crimson::zarr
