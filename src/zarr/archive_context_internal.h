#pragma once

#include <tensorstore/context.h>
#include <tensorstore/kvstore/kvstore.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "zarr/archive_context.h"

namespace crimson::zarr {

struct ArchiveContext::Impl {
  std::filesystem::path root_path;
  std::filesystem::path recording_root_path;
  tensorstore::Context context = tensorstore::Context::Default();
  tensorstore::kvstore::KvStore store;
  size_t cache_pool_bytes = 0;
};

namespace internal {

void SetArchiveError(std::string* error_message, std::string message);

std::optional<nlohmann::json> ReadArchiveJson(
    const ArchiveContext::Impl& archive, const std::string& key);

// Retain root attributes/header and complete consolidated metadata for these exact
// run prefixes only. Parsing still reads the serialized root document; this
// avoids constructing a DOM for unrelated products without skipping validation
// of any node under the selected runs. Empty prefixes request root attributes
// only, without retaining any consolidated product subtree.
std::optional<nlohmann::json> ParseArchiveRunMetadata(
    const std::string& payload, const std::vector<std::string>& run_prefixes);
std::optional<nlohmann::json> ReadArchiveRunMetadata(
    const ArchiveContext::Impl& archive,
    const std::vector<std::string>& run_prefixes);

std::optional<nlohmann::json> ReadArchiveAttributes(
    const ArchiveContext::Impl& archive, const std::string& group_path);

inline constexpr size_t kArchiveCachePoolBytes = 64ULL * 1024ULL * 1024ULL;

tensorstore::Result<tensorstore::Context> MakeArchiveTensorStoreContext(
    size_t cache_pool_bytes = kArchiveCachePoolBytes);

std::optional<nlohmann::json> MakeReadOnlyArraySpec(
    const ArchiveContext::Impl& archive, const std::string& path,
    const char* driver = "zarr3");

}  // namespace internal
}  // namespace crimson::zarr
