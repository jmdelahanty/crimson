#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace crimson::windows_path {

// MAX_PATH includes space for the terminating null character. A path with 260
// visible characters is therefore already outside the legacy Win32 limit.
constexpr std::size_t kLegacyMaxPath = 260;
constexpr std::size_t kLegacyMaxUsablePath = kLegacyMaxPath - 1;

std::string composePhysicalPath(const std::string& root,
                                const std::string& relative_path);

bool exceedsLegacyLimit(const std::string& physical_path);

// Reports the application-manifest and system-policy state when a Zarr archive
// is opened. This is a no-op outside Windows.
void logConfigurationForZarrRoot(const std::string& root_path);

// Reports the first over-limit physical path reached for each Zarr root. The
// kvstore spec is expected to describe TensorStore's file kvstore. This is a
// no-op outside Windows.
void noteZarrPathAccess(const nlohmann::json& kvstore_spec,
                        const std::string& relative_path);

}  // namespace crimson::windows_path
