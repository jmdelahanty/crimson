#include <tensorstore/array.h>
#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/internal/metrics/registry.h>
#include <tensorstore/kvstore/spec.h>
#include <tensorstore/open.h>
#include <tensorstore/spec.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"

namespace {
namespace ts = tensorstore;
using Clock = std::chrono::steady_clock;
using Store =
    std::variant<ts::TensorStore<int32_t, 1>, ts::TensorStore<int64_t, 1>>;

struct MetricSnapshot {
  int64_t reads = 0;
  int64_t batch_reads = 0;
  int64_t bytes_read = 0;
};

struct Options {
  std::filesystem::path archive;
  std::string array_path;
  size_t cache_bytes = 0;
  size_t forward_start = 0;
  size_t forward_count = 700;
  size_t random_count = 100;
  uint64_t random_seed = 0x4352494d534f4eULL;
};

int64_t CounterValue(std::string_view name) {
  const auto metric = ts::internal_metrics::GetMetricRegistry().Collect(name);
  if (!metric || metric->values.empty()) {
    return 0;
  }
  return std::get<int64_t>(metric->values.front().value);
}

MetricSnapshot SnapshotMetrics() {
  return {
      CounterValue("/tensorstore/kvstore/file/read"),
      CounterValue("/tensorstore/kvstore/file/batch_read"),
      CounterValue("/tensorstore/kvstore/file/bytes_read"),
  };
}

MetricSnapshot operator-(const MetricSnapshot& after,
                         const MetricSnapshot& before) {
  return {
      after.reads - before.reads,
      after.batch_reads - before.batch_reads,
      after.bytes_read - before.bytes_read,
  };
}

double ElapsedMilliseconds(Clock::time_point start, Clock::time_point stop) {
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

size_t RowCount(const Store& store) {
  return std::visit(
      [](const auto& typed) {
        const auto rows = typed.domain().shape()[0];
        return rows > 0 ? static_cast<size_t>(rows) : size_t{0};
      },
      store);
}

template <typename T>
bool ReadRange(const ts::TensorStore<T, 1>& store, size_t first, size_t last,
               std::string* error) {
  ts::Box<1> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  auto read = ts::Read(store | ts::IdentityTransform(domain)).result();
  if (!read.ok()) {
    if (error) {
      *error = read.status().ToString();
    }
    return false;
  }
  if (read->rank() != 1 || read->shape()[0] != domain.shape()[0]) {
    if (error) {
      *error = "TensorStore returned an unexpected read shape";
    }
    return false;
  }
  return true;
}

bool ReadRange(const Store& store, size_t first, size_t last,
               std::string* error) {
  return std::visit(
      [&](const auto& typed) { return ReadRange(typed, first, last, error); },
      store);
}

std::optional<Store> OpenArray(const crimson::zarr::ArchiveContext::Impl& impl,
                               const std::string& path, std::string* error) {
  const auto spec_json =
      crimson::zarr::internal::MakeReadOnlyArraySpec(impl, path);
  if (!spec_json) {
    if (error) {
      *error = "Could not create the read-only array specification";
    }
    return std::nullopt;
  }

  auto int32_store = ts::Open<int32_t, 1>(*spec_json, ts::OpenMode::open,
                                          ts::ReadWriteMode::read, impl.context)
                         .result();
  if (int32_store.ok()) {
    return Store{*int32_store};
  }
  auto int64_store = ts::Open<int64_t, 1>(*spec_json, ts::OpenMode::open,
                                          ts::ReadWriteMode::read, impl.context)
                         .result();
  if (int64_store.ok()) {
    return Store{*int64_store};
  }
  if (error) {
    *error = "Array must be a rank-one int32 or int64 array: int32=" +
             int32_store.status().ToString() +
             " int64=" + int64_store.status().ToString();
  }
  return std::nullopt;
}

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<size_t>(
      std::max(0.0, std::ceil(percentile * values.size()) - 1.0));
  return values[std::min(rank, values.size() - 1)];
}

uint64_t NextRandom(uint64_t* state) {
  uint64_t value = *state;
  value ^= value >> 12;
  value ^= value << 25;
  value ^= value >> 27;
  *state = value;
  return value * 2685821657736338717ULL;
}

void PrintResult(const Options& options, std::string_view workload,
                 size_t row_count, size_t requests, size_t values,
                 double open_ms, const MetricSnapshot& open_metrics,
                 const std::vector<double>& latencies,
                 const MetricSnapshot& read_metrics) {
  double total_ms = 0.0;
  for (const auto latency : latencies) {
    total_ms += latency;
  }
  std::cout << std::fixed << std::setprecision(3) << "workload=" << workload
            << " cache_bytes=" << options.cache_bytes << " rows=" << row_count
            << " requests=" << requests << " values=" << values
            << " open_ms=" << open_ms
            << " open_file_reads=" << open_metrics.reads
            << " open_file_batch_reads=" << open_metrics.batch_reads
            << " open_file_bytes=" << open_metrics.bytes_read
            << " elapsed_ms=" << total_ms
            << " p50_ms=" << Percentile(latencies, 0.50)
            << " p95_ms=" << Percentile(latencies, 0.95)
            << " p99_ms=" << Percentile(latencies, 0.99)
            << " max_ms=" << Percentile(latencies, 1.00)
            << " file_reads=" << read_metrics.reads
            << " file_batch_reads=" << read_metrics.batch_reads
            << " file_bytes=" << read_metrics.bytes_read << '\n';
}

template <typename MakeRange>
bool RunWorkload(const Options& options, std::string_view workload,
                 size_t request_count, MakeRange make_range) {
  const auto metrics_before_open = SnapshotMetrics();
  const auto open_start = Clock::now();
  auto context = crimson::zarr::internal::MakeArchiveTensorStoreContext(
      options.cache_bytes);
  if (!context.ok()) {
    std::cerr << "Could not create TensorStore context: " << context.status()
              << '\n';
    return false;
  }
  auto kvstore_spec = ts::kvstore::Spec::FromJson(
      {{"driver", "file"}, {"path", options.archive.string() + "/"}});
  if (!kvstore_spec.ok()) {
    std::cerr << "Could not create file kvstore spec: " << kvstore_spec.status()
              << '\n';
    return false;
  }
  auto kvstore = ts::kvstore::Open(*kvstore_spec, *context).result();
  if (!kvstore.ok()) {
    std::cerr << "Could not open archive kvstore: " << kvstore.status() << '\n';
    return false;
  }

  crimson::zarr::ArchiveContext::Impl impl;
  impl.root_path = options.archive;
  impl.context = *context;
  impl.store = *kvstore;
  impl.cache_pool_bytes = options.cache_bytes;
  std::string error;
  auto store = OpenArray(impl, options.array_path, &error);
  if (!store) {
    std::cerr << "Could not open array " << options.array_path << ": " << error
              << '\n';
    return false;
  }
  const auto open_stop = Clock::now();
  const auto metrics_after_open = SnapshotMetrics();
  const auto row_count = RowCount(*store);
  if (row_count == 0) {
    std::cerr << "The selected array is empty\n";
    return false;
  }

  std::vector<double> latencies;
  latencies.reserve(request_count);
  size_t values_read = 0;
  const auto metrics_before_reads = SnapshotMetrics();
  for (size_t request = 0; request < request_count; ++request) {
    const auto [first, last] = make_range(request, row_count);
    if (first >= last || last > row_count) {
      std::cerr << "Workload generated invalid range [" << first << ", " << last
                << ") for " << row_count << " rows\n";
      return false;
    }
    const auto read_start = Clock::now();
    if (!ReadRange(*store, first, last, &error)) {
      std::cerr << "Read [" << first << ", " << last << ") failed: " << error
                << '\n';
      return false;
    }
    const auto read_stop = Clock::now();
    latencies.push_back(ElapsedMilliseconds(read_start, read_stop));
    values_read += last - first;
  }
  const auto metrics_after_reads = SnapshotMetrics();
  PrintResult(options, workload, row_count, request_count, values_read,
              ElapsedMilliseconds(open_start, open_stop),
              metrics_after_open - metrics_before_open, latencies,
              metrics_after_reads - metrics_before_reads);
  return true;
}

std::optional<size_t> ParseSize(const char* value, const char* name) {
  try {
    const auto parsed = std::stoull(value);
    if (parsed > std::numeric_limits<size_t>::max()) {
      throw std::out_of_range("size_t");
    }
    return static_cast<size_t>(parsed);
  } catch (const std::exception& exception) {
    std::cerr << "Invalid " << name << " value '" << value
              << "': " << exception.what() << '\n';
    return std::nullopt;
  }
}

std::optional<Options> ParseOptions(int argc, char** argv) {
  if (argc < 4 || argc > 8) {
    std::cerr << "Usage: " << argv[0]
              << " ARCHIVE.zarr ARRAY_PATH CACHE_BYTES [FORWARD_START]"
                 " [FORWARD_COUNT] [RANDOM_COUNT] [RANDOM_SEED]\n";
    return std::nullopt;
  }
  Options options;
  options.archive = argv[1];
  options.array_path = argv[2];
  const auto cache_bytes = ParseSize(argv[3], "cache byte");
  if (!cache_bytes) return std::nullopt;
  options.cache_bytes = *cache_bytes;
  if (argc >= 5) {
    const auto value = ParseSize(argv[4], "forward start");
    if (!value) return std::nullopt;
    options.forward_start = *value;
  }
  if (argc >= 6) {
    const auto value = ParseSize(argv[5], "forward count");
    if (!value) return std::nullopt;
    options.forward_count = *value;
  }
  if (argc >= 7) {
    const auto value = ParseSize(argv[6], "random count");
    if (!value) return std::nullopt;
    options.random_count = *value;
  }
  if (argc >= 8) {
    const auto value = ParseSize(argv[7], "random seed");
    if (!value) return std::nullopt;
    options.random_seed = *value;
  }
  if (options.forward_count == 0 || options.random_count == 0) {
    std::cerr << "Forward and random request counts must be positive\n";
    return std::nullopt;
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = ParseOptions(argc, argv);
  if (!options) {
    return 2;
  }
  if (!std::filesystem::is_directory(options->archive)) {
    std::cerr << "Archive is not a directory: " << options->archive << '\n';
    return 2;
  }

  bool passed = RunWorkload(
      *options, "forward", options->forward_count,
      [&](size_t request, size_t rows) {
        const auto first = options->forward_start + request;
        return std::pair<size_t, size_t>{first, std::min(first + 2, rows)};
      });

  uint64_t random_state = options->random_seed;
  passed &= RunWorkload(
      *options, "random", options->random_count, [&](size_t, size_t rows) {
        const auto span = rows > 1 ? rows - 1 : size_t{1};
        const auto first =
            static_cast<size_t>(NextRandom(&random_state) % span);
        return std::pair<size_t, size_t>{first, std::min(first + 2, rows)};
      });

  passed &= RunWorkload(*options, "eager", 1, [](size_t, size_t rows) {
    return std::pair<size_t, size_t>{0, rows};
  });
  return passed ? 0 : 1;
}
