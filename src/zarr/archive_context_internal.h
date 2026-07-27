#pragma once

#include <tensorstore/context.h>
#include <tensorstore/kvstore/kvstore.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

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
