#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

namespace crimson::zarr {

bool IsLowerSha256(const std::string &value);
std::string Sha256Hex(std::string_view value);
std::string CanonicalJsonSha256(const nlohmann::json &value);

} // namespace crimson::zarr
