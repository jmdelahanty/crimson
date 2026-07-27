#include "zarr/tensorstore_keypoint_overlay_repository.h"

#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "zarr/archive_context_internal.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

using IntegerStore =
    std::variant<ts::TensorStore<int64_t, 1>, ts::TensorStore<uint64_t, 1>,
                 ts::TensorStore<int32_t, 1>, ts::TensorStore<uint32_t, 1>,
                 ts::TensorStore<int16_t, 1>, ts::TensorStore<uint16_t, 1>,
                 ts::TensorStore<int8_t, 1>, ts::TensorStore<uint8_t, 1>>;
using RealStore =
    std::variant<ts::TensorStore<double, 1>, ts::TensorStore<float, 1>,
                 ts::TensorStore<int64_t, 1>, ts::TensorStore<int32_t, 1>>;
using BoolStore =
    std::variant<ts::TensorStore<bool, 1>, ts::TensorStore<uint8_t, 1>,
                 ts::TensorStore<int8_t, 1>, ts::TensorStore<int32_t, 1>>;
using MatrixStore =
    std::variant<ts::TensorStore<double, 2>, ts::TensorStore<float, 2>,
                 ts::TensorStore<int64_t, 2>, ts::TensorStore<int32_t, 2>>;
using KeypointStore =
    std::variant<ts::TensorStore<double, 3>, ts::TensorStore<float, 3>>;

double ElapsedMilliseconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

template <typename T>
const char* TypeName() {
  if constexpr (std::is_same_v<T, bool>) return "bool";
  if constexpr (std::is_same_v<T, int8_t>) return "int8";
  if constexpr (std::is_same_v<T, uint8_t>) return "uint8";
  if constexpr (std::is_same_v<T, int16_t>) return "int16";
  if constexpr (std::is_same_v<T, uint16_t>) return "uint16";
  if constexpr (std::is_same_v<T, int32_t>) return "int32";
  if constexpr (std::is_same_v<T, uint32_t>) return "uint32";
  if constexpr (std::is_same_v<T, int64_t>) return "int64";
  if constexpr (std::is_same_v<T, uint64_t>) return "uint64";
  if constexpr (std::is_same_v<T, float>) return "float32";
  if constexpr (std::is_same_v<T, double>) return "float64";
  return "unknown";
}

class KeypointOpenTrace {
 public:
  explicit KeypointOpenTrace(KeypointRepositoryOpenMetrics* metrics)
      : metrics_(metrics), started_(std::chrono::steady_clock::now()) {
    if (metrics_) {
      *metrics_ = {};
    }
  }

  ~KeypointOpenTrace() {
    if (metrics_) {
      metrics_->total_ms = ElapsedMilliseconds(started_);
    }
  }

  void finishPhase(double KeypointRepositoryOpenMetrics::* field,
                   std::chrono::steady_clock::time_point start) {
    if (metrics_) {
      metrics_->*field += ElapsedMilliseconds(start);
    }
  }

  void record(const std::string& phase, const std::string& operation,
              const std::string& path, const std::string& candidate,
              std::chrono::steady_clock::time_point start, bool success) {
    if (!metrics_) {
      return;
    }
    metrics_->events.push_back({phase, operation, path, candidate,
                                ElapsedMilliseconds(start), success});
    if (operation == "attributes") {
      ++metrics_->attribute_reads;
    } else if (operation == "array_open") {
      ++metrics_->array_open_attempts;
      if (success) {
        ++metrics_->array_open_successes;
      } else {
        ++metrics_->array_open_failures;
      }
    } else if (operation == "array_read") {
      ++metrics_->array_reads;
    }
  }

  void setLazyAttempted() {
    if (metrics_) metrics_->lazy_attempted = true;
  }
  void setLazyPath() {
    if (metrics_) metrics_->lazy_path = true;
  }
  void setFallbackPath() {
    if (metrics_) metrics_->fallback_path = true;
  }

 private:
  KeypointRepositoryOpenMetrics* metrics_ = nullptr;
  std::chrono::steady_clock::time_point started_;
};

class KeypointPhaseTimer {
 public:
  KeypointPhaseTimer(KeypointOpenTrace* trace,
                     double KeypointRepositoryOpenMetrics::* field)
      : trace_(trace),
        field_(field),
        started_(std::chrono::steady_clock::now()) {}
  ~KeypointPhaseTimer() {
    if (trace_) {
      trace_->finishPhase(field_, started_);
    }
  }

 private:
  KeypointOpenTrace* trace_ = nullptr;
  double KeypointRepositoryOpenMetrics::* field_ = nullptr;
  std::chrono::steady_clock::time_point started_;
};

std::optional<json> ReadAttributes(const ArchiveContext::Impl& archive,
                                   const std::string& path,
                                   KeypointOpenTrace* trace,
                                   const std::string& phase) {
  const auto started = std::chrono::steady_clock::now();
  auto attributes = internal::ReadArchiveAttributes(archive, path);
  if (trace) {
    trace->record(phase, "attributes", path, "json", started,
                  attributes.has_value());
  }
  return attributes;
}

std::optional<json> MakeArraySpec(const ArchiveContext::Impl& archive,
                                  const std::string& path) {
  return internal::MakeReadOnlyArraySpec(archive, path);
}

template <typename T, size_t Rank>
std::optional<ts::TensorStore<T, Rank>> OpenArray(
    const ArchiveContext::Impl& archive, const std::string& path,
    KeypointOpenTrace* trace, const std::string& phase) {
  const auto started = std::chrono::steady_clock::now();
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    if (trace) {
      trace->record(
          phase, "array_open", path,
          std::string(TypeName<T>()) + "[" + std::to_string(Rank) + "]",
          started, false);
    }
    return std::nullopt;
  }
  auto store = ts::Open<T, Rank>(*spec, ts::OpenMode::open,
                                 ts::ReadWriteMode::read, archive.context)
                   .result();
  if (trace) {
    trace->record(phase, "array_open", path,
                  std::string(TypeName<T>()) + "[" + std::to_string(Rank) + "]",
                  started, store.ok());
  }
  return store.ok() ? std::optional<ts::TensorStore<T, Rank>>(*store)
                    : std::nullopt;
}

std::optional<IntegerStore> OpenIntegerStore(
    const ArchiveContext::Impl& archive, const std::string& path,
    KeypointOpenTrace* trace, const std::string& phase) {
  if (auto store = OpenArray<int64_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  if (auto store = OpenArray<uint64_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  if (auto store = OpenArray<int32_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  if (auto store = OpenArray<uint32_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  if (auto store = OpenArray<int16_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  if (auto store = OpenArray<uint16_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  if (auto store = OpenArray<int8_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  if (auto store = OpenArray<uint8_t, 1>(archive, path, trace, phase))
    return IntegerStore{*store};
  return std::nullopt;
}

std::optional<RealStore> OpenRealStore(const ArchiveContext::Impl& archive,
                                       const std::string& path,
                                       KeypointOpenTrace* trace,
                                       const std::string& phase) {
  if (auto store = OpenArray<double, 1>(archive, path, trace, phase))
    return RealStore{*store};
  if (auto store = OpenArray<float, 1>(archive, path, trace, phase))
    return RealStore{*store};
  if (auto store = OpenArray<int64_t, 1>(archive, path, trace, phase))
    return RealStore{*store};
  if (auto store = OpenArray<int32_t, 1>(archive, path, trace, phase))
    return RealStore{*store};
  return std::nullopt;
}

std::optional<BoolStore> OpenBoolStore(const ArchiveContext::Impl& archive,
                                       const std::string& path,
                                       KeypointOpenTrace* trace,
                                       const std::string& phase) {
  if (auto store = OpenArray<bool, 1>(archive, path, trace, phase))
    return BoolStore{*store};
  if (auto store = OpenArray<uint8_t, 1>(archive, path, trace, phase))
    return BoolStore{*store};
  if (auto store = OpenArray<int8_t, 1>(archive, path, trace, phase))
    return BoolStore{*store};
  if (auto store = OpenArray<int32_t, 1>(archive, path, trace, phase))
    return BoolStore{*store};
  return std::nullopt;
}

std::optional<MatrixStore> OpenMatrixStore(const ArchiveContext::Impl& archive,
                                           const std::string& path,
                                           size_t minimum_columns,
                                           KeypointOpenTrace* trace,
                                           const std::string& phase) {
  auto valid = [minimum_columns](const auto& store) {
    return store.domain().shape()[1] >= static_cast<ts::Index>(minimum_columns);
  };
  if (auto store = OpenArray<double, 2>(archive, path, trace, phase);
      store && valid(*store))
    return MatrixStore{*store};
  if (auto store = OpenArray<float, 2>(archive, path, trace, phase);
      store && valid(*store))
    return MatrixStore{*store};
  if (auto store = OpenArray<int64_t, 2>(archive, path, trace, phase);
      store && valid(*store))
    return MatrixStore{*store};
  if (auto store = OpenArray<int32_t, 2>(archive, path, trace, phase);
      store && valid(*store))
    return MatrixStore{*store};
  return std::nullopt;
}

std::optional<KeypointStore> OpenKeypointStore(
    const ArchiveContext::Impl& archive, const std::string& path,
    KeypointOpenTrace* trace, const std::string& phase) {
  auto valid = [](const auto& store) {
    const auto shape = store.domain().shape();
    return shape[0] > 0 && shape[1] > 0 && shape[2] >= 2;
  };
  if (auto store = OpenArray<double, 3>(archive, path, trace, phase);
      store && valid(*store))
    return KeypointStore{*store};
  if (auto store = OpenArray<float, 3>(archive, path, trace, phase);
      store && valid(*store))
    return KeypointStore{*store};
  return std::nullopt;
}

template <typename T, ts::DimensionIndex Rank>
auto SliceRows(const ts::TensorStore<T, Rank>& store, size_t first,
               size_t last) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  return store | ts::IdentityTransform(domain);
}

template <typename Store>
size_t RowCount(const Store& store) {
  return std::visit(
      [](const auto& typed) {
        const auto rows = typed.domain().shape()[0];
        return rows > 0 ? static_cast<size_t>(rows) : size_t{0};
      },
      store);
}

template <typename Store, typename Output, typename Convert>
bool ReadRankOneRange(const Store& store, size_t first, size_t last,
                      std::vector<Output>* output, Convert convert) {
  if (!output || last < first || last > RowCount(store)) return false;
  output->resize(last - first);
  return std::visit(
      [&](const auto& typed) {
        auto read = ts::Read(SliceRows(typed, first, last)).result();
        if (!read.ok() || read->rank() != 1 ||
            static_cast<size_t>(read->shape()[0]) != last - first ||
            read->byte_strides().size() != 1)
          return false;
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        for (size_t index = 0; index < output->size(); ++index) {
          const Source value = *reinterpret_cast<const Source*>(
              origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
          (*output)[index] = convert(value);
        }
        return true;
      },
      store);
}

bool ReadIntegerRange(const IntegerStore& store, size_t first, size_t last,
                      std::vector<int64_t>* output) {
  return ReadRankOneRange(store, first, last, output, [](auto value) {
    return static_cast<int64_t>(value);
  });
}

bool ReadRealRange(const RealStore& store, size_t first, size_t last,
                   std::vector<double>* output) {
  return ReadRankOneRange(store, first, last, output, [](auto value) {
    return static_cast<double>(value);
  });
}

bool ReadBoolRange(const BoolStore& store, size_t first, size_t last,
                   std::vector<uint8_t>* output) {
  return ReadRankOneRange(store, first, last, output, [](auto value) {
    return value ? uint8_t{1} : uint8_t{0};
  });
}

bool ReadMatrixRange(const MatrixStore& store, size_t first, size_t last,
                     size_t columns, std::vector<double>* output) {
  if (!output || last < first || last > RowCount(store)) return false;
  return std::visit(
      [&](const auto& typed) {
        auto read = ts::Read(SliceRows(typed, first, last)).result();
        if (!read.ok() || read->rank() != 2 ||
            static_cast<size_t>(read->shape()[0]) != last - first ||
            static_cast<size_t>(read->shape()[1]) < columns ||
            read->byte_strides().size() != 2)
          return false;
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        output->resize((last - first) * columns);
        for (size_t row = 0; row < last - first; ++row) {
          for (size_t column = 0; column < columns; ++column) {
            const Source value = *reinterpret_cast<const Source*>(
                origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
                static_cast<ts::Index>(column) * read->byte_strides()[1]);
            (*output)[row * columns + column] = static_cast<double>(value);
          }
        }
        return true;
      },
      store);
}

bool ReadKeypointRange(const KeypointStore& store, size_t first, size_t last,
                       std::vector<std::vector<KeypointOverlayPoint>>* output) {
  if (!output || last < first || last > RowCount(store)) return false;
  return std::visit(
      [&](const auto& typed) {
        auto read = ts::Read(SliceRows(typed, first, last)).result();
        if (!read.ok() || read->rank() != 3 ||
            static_cast<size_t>(read->shape()[0]) != last - first ||
            read->shape()[1] <= 0 || read->shape()[2] < 2 ||
            read->byte_strides().size() != 3)
          return false;
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        const size_t keypoint_count = static_cast<size_t>(read->shape()[1]);
        output->assign(last - first,
                       std::vector<KeypointOverlayPoint>(keypoint_count));
        for (size_t row = 0; row < last - first; ++row) {
          for (size_t keypoint = 0; keypoint < keypoint_count; ++keypoint) {
            const auto* point =
                origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
                static_cast<ts::Index>(keypoint) * read->byte_strides()[1];
            (*output)[row][keypoint] = {
                static_cast<double>(*reinterpret_cast<const Source*>(point)),
                static_cast<double>(*reinterpret_cast<const Source*>(
                    point + read->byte_strides()[2]))};
          }
        }
        return true;
      },
      store);
}

template <typename Source>
bool ReadIntegerVector(const ArchiveContext::Impl& archive,
                       const std::string& path, std::vector<int64_t>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 1>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source* values = static_cast<const Source*>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = static_cast<int64_t>(values[index]);
  }
  return true;
}

bool ReadIntegers(const ArchiveContext::Impl& archive, const std::string& path,
                  std::vector<int64_t>* output) {
  return ReadIntegerVector<int64_t>(archive, path, output) ||
         ReadIntegerVector<uint64_t>(archive, path, output) ||
         ReadIntegerVector<int32_t>(archive, path, output) ||
         ReadIntegerVector<uint32_t>(archive, path, output) ||
         ReadIntegerVector<int16_t>(archive, path, output) ||
         ReadIntegerVector<uint16_t>(archive, path, output) ||
         ReadIntegerVector<int8_t>(archive, path, output) ||
         ReadIntegerVector<uint8_t>(archive, path, output);
}

template <typename Source>
bool ReadRealVector(const ArchiveContext::Impl& archive,
                    const std::string& path, std::vector<double>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 1>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source* values = static_cast<const Source*>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = static_cast<double>(values[index]);
  }
  return true;
}

bool ReadReals(const ArchiveContext::Impl& archive, const std::string& path,
               std::vector<double>* output) {
  return ReadRealVector<double>(archive, path, output) ||
         ReadRealVector<float>(archive, path, output) ||
         ReadRealVector<int64_t>(archive, path, output) ||
         ReadRealVector<int32_t>(archive, path, output);
}

template <typename Source>
bool ReadBoolVector(const ArchiveContext::Impl& archive,
                    const std::string& path, std::vector<uint8_t>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 1>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source* values = static_cast<const Source*>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = values[index] ? 1 : 0;
  }
  return true;
}

bool ReadBools(const ArchiveContext::Impl& archive, const std::string& path,
               std::vector<uint8_t>* output) {
  return ReadBoolVector<bool>(archive, path, output) ||
         ReadBoolVector<uint8_t>(archive, path, output) ||
         ReadBoolVector<int8_t>(archive, path, output) ||
         ReadBoolVector<int32_t>(archive, path, output);
}

template <typename Source>
bool ReadMatrix(const ArchiveContext::Impl& archive, const std::string& path,
                size_t minimum_columns,
                std::vector<std::vector<double>>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 2>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 2 ||
      read->shape()[1] < static_cast<ts::Index>(minimum_columns)) {
    return false;
  }
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  const size_t columns = static_cast<size_t>(read->shape()[1]);
  const Source* values = static_cast<const Source*>(read->data());
  output->assign(rows, std::vector<double>(columns));
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      (*output)[row][column] =
          static_cast<double>(values[row * columns + column]);
    }
  }
  return true;
}

bool ReadNumericMatrix(const ArchiveContext::Impl& archive,
                       const std::string& path, size_t minimum_columns,
                       std::vector<std::vector<double>>* output) {
  return ReadMatrix<double>(archive, path, minimum_columns, output) ||
         ReadMatrix<float>(archive, path, minimum_columns, output) ||
         ReadMatrix<int64_t>(archive, path, minimum_columns, output) ||
         ReadMatrix<int32_t>(archive, path, minimum_columns, output);
}

template <typename Source>
bool ReadKeypoints(const ArchiveContext::Impl& archive, const std::string& path,
                   size_t expected_rows,
                   std::vector<std::vector<KeypointOverlayPoint>>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 3>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 3 ||
      read->shape()[0] != static_cast<ts::Index>(expected_rows) ||
      read->shape()[1] <= 0 || read->shape()[2] < 2) {
    return false;
  }
  const size_t keypoint_count = static_cast<size_t>(read->shape()[1]);
  const size_t coordinate_count = static_cast<size_t>(read->shape()[2]);
  const Source* values = static_cast<const Source*>(read->data());
  output->assign(expected_rows,
                 std::vector<KeypointOverlayPoint>(keypoint_count));
  for (size_t row = 0; row < expected_rows; ++row) {
    for (size_t keypoint = 0; keypoint < keypoint_count; ++keypoint) {
      const size_t offset =
          (row * keypoint_count + keypoint) * coordinate_count;
      (*output)[row][keypoint] = {static_cast<double>(values[offset]),
                                  static_cast<double>(values[offset + 1])};
    }
  }
  return true;
}

bool ReadNumericKeypoints(
    const ArchiveContext::Impl& archive, const std::string& path,
    size_t expected_rows,
    std::vector<std::vector<KeypointOverlayPoint>>* output) {
  return ReadKeypoints<double>(archive, path, expected_rows, output) ||
         ReadKeypoints<float>(archive, path, expected_rows, output);
}

std::string LatestRun(const ArchiveContext::Impl& archive,
                      const std::string& group, KeypointOpenTrace* trace,
                      const std::string& phase) {
  const auto attributes = ReadAttributes(archive, group, trace, phase);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char*, 4> keys = {
      "latest", "latest_completed", "latest_complete", "latest_success"};
  for (const char* key : keys) {
    const auto found = attributes->find(key);
    if (found != attributes->end() && found->is_string() &&
        !found->get_ref<const std::string&>().empty()) {
      return found->get<std::string>();
    }
  }
  return {};
}

bool ValidRunName(const std::string& run_name) {
  return !run_name.empty() && run_name != "." && run_name != ".." &&
         run_name.find('/') == std::string::npos;
}

struct SelectedRun {
  std::string group;
  std::string name;
  bool refined = false;
};

std::optional<SelectedRun> SelectRun(const ArchiveContext::Impl& archive,
                                     std::string requested_run,
                                     KeypointOpenTrace* trace) {
  SelectedRun selected;
  if (requested_run.rfind("refined_keypoints_runs/", 0) == 0) {
    selected.group = "refined_keypoints_runs";
    selected.refined = true;
    requested_run.erase(0, selected.group.size() + 1);
  } else if (requested_run.rfind("keypoints_runs/", 0) == 0) {
    selected.group = "keypoints_runs";
    requested_run.erase(0, selected.group.size() + 1);
  } else if (!requested_run.empty()) {
    selected.name = requested_run;
    selected.group = "refined_keypoints_runs";
    selected.refined = true;
    if (!ReadAttributes(archive, selected.group + "/" + selected.name, trace,
                        "selection")) {
      selected.group = "keypoints_runs";
      selected.refined = false;
    }
  } else {
    selected.group = "refined_keypoints_runs";
    selected.refined = true;
    selected.name = LatestRun(archive, selected.group, trace, "selection");
    if (selected.name.empty()) {
      selected.group = "keypoints_runs";
      selected.refined = false;
      selected.name = LatestRun(archive, selected.group, trace, "selection");
    }
  }
  if (selected.name.empty()) {
    selected.name = requested_run;
  }
  if (!ValidRunName(selected.name) ||
      !ReadAttributes(archive, selected.group + "/" + selected.name, trace,
                      "selection")) {
    return std::nullopt;
  }
  return selected;
}

std::string ResolveCropRun(const ArchiveContext::Impl& archive,
                           const json& run_attributes, KeypointOpenTrace* trace,
                           const std::string& phase) {
  const auto source = run_attributes.find("source_crop_run");
  if (source != run_attributes.end() && source->is_string()) {
    std::string run_name = source->get<std::string>();
    if (run_name.rfind("crop_runs/", 0) == 0) {
      run_name.erase(0, std::string("crop_runs/").size());
    }
    if (ValidRunName(run_name)) {
      return run_name;
    }
  }
  return LatestRun(archive, "crop_runs", trace, phase);
}

void LoadLabelsAndEdges(const json& attributes, size_t keypoint_count,
                        KeypointOverlayDescriptor* descriptor) {
  const auto labels = attributes.find("keypoint_labels");
  if (labels != attributes.end() && labels->is_array()) {
    for (const auto& label : *labels) {
      if (label.is_string()) {
        descriptor->keypoint_labels.push_back(label.get<std::string>());
      }
    }
  }
  descriptor->keypoint_labels.resize(keypoint_count);
  static constexpr std::array<const char*, 3> defaults = {
      "swim_bladder", "left_eye", "right_eye"};
  for (size_t index = 0; index < keypoint_count; ++index) {
    if (!descriptor->keypoint_labels[index].empty()) {
      continue;
    }
    descriptor->keypoint_labels[index] = index < defaults.size()
                                             ? defaults[index]
                                             : "kp" + std::to_string(index);
  }

  const auto pose_schema = attributes.find("pose_schema");
  if (pose_schema == attributes.end() || !pose_schema->is_object()) {
    return;
  }
  const auto edges = pose_schema->find("edges");
  if (edges == pose_schema->end() || !edges->is_array()) {
    return;
  }
  for (const auto& edge : *edges) {
    if (!edge.is_array() || edge.size() != 2 || !edge[0].is_number_integer() ||
        !edge[1].is_number_integer()) {
      continue;
    }
    const int64_t first = edge[0].get<int64_t>();
    const int64_t second = edge[1].get<int64_t>();
    if (first >= 0 && second >= 0 &&
        static_cast<size_t>(first) < keypoint_count &&
        static_cast<size_t>(second) < keypoint_count) {
      descriptor->skeleton_edges.push_back(
          {static_cast<size_t>(first), static_cast<size_t>(second)});
    }
  }
}

bool ReadRoiSize(const json& crop_attributes, double* width, double* height) {
  const auto size = crop_attributes.find("roi_size");
  if (size == crop_attributes.end() || !size->is_array() || size->size() < 2 ||
      !(*size)[0].is_number() || !(*size)[1].is_number()) {
    return false;
  }
  *height = (*size)[0].get<double>();
  *width = (*size)[1].get<double>();
  return std::isfinite(*width) && std::isfinite(*height) && *width > 0.0 &&
         *height > 0.0;
}

struct LazyKeypointStores {
  IntegerStore frames;
  std::optional<IntegerStore> detections;
  IntegerStore source_crop_rows;
  KeypointStore keypoints;
  std::optional<RealStore> headings;
  std::optional<BoolStore> heading_valid;
  std::optional<BoolStore> keypoint_source;
  std::optional<BoolStore> usable;
  std::optional<BoolStore> flip_corrected;
  IntegerStore crop_frames;
  std::optional<IntegerStore> crop_detections;
  MatrixStore crop_offsets;
  std::optional<MatrixStore> crop_boxes;
  std::optional<BoolStore> crop_detection_source;
};

class LazyKeypointOverlayRepository final : public KeypointOverlayRepository {
 public:
  LazyKeypointOverlayRepository(KeypointOverlayDescriptor descriptor,
                                std::vector<uint64_t> frame_row_offsets,
                                LazyKeypointStores stores, double roi_width,
                                double roi_height)
      : descriptor_(std::move(descriptor)),
        frame_row_offsets_(std::move(frame_row_offsets)),
        stores_(std::move(stores)),
        roi_width_(roi_width),
        roi_height_(roi_height) {}

  const KeypointOverlayDescriptor& descriptor() const override {
    return descriptor_;
  }

  KeypointOverlayResolution resolveCameraFrame(
      int64_t camera_frame, int full_frame_width,
      int full_frame_height) const override {
    KeypointOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
      result.status = KeypointOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = KeypointOverlayStatus::InvalidDimensions;
      return result;
    }
    const size_t frame = static_cast<size_t>(camera_frame);
    const size_t first = static_cast<size_t>(frame_row_offsets_[frame]);
    const size_t last = static_cast<size_t>(frame_row_offsets_[frame + 1]);
    if (first == last) {
      result.status = KeypointOverlayStatus::Missing;
      return result;
    }

    auto fail = [&](std::string message) {
      result.status = KeypointOverlayStatus::ReadFailed;
      result.error = std::move(message);
      result.detections.clear();
      return result;
    };
    std::vector<int64_t> frames;
    std::vector<int64_t> detections(last - first, -1);
    std::vector<int64_t> crop_rows;
    std::vector<std::vector<KeypointOverlayPoint>> keypoints;
    if (!ReadIntegerRange(stores_.frames, first, last, &frames) ||
        !ReadIntegerRange(stores_.source_crop_rows, first, last, &crop_rows) ||
        !ReadKeypointRange(stores_.keypoints, first, last, &keypoints)) {
      return fail("Keypoint frame payload is unreadable");
    }
    if (stores_.detections &&
        !ReadIntegerRange(*stores_.detections, first, last, &detections)) {
      return fail("Keypoint detection indices are unreadable");
    }
    std::vector<double> headings(last - first,
                                 std::numeric_limits<double>::quiet_NaN());
    std::vector<uint8_t> heading_valid(last - first, 1);
    std::vector<uint8_t> keypoint_source(last - first, 0);
    std::vector<uint8_t> usable(last - first, descriptor_.refined ? 0 : 1);
    std::vector<uint8_t> flip_corrected(last - first, 0);
    if ((stores_.headings &&
         !ReadRealRange(*stores_.headings, first, last, &headings)) ||
        (stores_.heading_valid &&
         !ReadBoolRange(*stores_.heading_valid, first, last, &heading_valid)) ||
        (stores_.keypoint_source &&
         !ReadBoolRange(*stores_.keypoint_source, first, last,
                        &keypoint_source)) ||
        (stores_.usable &&
         !ReadBoolRange(*stores_.usable, first, last, &usable)) ||
        (stores_.flip_corrected &&
         !ReadBoolRange(*stores_.flip_corrected, first, last,
                        &flip_corrected))) {
      return fail("Keypoint status columns are unreadable");
    }

    size_t crop_first = std::numeric_limits<size_t>::max();
    size_t crop_last = 0;
    for (size_t index = 0; index < crop_rows.size(); ++index) {
      if (frames[index] != camera_frame || crop_rows[index] < 0 ||
          static_cast<uint64_t>(crop_rows[index]) >=
              RowCount(stores_.crop_frames)) {
        return fail("Keypoint frame-to-crop lineage is invalid");
      }
      const size_t crop_row = static_cast<size_t>(crop_rows[index]);
      crop_first = std::min(crop_first, crop_row);
      crop_last = std::max(crop_last, crop_row + 1);
    }
    std::vector<int64_t> crop_frames;
    std::vector<int64_t> crop_detections(crop_last - crop_first, -1);
    std::vector<double> crop_offsets;
    std::vector<double> crop_boxes;
    std::vector<uint8_t> crop_sources(crop_last - crop_first, 0);
    if (!ReadIntegerRange(stores_.crop_frames, crop_first, crop_last,
                          &crop_frames) ||
        !ReadMatrixRange(stores_.crop_offsets, crop_first, crop_last, 2,
                         &crop_offsets) ||
        (stores_.crop_detections &&
         !ReadIntegerRange(*stores_.crop_detections, crop_first, crop_last,
                           &crop_detections)) ||
        (stores_.crop_boxes && !ReadMatrixRange(*stores_.crop_boxes, crop_first,
                                                crop_last, 4, &crop_boxes)) ||
        (stores_.crop_detection_source &&
         !ReadBoolRange(*stores_.crop_detection_source, crop_first, crop_last,
                        &crop_sources))) {
      return fail("Keypoint crop placement rows are unreadable");
    }

    std::vector<KeypointOverlayRow> rows;
    rows.reserve(last - first);
    for (size_t index = 0; index < last - first; ++index) {
      const size_t crop_row = static_cast<size_t>(crop_rows[index]);
      const size_t local_crop = crop_row - crop_first;
      if (crop_frames[local_crop] != camera_frame ||
          (detections[index] >= 0 && crop_detections[local_crop] >= 0 &&
           detections[index] != crop_detections[local_crop])) {
        return fail("Keypoint crop row does not match its detection lineage");
      }
      KeypointOverlayRow row;
      row.camera_frame = camera_frame;
      row.detection_index = detections[index] >= 0
                                ? detections[index]
                                : crop_detections[local_crop];
      row.source_crop_row_id = crop_rows[index];
      row.keypoints = std::move(keypoints[index]);
      if (std::isfinite(headings[index])) row.heading_degrees = headings[index];
      row.heading_valid = heading_valid[index] != 0;
      row.detection_interpolated = crop_sources[local_crop] != 0;
      row.refined_keypoints = descriptor_.refined;
      row.keypoint_usable = usable[index] != 0;
      row.keypoint_detection_interpolated = keypoint_source[index] != 0;
      row.keypoint_flip_corrected = flip_corrected[index] != 0;
      row.roi_offset = KeypointOverlayPoint{crop_offsets[local_crop * 2],
                                            crop_offsets[local_crop * 2 + 1]};
      row.roi_width = roi_width_;
      row.roi_height = roi_height_;
      if (!crop_boxes.empty()) {
        const std::array<double, 4> box = {
            crop_boxes[local_crop * 4], crop_boxes[local_crop * 4 + 1],
            crop_boxes[local_crop * 4 + 2], crop_boxes[local_crop * 4 + 3]};
        if (std::all_of(box.begin(), box.end(),
                        [](double value) { return std::isfinite(value); }) &&
            box[2] > 0.0 && box[3] > 0.0) {
          row.normalized_detection_cxcywh = box;
        }
      }
      rows.push_back(std::move(row));
    }
    auto frame_repository =
        MakeKeypointOverlayRepository(descriptor_, std::move(rows));
    return frame_repository->resolveCameraFrame(camera_frame, full_frame_width,
                                                full_frame_height);
  }

 private:
  KeypointOverlayDescriptor descriptor_;
  std::vector<uint64_t> frame_row_offsets_;
  LazyKeypointStores stores_;
  double roi_width_ = 0.0;
  double roi_height_ = 0.0;
};

std::unique_ptr<KeypointOverlayRepository> TryOpenLazyKeypointRepository(
    const ArchiveContext::Impl& archive, const SelectedRun& selected,
    const json& run_attributes, const std::string& run_base,
    KeypointOpenTrace* trace) {
  std::optional<IntegerStore> frames;
  std::optional<IntegerStore> frame_counts;
  std::optional<IntegerStore> source_crop_rows;
  std::optional<KeypointStore> keypoints;
  KeypointOverlayDescriptor descriptor;
  {
    KeypointPhaseTimer timer(
        trace, &KeypointRepositoryOpenMetrics::required_handles_ms);
    frames = OpenIntegerStore(archive, run_base + "/frame_indices", trace,
                              "required_handles");
    frame_counts = OpenIntegerStore(archive, run_base + "/frame_counts", trace,
                                    "required_handles");
    source_crop_rows = OpenIntegerStore(
        archive, run_base + "/source_crop_row_ids", trace, "required_handles");
    if (!frames || !frame_counts || !source_crop_rows ||
        RowCount(*frames) == 0 ||
        RowCount(*source_crop_rows) != RowCount(*frames)) {
      return nullptr;
    }
    descriptor.source_group = selected.group;
    descriptor.run_name = selected.name;
    descriptor.refined = selected.refined;
    descriptor.row_count = RowCount(*frames);
    descriptor.camera_frame_count = RowCount(*frame_counts);
    descriptor.source_crop_run =
        ResolveCropRun(archive, run_attributes, trace, "required_handles");
    if (!ValidRunName(descriptor.source_crop_run)) return nullptr;

    keypoints = OpenKeypointStore(archive, run_base + "/keypoints_img", trace,
                                  "required_handles");
    if (keypoints) {
      descriptor.coordinate_space = KeypointCoordinateSpace::Image;
    } else if ((keypoints =
                    OpenKeypointStore(archive, run_base + "/keypoints_roi",
                                      trace, "required_handles"))) {
      descriptor.coordinate_space = KeypointCoordinateSpace::Roi;
    } else if ((keypoints =
                    OpenKeypointStore(archive, run_base + "/keypoints_norm",
                                      trace, "required_handles"))) {
      descriptor.coordinate_space = KeypointCoordinateSpace::NormalizedRoi;
    } else {
      return nullptr;
    }
    if (RowCount(*keypoints) != descriptor.row_count) return nullptr;
    const size_t keypoint_count = std::visit(
        [](const auto& store) {
          return static_cast<size_t>(store.domain().shape()[1]);
        },
        *keypoints);
    LoadLabelsAndEdges(run_attributes, keypoint_count, &descriptor);
  }

  std::vector<int64_t> counts;
  {
    KeypointPhaseTimer timer(
        trace, &KeypointRepositoryOpenMetrics::frame_counts_read_ms);
    const auto started = std::chrono::steady_clock::now();
    const bool read = ReadIntegerRange(*frame_counts, 0,
                                       descriptor.camera_frame_count, &counts);
    if (trace) {
      trace->record("frame_counts_read", "array_read",
                    run_base + "/frame_counts", "selected[1]", started, read);
    }
    if (!read) return nullptr;
  }
  std::vector<uint64_t> offsets(counts.size() + 1, 0);
  {
    KeypointPhaseTimer timer(trace,
                             &KeypointRepositoryOpenMetrics::prefix_sum_ms);
    for (size_t frame = 0; frame < counts.size(); ++frame) {
      if (counts[frame] < 0 ||
          static_cast<uint64_t>(counts[frame]) >
              std::numeric_limits<uint64_t>::max() - offsets[frame]) {
        return nullptr;
      }
      offsets[frame + 1] = offsets[frame] + counts[frame];
    }
    if (offsets.back() != descriptor.row_count) return nullptr;
  }

  const std::string crop_base = "crop_runs/" + descriptor.source_crop_run;
  std::optional<json> crop_attributes;
  std::optional<IntegerStore> crop_frames;
  std::optional<MatrixStore> crop_offsets;
  double roi_width = 0.0;
  double roi_height = 0.0;
  {
    KeypointPhaseTimer timer(trace,
                             &KeypointRepositoryOpenMetrics::crop_lineage_ms);
    crop_attributes = ReadAttributes(archive, crop_base, trace, "crop_lineage");
    crop_frames = OpenIntegerStore(archive, crop_base + "/frame_indices", trace,
                                   "crop_lineage");
    crop_offsets = OpenMatrixStore(archive, crop_base + "/roi_coordinates_full",
                                   2, trace, "crop_lineage");
    if (!crop_attributes || !crop_frames || !crop_offsets ||
        RowCount(*crop_frames) == 0 ||
        RowCount(*crop_offsets) != RowCount(*crop_frames)) {
      return nullptr;
    }
    ReadRoiSize(*crop_attributes, &roi_width, &roi_height);
  }

  std::optional<IntegerStore> detections;
  std::optional<RealStore> headings;
  std::optional<BoolStore> heading_valid;
  std::optional<BoolStore> keypoint_source;
  std::optional<BoolStore> usable;
  std::optional<BoolStore> flip_corrected;
  std::optional<IntegerStore> crop_detections;
  std::optional<MatrixStore> crop_boxes;
  std::optional<BoolStore> crop_detection_source;
  {
    KeypointPhaseTimer timer(
        trace, &KeypointRepositoryOpenMetrics::optional_handles_ms);
    detections = OpenIntegerStore(archive, run_base + "/detection_indices",
                                  trace, "optional_handles");
    headings = OpenRealStore(archive, run_base + "/heading", trace,
                             "optional_handles");
    heading_valid = OpenBoolStore(archive, run_base + "/detection_success",
                                  trace, "optional_handles");
    keypoint_source = OpenBoolStore(archive, run_base + "/detection_source",
                                    trace, "optional_handles");
    usable = OpenBoolStore(archive, run_base + "/usable_keypoints", trace,
                           "optional_handles");
    flip_corrected = OpenBoolStore(archive, run_base + "/flip_corrected", trace,
                                   "optional_handles");
    crop_detections = OpenIntegerStore(
        archive, crop_base + "/detection_indices", trace, "optional_handles");
    crop_boxes = OpenMatrixStore(archive, crop_base + "/bbox_norm_coords", 4,
                                 trace, "optional_handles");
    crop_detection_source = OpenBoolStore(
        archive, crop_base + "/detection_source", trace, "optional_handles");
  }
  LazyKeypointStores stores{
      std::move(*frames),           std::move(detections),
      std::move(*source_crop_rows), std::move(*keypoints),
      std::move(headings),          std::move(heading_valid),
      std::move(keypoint_source),   std::move(usable),
      std::move(flip_corrected),    std::move(*crop_frames),
      std::move(crop_detections),   std::move(*crop_offsets),
      std::move(crop_boxes),        std::move(crop_detection_source)};
  auto aligned = [row_count = descriptor.row_count](const auto& store) {
    return !store || RowCount(*store) == row_count;
  };
  if (!aligned(stores.detections) || !aligned(stores.headings) ||
      !aligned(stores.heading_valid) || !aligned(stores.keypoint_source) ||
      !aligned(stores.usable) || !aligned(stores.flip_corrected))
    return nullptr;
  const size_t crop_row_count = RowCount(stores.crop_frames);
  auto crop_aligned = [crop_row_count](const auto& store) {
    return !store || RowCount(*store) == crop_row_count;
  };
  if (!crop_aligned(stores.crop_detections) ||
      !crop_aligned(stores.crop_boxes) ||
      !crop_aligned(stores.crop_detection_source))
    return nullptr;
  if (trace) trace->setLazyPath();
  return std::make_unique<LazyKeypointOverlayRepository>(
      std::move(descriptor), std::move(offsets), std::move(stores), roi_width,
      roi_height);
}

}  // namespace

std::unique_ptr<KeypointOverlayRepository> OpenKeypointOverlayRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run, std::string* error_message,
    KeypointRepositoryOpenMetrics* open_metrics) {
  KeypointOpenTrace trace(open_metrics);
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto& impl = *archive->impl_;
  std::optional<SelectedRun> selected;
  {
    KeypointPhaseTimer timer(&trace,
                             &KeypointRepositoryOpenMetrics::selection_ms);
    selected = SelectRun(impl, requested_run, &trace);
  }
  if (!selected) {
    internal::SetArchiveError(
        error_message, "No valid refined or raw keypoint run is available");
    return nullptr;
  }

  const std::string run_base = selected->group + "/" + selected->name;
  std::optional<json> run_attributes;
  {
    KeypointPhaseTimer timer(&trace,
                             &KeypointRepositoryOpenMetrics::run_attributes_ms);
    run_attributes = ReadAttributes(impl, run_base, &trace, "run_attributes");
  }
  if (!run_attributes) {
    internal::SetArchiveError(error_message,
                              "Keypoint run attributes are unreadable");
    return nullptr;
  }

  trace.setLazyAttempted();
  if (auto lazy = TryOpenLazyKeypointRepository(
          impl, *selected, *run_attributes, run_base, &trace)) {
    return lazy;
  }
  trace.setFallbackPath();
  KeypointPhaseTimer fallback_timer(
      &trace, &KeypointRepositoryOpenMetrics::fallback_materialization_ms);

  std::vector<int64_t> frame_indices;
  if (!ReadIntegers(impl, run_base + "/frame_indices", &frame_indices) ||
      frame_indices.empty()) {
    internal::SetArchiveError(error_message,
                              "Keypoint run has no readable frame_indices");
    return nullptr;
  }
  const size_t row_count = frame_indices.size();

  KeypointOverlayDescriptor descriptor;
  descriptor.source_group = selected->group;
  descriptor.run_name = selected->name;
  descriptor.refined = selected->refined;
  std::vector<std::vector<KeypointOverlayPoint>> keypoints;
  if (ReadNumericKeypoints(impl, run_base + "/keypoints_img", row_count,
                           &keypoints)) {
    descriptor.coordinate_space = KeypointCoordinateSpace::Image;
  } else if (ReadNumericKeypoints(impl, run_base + "/keypoints_roi", row_count,
                                  &keypoints)) {
    descriptor.coordinate_space = KeypointCoordinateSpace::Roi;
  } else if (ReadNumericKeypoints(impl, run_base + "/keypoints_norm", row_count,
                                  &keypoints)) {
    descriptor.coordinate_space = KeypointCoordinateSpace::NormalizedRoi;
  } else {
    internal::SetArchiveError(
        error_message,
        "Keypoint run has no readable image, ROI, or normalized keypoints");
    return nullptr;
  }
  LoadLabelsAndEdges(*run_attributes, keypoints.front().size(), &descriptor);

  std::vector<int64_t> detection_indices;
  if (!ReadIntegers(impl, run_base + "/detection_indices",
                    &detection_indices) ||
      detection_indices.size() != row_count) {
    detection_indices.assign(row_count, -1);
  }
  std::vector<int64_t> source_crop_rows;
  if (!ReadIntegers(impl, run_base + "/source_crop_row_ids",
                    &source_crop_rows) ||
      source_crop_rows.size() != row_count) {
    source_crop_rows.clear();
  }

  std::vector<double> headings;
  if (!ReadReals(impl, run_base + "/heading", &headings) ||
      headings.size() != row_count) {
    headings.assign(row_count, std::numeric_limits<double>::quiet_NaN());
  }
  std::vector<uint8_t> heading_valid;
  if (!ReadBools(impl, run_base + "/detection_success", &heading_valid) ||
      heading_valid.size() != row_count) {
    heading_valid.assign(row_count, 1);
  }
  std::vector<uint8_t> keypoint_source;
  if (!ReadBools(impl, run_base + "/detection_source", &keypoint_source) ||
      keypoint_source.size() != row_count) {
    keypoint_source.assign(row_count, 0);
  }
  std::vector<uint8_t> usable;
  if (!ReadBools(impl, run_base + "/usable_keypoints", &usable) ||
      usable.size() != row_count) {
    usable.assign(row_count, selected->refined ? 0 : 1);
  }
  std::vector<uint8_t> flip_corrected;
  if (!ReadBools(impl, run_base + "/flip_corrected", &flip_corrected) ||
      flip_corrected.size() != row_count) {
    flip_corrected.assign(row_count, 0);
  }

  descriptor.source_crop_run =
      ResolveCropRun(impl, *run_attributes, &trace, "fallback_materialization");
  if (!ValidRunName(descriptor.source_crop_run)) {
    internal::SetArchiveError(
        error_message,
        "Keypoint run has no valid source crop run for row placement");
    return nullptr;
  }
  const std::string crop_base = "crop_runs/" + descriptor.source_crop_run;
  const auto crop_attributes =
      ReadAttributes(impl, crop_base, &trace, "fallback_materialization");
  if (!crop_attributes) {
    internal::SetArchiveError(error_message,
                              "Source crop run attributes are unreadable");
    return nullptr;
  }

  std::vector<int64_t> crop_frames;
  std::vector<int64_t> crop_detection_indices;
  std::vector<std::vector<double>> crop_offsets;
  if (!ReadIntegers(impl, crop_base + "/frame_indices", &crop_frames) ||
      crop_frames.empty() ||
      !ReadNumericMatrix(impl, crop_base + "/roi_coordinates_full", 2,
                         &crop_offsets) ||
      crop_offsets.size() != crop_frames.size()) {
    internal::SetArchiveError(
        error_message,
        "Source crop frame and ROI coordinates are not row-aligned");
    return nullptr;
  }
  if (!ReadIntegers(impl, crop_base + "/detection_indices",
                    &crop_detection_indices) ||
      crop_detection_indices.size() != crop_frames.size()) {
    crop_detection_indices.assign(crop_frames.size(), -1);
  }
  std::vector<std::vector<double>> crop_boxes;
  if (!ReadNumericMatrix(impl, crop_base + "/bbox_norm_coords", 4,
                         &crop_boxes) ||
      crop_boxes.size() != crop_frames.size()) {
    crop_boxes.clear();
  }
  std::vector<uint8_t> crop_detection_source;
  if (!ReadBools(impl, crop_base + "/detection_source",
                 &crop_detection_source) ||
      crop_detection_source.size() != crop_frames.size()) {
    crop_detection_source.assign(crop_frames.size(), 0);
  }
  double roi_width = 0.0;
  double roi_height = 0.0;
  ReadRoiSize(*crop_attributes, &roi_width, &roi_height);

  std::unordered_map<int64_t, std::vector<size_t>> crop_rows_by_frame;
  for (size_t index = 0; index < crop_frames.size(); ++index) {
    crop_rows_by_frame[crop_frames[index]].push_back(index);
  }
  std::unordered_map<int64_t, size_t> frame_cursors;
  std::vector<KeypointOverlayRow> rows;
  rows.reserve(row_count);
  for (size_t index = 0; index < row_count; ++index) {
    size_t crop_row = std::numeric_limits<size_t>::max();
    if (!source_crop_rows.empty()) {
      if (source_crop_rows[index] < 0 ||
          static_cast<uint64_t>(source_crop_rows[index]) >=
              crop_frames.size()) {
        internal::SetArchiveError(
            error_message,
            "Keypoint source_crop_row_ids contains an out-of-range row");
        return nullptr;
      }
      crop_row = static_cast<size_t>(source_crop_rows[index]);
      if (crop_frames[crop_row] != frame_indices[index]) {
        internal::SetArchiveError(
            error_message,
            "Keypoint source crop row does not match its camera frame");
        return nullptr;
      }
      if (detection_indices[index] >= 0 &&
          crop_detection_indices[crop_row] >= 0 &&
          detection_indices[index] != crop_detection_indices[crop_row]) {
        internal::SetArchiveError(
            error_message,
            "Keypoint source crop row does not match its detection index");
        return nullptr;
      }
    } else if (crop_frames.size() == row_count &&
               crop_frames[index] == frame_indices[index]) {
      crop_row = index;
    } else {
      const auto candidates = crop_rows_by_frame.find(frame_indices[index]);
      if (candidates == crop_rows_by_frame.end()) {
        internal::SetArchiveError(
            error_message,
            "Keypoint camera frame is absent from the source crop run");
        return nullptr;
      }
      auto& cursor = frame_cursors[frame_indices[index]];
      if (detection_indices[index] >= 0) {
        const auto exact =
            std::find_if(candidates->second.begin(), candidates->second.end(),
                         [&](size_t candidate) {
                           return crop_detection_indices[candidate] ==
                                  detection_indices[index];
                         });
        if (exact != candidates->second.end()) {
          crop_row = *exact;
        }
      }
      if (crop_row == std::numeric_limits<size_t>::max()) {
        if (cursor >= candidates->second.size()) {
          internal::SetArchiveError(
              error_message,
              "Legacy keypoint rows exceed source crop rows for a frame");
          return nullptr;
        }
        crop_row = candidates->second[cursor++];
      }
    }

    KeypointOverlayRow row;
    row.camera_frame = frame_indices[index];
    row.detection_index = detection_indices[index] >= 0
                              ? detection_indices[index]
                              : crop_detection_indices[crop_row];
    row.source_crop_row_id = static_cast<int64_t>(crop_row);
    row.keypoints = std::move(keypoints[index]);
    if (std::isfinite(headings[index])) {
      row.heading_degrees = headings[index];
    }
    row.heading_valid = heading_valid[index] != 0;
    row.detection_interpolated = crop_detection_source[crop_row] != 0;
    row.refined_keypoints = selected->refined;
    row.keypoint_usable = usable[index] != 0;
    row.keypoint_detection_interpolated = keypoint_source[index] != 0;
    row.keypoint_flip_corrected = flip_corrected[index] != 0;
    row.roi_offset = KeypointOverlayPoint{crop_offsets[crop_row][0],
                                          crop_offsets[crop_row][1]};
    row.roi_width = roi_width;
    row.roi_height = roi_height;
    if (!crop_boxes.empty()) {
      const std::array<double, 4> box = {
          crop_boxes[crop_row][0], crop_boxes[crop_row][1],
          crop_boxes[crop_row][2], crop_boxes[crop_row][3]};
      if (std::all_of(box.begin(), box.end(),
                      [](double value) { return std::isfinite(value); }) &&
          box[2] > 0.0 && box[3] > 0.0) {
        row.normalized_detection_cxcywh = box;
      }
    }
    rows.push_back(std::move(row));
  }

  return MakeKeypointOverlayRepository(std::move(descriptor), std::move(rows));
}

}  // namespace crimson::zarr
