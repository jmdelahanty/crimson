#include "zarr/canonical_json.h"

#include <tensorstore/internal/digest/sha256.h>

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

bool strictJson(const nlohmann::json &value) {
  if (value.is_number_float() && !std::isfinite(value.get<double>())) {
    return false;
  }
  if (value.is_array()) {
    return std::all_of(value.begin(), value.end(), strictJson);
  }
  if (value.is_object()) {
    return std::all_of(value.begin(), value.end(),
                       [](const auto &item) { return strictJson(item); });
  }
  return true;
}

} // namespace

bool IsLowerSha256(const std::string &value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::string CanonicalJsonSha256(const nlohmann::json &value) {
  if (!strictJson(value)) {
    return {};
  }
  return Sha256Hex(value.dump());
}

std::string Sha256Hex(std::string_view value) {
  tensorstore::internal::SHA256Digester digester;
  digester.Write(value);
  const auto digest = digester.Digest();
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded(digest.size() * 2, '0');
  for (size_t index = 0; index < digest.size(); ++index) {
    encoded[index * 2] = kHex[digest[index] >> 4];
    encoded[index * 2 + 1] = kHex[digest[index] & 0x0f];
  }
  return encoded;
}

} // namespace crimson::zarr
