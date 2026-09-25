#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/internal/metrics/registry.h>
#include <tensorstore/kvstore/spec.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data_access_scheduler.h"
#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"
#include "zarr/crop_geometry_contract.h"
#include "zarr/zarr_metadata_equivalence.h"

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif
#ifndef CRIMSON_WORKTREE_DIRTY
#define CRIMSON_WORKTREE_DIRTY 1
#endif

namespace {
namespace ts = tensorstore;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

constexpr std::string_view kSchemaId =
    "crimson.crop_geometry_v2_read_benchmark";
constexpr int kSchemaVersion = 1;
constexpr size_t kDefaultCacheBytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t kDefaultRandomSeekCount = 128;
constexpr size_t kDefaultCancellationSeekCount = 32;
constexpr size_t kSequentialWindowFrames = 70;
constexpr uint64_t kRandomSeed = 20260729;

struct Options {
  bool self_test = false;
  std::filesystem::path store_path;
  std::string run_name;
  std::filesystem::path output_path;
  size_t cache_bytes = kDefaultCacheBytes;
  size_t random_seek_count = kDefaultRandomSeekCount;
  size_t cancellation_seek_count = kDefaultCancellationSeekCount;
};

struct TensorStoreMetrics {
  int64_t file_reads = 0;
  int64_t file_batch_reads = 0;
  int64_t file_bytes = 0;
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;
  int64_t cache_evictions = 0;
};

struct ArrayDeclaration {
  const char *relative_path;
  const char *dtype;
  size_t rank;
  size_t columns;
};

constexpr ArrayDeclaration kDeclarations[] = {
    {"instance_key", "uint64", 1, 1},
    {"source_refined_row_ids", "int64", 1, 1},
    {"frame_indices", "int64", 1, 1},
    {"source_acquisition_frame_index", "int64", 1, 1},
    {"frame_row_offsets", "int64", 1, 1},
    {"bbox_norm_coords", "float32", 2, 4},
    {"bbox_img_xyxy", "float32", 2, 4},
    {"centers_img_xy", "float32", 2, 2},
    {"roi_coordinates_full", "int32", 2, 2},
    {"roi_sizes_full", "int32", 2, 2},
    {"source_crop_xywh", "float32", 2, 4},
    {"bbox_roi_xyxy", "float32", 2, 4},
    {"source_row_signature", "uint8", 2, 32},
};

struct CropStores {
  ts::TensorStore<uint64_t, 1> instance_key;
  ts::TensorStore<int64_t, 1> source_refined_row_ids;
  ts::TensorStore<int64_t, 1> frame_indices;
  ts::TensorStore<int64_t, 1> source_acquisition_frame_index;
  std::optional<ts::TensorStore<int64_t, 1>> frame_row_offsets;
  ts::TensorStore<float, 2> bbox_norm_coords;
  ts::TensorStore<float, 2> bbox_img_xyxy;
  ts::TensorStore<float, 2> centers_img_xy;
  ts::TensorStore<int32_t, 2> roi_coordinates_full;
  ts::TensorStore<int32_t, 2> roi_sizes_full;
  ts::TensorStore<float, 2> source_crop_xywh;
  ts::TensorStore<float, 2> bbox_roi_xyxy;
  ts::TensorStore<uint8_t, 2> source_row_signature;
};

struct FrameRowRange {
  size_t first = 0;
  size_t last = 0;

  size_t size() const { return last - first; }
};

struct Digest {
  template <typename T> void scalar(const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    write(std::string_view(reinterpret_cast<const char *>(&value),
                           sizeof(value)));
  }

  void label(std::string_view value) {
    scalar(value.size());
    write(value);
  }

  std::string finish() const {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint64_t state : states_) {
      output << std::setw(16) << state;
    }
    return output.str();
  }

private:
  void write(std::string_view bytes) {
    constexpr std::array<uint64_t, 4> primes = {
        1099511628211ULL, 1099511627791ULL, 1099511627689ULL, 1099511627563ULL};
    for (char byte : bytes) {
      const auto value = static_cast<uint8_t>(byte);
      for (size_t lane = 0; lane < states_.size(); ++lane) {
        states_[lane] ^= static_cast<uint64_t>(value + lane * 37U);
        states_[lane] *= primes[lane];
      }
    }
  }

  std::array<uint64_t, 4> states_ = {1469598103934665603ULL, 1099511628211ULL,
                                     7809847782465536322ULL,
                                     9650029242287828579ULL};
};

double elapsedMs(Clock::time_point start, Clock::time_point stop) {
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<size_t>(
      std::max(0.0, std::ceil(fraction * values.size()) - 1.0));
  return values[std::min(rank, values.size() - 1)];
}

int64_t counterValue(std::string_view name) {
  const auto metric = ts::internal_metrics::GetMetricRegistry().Collect(name);
  if (!metric || metric->values.empty()) {
    return 0;
  }
  return std::get<int64_t>(metric->values.front().value);
}

TensorStoreMetrics snapshotMetrics() {
  return {
      counterValue("/tensorstore/kvstore/file/read"),
      counterValue("/tensorstore/kvstore/file/batch_read"),
      counterValue("/tensorstore/kvstore/file/bytes_read"),
      counterValue("/tensorstore/cache/hit_count"),
      counterValue("/tensorstore/cache/miss_count"),
      counterValue("/tensorstore/cache/evict_count"),
  };
}

TensorStoreMetrics operator-(const TensorStoreMetrics &after,
                             const TensorStoreMetrics &before) {
  return {after.file_reads - before.file_reads,
          after.file_batch_reads - before.file_batch_reads,
          after.file_bytes - before.file_bytes,
          after.cache_hits - before.cache_hits,
          after.cache_misses - before.cache_misses,
          after.cache_evictions - before.cache_evictions};
}

json metricsJson(const TensorStoreMetrics &metrics) {
  return {{"file_reads", metrics.file_reads},
          {"file_batch_reads", metrics.file_batch_reads},
          {"file_bytes", metrics.file_bytes},
          {"cache_hits", metrics.cache_hits},
          {"cache_misses", metrics.cache_misses},
          {"cache_evictions", metrics.cache_evictions}};
}

uint64_t peakRssBytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

bool require(bool condition, std::string message, std::string *error) {
  if (!condition && error) {
    *error = std::move(message);
  }
  return condition;
}

std::optional<size_t> parseSize(std::string_view value, std::string_view name,
                                std::string *error) {
  try {
    const auto parsed = std::stoull(std::string(value));
    if (parsed > std::numeric_limits<size_t>::max()) {
      throw std::out_of_range("size_t");
    }
    return static_cast<size_t>(parsed);
  } catch (const std::exception &exception) {
    if (error) {
      *error = "Invalid " + std::string(name) +
               " value: " + std::string(exception.what());
    }
    return std::nullopt;
  }
}

std::optional<Options> parseOptions(int argc, char **argv, std::string *error) {
  Options options;
  if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
    options.self_test = true;
    return options;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    auto next = [&](std::string_view name) -> std::optional<std::string_view> {
      if (index + 1 >= argc) {
        if (error) {
          *error = "Missing value for " + std::string(name);
        }
        return std::nullopt;
      }
      return std::string_view(argv[++index]);
    };
    if (argument == "--store") {
      const auto value = next(argument);
      if (!value) {
        return std::nullopt;
      }
      options.store_path = *value;
    } else if (argument == "--run") {
      const auto value = next(argument);
      if (!value) {
        return std::nullopt;
      }
      options.run_name = *value;
    } else if (argument == "--output") {
      const auto value = next(argument);
      if (!value) {
        return std::nullopt;
      }
      options.output_path = *value;
    } else if (argument == "--cache-bytes" || argument == "--random-seeks" ||
               argument == "--cancellation-seeks") {
      const auto value = next(argument);
      if (!value) {
        return std::nullopt;
      }
      const auto parsed = parseSize(*value, argument, error);
      if (!parsed) {
        return std::nullopt;
      }
      if (argument == "--cache-bytes") {
        options.cache_bytes = *parsed;
      } else if (argument == "--random-seeks") {
        options.random_seek_count = *parsed;
      } else {
        options.cancellation_seek_count = *parsed;
      }
    } else {
      if (error) {
        *error = "Unknown argument: " + std::string(argument);
      }
      return std::nullopt;
    }
  }
  if (options.store_path.empty() || options.run_name.empty() ||
      options.run_name.find('/') != std::string::npos ||
      options.random_seek_count == 0 || options.cancellation_seek_count == 0) {
    if (error) {
      *error = "--store, a simple --run name, and nonzero workload counts are "
               "required";
    }
    return std::nullopt;
  }
  return options;
}

std::optional<crimson::zarr::ArchiveContext::Impl>
openContext(const Options &options, std::string *error) {
  auto context = crimson::zarr::internal::MakeArchiveTensorStoreContext(
      options.cache_bytes);
  if (!context.ok()) {
    if (error) {
      *error = context.status().ToString();
    }
    return std::nullopt;
  }
  auto spec = ts::kvstore::Spec::FromJson(
      {{"driver", "file"}, {"path", options.store_path.string() + "/"}});
  if (!spec.ok()) {
    if (error) {
      *error = spec.status().ToString();
    }
    return std::nullopt;
  }
  auto store = ts::kvstore::Open(*spec, *context).result();
  if (!store.ok()) {
    if (error) {
      *error = store.status().ToString();
    }
    return std::nullopt;
  }
  crimson::zarr::ArchiveContext::Impl impl;
  impl.root_path = options.store_path;
  impl.context = *context;
  impl.store = *store;
  impl.cache_pool_bytes = options.cache_bytes;
  return impl;
}

const json *consolidatedEntry(const json &root, std::string_view path) {
  try {
    const auto &metadata = root.at("consolidated_metadata").at("metadata");
    const auto found = metadata.find(std::string(path));
    return found == metadata.end() ? nullptr : &*found;
  } catch (const json::exception &) {
    return nullptr;
  }
}

std::optional<crimson::zarr::CropGeometryManifestSummary>
validateMetadata(const crimson::zarr::ArchiveContext::Impl &impl,
                 const json &root, const Options &options, json *output,
                 std::string *error) {
  const std::string base = "crop_runs/" + options.run_name;
  try {
    if (!require(
            root.at("zarr_format") == 3 && root.at("node_type") == "group" &&
                root.at("consolidated_metadata").at("kind") == "inline",
            "Archive is not an inline-consolidated Zarr v3 group", error) ||
        !require(root.at("attributes").value("benchmark_only", false) &&
                     !root.at("attributes").value("selector_eligible", true),
                 "Archive is not selector-ineligible benchmark evidence",
                 error)) {
      return std::nullopt;
    }
    const auto *run = consolidatedEntry(root, base);
    if (!require(run != nullptr, "Consolidated crop run is missing", error)) {
      return std::nullopt;
    }
    crimson::zarr::CropGeometryManifestSummary manifest;
    if (!crimson::zarr::ValidateCropGeometryRunManifest(
            run->at("attributes").at("run_manifest"), options.run_name,
            &manifest, error) ||
        !require(!manifest.selector_eligible,
                 "Benchmark crop run is selector eligible", error)) {
      return std::nullopt;
    }

    size_t direct_array_reads = 0;
    size_t direct_group_reads = 0;
    for (const std::string &group :
         std::array<std::string, 2>{"crop_runs", base}) {
      const auto direct =
          crimson::zarr::internal::ReadArchiveJson(impl, group + "/zarr.json");
      const auto *consolidated = consolidatedEntry(root, group);
      if (!require(direct && consolidated &&
                       crimson::zarr::internal::
                           EquivalentDirectAndConsolidatedZarrNode(
                               *direct, *consolidated),
                   "Direct/consolidated crop group mismatch: " + group,
                   error)) {
        return std::nullopt;
      }
      ++direct_group_reads;
    }

    for (const auto &declaration : kDeclarations) {
      const std::string path = base + "/" + declaration.relative_path;
      const auto *metadata = consolidatedEntry(root, path);
      const auto direct =
          crimson::zarr::internal::ReadArchiveJson(impl, path + "/zarr.json");
      if (!require(metadata != nullptr && direct && *direct == *metadata,
                   "Direct/consolidated crop array mismatch: " + path, error)) {
        return std::nullopt;
      }
      const auto shape = metadata->at("shape").get<std::vector<size_t>>();
      const bool offsets =
          std::string_view(declaration.relative_path) == "frame_row_offsets";
      const size_t expected_rows =
          offsets ? manifest.frame_count + 1 : manifest.instance_count;
      if (!require(
              metadata->value("node_type", "") == "array" &&
                  !metadata->contains("consolidated_metadata") &&
                  metadata->value("data_type", "") == declaration.dtype &&
                  shape.size() == declaration.rank &&
                  shape[0] == expected_rows &&
                  (declaration.rank == 1 || shape[1] == declaration.columns),
              "Exact crop array declaration mismatch: " + path, error)) {
        return std::nullopt;
      }
      ++direct_array_reads;
    }

    const bool roi_images_consolidated =
        consolidatedEntry(root, base + "/roi_images") != nullptr;
    const bool roi_images_delta_consolidated =
        consolidatedEntry(root, base + "/roi_images_delta") != nullptr;
    if (!require(!roi_images_consolidated && !roi_images_delta_consolidated,
                 "Geometry-only crop unexpectedly declares pixel arrays",
                 error)) {
      return std::nullopt;
    }
    *output = {
        {"manifest_schema", "palette.crop_geometry.run_manifest:2"},
        {"manifest_payload_digest", manifest.payload_digest},
        {"consolidated_entries",
         root.at("consolidated_metadata").at("metadata").size()},
        {"direct_array_reads", direct_array_reads},
        {"direct_group_reads", direct_group_reads},
        {"array_declarations_validated", std::size(kDeclarations)},
        {"direct_consolidated_equivalent", true},
        {"roi_images_consolidated", false},
        {"roi_images_delta_consolidated", false},
    };
    return manifest;
  } catch (const json::exception &exception) {
    if (error) {
      *error = "Invalid crop metadata: " + std::string(exception.what());
    }
    return std::nullopt;
  }
}

template <typename T, ts::DimensionIndex Rank>
std::optional<ts::TensorStore<T, Rank>>
openExact(const crimson::zarr::ArchiveContext::Impl &impl, const json &root,
          const std::string &path, std::vector<std::string> *open_paths,
          std::string *error) {
  open_paths->push_back(path);
  auto spec = crimson::zarr::internal::MakeReadOnlyArraySpec(impl, path);
  const auto *metadata = consolidatedEntry(root, path);
  if (!spec || !metadata) {
    if (error) {
      *error = "Missing exact array metadata: " + path;
    }
    return std::nullopt;
  }
  (*spec)["metadata"] = *metadata;
  auto opened = ts::Open<T, Rank>(
                    *spec, ts::OpenMode::open | ts::OpenMode::assume_metadata,
                    ts::ReadWriteMode::read, impl.context)
                    .result();
  if (!opened.ok()) {
    if (error) {
      *error = path + ": " + opened.status().ToString();
    }
    return std::nullopt;
  }
  return *opened;
}

std::optional<CropStores>
openStores(const crimson::zarr::ArchiveContext::Impl &impl, const json &root,
           const Options &options, json *output, std::string *error) {
  const std::string base = "crop_runs/" + options.run_name + "/";
  std::vector<std::string> paths;
  auto instance_key =
      openExact<uint64_t, 1>(impl, root, base + "instance_key", &paths, error);
  auto source_rows = openExact<int64_t, 1>(
      impl, root, base + "source_refined_row_ids", &paths, error);
  auto frames =
      openExact<int64_t, 1>(impl, root, base + "frame_indices", &paths, error);
  auto source_frames = openExact<int64_t, 1>(
      impl, root, base + "source_acquisition_frame_index", &paths, error);
  auto offsets = openExact<int64_t, 1>(impl, root, base + "frame_row_offsets",
                                       &paths, error);
  auto bbox_norm =
      openExact<float, 2>(impl, root, base + "bbox_norm_coords", &paths, error);
  auto bbox_img =
      openExact<float, 2>(impl, root, base + "bbox_img_xyxy", &paths, error);
  auto centers =
      openExact<float, 2>(impl, root, base + "centers_img_xy", &paths, error);
  auto roi_coordinates = openExact<int32_t, 2>(
      impl, root, base + "roi_coordinates_full", &paths, error);
  auto roi_sizes =
      openExact<int32_t, 2>(impl, root, base + "roi_sizes_full", &paths, error);
  auto source_crop =
      openExact<float, 2>(impl, root, base + "source_crop_xywh", &paths, error);
  auto bbox_roi =
      openExact<float, 2>(impl, root, base + "bbox_roi_xyxy", &paths, error);
  auto signature = openExact<uint8_t, 2>(
      impl, root, base + "source_row_signature", &paths, error);
  if (!instance_key || !source_rows || !frames || !source_frames || !offsets ||
      !bbox_norm || !bbox_img || !centers || !roi_coordinates || !roi_sizes ||
      !source_crop || !bbox_roi || !signature) {
    return std::nullopt;
  }
  const bool opened_pixels =
      std::any_of(paths.begin(), paths.end(), [](const std::string &path) {
        return path.find("roi_images") != std::string::npos;
      });
  if (!require(!opened_pixels && paths.size() == std::size(kDeclarations),
               "Exact open path inventory is invalid", error)) {
    return std::nullopt;
  }
  *output = {{"typed_open_attempts", paths.size()},
             {"fallback_open_attempts", 0},
             {"metadata_source", "validated_inline_assume_metadata"},
             {"opened_paths", paths},
             {"roi_images_open_attempts", 0},
             {"roi_images_delta_open_attempts", 0}};
  return CropStores{
      *instance_key, *source_rows, *frames,   *source_frames,   *offsets,
      *bbox_norm,    *bbox_img,    *centers,  *roi_coordinates, *roi_sizes,
      *source_crop,  *bbox_roi,    *signature};
}

template <typename T, ts::DimensionIndex Rank, typename Visitor>
std::optional<double> readRows(const ts::TensorStore<T, Rank> &store,
                               size_t first, size_t last, Visitor visitor,
                               size_t *logical_bytes, std::string *error) {
  if (last < first || last > static_cast<size_t>(store.domain().shape()[0])) {
    if (error) {
      *error = "Invalid TensorStore row range";
    }
    return std::nullopt;
  }
  const size_t columns =
      Rank == 1 ? 1 : static_cast<size_t>(store.domain().shape()[1]);
  if (first == last) {
    return 0.0;
  }
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  const auto started = Clock::now();
  auto read = ts::Read(store | ts::IdentityTransform(domain)).result();
  const auto stopped = Clock::now();
  if (!read.ok() || read->rank() != Rank ||
      read->byte_strides().size() != Rank) {
    if (error) {
      *error = read.ok() ? "Unexpected TensorStore result shape"
                         : read.status().ToString();
    }
    return std::nullopt;
  }
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < last - first; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      ts::Index offset = static_cast<ts::Index>(row) * read->byte_strides()[0];
      if constexpr (Rank == 2) {
        offset += static_cast<ts::Index>(column) * read->byte_strides()[1];
      }
      visitor(row, column, *reinterpret_cast<const T *>(origin + offset));
    }
  }
  *logical_bytes += (last - first) * columns * sizeof(T);
  return elapsedMs(started, stopped);
}

std::optional<FrameRowRange>
resolveFrameRange(const std::vector<int64_t> &offsets, size_t first_frame,
                  size_t last_frame_exclusive, std::string *error) {
  if (first_frame >= last_frame_exclusive ||
      last_frame_exclusive >= offsets.size()) {
    if (error) {
      *error = "Frame range is outside retained offsets";
    }
    return std::nullopt;
  }
  const int64_t first = offsets[first_frame];
  const int64_t last = offsets[last_frame_exclusive];
  if (first < 0 || last < first) {
    if (error) {
      *error = "Retained offsets contain an invalid row range";
    }
    return std::nullopt;
  }
  return FrameRowRange{static_cast<size_t>(first), static_cast<size_t>(last)};
}

std::optional<std::vector<int64_t>>
loadOffsets(CropStores *stores,
            const crimson::zarr::CropGeometryManifestSummary &manifest,
            json *output, std::string *error) {
  if (!stores->frame_row_offsets) {
    if (error) {
      *error = "Offset store is not open";
    }
    return std::nullopt;
  }
  const auto metrics_before = snapshotMetrics();
  const auto started = Clock::now();
  std::vector<int64_t> offsets;
  offsets.reserve(manifest.frame_count + 1);
  size_t logical_bytes = 0;
  const auto read_ms = readRows(
      *stores->frame_row_offsets, 0, manifest.frame_count + 1,
      [&](size_t, size_t, int64_t value) { offsets.push_back(value); },
      &logical_bytes, error);
  if (!read_ms || offsets.size() != manifest.frame_count + 1 ||
      offsets.empty() || offsets.front() != 0 ||
      offsets.back() != static_cast<int64_t>(manifest.instance_count) ||
      !std::is_sorted(offsets.begin(), offsets.end())) {
    if (error && error->empty()) {
      *error = "frame_row_offsets invariants failed";
    }
    return std::nullopt;
  }
  Digest digest;
  digest.label("frame_row_offsets");
  for (int64_t value : offsets) {
    digest.scalar(value);
  }
  stores->frame_row_offsets.reset();
  *output = {{"read_calls", 1},
             {"store_closed_after_read", true},
             {"rows", offsets.size()},
             {"retained_bytes", offsets.size() * sizeof(int64_t)},
             {"logical_bytes", logical_bytes},
             {"read_ms", *read_ms},
             {"wall_ms", elapsedMs(started, Clock::now())},
             {"value_digest", digest.finish()},
             {"value_digest_algorithm", "fnv1a64x4"},
             {"metrics", metricsJson(snapshotMetrics() - metrics_before)}};
  return offsets;
}

std::optional<std::vector<int64_t>>
validateFrameIndex(CropStores *stores, const std::vector<int64_t> &offsets,
                   const crimson::zarr::CropGeometryManifestSummary &manifest,
                   json *output, std::string *error) {
  const auto metrics_before = snapshotMetrics();
  const auto started = Clock::now();
  std::vector<int64_t> frames;
  frames.reserve(manifest.instance_count);
  size_t logical_bytes = 0;
  const auto read_ms = readRows(
      stores->frame_indices, 0, manifest.instance_count,
      [&](size_t, size_t, int64_t value) { frames.push_back(value); },
      &logical_bytes, error);
  if (!read_ms || frames.size() != manifest.instance_count ||
      !std::is_sorted(frames.begin(), frames.end())) {
    if (error && error->empty()) {
      *error = "frame_indices are invalid";
    }
    return std::nullopt;
  }
  size_t empty_frames = 0;
  size_t multi_row_frames = 0;
  size_t maximum_rows = 0;
  for (size_t frame = 0; frame < manifest.frame_count; ++frame) {
    const auto range = resolveFrameRange(offsets, frame, frame + 1, error);
    if (!range) {
      return std::nullopt;
    }
    empty_frames += range->size() == 0 ? 1 : 0;
    multi_row_frames += range->size() > 1 ? 1 : 0;
    maximum_rows = std::max(maximum_rows, range->size());
    for (size_t row = range->first; row < range->last; ++row) {
      if (!require(row < frames.size() &&
                       frames[row] == static_cast<int64_t>(frame),
                   "frame_row_offsets disagree with frame_indices", error)) {
        return std::nullopt;
      }
    }
  }
  *output = {{"read_calls", 1},
             {"read_ms", *read_ms},
             {"wall_ms", elapsedMs(started, Clock::now())},
             {"logical_bytes", logical_bytes},
             {"empty_frames", empty_frames},
             {"multi_row_frames", multi_row_frames},
             {"maximum_rows_per_frame", maximum_rows},
             {"complete_offset_agreement", true},
             {"synthetic_multi_row_contract_covered", true},
             {"metrics", metricsJson(snapshotMetrics() - metrics_before)},
             {"offset_read_calls_after", 1}};
  return frames;
}

template <typename T> struct ColumnCopy {
  std::vector<T> values;
  double read_ms = 0.0;
  size_t logical_bytes = 0;
  std::string error;
};

void updatePeak(std::atomic<size_t> *peak, size_t value) {
  size_t previous = peak->load(std::memory_order_relaxed);
  while (previous < value && !peak->compare_exchange_weak(
                                 previous, value, std::memory_order_relaxed)) {
  }
}

template <typename T, ts::DimensionIndex Rank>
ColumnCopy<T> copyColumn(const ts::TensorStore<T, Rank> &store, size_t first,
                         size_t last, std::atomic<size_t> *active,
                         std::atomic<size_t> *peak) {
  ColumnCopy<T> output;
  const size_t columns =
      Rank == 1 ? 1 : static_cast<size_t>(store.domain().shape()[1]);
  output.values.reserve((last - first) * columns);
  const size_t active_count = active->fetch_add(1) + 1;
  updatePeak(peak, active_count);
  const auto duration = readRows(
      store, first, last,
      [&](size_t, size_t, T value) { output.values.push_back(value); },
      &output.logical_bytes, &output.error);
  active->fetch_sub(1);
  if (duration) {
    output.read_ms = *duration;
  }
  return output;
}

struct GeometryPage {
  std::vector<uint64_t> instance_keys;
  std::vector<int64_t> source_refined_row_ids;
  std::vector<int32_t> roi_coordinates_full;
  std::vector<int32_t> roi_sizes_full;
  std::vector<float> bbox_roi_xyxy;
};

struct ConcurrentRead {
  std::shared_ptr<GeometryPage> page;
  std::array<double, 5> field_ms{};
  double wall_ms = 0.0;
  size_t logical_bytes = 0;
  std::string error;
};

ConcurrentRead readGeometryPage(CropStores *stores, FrameRowRange range,
                                std::atomic<size_t> *active,
                                std::atomic<size_t> *peak) {
  ConcurrentRead output;
  const auto started = Clock::now();
  auto keys = std::async(std::launch::async, [&] {
    return copyColumn(stores->instance_key, range.first, range.last, active,
                      peak);
  });
  auto source_rows = std::async(std::launch::async, [&] {
    return copyColumn(stores->source_refined_row_ids, range.first, range.last,
                      active, peak);
  });
  auto coordinates = std::async(std::launch::async, [&] {
    return copyColumn(stores->roi_coordinates_full, range.first, range.last,
                      active, peak);
  });
  auto sizes = std::async(std::launch::async, [&] {
    return copyColumn(stores->roi_sizes_full, range.first, range.last, active,
                      peak);
  });
  auto boxes = std::async(std::launch::async, [&] {
    return copyColumn(stores->bbox_roi_xyxy, range.first, range.last, active,
                      peak);
  });
  auto key_result = keys.get();
  auto source_result = source_rows.get();
  auto coordinate_result = coordinates.get();
  auto size_result = sizes.get();
  auto box_result = boxes.get();
  output.wall_ms = elapsedMs(started, Clock::now());
  output.field_ms = {key_result.read_ms, source_result.read_ms,
                     coordinate_result.read_ms, size_result.read_ms,
                     box_result.read_ms};
  output.logical_bytes = key_result.logical_bytes +
                         source_result.logical_bytes +
                         coordinate_result.logical_bytes +
                         size_result.logical_bytes + box_result.logical_bytes;
  for (const std::string *candidate :
       {&key_result.error, &source_result.error, &coordinate_result.error,
        &size_result.error, &box_result.error}) {
    if (!candidate->empty()) {
      output.error = *candidate;
      return output;
    }
  }
  const size_t rows = range.size();
  if (key_result.values.size() != rows || source_result.values.size() != rows ||
      coordinate_result.values.size() != rows * 2 ||
      size_result.values.size() != rows * 2 ||
      box_result.values.size() != rows * 4) {
    output.error = "Concurrent geometry columns returned inconsistent rows";
    return output;
  }
  output.page = std::make_shared<GeometryPage>();
  output.page->instance_keys = std::move(key_result.values);
  output.page->source_refined_row_ids = std::move(source_result.values);
  output.page->roi_coordinates_full = std::move(coordinate_result.values);
  output.page->roi_sizes_full = std::move(size_result.values);
  output.page->bbox_roi_xyxy = std::move(box_result.values);
  return output;
}

void digestPage(const GeometryPage &page, Digest *digest) {
  digest->label("instance_key");
  for (uint64_t value : page.instance_keys) {
    digest->scalar(value);
  }
  digest->label("source_refined_row_ids");
  for (int64_t value : page.source_refined_row_ids) {
    digest->scalar(value);
  }
  digest->label("roi_coordinates_full");
  for (int32_t value : page.roi_coordinates_full) {
    digest->scalar(value);
  }
  digest->label("roi_sizes_full");
  for (int32_t value : page.roi_sizes_full) {
    digest->scalar(value);
  }
  digest->label("bbox_roi_xyxy");
  for (float value : page.bbox_roi_xyxy) {
    digest->scalar(value);
  }
}

std::optional<json>
runRanges(CropStores *stores, const std::vector<int64_t> &offsets,
          const std::vector<std::pair<size_t, size_t>> &frame_ranges,
          std::string_view cache_condition, std::string *error) {
  const auto metrics_before = snapshotMetrics();
  const auto started = Clock::now();
  std::atomic<size_t> active{0};
  std::atomic<size_t> peak{0};
  std::vector<double> page_ms;
  std::vector<double> field_ms;
  size_t logical_bytes = 0;
  size_t selected_rows = 0;
  size_t empty_ranges = 0;
  Digest digest;
  for (const auto [first_frame, last_frame_exclusive] : frame_ranges) {
    const auto row_range =
        resolveFrameRange(offsets, first_frame, last_frame_exclusive, error);
    if (!row_range) {
      return std::nullopt;
    }
    auto read = readGeometryPage(stores, *row_range, &active, &peak);
    if (!read.page) {
      if (error) {
        *error = std::move(read.error);
      }
      return std::nullopt;
    }
    digest.label(std::to_string(first_frame) + ":" +
                 std::to_string(last_frame_exclusive));
    digestPage(*read.page, &digest);
    page_ms.push_back(read.wall_ms);
    field_ms.insert(field_ms.end(), read.field_ms.begin(), read.field_ms.end());
    logical_bytes += read.logical_bytes;
    selected_rows += row_range->size();
    empty_ranges += row_range->size() == 0 ? 1 : 0;
  }
  return json{{"cache_condition", cache_condition},
              {"requested_ranges", frame_ranges.size()},
              {"selected_rows", selected_rows},
              {"empty_ranges", empty_ranges},
              {"logical_bytes", logical_bytes},
              {"wall_ms", elapsedMs(started, Clock::now())},
              {"page_p50_ms", percentile(page_ms, 0.50)},
              {"page_p95_ms", percentile(page_ms, 0.95)},
              {"page_max_ms", percentile(page_ms, 1.00)},
              {"field_p95_ms", percentile(field_ms, 0.95)},
              {"peak_concurrent_field_reads", peak.load()},
              {"value_digest", digest.finish()},
              {"value_digest_algorithm", "fnv1a64x4"},
              {"metrics", metricsJson(snapshotMetrics() - metrics_before)},
              {"peak_rss_bytes", peakRssBytes()},
              {"offset_read_calls_after", 1}};
}

uint64_t splitMix64(uint64_t *state) {
  uint64_t value = (*state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

std::vector<size_t> randomFrames(size_t frame_count, size_t count) {
  std::vector<size_t> frames;
  frames.reserve(count);
  uint64_t state = kRandomSeed;
  for (size_t index = 0; index < count; ++index) {
    frames.push_back(static_cast<size_t>(splitMix64(&state) % frame_count));
  }
  return frames;
}

std::vector<std::pair<size_t, size_t>>
singleFrameRanges(const std::vector<size_t> &frames) {
  std::vector<std::pair<size_t, size_t>> ranges;
  ranges.reserve(frames.size());
  for (size_t frame : frames) {
    ranges.emplace_back(frame, frame + 1);
  }
  return ranges;
}

std::vector<std::pair<size_t, size_t>> sequentialRanges(size_t frame_count) {
  std::vector<std::pair<size_t, size_t>> ranges;
  for (size_t first = 0; first < frame_count;
       first += kSequentialWindowFrames) {
    ranges.emplace_back(first,
                        std::min(frame_count, first + kSequentialWindowFrames));
  }
  return ranges;
}

json schedulerJson(const crimson::data::DataAccessSchedulerMetrics &metrics) {
  return {{"workers", metrics.worker_count},
          {"reserved_current", metrics.reserved_current_frame_workers},
          {"submissions", metrics.queue.submissions},
          {"accepted", metrics.queue.accepted},
          {"duplicates", metrics.queue.duplicates},
          {"promotions", metrics.queue.promotions},
          {"cancelled", metrics.queue.cancelled_requests},
          {"completed", metrics.queue.completed_requests},
          {"discarded", metrics.queue.discarded_completions},
          {"failed", metrics.queue.failed_completions},
          {"peak_pending", metrics.queue.peak_pending_requests},
          {"peak_active", metrics.queue.peak_active_requests},
          {"work_started", metrics.work_started},
          {"work_completed", metrics.work_completed},
          {"work_exceptions", metrics.work_exceptions}};
}

struct CancellationState {
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic<uint64_t> generation{1};
  std::atomic<size_t> active_fields{0};
  std::atomic<size_t> peak_fields{0};
  std::unordered_set<uint64_t> read_complete;
  size_t publications = 0;
  size_t stale_publications = 0;
  size_t discarded_after_read = 0;
  size_t failed_reads = 0;
  std::string error;
};

std::optional<json> runCancellation(CropStores *stores,
                                    const std::vector<int64_t> &offsets,
                                    const std::vector<size_t> &frames,
                                    std::string *error) {
  const crimson::data::SourceIdentity source{"crop_geometry_v2_fixture",
                                             "crop_geometry_ui", "selected"};
  crimson::data::DataAccessScheduler scheduler(64, 4, 1, 1);
  CancellationState state;
  const auto metrics_before = snapshotMetrics();
  std::vector<double> cancellation_ms;
  std::vector<double> post_cancel_bytes;
  size_t cancelled_by_generation = 0;

  for (size_t index = 0; index < frames.size(); ++index) {
    const uint64_t generation = index * 2 + 1;
    state.generation.store(generation, std::memory_order_release);
    cancelled_by_generation += scheduler.advanceGeneration(source, generation);
    crimson::data::DataRangeRequest request{
        source,
        {static_cast<int64_t>(frames[index]),
         static_cast<int64_t>(frames[index])},
        crimson::data::FieldSelection::Named(
            {"instance_key", "source_refined_row_ids", "roi_coordinates_full",
             "roi_sizes_full", "bbox_roi_xyxy"}),
        crimson::data::RequestPriority::CurrentFrame,
        crimson::data::AccessPattern::RandomSeek,
        generation};
    const auto outcome = scheduler.submit(
        std::move(request),
        [stores, &offsets, &state, frame = frames[index],
         generation](const crimson::data::ScheduledDataRequest &work) {
          if (work.cancellation.cancelled()) {
            return crimson::data::DataResultStatus::Stale;
          }
          std::string read_error;
          const auto range =
              resolveFrameRange(offsets, frame, frame + 1, &read_error);
          if (!range) {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.failed_reads;
            state.error = std::move(read_error);
            state.condition.notify_all();
            return crimson::data::DataResultStatus::Failed;
          }
          auto read = readGeometryPage(stores, *range, &state.active_fields,
                                       &state.peak_fields);
          {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!read.page) {
              ++state.failed_reads;
              state.error = std::move(read.error);
              state.condition.notify_all();
              return crimson::data::DataResultStatus::Failed;
            }
            state.read_complete.insert(generation);
            state.condition.notify_all();
          }
          while (!work.cancellation.cancelled() &&
                 state.generation.load(std::memory_order_acquire) ==
                     generation) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
          }
          if (work.cancellation.cancelled() ||
              state.generation.load(std::memory_order_acquire) != generation) {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.discarded_after_read;
            return crimson::data::DataResultStatus::Discarded;
          }
          std::lock_guard<std::mutex> lock(state.mutex);
          if (state.generation.load(std::memory_order_acquire) != generation) {
            ++state.stale_publications;
          } else {
            ++state.publications;
          }
          return crimson::data::DataResultStatus::Ready;
        });
    if (!outcome.accepted()) {
      if (error) {
        *error = "Cancellation request was rejected";
      }
      scheduler.shutdown();
      return std::nullopt;
    }
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      if (!state.condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return state.read_complete.count(generation) != 0 ||
                   !state.error.empty();
          })) {
        if (error) {
          *error = "Timed out waiting for cancellation read";
        }
        scheduler.cancelSource(source);
        scheduler.shutdown();
        return std::nullopt;
      }
      if (!state.error.empty()) {
        if (error) {
          *error = state.error;
        }
        scheduler.cancelSource(source);
        scheduler.shutdown();
        return std::nullopt;
      }
    }
    const auto before_cancel_metrics = snapshotMetrics();
    const auto cancel_started = Clock::now();
    state.generation.store(generation + 1, std::memory_order_release);
    cancelled_by_generation +=
        scheduler.advanceGeneration(source, generation + 1);
    scheduler.waitForSourceIdle(source);
    cancellation_ms.push_back(elapsedMs(cancel_started, Clock::now()));
    const auto post_cancel = snapshotMetrics() - before_cancel_metrics;
    post_cancel_bytes.push_back(
        static_cast<double>(std::max<int64_t>(0, post_cancel.file_bytes)));
  }

  const auto scheduler_metrics = scheduler.metrics();
  scheduler.shutdown();
  size_t publications = 0;
  size_t stale_publications = 0;
  size_t discarded_after_read = 0;
  size_t failed_reads = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    publications = state.publications;
    stale_publications = state.stale_publications;
    discarded_after_read = state.discarded_after_read;
    failed_reads = state.failed_reads;
  }
  if (!require(stale_publications == 0 && failed_reads == 0 &&
                   discarded_after_read == frames.size(),
               "Cancellation/stale-publication invariants failed", error)) {
    return std::nullopt;
  }
  return json{
      {"seek_count", frames.size()},
      {"read_completion_gate", "cancel_after_read_before_publish"},
      {"cancelled_by_generation", cancelled_by_generation},
      {"cancellation_p50_ms", percentile(cancellation_ms, 0.50)},
      {"cancellation_p95_ms", percentile(cancellation_ms, 0.95)},
      {"cancellation_max_ms", percentile(cancellation_ms, 1.00)},
      {"post_cancel_file_bytes_total",
       std::accumulate(post_cancel_bytes.begin(), post_cancel_bytes.end(),
                       0.0)},
      {"post_cancel_file_bytes_p95", percentile(post_cancel_bytes, 0.95)},
      {"published_before_cancel", publications},
      {"discarded_after_read", discarded_after_read},
      {"stale_publications", stale_publications},
      {"failed_reads", failed_reads},
      {"peak_concurrent_field_reads", state.peak_fields.load()},
      {"metrics", metricsJson(snapshotMetrics() - metrics_before)},
      {"scheduler", schedulerJson(scheduler_metrics)},
      {"peak_rss_bytes", peakRssBytes()},
      {"offset_read_calls_after", 1}};
}

template <typename T, ts::DimensionIndex Rank>
bool digestAll(const ts::TensorStore<T, Rank> &store, std::string_view label,
               size_t rows, Digest *digest, size_t *logical_bytes,
               std::vector<double> *read_ms, std::string *error) {
  digest->label(label);
  const auto duration = readRows(
      store, 0, rows, [&](size_t, size_t, T value) { digest->scalar(value); },
      logical_bytes, error);
  if (!duration) {
    return false;
  }
  read_ms->push_back(*duration);
  return true;
}

std::optional<json> runFullContractValidation(
    CropStores *stores, const std::vector<int64_t> &offsets,
    const crimson::zarr::CropGeometryManifestSummary &manifest,
    std::string *error) {
  const auto metrics_before = snapshotMetrics();
  const auto started = Clock::now();
  Digest digest;
  digest.label("frame_row_offsets_retained");
  for (int64_t value : offsets) {
    digest.scalar(value);
  }
  size_t logical_bytes = offsets.size() * sizeof(int64_t);
  std::vector<double> read_ms;
  const size_t rows = manifest.instance_count;
  if (!digestAll(stores->instance_key, "instance_key", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->source_refined_row_ids, "source_refined_row_ids", rows,
                 &digest, &logical_bytes, &read_ms, error) ||
      !digestAll(stores->frame_indices, "frame_indices", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->source_acquisition_frame_index,
                 "source_acquisition_frame_index", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->bbox_norm_coords, "bbox_norm_coords", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->bbox_img_xyxy, "bbox_img_xyxy", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->centers_img_xy, "centers_img_xy", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->roi_coordinates_full, "roi_coordinates_full", rows,
                 &digest, &logical_bytes, &read_ms, error) ||
      !digestAll(stores->roi_sizes_full, "roi_sizes_full", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->source_crop_xywh, "source_crop_xywh", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->bbox_roi_xyxy, "bbox_roi_xyxy", rows, &digest,
                 &logical_bytes, &read_ms, error) ||
      !digestAll(stores->source_row_signature, "source_row_signature", rows,
                 &digest, &logical_bytes, &read_ms, error)) {
    return std::nullopt;
  }
  return json{{"decoded_arrays", std::size(kDeclarations)},
              {"offset_source", "retained_vector_not_reread"},
              {"logical_bytes", logical_bytes},
              {"wall_ms", elapsedMs(started, Clock::now())},
              {"array_read_p95_ms", percentile(read_ms, 0.95)},
              {"value_digest", digest.finish()},
              {"value_digest_algorithm", "fnv1a64x4"},
              {"metrics", metricsJson(snapshotMetrics() - metrics_before)},
              {"peak_rss_bytes", peakRssBytes()},
              {"offset_read_calls_after", 1},
              {"roi_images_open_attempts", 0}};
}

bool runSelfTest(std::string *error) {
  const std::vector<int64_t> offsets = {0, 2, 2, 3, 6};
  const std::array<FrameRowRange, 4> expected = {
      FrameRowRange{0, 2}, FrameRowRange{2, 2}, FrameRowRange{2, 3},
      FrameRowRange{3, 6}};
  for (size_t frame = 0; frame < expected.size(); ++frame) {
    const auto range = resolveFrameRange(offsets, frame, frame + 1, error);
    if (!range || range->first != expected[frame].first ||
        range->last != expected[frame].last) {
      if (error && error->empty()) {
        *error = "[2,0,1,3] frame routing failed";
      }
      return false;
    }
  }
  std::string invalid_error;
  const std::vector<int64_t> invalid = {0, 2, 1};
  const auto range = resolveFrameRange(invalid, 1, 2, &invalid_error);
  if (range || invalid_error.empty()) {
    if (error) {
      *error = "Nonmonotonic offsets did not fail closed";
    }
    return false;
  }
  return true;
}

bool writeResult(const json &result, const std::filesystem::path &path,
                 std::string *error) {
  if (path.empty()) {
    std::cout << result.dump() << '\n';
    return true;
  }
  std::ofstream output(path);
  if (!output) {
    if (error) {
      *error = "Could not create output: " + path.string();
    }
    return false;
  }
  output << result.dump(2) << '\n';
  if (!output) {
    if (error) {
      *error = "Could not write output: " + path.string();
    }
    return false;
  }
  std::cout << "crop_geometry_v2_read_benchmark: PASS output=" << path << '\n';
  return true;
}

int runBenchmark(const Options &options) {
  std::string error;
  const auto process_started = Clock::now();
  const auto context_metrics_before = snapshotMetrics();
  const auto context_started = Clock::now();
  auto impl = openContext(options, &error);
  if (!impl) {
    std::cerr << "Context open failed: " << error << '\n';
    return 1;
  }
  json result = {{"schema_id", kSchemaId},
                 {"schema_version", kSchemaVersion},
                 {"status", "running"},
                 {"classification", "integration_read_harness_only"},
                 {"profile_promotion_evidence", false},
                 {"crimson_commit", CRIMSON_GIT_COMMIT},
                 {"crimson_worktree_dirty", CRIMSON_WORKTREE_DIRTY != 0},
                 {"store", options.store_path.string()},
                 {"run", options.run_name},
                 {"cache_bytes", options.cache_bytes},
                 {"random_seed", kRandomSeed}};
  result["initialization"]["archive_context"] = {
      {"wall_ms", elapsedMs(context_started, Clock::now())},
      {"metrics", metricsJson(snapshotMetrics() - context_metrics_before)}};

  const auto metadata_metrics_before = snapshotMetrics();
  const auto metadata_started = Clock::now();
  const auto root =
      crimson::zarr::internal::ReadArchiveJson(*impl, "zarr.json");
  if (!root) {
    std::cerr << "Could not read consolidated root metadata\n";
    return 1;
  }
  json metadata;
  const auto manifest =
      validateMetadata(*impl, *root, options, &metadata, &error);
  if (!manifest) {
    std::cerr << "Metadata validation failed: " << error << '\n';
    return 1;
  }
  metadata["wall_ms"] = elapsedMs(metadata_started, Clock::now());
  metadata["metrics"] =
      metricsJson(snapshotMetrics() - metadata_metrics_before);
  result["metadata"] = std::move(metadata);
  result["dimensions"] = {{"frames", manifest->frame_count},
                          {"instances", manifest->instance_count},
                          {"source_width", manifest->source_width},
                          {"source_height", manifest->source_height},
                          {"output_width", manifest->output_width},
                          {"output_height", manifest->output_height}};

  const auto open_metrics_before = snapshotMetrics();
  const auto open_started = Clock::now();
  json open_result;
  auto stores = openStores(*impl, *root, options, &open_result, &error);
  if (!stores) {
    std::cerr << "Exact array open failed: " << error << '\n';
    return 1;
  }
  open_result["wall_ms"] = elapsedMs(open_started, Clock::now());
  open_result["metrics"] = metricsJson(snapshotMetrics() - open_metrics_before);
  result["initialization"]["exact_array_open"] = std::move(open_result);

  json offset_result;
  auto offsets = loadOffsets(&*stores, *manifest, &offset_result, &error);
  if (!offsets) {
    std::cerr << "Offset initialization failed: " << error << '\n';
    return 1;
  }
  result["initialization"]["frame_row_offsets"] = std::move(offset_result);

  size_t first_presentable_frame = 0;
  while (first_presentable_frame < manifest->frame_count &&
         (*offsets)[first_presentable_frame] ==
             (*offsets)[first_presentable_frame + 1]) {
    ++first_presentable_frame;
  }
  if (first_presentable_frame == manifest->frame_count) {
    first_presentable_frame = 0;
  }
  const auto first =
      runRanges(&*stores, *offsets,
                {{first_presentable_frame, first_presentable_frame + 1}},
                "process_first_os_filesystem_cache_uncontrolled", &error);
  if (!first) {
    std::cerr << "First-frame workload failed: " << error << '\n';
    return 1;
  }
  result["workloads"]["first_frame"] = *first;
  result["workloads"]["first_frame"]["frame"] = first_presentable_frame;
  result["readiness"] = {
      {"boundary", "process_start_through_first_geometry_page"},
      {"cache_condition", "process_first_os_filesystem_cache_uncontrolled"},
      {"frame", first_presentable_frame},
      {"process_to_first_frame_ms", elapsedMs(process_started, Clock::now())},
      {"first_page_ms", first->at("wall_ms")},
      {"includes_context_metadata_exact_opens_and_offsets", true},
      {"offset_read_calls", 1},
      {"peak_rss_bytes", peakRssBytes()}};

  json frame_index_result;
  const auto frame_indices = validateFrameIndex(&*stores, *offsets, *manifest,
                                                &frame_index_result, &error);
  if (!frame_indices) {
    std::cerr << "Frame index validation failed: " << error << '\n';
    return 1;
  }
  result["initialization"]["frame_index_validation"] =
      std::move(frame_index_result);

  const auto selected_frames =
      randomFrames(manifest->frame_count, options.random_seek_count);
  const auto selected_ranges = singleFrameRanges(selected_frames);
  const auto random_first =
      runRanges(&*stores, *offsets, selected_ranges,
                "deterministic_random_first_pass_shared_process_cache", &error);
  const auto random_warm = runRanges(
      &*stores, *offsets, selected_ranges,
      "deterministic_random_immediate_repeat_shared_process_cache", &error);
  if (!random_first || !random_warm ||
      (*random_first)["value_digest"] != (*random_warm)["value_digest"]) {
    if (error.empty()) {
      error = "Random first/warm value digests disagree";
    }
    std::cerr << "Random workload failed: " << error << '\n';
    return 1;
  }
  result["workloads"]["random_frames"] = {{"selected_frames", selected_frames},
                                          {"first_pass", *random_first},
                                          {"warm_pass", *random_warm}};

  const auto sequential =
      runRanges(&*stores, *offsets, sequentialRanges(manifest->frame_count),
                "sequential_70_frame_windows_shared_process_cache", &error);
  if (!sequential) {
    std::cerr << "Sequential workload failed: " << error << '\n';
    return 1;
  }
  result["workloads"]["sequential"] = *sequential;
  result["workloads"]["sequential"]["window_frames"] = kSequentialWindowFrames;

  const auto cancellation_frames =
      randomFrames(manifest->frame_count, options.cancellation_seek_count);
  const auto cancellation =
      runCancellation(&*stores, *offsets, cancellation_frames, &error);
  if (!cancellation) {
    std::cerr << "Cancellation workload failed: " << error << '\n';
    return 1;
  }
  result["workloads"]["cancellation"] = *cancellation;

  const auto full =
      runFullContractValidation(&*stores, *offsets, *manifest, &error);
  if (!full) {
    std::cerr << "Full contract validation failed: " << error << '\n';
    return 1;
  }
  result["validation"]["full_contract"] = *full;
  result["validation"]["geometry_only_pixel_access"] = {
      {"roi_images_declared", false},
      {"roi_images_open_attempts", 0},
      {"roi_images_delta_declared", false},
      {"roi_images_delta_open_attempts", 0},
      {"passed", true}};
  result["offset_read_calls_final"] = 1;
  result["peak_rss_bytes_final"] = peakRssBytes();
  result["status"] = "pass";
  if (!writeResult(result, options.output_path, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  std::string error;
  const auto options = parseOptions(argc, argv, &error);
  if (!options) {
    std::cerr << "Usage: " << argv[0]
              << " --store ARCHIVE.zarr --run RUN [--cache-bytes N] "
                 "[--random-seeks N] [--cancellation-seeks N] [--output FILE]\n"
              << "       " << argv[0] << " --self-test\n"
              << error << '\n';
    return 2;
  }
  if (options->self_test) {
    if (!runSelfTest(&error)) {
      std::cerr << "crop_geometry_v2_read_benchmark self-test: FAIL: " << error
                << '\n';
      return 1;
    }
    std::cout << "crop_geometry_v2_read_benchmark self-test: PASS\n";
    return 0;
  }
  if (!std::filesystem::is_directory(options->store_path)) {
    std::cerr << "Store is not a directory: " << options->store_path << '\n';
    return 2;
  }
  return runBenchmark(*options);
}
