#pragma once

#include "zarr/archive_context.h"

#include <nlohmann/json.hpp>
#include <tensorstore/context.h>
#include <tensorstore/kvstore/kvstore.h>

#include <optional>
#include <string>

namespace crimson::zarr {

struct ArchiveContext::Impl {
  std::filesystem::path root_path;
  std::filesystem::path recording_root_path;
  tensorstore::Context context = tensorstore::Context::Default();
  tensorstore::kvstore::KvStore store;
};

namespace internal {

void SetArchiveError(std::string* error_message, std::string message);

std::optional<nlohmann::json> ReadArchiveJson(
    const ArchiveContext::Impl& archive,
    const std::string& key);

std::optional<nlohmann::json> ReadArchiveAttributes(
    const ArchiveContext::Impl& archive,
    const std::string& group_path);

}  // namespace internal
}  // namespace crimson::zarr
