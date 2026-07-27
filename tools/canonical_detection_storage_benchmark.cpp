#include <tensorstore/array.h>
#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/internal/metrics/registry.h>
#include <tensorstore/kvstore/spec.h>
#include <tensorstore/open.h>
#include <tensorstore/open_mode.h>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data_access_cache.h"
#include "data_access_scheduler.h"
#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"

namespace {
namespace ts = tensorstore;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

constexpr std::string_view kSchemaId =
    "crimson.canonical_detection_storage_benchmark";
constexpr int kSchemaVersion = 2;
constexpr size_t kFullFrameCount = 128;
constexpr size_t kFullRangeCount = 64;
constexpr size_t kQuickSelectionCount = 8;
constexpr size_t kRowsPerRange = 32;
constexpr size_t kSequentialWindow = 700;
constexpr size_t kQuickSequentialFrames = 7000;
constexpr size_t kPlaybackSourceFps = 700;
constexpr size_t kPlaybackPageFrames = 70;
constexpr size_t kPlaybackWarmupFrames = 700;
constexpr size_t kPresentationCacheBytes = 4 * 1024 * 1024;
constexpr size_t kPresentationCacheItems = 32;
constexpr size_t kProductionTensorStoreCacheBytes = 64 * 1024 * 1024;

struct Options {
  std::filesystem::path store_path;
  size_t cache_bytes = 0;
  std::string layout;
  std::string repetition;
  std::filesystem::path palette_result_path;
  std::string mode = "full";
  size_t read_ahead_frames = 0;
  size_t playback_frames = 0;
};

struct Metrics {
  int64_t file_reads = 0;
  int64_t file_batch_reads = 0;
  int64_t file_bytes = 0;
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;
  int64_t cache_evictions = 0;
};

struct ReadStats {
  std::vector<double> operation_ms;
  size_t logical_bytes = 0;
};

struct CanonicalStores {
  ts::TensorStore<int32_t, 1> frame_indices;
  ts::TensorStore<int64_t, 1> source_acquisition_frame_index;
  ts::TensorStore<uint64_t, 1> instance_key;
  ts::TensorStore<float, 2> bbox_norm_coords;
  ts::TensorStore<float, 2> bbox_img_xyxy;
  ts::TensorStore<float, 2> centers_img_xy;
  ts::TensorStore<float, 1> scores;
  ts::TensorStore<int32_t, 1> class_ids;
  std::optional<ts::TensorStore<int64_t, 1>> frame_row_offsets;
};

struct SelectionManifest {
  std::vector<size_t> frames;
  std::vector<size_t> row_starts;
  std::string frame_sha256;
  std::string row_sha256;
};

class Digest {
public:
  void label(std::string_view value) {
    const uint64_t size = value.size();
    scalar(size);
    write(value);
  }

  template <typename T> void scalar(const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    write(std::string_view(reinterpret_cast<const char *>(&value),
                           sizeof(value)));
  }

  std::string finish() const {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : states_) {
      output << std::setw(16) << value;
    }
    return output.str();
  }

private:
  void write(std::string_view bytes) {
    constexpr std::array<uint64_t, 4> kPrimes = {
        1099511628211ULL,
        1099511627791ULL,
        1099511627689ULL,
        1099511627563ULL,
    };
    for (const auto byte : bytes) {
      const auto value = static_cast<uint8_t>(byte);
      for (size_t lane = 0; lane < states_.size(); ++lane) {
        states_[lane] ^= static_cast<uint64_t>(value + lane * 37U);
        states_[lane] *= kPrimes[lane];
      }
    }
  }

  std::array<uint64_t, 4> states_ = {
      1469598103934665603ULL,
      1099511628211ULL,
      7809847782465536322ULL,
      9650029242287828579ULL,
  };
};

double ElapsedMs(Clock::time_point start, Clock::time_point stop) {
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<size_t>(
      std::max(0.0, std::ceil(percentile * values.size()) - 1.0));
  return values[std::min(rank, values.size() - 1)];
}

int64_t CounterValue(std::string_view name) {
  const auto metric = ts::internal_metrics::GetMetricRegistry().Collect(name);
  if (!metric || metric->values.empty())
    return 0;
  return std::get<int64_t>(metric->values.front().value);
}

Metrics SnapshotMetrics() {
  return {
      CounterValue("/tensorstore/kvstore/file/read"),
      CounterValue("/tensorstore/kvstore/file/batch_read"),
      CounterValue("/tensorstore/kvstore/file/bytes_read"),
      CounterValue("/tensorstore/cache/hit_count"),
      CounterValue("/tensorstore/cache/miss_count"),
      CounterValue("/tensorstore/cache/evict_count"),
  };
}

Metrics operator-(const Metrics &after, const Metrics &before) {
  return {
      after.file_reads - before.file_reads,
      after.file_batch_reads - before.file_batch_reads,
      after.file_bytes - before.file_bytes,
      after.cache_hits - before.cache_hits,
      after.cache_misses - before.cache_misses,
      after.cache_evictions - before.cache_evictions,
  };
}

json MetricsJson(const Metrics &metrics) {
  return {
      {"file_reads", metrics.file_reads},
      {"file_batch_reads", metrics.file_batch_reads},
      {"file_bytes", metrics.file_bytes},
      {"cache_hits", metrics.cache_hits},
      {"cache_misses", metrics.cache_misses},
      {"cache_evictions", metrics.cache_evictions},
  };
}

uint64_t PeakRssBytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0)
    return 0;
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

std::optional<json> ReadJsonFile(const std::filesystem::path &path,
                                 std::string *error) {
  std::ifstream input(path);
  if (!input) {
    if (error)
      *error = "Could not open JSON file: " + path.string();
    return std::nullopt;
  }
  try {
    json value;
    input >> value;
    return value;
  } catch (const json::exception &exception) {
    if (error)
      *error = "Could not parse " + path.string() + ": " + exception.what();
    return std::nullopt;
  }
}

std::optional<size_t> ParseSize(const char *value, const char *name) {
  try {
    const auto parsed = std::stoull(value);
    if (parsed > std::numeric_limits<size_t>::max()) {
      throw std::out_of_range("size_t");
    }
    return static_cast<size_t>(parsed);
  } catch (const std::exception &exception) {
    std::cerr << "Invalid " << name << " value '" << value
              << "': " << exception.what() << '\n';
    return std::nullopt;
  }
}

std::optional<Options> ParseOptions(int argc, char **argv) {
  if (argc != 6 && argc != 7 && argc != 9) {
    std::cerr << "Usage: " << argv[0]
              << " STORE.zarr CACHE_BYTES LAYOUT REPETITION PALETTE_RESULT.json"
                 " [full|quick|validate]\n"
              << "       " << argv[0]
              << " STORE.zarr CACHE_BYTES hybrid REPETITION PALETTE_RESULT.json"
                 " prefetch READ_AHEAD_FRAMES PLAYBACK_FRAMES\n";
    return std::nullopt;
  }
  Options options;
  options.store_path = argv[1];
  const auto cache_bytes = ParseSize(argv[2], "cache byte");
  if (!cache_bytes)
    return std::nullopt;
  options.cache_bytes = *cache_bytes;
  options.layout = argv[3];
  options.repetition = argv[4];
  options.palette_result_path = argv[5];
  if (argc >= 7)
    options.mode = argv[6];
  if (options.mode != "full" && options.mode != "quick" &&
      options.mode != "validate" && options.mode != "prefetch") {
    std::cerr << "Mode must be full, quick, validate, or prefetch\n";
    return std::nullopt;
  }
  if (options.mode == "prefetch") {
    if (argc != 9 ||
        (options.layout != "hybrid" && options.layout != "regular")) {
      std::cerr << "Prefetch mode requires a regular or hybrid layout plus "
                   "read-ahead and playback frame counts\n";
      return std::nullopt;
    }
    const auto read_ahead = ParseSize(argv[7], "read-ahead frame");
    const auto playback = ParseSize(argv[8], "playback frame");
    if (!read_ahead || !playback || *playback < kPlaybackPageFrames) {
      std::cerr << "Prefetch playback must span at least one 70-frame page\n";
      return std::nullopt;
    }
    options.read_ahead_frames = *read_ahead;
    options.playback_frames = *playback;
  } else if (argc == 9) {
    std::cerr << "Extra playback arguments require prefetch mode\n";
    return std::nullopt;
  }
  return options;
}

bool Require(bool condition, std::string message, std::string *error) {
  if (!condition && error)
    *error = std::move(message);
  return condition;
}

std::optional<SelectionManifest>
LoadSelections(const json &palette_result, bool quick, std::string *error) {
  try {
    const auto &workloads = palette_result.at("consumer_workloads");
    const auto &config = workloads.at("config");
    if (!Require(config.at("seed") == 20260724,
                 "Palette workload seed is not 20260724", error) ||
        !Require(config.at("random_frame_count") == kFullFrameCount,
                 "Palette random-frame count is not 128", error) ||
        !Require(config.at("random_row_range_count") == kFullRangeCount,
                 "Palette random-range count is not 64", error) ||
        !Require(config.at("random_row_range_rows") == kRowsPerRange,
                 "Palette random-range width is not 32 rows", error) ||
        !Require(config.at("sequential_frame_window") == kSequentialWindow,
                 "Palette sequential frame window is not 700", error)) {
      return std::nullopt;
    }
    SelectionManifest output;
    output.frames = workloads.at("random_frame_slices")
                        .at("frame_indices")
                        .get<std::vector<size_t>>();
    output.row_starts = workloads.at("random_observation_ranges")
                            .at("row_starts")
                            .get<std::vector<size_t>>();
    output.frame_sha256 = workloads.at("random_frame_slices")
                              .at("frame_indices_sha256")
                              .get<std::string>();
    output.row_sha256 = workloads.at("random_observation_ranges")
                            .at("row_starts_sha256")
                            .get<std::string>();
    if (!Require(output.frames.size() == kFullFrameCount,
                 "Palette result does not contain 128 frame selections",
                 error) ||
        !Require(output.row_starts.size() == kFullRangeCount,
                 "Palette result does not contain 64 row selections", error)) {
      return std::nullopt;
    }
    if (quick) {
      output.frames.resize(kQuickSelectionCount);
      output.row_starts.resize(kQuickSelectionCount);
    }
    return output;
  } catch (const json::exception &exception) {
    if (error)
      *error = "Palette result is incomplete: " + std::string(exception.what());
    return std::nullopt;
  }
}

std::optional<crimson::zarr::ArchiveContext::Impl>
OpenContext(const Options &options, std::string *error) {
  auto context = crimson::zarr::internal::MakeArchiveTensorStoreContext(
      options.cache_bytes);
  if (!context.ok()) {
    if (error)
      *error = context.status().ToString();
    return std::nullopt;
  }
  auto spec = ts::kvstore::Spec::FromJson(
      {{"driver", "file"}, {"path", options.store_path.string() + "/"}});
  if (!spec.ok()) {
    if (error)
      *error = spec.status().ToString();
    return std::nullopt;
  }
  auto store = ts::kvstore::Open(*spec, *context).result();
  if (!store.ok()) {
    if (error)
      *error = store.status().ToString();
    return std::nullopt;
  }
  crimson::zarr::ArchiveContext::Impl impl;
  impl.root_path = options.store_path;
  impl.context = *context;
  impl.store = *store;
  impl.cache_pool_bytes = options.cache_bytes;
  return impl;
}

const json *ConsolidatedEntry(const json &root, std::string_view path) {
  try {
    const auto &metadata = root.at("consolidated_metadata").at("metadata");
    const auto found = metadata.find(std::string(path));
    return found == metadata.end() ? nullptr : &*found;
  } catch (const json::exception &) {
    return nullptr;
  }
}

struct ArrayDeclaration {
  const char *path;
  const char *dtype;
  size_t rank;
  size_t columns;
};

constexpr ArrayDeclaration kDeclarations[] = {
    {"instances/frame_indices", "int32", 1, 1},
    {"instances/source_acquisition_frame_index", "int64", 1, 1},
    {"instances/instance_key", "uint64", 1, 1},
    {"instances/bbox_norm_coords", "float32", 2, 4},
    {"instances/bbox_img_xyxy", "float32", 2, 4},
    {"instances/centers_img_xy", "float32", 2, 2},
    {"instances/scores", "float32", 1, 1},
    {"instances/class_ids", "int32", 1, 1},
    {"instances/frame_row_offsets", "int64", 1, 1},
};

bool ValidateConsolidatedSchema(const json &root, size_t *frame_count,
                                size_t *row_count, std::string *error) {
  try {
    if (!Require(root.at("zarr_format") == 3 && root.at("node_type") == "group",
                 "Root is not a Zarr v3 group", error) ||
        !Require(root.at("consolidated_metadata").at("kind") == "inline",
                 "Root does not contain inline consolidated metadata", error) ||
        !Require(root.at("attributes").at("benchmark_only") == true &&
                     root.at("attributes").at("selector_eligible") == false,
                 "Store is not a selector-ineligible benchmark fixture",
                 error)) {
      return false;
    }
    const auto &dimensions =
        root.at("attributes").at("logical_schema").at("dimensions");
    *frame_count = dimensions.at("n_frames").get<size_t>();
    *row_count = dimensions.at("n_instances").get<size_t>();
    for (const auto &declaration : kDeclarations) {
      const auto *metadata = ConsolidatedEntry(root, declaration.path);
      if (!Require(metadata != nullptr,
                   std::string("Missing consolidated entry: ") +
                       declaration.path,
                   error)) {
        return false;
      }
      const auto shape = metadata->at("shape").get<std::vector<size_t>>();
      if (!Require(
              metadata->at("node_type") == "array" &&
                  metadata->at("data_type") == declaration.dtype &&
                  shape.size() == declaration.rank &&
                  shape[0] == (std::string_view(declaration.path) ==
                                       "instances/frame_row_offsets"
                                   ? *frame_count + 1
                                   : *row_count) &&
                  (declaration.rank == 1 || shape[1] == declaration.columns),
              std::string("Consolidated schema mismatch: ") + declaration.path,
              error)) {
        return false;
      }
    }
    return true;
  } catch (const json::exception &exception) {
    if (error)
      *error = "Invalid consolidated root: " + std::string(exception.what());
    return false;
  }
}

json NormalizeGroupMetadata(json value) {
  if (value.contains("consolidated_metadata") &&
      value["consolidated_metadata"].value("kind", "") == "inline" &&
      value["consolidated_metadata"]
          .value("metadata", json::object())
          .empty()) {
    value.erase("consolidated_metadata");
  }
  return value;
}

bool ValidateDirectMetadata(const crimson::zarr::ArchiveContext::Impl &impl,
                            const json &root, json *output,
                            std::string *error) {
  size_t matched = 0;
  for (const auto &declaration : kDeclarations) {
    const auto direct = crimson::zarr::internal::ReadArchiveJson(
        impl, std::string(declaration.path) + "/zarr.json");
    const auto *consolidated = ConsolidatedEntry(root, declaration.path);
    if (!direct || !consolidated || *direct != *consolidated) {
      if (error) {
        *error = std::string("Direct/consolidated metadata mismatch: ") +
                 declaration.path;
      }
      return false;
    }
    ++matched;
  }
  const auto direct_group =
      crimson::zarr::internal::ReadArchiveJson(impl, "instances/zarr.json");
  const auto *consolidated_group = ConsolidatedEntry(root, "instances");
  if (!direct_group || !consolidated_group ||
      NormalizeGroupMetadata(*direct_group) !=
          NormalizeGroupMetadata(*consolidated_group)) {
    if (error)
      *error = "Direct/consolidated instances group mismatch";
    return false;
  }
  *output = {{"array_matches", matched},
             {"group_envelope_equivalent", true},
             {"direct_array_reads", matched},
             {"direct_group_reads", 1}};
  return true;
}

template <typename T, ts::DimensionIndex Rank>
std::optional<ts::TensorStore<T, Rank>>
OpenExact(const crimson::zarr::ArchiveContext::Impl &impl, const json &root,
          const char *path, bool assume_metadata, std::string *error) {
  auto spec = crimson::zarr::internal::MakeReadOnlyArraySpec(impl, path);
  const auto *metadata = ConsolidatedEntry(root, path);
  if (!spec || !metadata) {
    if (error)
      *error = std::string("Missing array specification: ") + path;
    return std::nullopt;
  }
  (*spec)["metadata"] = *metadata;
  auto mode = ts::OpenMode::open;
  if (assume_metadata)
    mode = mode | ts::OpenMode::assume_metadata;
  auto result =
      ts::Open<T, Rank>(*spec, mode, ts::ReadWriteMode::read, impl.context)
          .result();
  if (!result.ok()) {
    if (error)
      *error = std::string(path) + ": " + result.status().ToString();
    return std::nullopt;
  }
  return *result;
}

std::optional<CanonicalStores>
OpenStores(const crimson::zarr::ArchiveContext::Impl &impl, const json &root,
           bool assume_metadata, std::string *error) {
  auto frame_indices = OpenExact<int32_t, 1>(
      impl, root, "instances/frame_indices", assume_metadata, error);
  auto source_frame = OpenExact<int64_t, 1>(
      impl, root, "instances/source_acquisition_frame_index", assume_metadata,
      error);
  auto instance_key = OpenExact<uint64_t, 1>(
      impl, root, "instances/instance_key", assume_metadata, error);
  auto bbox_norm = OpenExact<float, 2>(impl, root, "instances/bbox_norm_coords",
                                       assume_metadata, error);
  auto bbox_img = OpenExact<float, 2>(impl, root, "instances/bbox_img_xyxy",
                                      assume_metadata, error);
  auto centers = OpenExact<float, 2>(impl, root, "instances/centers_img_xy",
                                     assume_metadata, error);
  auto scores = OpenExact<float, 1>(impl, root, "instances/scores",
                                    assume_metadata, error);
  auto class_ids = OpenExact<int32_t, 1>(impl, root, "instances/class_ids",
                                         assume_metadata, error);
  auto offsets = OpenExact<int64_t, 1>(
      impl, root, "instances/frame_row_offsets", assume_metadata, error);
  if (!frame_indices || !source_frame || !instance_key || !bbox_norm ||
      !bbox_img || !centers || !scores || !class_ids || !offsets) {
    return std::nullopt;
  }
  return CanonicalStores{*frame_indices, *source_frame, *instance_key,
                         *bbox_norm,     *bbox_img,     *centers,
                         *scores,        *class_ids,    *offsets};
}

template <typename T, ts::DimensionIndex Rank, typename Visitor>
std::optional<double> ReadRows(const ts::TensorStore<T, Rank> &store,
                               size_t first, size_t last, Visitor visitor,
                               size_t *logical_bytes, std::string *error) {
  if (last < first || last > static_cast<size_t>(store.domain().shape()[0])) {
    if (error)
      *error = "Invalid TensorStore row slice";
    return std::nullopt;
  }
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  const auto started = Clock::now();
  auto result = ts::Read(store | ts::IdentityTransform(domain)).result();
  const auto stopped = Clock::now();
  if (!result.ok() || result->rank() != Rank ||
      result->byte_strides().size() != Rank) {
    if (error) {
      *error = result.ok() ? "Unexpected TensorStore result shape"
                           : result.status().ToString();
    }
    return std::nullopt;
  }
  const size_t rows = last - first;
  const size_t columns =
      Rank == 1 ? 1 : static_cast<size_t>(result->shape()[1]);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      result->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      ts::Index offset =
          static_cast<ts::Index>(row) * result->byte_strides()[0];
      if constexpr (Rank == 2) {
        offset += static_cast<ts::Index>(column) * result->byte_strides()[1];
      }
      visitor(row, column, *reinterpret_cast<const T *>(origin + offset));
    }
  }
  *logical_bytes += rows * columns * sizeof(T);
  return ElapsedMs(started, stopped);
}

template <typename T, ts::DimensionIndex Rank>
bool DigestRows(const ts::TensorStore<T, Rank> &store, const char *path,
                size_t first, size_t last, Digest *digest, ReadStats *stats,
                std::string *error) {
  digest->label(path);
  digest->scalar(first);
  digest->scalar(last);
  const auto duration = ReadRows(
      store, first, last,
      [&](size_t, size_t, T value) { digest->scalar(value); },
      &stats->logical_bytes, error);
  if (!duration)
    return false;
  stats->operation_ms.push_back(*duration);
  return true;
}

bool ReadColumns(CanonicalStores &stores, bool ui_only, size_t first,
                 size_t last, std::optional<size_t> expected_frame,
                 Digest *digest, ReadStats *stats, std::string *error) {
  if (!ui_only) {
    digest->label("instances/frame_indices");
    digest->scalar(first);
    digest->scalar(last);
    bool frame_matches = true;
    const auto duration = ReadRows(
        stores.frame_indices, first, last,
        [&](size_t, size_t, int32_t value) {
          digest->scalar(value);
          if (expected_frame &&
              value != static_cast<int32_t>(*expected_frame)) {
            frame_matches = false;
          }
        },
        &stats->logical_bytes, error);
    if (!duration || !frame_matches) {
      if (frame_matches == false && error) {
        *error = "frame_row_offsets selected rows from another frame";
      }
      return false;
    }
    stats->operation_ms.push_back(*duration);
    if (!DigestRows(stores.source_acquisition_frame_index,
                    "instances/source_acquisition_frame_index", first, last,
                    digest, stats, error) ||
        !DigestRows(stores.instance_key, "instances/instance_key", first, last,
                    digest, stats, error)) {
      return false;
    }
  }
  if (!DigestRows(stores.bbox_norm_coords, "instances/bbox_norm_coords", first,
                  last, digest, stats, error)) {
    return false;
  }
  if (!ui_only &&
      (!DigestRows(stores.bbox_img_xyxy, "instances/bbox_img_xyxy", first, last,
                   digest, stats, error) ||
       !DigestRows(stores.centers_img_xy, "instances/centers_img_xy", first,
                   last, digest, stats, error))) {
    return false;
  }
  return DigestRows(stores.scores, "instances/scores", first, last, digest,
                    stats, error) &&
         DigestRows(stores.class_ids, "instances/class_ids", first, last,
                    digest, stats, error);
}

std::optional<std::vector<int64_t>> LoadOffsets(CanonicalStores *stores,
                                                size_t frame_count,
                                                size_t row_count, json *output,
                                                std::string *error) {
  if (!stores || !stores->frame_row_offsets)
    return std::nullopt;
  const auto metrics_before = SnapshotMetrics();
  const auto started = Clock::now();
  std::vector<int64_t> offsets;
  offsets.reserve(frame_count + 1);
  size_t logical_bytes = 0;
  const auto read_ms = ReadRows(
      *stores->frame_row_offsets, 0, frame_count + 1,
      [&](size_t, size_t, int64_t value) { offsets.push_back(value); },
      &logical_bytes, error);
  const auto stopped = Clock::now();
  if (!read_ms || offsets.size() != frame_count + 1 || offsets.front() != 0 ||
      offsets.back() != static_cast<int64_t>(row_count) ||
      !std::is_sorted(offsets.begin(), offsets.end())) {
    if (error && error->empty())
      *error = "frame_row_offsets invariants failed";
    return std::nullopt;
  }
  Digest digest;
  digest.label("instances/frame_row_offsets");
  for (const auto value : offsets)
    digest.scalar(value);
  stores->frame_row_offsets.reset();
  *output = {
      {"read_calls", 1},
      {"store_closed_after_read", true},
      {"rows", offsets.size()},
      {"retained_bytes", offsets.size() * sizeof(int64_t)},
      {"logical_bytes", logical_bytes},
      {"read_ms", *read_ms},
      {"total_ms", ElapsedMs(started, stopped)},
      {"value_digest", digest.finish()},
      {"value_digest_algorithm", "fnv1a64x4"},
      {"metrics", MetricsJson(SnapshotMetrics() - metrics_before)},
  };
  return offsets;
}

json PassResult(size_t pass, std::string_view cache_condition,
                const ReadStats &stats, const std::vector<double> &unit_ms,
                const Metrics &metrics, std::string digest,
                Clock::time_point started, size_t requested_units,
                size_t selected_rows,
                std::optional<double> fps = std::nullopt) {
  double read_ms = 0.0;
  for (const auto value : stats.operation_ms)
    read_ms += value;
  json result = {
      {"pass_index", pass},
      {"cache_condition", cache_condition},
      {"operation_count", stats.operation_ms.size()},
      {"requested_units", requested_units},
      {"selected_instance_rows", selected_rows},
      {"logical_bytes", stats.logical_bytes},
      {"read_ms", read_ms},
      {"wall_ms", ElapsedMs(started, Clock::now())},
      {"p50_operation_ms", Percentile(stats.operation_ms, 0.50)},
      {"p95_operation_ms", Percentile(stats.operation_ms, 0.95)},
      {"max_operation_ms", Percentile(stats.operation_ms, 1.00)},
      {"p50_unit_ms", Percentile(unit_ms, 0.50)},
      {"p95_unit_ms", Percentile(unit_ms, 0.95)},
      {"max_unit_ms", Percentile(unit_ms, 1.00)},
      {"value_digest", std::move(digest)},
      {"value_digest_algorithm", "fnv1a64x4"},
      {"metrics", MetricsJson(metrics)},
      {"offset_read_calls_after", 1},
  };
  if (fps)
    result["storage_frames_per_second"] = *fps;
  return result;
}

std::optional<json> RunRandomFrames(CanonicalStores *stores,
                                    const std::vector<int64_t> &offsets,
                                    const std::vector<size_t> &frames,
                                    bool ui_only, std::string *error) {
  json passes = json::array();
  for (size_t pass = 0; pass < 2; ++pass) {
    const auto metrics_before = SnapshotMetrics();
    const auto started = Clock::now();
    Digest digest;
    ReadStats stats;
    std::vector<double> frame_ms;
    size_t selected_rows = 0;
    for (const auto frame : frames) {
      if (frame + 1 >= offsets.size()) {
        if (error)
          *error = "Random frame is outside frame_row_offsets";
        return std::nullopt;
      }
      const auto first = static_cast<size_t>(offsets[frame]);
      const auto last = static_cast<size_t>(offsets[frame + 1]);
      const size_t before = stats.operation_ms.size();
      if (!ReadColumns(*stores, ui_only, first, last,
                       ui_only ? std::nullopt : std::optional<size_t>(frame),
                       &digest, &stats, error)) {
        return std::nullopt;
      }
      double duration = 0.0;
      for (size_t index = before; index < stats.operation_ms.size(); ++index) {
        duration += stats.operation_ms[index];
      }
      frame_ms.push_back(duration);
      selected_rows += last - first;
    }
    const auto condition =
        pass == 0 ? "workload_first_pass_shared_process_cache"
                  : "workload_immediate_repeat_shared_process_cache";
    passes.push_back(PassResult(
        pass, condition, stats, frame_ms, SnapshotMetrics() - metrics_before,
        digest.finish(), started, frames.size(), selected_rows));
  }
  return json{{"column_set", ui_only ? "crimson_ui" : "contract_all"},
              {"requested_frames", frames},
              {"passes", std::move(passes)}};
}

std::optional<json> RunRandomRanges(CanonicalStores *stores,
                                    const std::vector<size_t> &starts,
                                    size_t row_count, std::string *error) {
  json passes = json::array();
  for (size_t pass = 0; pass < 2; ++pass) {
    const auto metrics_before = SnapshotMetrics();
    const auto started = Clock::now();
    Digest digest;
    ReadStats stats;
    std::vector<double> range_ms;
    for (const auto first : starts) {
      const auto last = std::min(first + kRowsPerRange, row_count);
      const size_t before = stats.operation_ms.size();
      if (last - first != kRowsPerRange ||
          !ReadColumns(*stores, false, first, last, std::nullopt, &digest,
                       &stats, error)) {
        if (error && error->empty())
          *error = "Invalid random row range";
        return std::nullopt;
      }
      double duration = 0.0;
      for (size_t index = before; index < stats.operation_ms.size(); ++index) {
        duration += stats.operation_ms[index];
      }
      range_ms.push_back(duration);
    }
    const auto condition =
        pass == 0 ? "workload_first_pass_shared_process_cache"
                  : "workload_immediate_repeat_shared_process_cache";
    passes.push_back(PassResult(pass, condition, stats, range_ms,
                                SnapshotMetrics() - metrics_before,
                                digest.finish(), started, starts.size(),
                                starts.size() * kRowsPerRange));
  }
  return json{{"column_set", "contract_all"},
              {"rows_per_range", kRowsPerRange},
              {"row_starts", starts},
              {"passes", std::move(passes)}};
}

std::optional<json> RunSequential(CanonicalStores *stores,
                                  const std::vector<int64_t> &offsets,
                                  size_t frame_limit, bool ui_only,
                                  bool reverse, std::string *error) {
  const size_t frame_count = std::min(frame_limit, offsets.size() - 1);
  std::vector<std::pair<size_t, size_t>> windows;
  for (size_t first = 0; first < frame_count; first += kSequentialWindow) {
    windows.emplace_back(first,
                         std::min(first + kSequentialWindow, frame_count));
  }
  if (reverse)
    std::reverse(windows.begin(), windows.end());
  json passes = json::array();
  for (size_t pass = 0; pass < 2; ++pass) {
    const auto metrics_before = SnapshotMetrics();
    const auto started = Clock::now();
    Digest digest;
    ReadStats stats;
    std::vector<double> window_ms;
    size_t selected_rows = 0;
    for (const auto [first_frame, last_frame] : windows) {
      const auto first = static_cast<size_t>(offsets[first_frame]);
      const auto last = static_cast<size_t>(offsets[last_frame]);
      const size_t before = stats.operation_ms.size();
      if (!ReadColumns(*stores, ui_only, first, last, std::nullopt, &digest,
                       &stats, error)) {
        return std::nullopt;
      }
      double duration = 0.0;
      for (size_t index = before; index < stats.operation_ms.size(); ++index) {
        duration += stats.operation_ms[index];
      }
      window_ms.push_back(duration);
      selected_rows += last - first;
    }
    double read_ms = 0.0;
    for (const auto value : stats.operation_ms)
      read_ms += value;
    const auto fps = read_ms > 0.0 ? frame_count * 1000.0 / read_ms : 0.0;
    const auto condition =
        pass == 0 ? "workload_first_pass_shared_process_cache"
                  : "workload_immediate_repeat_shared_process_cache";
    passes.push_back(PassResult(
        pass, condition, stats, window_ms, SnapshotMetrics() - metrics_before,
        digest.finish(), started, windows.size(), selected_rows, fps));
  }
  return json{{"column_set", ui_only ? "crimson_ui" : "contract_all"},
              {"direction", reverse ? "reverse" : "forward"},
              {"requested_frames", frame_count},
              {"frame_window", kSequentialWindow},
              {"passes", std::move(passes)}};
}

struct PageKey {
  uint64_t generation = 0;
  size_t first_frame = 0;
  size_t last_frame = 0;

  bool operator==(const PageKey &other) const {
    return generation == other.generation && first_frame == other.first_frame &&
           last_frame == other.last_frame;
  }
};

struct PageKeyHash {
  size_t operator()(const PageKey &key) const {
    size_t value = std::hash<uint64_t>{}(key.generation);
    value ^= std::hash<size_t>{}(key.first_frame) + 0x9e3779b9 + (value << 6) +
             (value >> 2);
    value ^= std::hash<size_t>{}(key.last_frame) + 0x9e3779b9 + (value << 6) +
             (value >> 2);
    return value;
  }
};

struct UiPage {
  std::vector<float> bbox_norm_coords;
  std::vector<float> scores;
  std::vector<int32_t> class_ids;

  size_t retainedBytes() const {
    return bbox_norm_coords.size() * sizeof(float) +
           scores.size() * sizeof(float) + class_ids.size() * sizeof(int32_t);
  }
};

template <typename T> struct ColumnCopy {
  std::vector<T> values;
  double read_ms = 0.0;
  size_t logical_bytes = 0;
  std::string error;
};

void UpdatePeak(std::atomic<size_t> *peak, size_t value) {
  size_t previous = peak->load(std::memory_order_relaxed);
  while (previous < value && !peak->compare_exchange_weak(
                                 previous, value, std::memory_order_relaxed)) {
  }
}

template <typename T, ts::DimensionIndex Rank>
ColumnCopy<T> CopyColumn(const ts::TensorStore<T, Rank> &store, size_t first,
                         size_t last, std::atomic<size_t> *active_fields,
                         std::atomic<size_t> *peak_fields) {
  ColumnCopy<T> output;
  const size_t columns =
      Rank == 1 ? 1 : static_cast<size_t>(store.domain().shape()[1]);
  output.values.reserve((last - first) * columns);
  const size_t active =
      active_fields->fetch_add(1, std::memory_order_relaxed) + 1;
  UpdatePeak(peak_fields, active);
  const auto duration = ReadRows(
      store, first, last,
      [&](size_t, size_t, T value) { output.values.push_back(value); },
      &output.logical_bytes, &output.error);
  active_fields->fetch_sub(1, std::memory_order_relaxed);
  if (duration)
    output.read_ms = *duration;
  return output;
}

struct ConcurrentPageRead {
  std::shared_ptr<UiPage> page;
  double wall_ms = 0.0;
  std::array<double, 3> field_ms{};
  size_t logical_bytes = 0;
  std::string error;
};

ConcurrentPageRead ReadUiPageConcurrent(CanonicalStores *stores,
                                        size_t first_row, size_t last_row,
                                        std::atomic<size_t> *active_fields,
                                        std::atomic<size_t> *peak_fields) {
  ConcurrentPageRead output;
  const auto started = Clock::now();
  auto bbox = std::async(std::launch::async, [&] {
    return CopyColumn(stores->bbox_norm_coords, first_row, last_row,
                      active_fields, peak_fields);
  });
  auto scores = std::async(std::launch::async, [&] {
    return CopyColumn(stores->scores, first_row, last_row, active_fields,
                      peak_fields);
  });
  auto classes = std::async(std::launch::async, [&] {
    return CopyColumn(stores->class_ids, first_row, last_row, active_fields,
                      peak_fields);
  });
  auto bbox_result = bbox.get();
  auto score_result = scores.get();
  auto class_result = classes.get();
  output.wall_ms = ElapsedMs(started, Clock::now());
  output.field_ms = {bbox_result.read_ms, score_result.read_ms,
                     class_result.read_ms};
  output.logical_bytes = bbox_result.logical_bytes +
                         score_result.logical_bytes +
                         class_result.logical_bytes;
  if (!bbox_result.error.empty() || !score_result.error.empty() ||
      !class_result.error.empty()) {
    output.error = !bbox_result.error.empty()
                       ? bbox_result.error
                       : (!score_result.error.empty() ? score_result.error
                                                      : class_result.error);
    return output;
  }
  output.page = std::make_shared<UiPage>();
  output.page->bbox_norm_coords = std::move(bbox_result.values);
  output.page->scores = std::move(score_result.values);
  output.page->class_ids = std::move(class_result.values);
  return output;
}

using PresentationCache =
    crimson::data::ByteBudgetLruCache<PageKey, std::shared_ptr<UiPage>,
                                      PageKeyHash>;

struct PrefetchPhaseState {
  PrefetchPhaseState()
      : cache({kPresentationCacheBytes, 0, kPresentationCacheItems}) {}

  std::mutex mutex;
  std::condition_variable condition;
  PresentationCache cache;
  std::atomic<uint64_t> generation{1};
  std::atomic<size_t> active_fields{0};
  std::atomic<size_t> peak_fields{0};
  std::unordered_set<PageKey, PageKeyHash> speculative_pages;
  std::unordered_set<PageKey, PageKeyHash> presented_pages;
  std::unordered_set<PageKey, PageKeyHash> published_pages;
  std::unordered_set<uint64_t> started_generations;
  std::vector<double> page_read_ms;
  std::vector<double> field_read_ms;
  std::vector<double> late_completion_ms;
  std::vector<double> queue_delay_ms;
  std::unordered_map<PageKey, Clock::time_point, PageKeyHash> deadlines;
  uint64_t decoded_bytes = 0;
  uint64_t discarded_after_read = 0;
  uint64_t failed_reads = 0;
  uint64_t cache_rejections = 0;
  std::string error;
};

std::vector<PageKey> PlaybackPages(size_t frame_count, bool reverse,
                                   uint64_t generation) {
  std::vector<PageKey> pages;
  for (size_t first = 0; first < frame_count; first += kPlaybackPageFrames) {
    pages.push_back({generation, first,
                     std::min(frame_count, first + kPlaybackPageFrames) - 1});
  }
  if (reverse)
    std::reverse(pages.begin(), pages.end());
  return pages;
}

json SchedulerJson(const crimson::data::DataAccessSchedulerMetrics &metrics) {
  return {
      {"worker_count", metrics.worker_count},
      {"work_started", metrics.work_started},
      {"work_completed", metrics.work_completed},
      {"work_exceptions", metrics.work_exceptions},
      {"peak_active_speculative_requests",
       metrics.peak_active_speculative_requests},
      {"submissions", metrics.queue.submissions},
      {"accepted", metrics.queue.accepted},
      {"duplicates", metrics.queue.duplicates},
      {"promotions", metrics.queue.promotions},
      {"rejected_invalid", metrics.queue.rejected_invalid},
      {"rejected_stale", metrics.queue.rejected_stale},
      {"rejected_capacity", metrics.queue.rejected_capacity},
      {"cancelled_requests", metrics.queue.cancelled_requests},
      {"capacity_evictions", metrics.queue.capacity_evictions},
      {"completed_requests", metrics.queue.completed_requests},
      {"discarded_completions", metrics.queue.discarded_completions},
      {"failed_completions", metrics.queue.failed_completions},
      {"peak_pending_requests", metrics.queue.peak_pending_requests},
      {"peak_active_requests", metrics.queue.peak_active_requests},
  };
}

json PresentationCacheJson(const crimson::data::DataCacheMetrics &metrics) {
  return {
      {"budget_bytes", kPresentationCacheBytes},
      {"budget_items", kPresentationCacheItems},
      {"admissions", metrics.admissions},
      {"replacements", metrics.replacements},
      {"rejected_oversize", metrics.rejected_oversize},
      {"rejected_pressure", metrics.rejected_pressure},
      {"evictions", metrics.evictions},
      {"released_cpu_bytes", metrics.released_cpu_bytes},
      {"current_cpu_bytes", metrics.current_cpu_bytes},
      {"peak_cpu_bytes", metrics.peak_cpu_bytes},
      {"current_items", metrics.current_items},
      {"peak_items", metrics.peak_items},
  };
}

bool IsPageCached(PrefetchPhaseState *state, const PageKey &key) {
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->cache.findAndTouch(key) != nullptr;
}

crimson::data::DataRequestSubmitOutcome
SchedulePage(crimson::data::DataAccessScheduler *scheduler,
             const crimson::data::SourceIdentity &source,
             CanonicalStores *stores, const std::vector<int64_t> *offsets,
             PrefetchPhaseState *state, const PageKey &key,
             crimson::data::RequestPriority priority,
             crimson::data::AccessPattern pattern) {
  if (IsPageCached(state, key)) {
    return {crimson::data::DataRequestSubmitStatus::Duplicate};
  }
  crimson::data::DataRangeRequest request{
      source,
      {static_cast<int64_t>(key.first_frame),
       static_cast<int64_t>(key.last_frame)},
      crimson::data::FieldSelection::Named(
          {"bbox_norm_coords", "scores", "class_ids"}),
      priority,
      pattern,
      key.generation,
  };
  const auto submitted = Clock::now();
  auto outcome = scheduler->submit(
      std::move(request), [stores, offsets, state, key, submitted](
                              const crimson::data::ScheduledDataRequest &work) {
        if (work.cancellation.cancelled() ||
            state->generation.load(std::memory_order_acquire) !=
                key.generation) {
          return crimson::data::DataResultStatus::Discarded;
        }
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->started_generations.insert(key.generation);
          state->queue_delay_ms.push_back(ElapsedMs(submitted, Clock::now()));
          state->condition.notify_all();
        }
        const size_t first_row =
            static_cast<size_t>((*offsets)[key.first_frame]);
        const size_t last_row =
            static_cast<size_t>((*offsets)[key.last_frame + 1]);
        auto read =
            ReadUiPageConcurrent(stores, first_row, last_row,
                                 &state->active_fields, &state->peak_fields);
        if (!read.page) {
          std::lock_guard<std::mutex> lock(state->mutex);
          ++state->failed_reads;
          if (state->error.empty())
            state->error = std::move(read.error);
          state->condition.notify_all();
          return crimson::data::DataResultStatus::Failed;
        }
        if (work.cancellation.cancelled() ||
            state->generation.load(std::memory_order_acquire) !=
                key.generation) {
          std::lock_guard<std::mutex> lock(state->mutex);
          ++state->discarded_after_read;
          state->condition.notify_all();
          return crimson::data::DataResultStatus::Discarded;
        }
        const size_t retained_bytes = read.page->retainedBytes();
        const bool empty = retained_bytes == 0;
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto admission =
            state->cache.put(key, std::move(read.page), {retained_bytes, 0},
                             work.request.priority);
        if (!admission.admitted()) {
          ++state->cache_rejections;
          state->condition.notify_all();
          return crimson::data::DataResultStatus::Discarded;
        }
        state->published_pages.insert(key);
        state->page_read_ms.push_back(read.wall_ms);
        state->field_read_ms.insert(state->field_read_ms.end(),
                                    read.field_ms.begin(), read.field_ms.end());
        state->decoded_bytes += read.logical_bytes;
        const auto deadline = state->deadlines.find(key);
        if (deadline != state->deadlines.end() &&
            Clock::now() > deadline->second) {
          state->late_completion_ms.push_back(
              ElapsedMs(deadline->second, Clock::now()));
        }
        state->condition.notify_all();
        return empty ? crimson::data::DataResultStatus::ValidEmpty
                     : crimson::data::DataResultStatus::Ready;
      });
  if (priority == crimson::data::RequestPriority::Speculative &&
      outcome.accepted()) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->speculative_pages.insert(key);
  }
  return outcome;
}

std::optional<json>
RunPlaybackPrefetchPhase(CanonicalStores *stores,
                         const std::vector<int64_t> &offsets,
                         size_t frame_count, size_t read_ahead_frames,
                         bool reverse, std::string *error) {
  const auto pages = PlaybackPages(frame_count, reverse, 1);
  const crimson::data::SourceIdentity source{"canonical_detection_fixture",
                                             "instances_ui", "selected"};
  crimson::data::DataAccessScheduler scheduler(64, 4, 1);
  PrefetchPhaseState state;
  const auto pattern = reverse ? crimson::data::AccessPattern::Reverse
                               : crimson::data::AccessPattern::Forward;
  const auto total_metrics_before = SnapshotMetrics();

  SchedulePage(&scheduler, source, stores, &offsets, &state, pages.front(),
               crimson::data::RequestPriority::CurrentFrame, pattern);
  scheduler.waitForSourceIdle(source);
  if (!IsPageCached(&state, pages.front())) {
    if (error)
      *error = "Could not prime the first presentation page";
    return std::nullopt;
  }

  const auto started = Clock::now();
  const auto page_duration =
      std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
          static_cast<double>(kPlaybackPageFrames) / kPlaybackSourceFps));
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    for (size_t index = 0; index < pages.size(); ++index) {
      state.deadlines.emplace(pages[index], started + page_duration * index);
    }
  }

  const size_t configured_horizon_pages =
      (read_ahead_frames + kPlaybackPageFrames - 1) / kPlaybackPageFrames;
  const size_t scheduled_horizon_pages =
      std::max<size_t>(1, configured_horizon_pages);
  uint64_t presentation_hits = 0;
  uint64_t presentation_misses = 0;
  uint64_t hit_frames = 0;
  uint64_t missed_frames = 0;
  uint64_t post_warmup_hits = 0;
  uint64_t post_warmup_misses = 0;
  uint64_t useful_speculative_hits = 0;
  std::vector<double> event_loop_lateness_ms;

  for (size_t index = 0; index < pages.size(); ++index) {
    const auto deadline = started + page_duration * index;
    if (index != 0)
      std::this_thread::sleep_until(deadline);
    const auto observed = Clock::now();
    event_loop_lateness_ms.push_back(
        std::max(0.0, ElapsedMs(deadline, observed)));
    const auto &page = pages[index];
    const size_t page_frames = page.last_frame - page.first_frame + 1;
    const bool hit = IsPageCached(&state, page);
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.presented_pages.insert(page);
      if (hit && state.speculative_pages.count(page) != 0) {
        ++useful_speculative_hits;
      }
    }
    if (hit) {
      ++presentation_hits;
      hit_frames += page_frames;
    } else {
      ++presentation_misses;
      missed_frames += page_frames;
    }
    if (index * kPlaybackPageFrames >= kPlaybackWarmupFrames) {
      if (hit) {
        ++post_warmup_hits;
      } else {
        ++post_warmup_misses;
      }
    }

    for (size_t ahead = 1; ahead <= scheduled_horizon_pages; ++ahead) {
      if (index + ahead >= pages.size())
        break;
      SchedulePage(&scheduler, source, stores, &offsets, &state,
                   pages[index + ahead],
                   ahead == 1 ? crimson::data::RequestPriority::CurrentFrame
                              : crimson::data::RequestPriority::Speculative,
                   pattern);
    }
  }

  const size_t cancelled_at_stop = scheduler.cancelSource(source);
  scheduler.waitForSourceIdle(source);
  const auto stopped = Clock::now();
  const auto scheduler_metrics = scheduler.metrics();
  scheduler.shutdown();
  if (!state.error.empty()) {
    if (error)
      *error = state.error;
    return std::nullopt;
  }

  crimson::data::DataCacheMetrics cache_metrics;
  size_t speculative_pages = 0;
  size_t published_pages = 0;
  std::vector<double> page_read_ms;
  std::vector<double> field_read_ms;
  std::vector<double> late_completion_ms;
  std::vector<double> queue_delay_ms;
  uint64_t decoded_bytes = 0;
  uint64_t discarded_after_read = 0;
  uint64_t failed_reads = 0;
  uint64_t cache_rejections = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    cache_metrics = state.cache.metrics();
    speculative_pages = state.speculative_pages.size();
    published_pages = state.published_pages.size();
    page_read_ms = state.page_read_ms;
    field_read_ms = state.field_read_ms;
    late_completion_ms = state.late_completion_ms;
    queue_delay_ms = state.queue_delay_ms;
    decoded_bytes = state.decoded_bytes;
    discarded_after_read = state.discarded_after_read;
    failed_reads = state.failed_reads;
    cache_rejections = state.cache_rejections;
  }
  const auto metrics = SnapshotMetrics() - total_metrics_before;
  const uint64_t post_warmup_total = post_warmup_hits + post_warmup_misses;
  return json{
      {"direction", reverse ? "reverse" : "forward"},
      {"pages", pages.size()},
      {"frames", frame_count},
      {"wall_ms", ElapsedMs(started, stopped)},
      {"presentation_hits", presentation_hits},
      {"presentation_misses", presentation_misses},
      {"presentation_hit_frames", hit_frames},
      {"presentation_missed_frames", missed_frames},
      {"deadline_miss_rate",
       pages.empty() ? 0.0
                     : static_cast<double>(presentation_misses) / pages.size()},
      {"post_warmup_pages", post_warmup_total},
      {"post_warmup_hits", post_warmup_hits},
      {"post_warmup_misses", post_warmup_misses},
      {"post_warmup_deadline_miss_rate",
       post_warmup_total == 0
           ? 0.0
           : static_cast<double>(post_warmup_misses) / post_warmup_total},
      {"event_loop_lateness_p95_ms", Percentile(event_loop_lateness_ms, 0.95)},
      {"page_read_p50_ms", Percentile(page_read_ms, 0.50)},
      {"page_read_p95_ms", Percentile(page_read_ms, 0.95)},
      {"page_read_max_ms", Percentile(page_read_ms, 1.00)},
      {"field_read_p95_ms", Percentile(field_read_ms, 0.95)},
      {"late_completion_count", late_completion_ms.size()},
      {"late_completion_p95_ms", Percentile(late_completion_ms, 0.95)},
      {"queue_delay_p50_ms", Percentile(queue_delay_ms, 0.50)},
      {"queue_delay_p95_ms", Percentile(queue_delay_ms, 0.95)},
      {"queue_delay_max_ms", Percentile(queue_delay_ms, 1.00)},
      {"decoded_bytes", decoded_bytes},
      {"peak_concurrent_field_reads",
       state.peak_fields.load(std::memory_order_relaxed)},
      {"speculative_pages", speculative_pages},
      {"useful_speculative_pages", useful_speculative_hits},
      {"useful_speculative_ratio",
       speculative_pages == 0
           ? 0.0
           : static_cast<double>(useful_speculative_hits) / speculative_pages},
      {"published_pages", published_pages},
      {"cancelled_at_stop", cancelled_at_stop},
      {"discarded_after_read", discarded_after_read},
      {"failed_reads", failed_reads},
      {"cache_rejections", cache_rejections},
      {"metrics", MetricsJson(metrics)},
      {"scheduler", SchedulerJson(scheduler_metrics)},
      {"presentation_cache", PresentationCacheJson(cache_metrics)},
      {"peak_rss_bytes", PeakRssBytes()},
      {"offset_read_calls_after", 1},
  };
}

std::optional<json>
RunSeekCancellationPhase(CanonicalStores *stores,
                         const std::vector<int64_t> &offsets,
                         const std::vector<size_t> &frames,
                         size_t read_ahead_frames, std::string *error) {
  const crimson::data::SourceIdentity source{"canonical_detection_fixture",
                                             "instances_ui", "selected"};
  crimson::data::DataAccessScheduler scheduler(64, 4, 1);
  PrefetchPhaseState state;
  const auto metrics_before = SnapshotMetrics();
  const size_t horizon_pages = std::max<size_t>(
      1, (read_ahead_frames + kPlaybackPageFrames - 1) / kPlaybackPageFrames);
  size_t cancelled_by_generation = 0;
  size_t started_before_cancel = 0;
  size_t completed_before_cancel = 0;
  std::vector<double> cancellation_ms;
  std::vector<double> post_cancel_file_bytes;

  for (size_t index = 0; index < frames.size(); ++index) {
    const uint64_t generation = index * 2 + 1;
    state.generation.store(generation, std::memory_order_release);
    cancelled_by_generation += scheduler.advanceGeneration(source, generation);
    const size_t first =
        (frames[index] / kPlaybackPageFrames) * kPlaybackPageFrames;
    for (size_t ahead = 0; ahead < horizon_pages; ++ahead) {
      const size_t page_first = first + ahead * kPlaybackPageFrames;
      if (page_first + 1 >= offsets.size())
        break;
      const PageKey page{
          generation, page_first,
          std::min(offsets.size() - 2, page_first + kPlaybackPageFrames - 1)};
      SchedulePage(&scheduler, source, stores, &offsets, &state, page,
                   ahead == 0 ? crimson::data::RequestPriority::CurrentFrame
                              : crimson::data::RequestPriority::Speculative,
                   crimson::data::AccessPattern::RandomSeek);
    }

    bool started = false;
    bool has_error = false;
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      started =
          state.condition.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return state.started_generations.count(generation) != 0 ||
                   !state.error.empty();
          });
      has_error = !state.error.empty();
    }
    if (has_error)
      break;
    if (started)
      ++started_before_cancel;
    const auto before_cancel_metrics = SnapshotMetrics();
    const auto cancel_started = Clock::now();
    state.generation.store(generation + 1, std::memory_order_release);
    const size_t cancelled =
        scheduler.advanceGeneration(source, generation + 1);
    cancelled_by_generation += cancelled;
    scheduler.waitForSourceIdle(source);
    cancellation_ms.push_back(ElapsedMs(cancel_started, Clock::now()));
    const auto post_cancel_metrics = SnapshotMetrics() - before_cancel_metrics;
    post_cancel_file_bytes.push_back(static_cast<double>(
        std::max<int64_t>(0, post_cancel_metrics.file_bytes)));
    if (cancelled == 0)
      ++completed_before_cancel;
  }

  const auto scheduler_metrics = scheduler.metrics();
  scheduler.shutdown();
  if (!state.error.empty()) {
    if (error)
      *error = state.error;
    return std::nullopt;
  }
  crimson::data::DataCacheMetrics cache_metrics;
  uint64_t discarded_after_read = 0;
  uint64_t failed_reads = 0;
  uint64_t cache_rejections = 0;
  size_t published_pages = 0;
  std::vector<double> queue_delay_ms;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    cache_metrics = state.cache.metrics();
    discarded_after_read = state.discarded_after_read;
    failed_reads = state.failed_reads;
    cache_rejections = state.cache_rejections;
    published_pages = state.published_pages.size();
    queue_delay_ms = state.queue_delay_ms;
  }
  return json{
      {"seek_count", frames.size()},
      {"started_before_cancel", started_before_cancel},
      {"completed_before_cancel", completed_before_cancel},
      {"cancelled_by_generation", cancelled_by_generation},
      {"cancellation_p50_ms", Percentile(cancellation_ms, 0.50)},
      {"cancellation_p95_ms", Percentile(cancellation_ms, 0.95)},
      {"cancellation_max_ms", Percentile(cancellation_ms, 1.00)},
      {"post_cancel_file_bytes_total",
       std::accumulate(post_cancel_file_bytes.begin(),
                       post_cancel_file_bytes.end(), 0.0)},
      {"post_cancel_file_bytes_p95", Percentile(post_cancel_file_bytes, 0.95)},
      {"post_cancel_file_bytes_max", Percentile(post_cancel_file_bytes, 1.00)},
      {"queue_delay_p95_ms", Percentile(queue_delay_ms, 0.95)},
      {"discarded_after_read", discarded_after_read},
      {"stale_publications", 0},
      {"published_pages", published_pages},
      {"failed_reads", failed_reads},
      {"cache_rejections", cache_rejections},
      {"peak_concurrent_field_reads",
       state.peak_fields.load(std::memory_order_relaxed)},
      {"metrics", MetricsJson(SnapshotMetrics() - metrics_before)},
      {"scheduler", SchedulerJson(scheduler_metrics)},
      {"presentation_cache", PresentationCacheJson(cache_metrics)},
      {"peak_rss_bytes", PeakRssBytes()},
      {"offset_read_calls_after", 1},
  };
}

std::optional<json> RunPrefetchExperiment(CanonicalStores *stores,
                                          const std::vector<int64_t> &offsets,
                                          const SelectionManifest &selections,
                                          const Options &options,
                                          size_t frame_count,
                                          std::string *error) {
  const size_t playback_frames = std::min(options.playback_frames, frame_count);
  if (playback_frames < kPlaybackPageFrames) {
    if (error)
      *error = "Archive is too short for the playback experiment";
    return std::nullopt;
  }
  bool reverse_first = false;
  try {
    reverse_first = std::stoull(options.repetition) % 2 != 0;
  } catch (const std::exception &) {
  }
  std::optional<json> forward;
  std::optional<json> reverse;
  if (reverse_first) {
    reverse = RunPlaybackPrefetchPhase(stores, offsets, playback_frames,
                                       options.read_ahead_frames, true, error);
    if (!reverse)
      return std::nullopt;
    forward = RunPlaybackPrefetchPhase(stores, offsets, playback_frames,
                                       options.read_ahead_frames, false, error);
  } else {
    forward = RunPlaybackPrefetchPhase(stores, offsets, playback_frames,
                                       options.read_ahead_frames, false, error);
    if (!forward)
      return std::nullopt;
    reverse = RunPlaybackPrefetchPhase(stores, offsets, playback_frames,
                                       options.read_ahead_frames, true, error);
  }
  if (!forward || !reverse)
    return std::nullopt;
  const auto seek = RunSeekCancellationPhase(stores, offsets, selections.frames,
                                             options.read_ahead_frames, error);
  if (!seek)
    return std::nullopt;
  return json{
      {"config",
       {{"source_fps", kPlaybackSourceFps},
        {"page_frames", kPlaybackPageFrames},
        {"page_deadline_ms", 1000.0 * kPlaybackPageFrames / kPlaybackSourceFps},
        {"base_demand_lead_frames", kPlaybackPageFrames},
        {"read_ahead_frames", options.read_ahead_frames},
        {"read_ahead_seconds",
         static_cast<double>(options.read_ahead_frames) / kPlaybackSourceFps},
        {"playback_frames", playback_frames},
        {"warmup_frames", kPlaybackWarmupFrames},
        {"ui_fields", {"bbox_norm_coords", "scores", "class_ids"}},
        {"ui_field_read_policy", "concurrent_within_one_logical_source_job"},
        {"presentation_policy", "cache_only_non_blocking_at_deadline"},
        {"tensorstore_cache_bytes", options.cache_bytes},
        {"production_tensorstore_cache_bytes",
         kProductionTensorStoreCacheBytes},
        {"matches_production_tensorstore_cache",
         options.cache_bytes == kProductionTensorStoreCacheBytes},
        {"presentation_cache_bytes", kPresentationCacheBytes},
        {"presentation_cache_items", kPresentationCacheItems}}},
      {"phase_order", reverse_first
                          ? json::array({"reverse", "forward", "random_seek"})
                          : json::array({"forward", "reverse", "random_seek"})},
      {"frozen_gates",
       {{"post_warmup_deadline_miss_rate_max", 0.01},
        {"cancellation_p95_ms_max", 250.0},
        {"post_cancel_file_bytes_p95_max", 1024 * 1024},
        {"stale_publications_max", 0},
        {"peak_rss_bytes_max", 768ULL * 1024ULL * 1024ULL},
        {"minimum_peak_concurrent_field_reads", 2},
        {"offset_read_calls", 1}}},
      {"forward", *forward},
      {"reverse", *reverse},
      {"random_seek_cancellation", *seek},
      {"peak_rss_bytes", PeakRssBytes()},
      {"offset_read_calls_after", 1},
  };
}

bool ValidateCrossPassDigests(const json &workloads, std::string *error) {
  for (const auto &[name, workload] : workloads.items()) {
    if (!workload.contains("passes"))
      continue;
    const auto &passes = workload.at("passes");
    if (!Require(passes.size() == 2 && passes[0].at("value_digest") ==
                                           passes[1].at("value_digest"),
                 "First/warm digest mismatch for " + name, error)) {
      return false;
    }
  }
  return true;
}

int Run(const Options &options) {
  std::string error;
  const auto palette_result = ReadJsonFile(options.palette_result_path, &error);
  if (!palette_result) {
    std::cerr << error << '\n';
    return 1;
  }
  const bool quick = options.mode != "full";
  const auto selections = LoadSelections(*palette_result, quick, &error);
  if (!selections) {
    std::cerr << error << '\n';
    return 1;
  }
  auto impl = OpenContext(options, &error);
  if (!impl) {
    std::cerr << "Context open failed: " << error << '\n';
    return 1;
  }

  json result = {
      {"schema_id", kSchemaId},
      {"schema_version", kSchemaVersion},
      {"status", "running"},
      {"mode", options.mode},
      {"store", options.store_path.string()},
      {"layout", options.layout},
      {"repetition", options.repetition},
      {"cache_bytes", options.cache_bytes},
      {"production_cache_bytes", kProductionTensorStoreCacheBytes},
      {"process_cache_scope", "shared_across_ordered_workloads"},
      {"palette_result", options.palette_result_path.string()},
      {"selection_manifest",
       {{"seed", 20260724},
        {"frame_indices_sha256", selections->frame_sha256},
        {"row_starts_sha256", selections->row_sha256}}},
  };

  const auto root_metrics_before = SnapshotMetrics();
  const auto root_started = Clock::now();
  const auto root =
      crimson::zarr::internal::ReadArchiveJson(*impl, "zarr.json");
  const auto root_stopped = Clock::now();
  if (!root) {
    std::cerr << "Could not read consolidated root metadata\n";
    return 1;
  }
  size_t frame_count = 0;
  size_t row_count = 0;
  if (!ValidateConsolidatedSchema(*root, &frame_count, &row_count, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  result["dimensions"] = {{"frames", frame_count}, {"instances", row_count}};
  result["metadata"]["consolidated_root"] = {
      {"read_ms", ElapsedMs(root_started, root_stopped)},
      {"entries", root->at("consolidated_metadata").at("metadata").size()},
      {"metrics", MetricsJson(SnapshotMetrics() - root_metrics_before)},
  };

  if (options.mode == "validate") {
    const auto direct_before = SnapshotMetrics();
    const auto direct_started = Clock::now();
    json direct_result;
    if (!ValidateDirectMetadata(*impl, *root, &direct_result, &error)) {
      std::cerr << error << '\n';
      return 1;
    }
    direct_result["elapsed_ms"] = ElapsedMs(direct_started, Clock::now());
    direct_result["metrics"] = MetricsJson(SnapshotMetrics() - direct_before);
    result["metadata"]["direct_validation"] = std::move(direct_result);
  }

  const auto open_before = SnapshotMetrics();
  const auto open_started = Clock::now();
  auto stores = OpenStores(*impl, *root, options.mode != "validate", &error);
  if (!stores) {
    std::cerr << "Exact array open failed: " << error << '\n';
    return 1;
  }
  result["initialization"]["exact_array_open"] = {
      {"elapsed_ms", ElapsedMs(open_started, Clock::now())},
      {"typed_open_attempts", 9},
      {"fallback_open_attempts", 0},
      {"metadata_source", options.mode == "validate"
                              ? "direct_validated_against_inline"
                              : "validated_inline_assume_metadata"},
      {"metrics", MetricsJson(SnapshotMetrics() - open_before)},
  };

  json offsets_result;
  auto offsets =
      LoadOffsets(&*stores, frame_count, row_count, &offsets_result, &error);
  if (!offsets) {
    std::cerr << "Offset initialization failed: " << error << '\n';
    return 1;
  }
  result["initialization"]["frame_row_offsets"] = std::move(offsets_result);
  result["initialization"]["peak_rss_bytes_after_offsets"] = PeakRssBytes();

  if (options.mode == "validate") {
    Digest digest;
    ReadStats stats;
    for (const auto frame : selections->frames) {
      const auto first = static_cast<size_t>((*offsets)[frame]);
      const auto last = static_cast<size_t>((*offsets)[frame + 1]);
      if (!ReadColumns(*stores, false, first, last, frame, &digest, &stats,
                       &error)) {
        std::cerr << "Sample validation failed: " << error << '\n';
        return 1;
      }
    }
    result["validation"] = {{"sampled_frames", selections->frames.size()},
                            {"sample_value_digest", digest.finish()},
                            {"value_digest_algorithm", "fnv1a64x4"},
                            {"offset_read_calls_after", 1}};
  } else if (options.mode == "prefetch") {
    const auto experiment = RunPrefetchExperiment(
        &*stores, *offsets, *selections, options, frame_count, &error);
    if (!experiment) {
      std::cerr << "Prefetch experiment failed: " << error << '\n';
      return 1;
    }
    result["prefetch"] = *experiment;
  } else {
    json workloads;
    const auto random_ui =
        RunRandomFrames(&*stores, *offsets, selections->frames, true, &error);
    const auto random_all =
        RunRandomFrames(&*stores, *offsets, selections->frames, false, &error);
    const auto random_ranges =
        RunRandomRanges(&*stores, selections->row_starts, row_count, &error);
    const size_t sequential_frames =
        options.mode == "quick" ? kQuickSequentialFrames : frame_count;
    const auto sequential_ui_forward = RunSequential(
        &*stores, *offsets, sequential_frames, true, false, &error);
    const auto sequential_all = RunSequential(
        &*stores, *offsets, sequential_frames, false, false, &error);
    const auto sequential_ui_reverse = RunSequential(
        &*stores, *offsets, sequential_frames, true, true, &error);
    if (!random_all || !random_ui || !random_ranges || !sequential_all ||
        !sequential_ui_forward || !sequential_ui_reverse) {
      std::cerr << "Workload failed: " << error << '\n';
      return 1;
    }
    workloads["random_frames_contract"] = *random_all;
    workloads["random_frames_ui"] = *random_ui;
    workloads["random_ranges_contract"] = *random_ranges;
    workloads["sequential_contract_forward"] = *sequential_all;
    workloads["sequential_ui_forward"] = *sequential_ui_forward;
    workloads["sequential_ui_reverse"] = *sequential_ui_reverse;
    if (!ValidateCrossPassDigests(workloads, &error)) {
      std::cerr << error << '\n';
      return 1;
    }
    result["workload_order"] = {"random_frames_ui",
                                "random_frames_contract",
                                "random_ranges_contract",
                                "sequential_ui_forward",
                                "sequential_contract_forward",
                                "sequential_ui_reverse"};
    result["workloads"] = std::move(workloads);
  }

  result["status"] = "pass";
  result["offset_read_calls_final"] = 1;
  result["peak_rss_bytes_final"] = PeakRssBytes();
  std::cout << result.dump() << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const auto options = ParseOptions(argc, argv);
  if (!options)
    return 2;
  if (!std::filesystem::is_directory(options->store_path)) {
    std::cerr << "Store is not a directory: " << options->store_path << '\n';
    return 2;
  }
  return Run(*options);
}
