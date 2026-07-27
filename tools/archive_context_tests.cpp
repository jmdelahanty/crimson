#include <tensorstore/array.h>
#include <tensorstore/context.h>
#include <tensorstore/internal/metrics/registry.h>
#include <tensorstore/kvstore/spec.h>
#include <tensorstore/open.h>
#include <tensorstore/spec.h>
#include <tensorstore/tensorstore.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>

#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = base / ("crimson-archive-context-" + std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    path_.clear();
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

int64_t CounterValue(std::string_view name) {
  const auto metric = ts::internal_metrics::GetMetricRegistry().Collect(name);
  if (!metric || metric->values.empty()) {
    return 0;
  }
  return std::get<int64_t>(metric->values.front().value);
}

json ShardedArrayMetadata() {
  return {
      {"shape", {4096}},
      {"data_type", "int32"},
      {"chunk_grid",
       {{"name", "regular"}, {"configuration", {{"chunk_shape", {4096}}}}}},
      {"chunk_key_encoding", {{"name", "default"}}},
      {"codecs",
       {{{"name", "sharding_indexed"},
         {"configuration",
          {{"chunk_shape", {1024}},
           {"codecs",
            {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}}},
           {"index_codecs",
            {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}},
             {{"name", "crc32c"}}}},
           {"index_location", "end"}}}}}},
      {"fill_value", 0},
  };
}

bool CreateShardedArray(const std::filesystem::path& root) {
  json spec_json = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", "values"},
      {"metadata", ShardedArrayMetadata()},
  };
  auto store = ts::Open(spec_json, ts::Context::Default(), ts::OpenMode::create)
                   .result();
  if (!store.ok()) {
    std::cerr << "Could not create sharded array: " << store.status() << '\n';
    return false;
  }
  auto values = ts::AllocateArray<int32_t>({4096});
  for (ts::Index i = 0; i < 4096; ++i) {
    values(i) = static_cast<int32_t>(i);
  }
  const auto write = ts::Write(values, *store).result();
  if (!write.ok()) {
    std::cerr << "Could not write sharded array: " << write.status() << '\n';
    return false;
  }
  return true;
}

bool ReadShardedArray(const ts::TensorStore<>& store) {
  auto read = ts::Read(store).result();
  if (!read.ok()) {
    std::cerr << "Could not read sharded array: " << read.status() << '\n';
    return false;
  }
  const auto typed = ts::StaticDataTypeCast<int32_t>(*read);
  return Check(typed.ok(), "Sharded read returned the wrong dtype") &&
         Check((*typed)(0) == 0 && (*typed)(1023) == 1023,
               "Sharded read returned unexpected values");
}

bool TestArchiveCachePolicy(const std::filesystem::path& root) {
  std::string error;
  const auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  bool passed =
      Check(archive != nullptr, "ArchiveContext::Open failed: " + error);
  if (!archive) {
    return false;
  }
  passed &=
      Check(archive->cachePoolBytes() ==
                crimson::zarr::internal::kArchiveCachePoolBytes,
            "ArchiveContext did not retain the configured cache-pool size");

  auto context = crimson::zarr::internal::MakeArchiveTensorStoreContext();
  passed &= Check(context.ok(), "Could not create archive TensorStore context");
  if (!context.ok()) {
    return false;
  }

  auto kvstore_spec = ts::kvstore::Spec::FromJson(
      {{"driver", "file"}, {"path", root.string() + "/"}});
  passed &= Check(kvstore_spec.ok(), "Could not create file kvstore spec");
  if (!kvstore_spec.ok()) {
    return false;
  }
  auto kvstore = ts::kvstore::Open(*kvstore_spec, *context).result();
  passed &= Check(kvstore.ok(), "Could not open file kvstore");
  if (!kvstore.ok()) {
    return false;
  }

  crimson::zarr::ArchiveContext::Impl impl;
  impl.context = *context;
  impl.store = *kvstore;
  impl.cache_pool_bytes = crimson::zarr::internal::kArchiveCachePoolBytes;
  auto spec_json =
      crimson::zarr::internal::MakeReadOnlyArraySpec(impl, "values");
  passed &=
      Check(spec_json.has_value(), "Could not create read-only array spec");
  if (!spec_json) {
    return false;
  }
  passed &= Check((*spec_json)["recheck_cached_metadata"] == "open",
                  "Metadata revalidation is not scoped to array open");
  passed &= Check((*spec_json)["recheck_cached_data"] == "open",
                  "Data revalidation is not scoped to array open");

  auto spec = ts::Spec::FromJson(*spec_json);
  passed &= Check(spec.ok(), "Could not parse read-only array spec");
  if (!spec.ok()) {
    return false;
  }
  auto store = ts::Open(*spec, impl.context, ts::OpenMode::open).result();
  passed &= Check(store.ok(), "Could not open sharded array for reading");
  if (!store.ok()) {
    return false;
  }

  const int64_t reads_before =
      CounterValue("/tensorstore/kvstore/file/batch_read");
  const int64_t bytes_before =
      CounterValue("/tensorstore/kvstore/file/bytes_read");
  passed &= ReadShardedArray(*store);
  const int64_t reads_after_first =
      CounterValue("/tensorstore/kvstore/file/batch_read");
  const int64_t bytes_after_first =
      CounterValue("/tensorstore/kvstore/file/bytes_read");
  passed &= ReadShardedArray(*store);
  const int64_t reads_after_second =
      CounterValue("/tensorstore/kvstore/file/batch_read");
  const int64_t bytes_after_second =
      CounterValue("/tensorstore/kvstore/file/bytes_read");

  passed &= Check(reads_after_first > reads_before,
                  "First sharded read did not reach the file kvstore");
  passed &= Check(bytes_after_first > bytes_before,
                  "First sharded read transferred no file bytes");
  passed &= Check(reads_after_second == reads_after_first,
                  "Repeated sharded read issued another file read");
  passed &= Check(bytes_after_second == bytes_after_first,
                  "Repeated sharded read transferred file bytes again");
  return passed;
}

}  // namespace

int main() {
  TemporaryDirectory temporary_directory;
  if (temporary_directory.path().empty()) {
    std::cerr << "Unable to create a temporary test directory\n";
    return 1;
  }
  if (!CreateShardedArray(temporary_directory.path()) ||
      !TestArchiveCachePolicy(temporary_directory.path())) {
    return 1;
  }
  std::cout << "Archive cache policy and repeated sharded read reuse: PASS\n";
  return 0;
}
