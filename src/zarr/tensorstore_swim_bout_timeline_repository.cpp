#include "zarr/tensorstore_swim_bout_timeline_repository.h"

#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
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

using FrameStore =
    std::variant<ts::TensorStore<int64_t, 1>, ts::TensorStore<int32_t, 1>>;
using ScalarStore =
    std::variant<ts::TensorStore<float, 1>, ts::TensorStore<double, 1>>;
using MatrixStore =
    std::variant<ts::TensorStore<float, 2>, ts::TensorStore<double, 2>>;

std::optional<json> makeArraySpec(const ArchiveContext::Impl& archive,
                                  const std::string& path) {
  return internal::MakeReadOnlyArraySpec(archive, path);
}

template <typename T, size_t Rank>
std::optional<ts::TensorStore<T, Rank>> openArray(
    const ArchiveContext::Impl& archive, const std::string& path) {
  const auto spec = makeArraySpec(archive, path);
  if (!spec) {
    return std::nullopt;
  }
  auto result = ts::Open<T, Rank>(*spec, ts::OpenMode::open,
                                  ts::ReadWriteMode::read, archive.context)
                    .result();
  return result.ok() ? std::optional<ts::TensorStore<T, Rank>>(*result)
                     : std::nullopt;
}

template <typename T>
bool readTypedIntegers(const ArchiveContext::Impl& archive,
                       const std::string& path, std::vector<int64_t>* output) {
  const auto store = openArray<T, 1>(archive, path);
  if (!store) {
    return false;
  }
  const auto result = ts::Read(*store).result();
  if (!result.ok() || result->rank() != 1 ||
      result->byte_strides().size() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(result->shape()[0]);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      result->byte_strided_origin_pointer().get());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    const auto* value = reinterpret_cast<const T*>(
        origin + static_cast<ts::Index>(index) * result->byte_strides()[0]);
    if constexpr (std::is_unsigned_v<T>) {
      (*output)[index] = *value <= static_cast<std::make_unsigned_t<int64_t>>(
                                       std::numeric_limits<int64_t>::max())
                             ? static_cast<int64_t>(*value)
                             : -1;
    } else {
      (*output)[index] = static_cast<int64_t>(*value);
    }
  }
  return true;
}

bool readIntegers(const ArchiveContext::Impl& archive, const std::string& path,
                  std::vector<int64_t>* output) {
  return readTypedIntegers<int64_t>(archive, path, output) ||
         readTypedIntegers<uint64_t>(archive, path, output) ||
         readTypedIntegers<int32_t>(archive, path, output) ||
         readTypedIntegers<uint32_t>(archive, path, output) ||
         readTypedIntegers<int16_t>(archive, path, output) ||
         readTypedIntegers<uint16_t>(archive, path, output) ||
         readTypedIntegers<int8_t>(archive, path, output) ||
         readTypedIntegers<uint8_t>(archive, path, output);
}

template <typename T>
bool readTypedScalars(const ArchiveContext::Impl& archive,
                      const std::string& path, std::vector<double>* output) {
  const auto store = openArray<T, 1>(archive, path);
  if (!store) {
    return false;
  }
  const auto result = ts::Read(*store).result();
  if (!result.ok() || result->rank() != 1 ||
      result->byte_strides().size() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(result->shape()[0]);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      result->byte_strided_origin_pointer().get());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    const auto* value = reinterpret_cast<const T*>(
        origin + static_cast<ts::Index>(index) * result->byte_strides()[0]);
    (*output)[index] = static_cast<double>(*value);
  }
  return true;
}

bool readScalars(const ArchiveContext::Impl& archive, const std::string& path,
                 std::vector<double>* output) {
  return readTypedScalars<float>(archive, path, output) ||
         readTypedScalars<double>(archive, path, output);
}

bool readBooleans(const ArchiveContext::Impl& archive, const std::string& path,
                  std::vector<uint8_t>* output) {
  if (const auto store = openArray<bool, 1>(archive, path)) {
    const auto result = ts::Read(*store).result();
    if (result.ok() && result->rank() == 1) {
      const size_t count = static_cast<size_t>(result->shape()[0]);
      const auto* values = static_cast<const bool*>(result->data());
      output->resize(count);
      for (size_t index = 0; index < count; ++index) {
        (*output)[index] = values[index] ? 1 : 0;
      }
      return true;
    }
  }
  std::vector<int64_t> integers;
  if (!readIntegers(archive, path, &integers)) {
    return false;
  }
  output->resize(integers.size());
  for (size_t index = 0; index < integers.size(); ++index) {
    (*output)[index] = integers[index] != 0 ? 1 : 0;
  }
  return true;
}

bool readStrings(const ArchiveContext::Impl& archive, const std::string& path,
                 std::vector<std::string>* output) {
  if (const auto store = openArray<std::string, 1>(archive, path)) {
    const auto result = ts::Read(*store).result();
    if (result.ok() && result->rank() == 1) {
      const size_t count = static_cast<size_t>(result->shape()[0]);
      const auto* values = static_cast<const std::string*>(result->data());
      output->assign(values, values + count);
      return true;
    }
  }
  const auto store = openArray<uint8_t, 2>(archive, path);
  if (!store) {
    return false;
  }
  const auto result = ts::Read(*store).result();
  if (!result.ok() || result->rank() != 2 ||
      result->byte_strides().size() != 2) {
    return false;
  }
  const size_t rows = static_cast<size_t>(result->shape()[0]);
  const size_t width = static_cast<size_t>(result->shape()[1]);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      result->byte_strided_origin_pointer().get());
  output->clear();
  output->reserve(rows);
  for (size_t row = 0; row < rows; ++row) {
    std::string value;
    value.reserve(width);
    for (size_t column = 0; column < width; ++column) {
      const char byte = *reinterpret_cast<const char*>(
          origin + static_cast<ts::Index>(row) * result->byte_strides()[0] +
          static_cast<ts::Index>(column) * result->byte_strides()[1]);
      if (byte == '\0') {
        break;
      }
      value.push_back(byte);
    }
    output->push_back(std::move(value));
  }
  return true;
}

std::string stringValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

int64_t integerValue(const json& attributes, const char* key,
                     int64_t fallback = -1) {
  const auto found = attributes.find(key);
  if (found == attributes.end() || !found->is_number_integer()) {
    return fallback;
  }
  if (found->is_number_unsigned()) {
    const uint64_t value = found->get<uint64_t>();
    return value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? static_cast<int64_t>(value)
               : fallback;
  }
  return found->get<int64_t>();
}

double doubleValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_number()
             ? found->get<double>()
             : std::numeric_limits<double>::quiet_NaN();
}

template <typename T>
T atOr(const std::vector<T>& values, size_t index, T fallback) {
  return index < values.size() ? values[index] : std::move(fallback);
}

std::string normalizedLevel(std::string value) {
  if (value.rfind("speed_", 0) == 0) {
    value.erase(0, 6);
  }
  return value;
}

bool validName(const std::string& value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string::npos;
}

bool arrayMetadataExists(const std::filesystem::path& root,
                         const std::string& path) {
  const auto base = root / path;
  return std::filesystem::is_regular_file(base / "zarr.json") ||
         std::filesystem::is_regular_file(base / ".zarray");
}

std::string latestRun(const ArchiveContext::Impl& archive,
                      const std::string& group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char*, 4> keys = {
      "latest", "latest_completed", "latest_complete", "latest_success"};
  for (const char* key : keys) {
    const std::string value = stringValue(*attributes, key);
    if (validName(value)) {
      return value;
    }
  }
  return {};
}

std::vector<std::string> runNames(const ArchiveContext::Impl& archive,
                                  const std::string& group,
                                  const std::string& requested_run) {
  if (!requested_run.empty()) {
    return validName(requested_run) ? std::vector<std::string>{requested_run}
                                    : std::vector<std::string>{};
  }
  std::vector<std::string> result;
  const std::filesystem::path group_path = archive.root_path / group;
  std::error_code error;
  if (std::filesystem::is_directory(group_path, error)) {
    for (std::filesystem::directory_iterator iterator(group_path, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (iterator->is_directory(error)) {
        const std::string name = iterator->path().filename().string();
        if (validName(name)) {
          result.push_back(name);
        }
      }
    }
  }
  const std::string latest = latestRun(archive, group);
  if (!latest.empty()) {
    result.push_back(latest);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::optional<FrameStore> openFrameStore(const ArchiveContext::Impl& archive,
                                         const std::string& path) {
  if (auto store = openArray<int64_t, 1>(archive, path)) {
    return FrameStore{std::move(*store)};
  }
  if (auto store = openArray<int32_t, 1>(archive, path)) {
    return FrameStore{std::move(*store)};
  }
  return std::nullopt;
}

std::optional<ScalarStore> openScalarStore(const ArchiveContext::Impl& archive,
                                           const std::string& path) {
  if (auto store = openArray<float, 1>(archive, path)) {
    return ScalarStore{std::move(*store)};
  }
  if (auto store = openArray<double, 1>(archive, path)) {
    return ScalarStore{std::move(*store)};
  }
  return std::nullopt;
}

std::optional<MatrixStore> openMatrixStore(const ArchiveContext::Impl& archive,
                                           const std::string& path) {
  if (auto store = openArray<float, 2>(archive, path)) {
    return MatrixStore{std::move(*store)};
  }
  if (auto store = openArray<double, 2>(archive, path)) {
    return MatrixStore{std::move(*store)};
  }
  return std::nullopt;
}

size_t rowCount(const FrameStore& store) {
  return std::visit(
      [](const auto& value) {
        return value.domain().shape()[0] > 0
                   ? static_cast<size_t>(value.domain().shape()[0])
                   : 0;
      },
      store);
}

size_t rowCount(const ScalarStore& store) {
  return std::visit(
      [](const auto& value) {
        return value.domain().shape()[0] > 0
                   ? static_cast<size_t>(value.domain().shape()[0])
                   : 0;
      },
      store);
}

size_t matrixRows(const MatrixStore& store) {
  return std::visit(
      [](const auto& value) {
        return value.domain().shape()[0] > 0
                   ? static_cast<size_t>(value.domain().shape()[0])
                   : 0;
      },
      store);
}

size_t matrixColumns(const MatrixStore& store) {
  return std::visit(
      [](const auto& value) {
        return value.domain().shape()[1] > 0
                   ? static_cast<size_t>(value.domain().shape()[1])
                   : 0;
      },
      store);
}

template <typename Variant, typename Output>
bool readRankOneStore(const Variant& store, std::vector<Output>* output) {
  return std::visit(
      [&](const auto& typed) {
        const auto result = ts::Read(typed).result();
        if (!result.ok() || result->rank() != 1 ||
            result->byte_strides().size() != 1) {
          return false;
        }
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const size_t count = static_cast<size_t>(result->shape()[0]);
        const auto* origin = reinterpret_cast<const uint8_t*>(
            result->byte_strided_origin_pointer().get());
        output->resize(count);
        for (size_t index = 0; index < count; ++index) {
          const auto* value = reinterpret_cast<const Source*>(
              origin +
              static_cast<ts::Index>(index) * result->byte_strides()[0]);
          (*output)[index] = static_cast<Output>(*value);
        }
        return true;
      },
      store);
}

bool readMatrixRow(const MatrixStore& store, size_t row,
                   std::vector<double>* output) {
  return std::visit(
      [&](const auto& typed) {
        if (row >= static_cast<size_t>(typed.domain().shape()[0])) {
          return false;
        }
        ts::Box<2> domain(typed.domain().box());
        domain.origin()[0] = static_cast<ts::Index>(row);
        domain.shape()[0] = 1;
        const auto result =
            ts::Read(typed | ts::IdentityTransform(domain)).result();
        if (!result.ok() || result->rank() != 2 || result->shape()[0] != 1 ||
            result->byte_strides().size() != 2) {
          return false;
        }
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const size_t columns = static_cast<size_t>(result->shape()[1]);
        const auto* origin = reinterpret_cast<const uint8_t*>(
            result->byte_strided_origin_pointer().get());
        output->resize(columns);
        for (size_t column = 0; column < columns; ++column) {
          const auto* value = reinterpret_cast<const Source*>(
              origin +
              static_cast<ts::Index>(column) * result->byte_strides()[1]);
          (*output)[column] = static_cast<double>(*value);
        }
        return true;
      },
      store);
}

struct DetectorCache {
  std::optional<FrameStore> frames;
  std::optional<ScalarStore> scalar;
  std::optional<MatrixStore> matrix;
  size_t matrix_row = 0;
  mutable std::mutex mutex;
  mutable bool attempted = false;
  mutable bool loaded = false;
  mutable std::vector<int64_t> cached_frames;
  mutable std::vector<double> cached_values;
  mutable std::string error;

  bool load() const {
    std::lock_guard<std::mutex> lock(mutex);
    if (attempted) {
      return loaded;
    }
    attempted = true;
    if (scalar) {
      loaded = readRankOneStore(*scalar, &cached_values);
    } else if (matrix) {
      loaded = readMatrixRow(*matrix, matrix_row, &cached_values);
    }
    if (!loaded) {
      error = "Failed to read the swim-bout detector response";
      return false;
    }
    if (frames) {
      std::vector<int64_t> read_frames;
      if (!readRankOneStore(*frames, &read_frames)) {
        error = "Failed to read swim-bout detector frame indices";
        loaded = false;
        return false;
      }
      cached_frames.reserve(cached_values.size());
      for (size_t index = 0; index < cached_values.size(); ++index) {
        const int64_t fallback = cached_frames.empty()
                                     ? static_cast<int64_t>(index)
                                     : cached_frames.back() + 1;
        cached_frames.push_back(index < read_frames.size() &&
                                        read_frames[index] >= 0
                                    ? read_frames[index]
                                    : fallback);
      }
    } else {
      cached_frames.resize(cached_values.size());
      for (size_t index = 0; index < cached_frames.size(); ++index) {
        cached_frames[index] = static_cast<int64_t>(index);
      }
    }
    if (!std::is_sorted(cached_frames.begin(), cached_frames.end())) {
      error = "Swim-bout detector frame indices are not sorted";
      loaded = false;
    }
    return loaded;
  }
};

struct CandidateStore {
  crimson::timeline::SwimBoutCandidateDescriptor descriptor;
  std::vector<crimson::timeline::SwimBoutInterval> intervals;
  std::shared_ptr<DetectorCache> detector;
};

class TensorStoreRepository final
    : public crimson::timeline::SwimBoutTimelineRepository {
 public:
  TensorStoreRepository(
      crimson::timeline::SwimBoutTimelineDescriptor descriptor,
      std::vector<CandidateStore> candidates)
      : descriptor_(std::move(descriptor)) {
    for (auto& candidate : candidates) {
      candidates_[candidate.descriptor.key] = std::move(candidate);
    }
  }

  const crimson::timeline::SwimBoutTimelineDescriptor& descriptor()
      const override {
    return descriptor_;
  }

  crimson::timeline::SwimBoutTimelineWindow resolveWindow(
      const crimson::timeline::SwimBoutTimelineRequest& request)
      const override {
    const auto found = candidates_.find(request.candidate_key);
    if (found == candidates_.end()) {
      return failure(request,
                     crimson::timeline::SwimBoutTimelineStatus::InvalidRequest,
                     "Swim-bout candidate is unavailable");
    }
    if (request.include_detector_trace && found->second.detector) {
      if (!found->second.detector->load()) {
        return failure(request,
                       crimson::timeline::SwimBoutTimelineStatus::ReadFailed,
                       found->second.detector->error);
      }
      std::lock_guard<std::mutex> lock(found->second.detector->mutex);
      return crimson::timeline::buildSwimBoutTimelineWindow(
          descriptor_, request, found->second.intervals,
          found->second.detector->cached_frames, {},
          found->second.detector->cached_values);
    }
    return crimson::timeline::buildSwimBoutTimelineWindow(
        descriptor_, request, found->second.intervals, {}, {}, {});
  }

 private:
  static crimson::timeline::SwimBoutTimelineWindow failure(
      const crimson::timeline::SwimBoutTimelineRequest& request,
      crimson::timeline::SwimBoutTimelineStatus status, std::string error) {
    crimson::timeline::SwimBoutTimelineWindow result;
    result.request = request;
    result.status = status;
    result.error = std::move(error);
    return result;
  }

  crimson::timeline::SwimBoutTimelineDescriptor descriptor_;
  std::unordered_map<std::string, CandidateStore> candidates_;
};

std::vector<crimson::timeline::SwimBoutInterval> filteredIntervals(
    const std::vector<int64_t>& table_candidate_ids,
    const std::vector<int64_t>& table_signal_ids, int32_t candidate_id,
    int32_t signal_id, const std::vector<int64_t>& starts,
    const std::vector<int64_t>& ends, const std::vector<int64_t>& core_starts,
    const std::vector<int64_t>& core_ends,
    const std::vector<uint8_t>& gap_censored) {
  std::vector<crimson::timeline::SwimBoutInterval> result;
  const size_t count =
      std::min({table_candidate_ids.size(), table_signal_ids.size(),
                starts.size(), ends.size()});
  for (size_t row = 0; row < count; ++row) {
    if (table_candidate_ids[row] != candidate_id ||
        table_signal_ids[row] != signal_id) {
      continue;
    }
    result.push_back({row, starts[row], ends[row],
                      atOr(core_starts, row, int64_t{-1}),
                      atOr(core_ends, row, int64_t{-1}),
                      atOr(gap_censored, row, uint8_t{0}) != 0});
  }
  return result;
}

bool loadCompactRun(const ArchiveContext::Impl& archive,
                    const std::string& group, const std::string& run,
                    const json& attributes, bool latest,
                    std::vector<CandidateStore>* output, size_t* frame_count) {
  const std::string base = group + "/" + run + "/";
  const std::string candidate_base = base + "indexes/candidates/";
  const std::string signal_base = base + "indexes/signal_variants/";
  const std::string bouts_base = base + "tables/bouts/";

  std::vector<int64_t> candidate_ids;
  if (!readIntegers(archive, candidate_base + "candidate_id", &candidate_ids) ||
      candidate_ids.empty()) {
    return false;
  }
  std::vector<uint8_t> candidate_defaults;
  std::vector<std::string> candidate_methods;
  std::vector<double> candidate_min_bout;
  std::vector<double> candidate_min_gap;
  readBooleans(archive, candidate_base + "is_default", &candidate_defaults);
  readStrings(archive, candidate_base + "detection_method", &candidate_methods);
  readScalars(archive, candidate_base + "min_bout_duration_s",
              &candidate_min_bout);
  readScalars(archive, candidate_base + "min_gap_duration_s",
              &candidate_min_gap);

  int64_t selected_candidate =
      integerValue(attributes, "default_candidate_id", -1);
  auto selected =
      std::find(candidate_ids.begin(), candidate_ids.end(), selected_candidate);
  if (selected == candidate_ids.end()) {
    for (size_t index = 0;
         index < candidate_defaults.size() && index < candidate_ids.size();
         ++index) {
      if (candidate_defaults[index] != 0) {
        selected = candidate_ids.begin() + static_cast<std::ptrdiff_t>(index);
        break;
      }
    }
  }
  if (selected == candidate_ids.end()) {
    selected = candidate_ids.begin();
  }
  const size_t selected_candidate_row =
      static_cast<size_t>(selected - candidate_ids.begin());
  selected_candidate = *selected;
  if (selected_candidate < std::numeric_limits<int32_t>::min() ||
      selected_candidate > std::numeric_limits<int32_t>::max()) {
    return false;
  }

  std::vector<int64_t> signal_ids;
  if (!readIntegers(archive, signal_base + "signal_id", &signal_ids) ||
      signal_ids.empty()) {
    return false;
  }
  std::vector<int64_t> signal_candidate_ids;
  std::vector<std::string> speed_levels;
  std::vector<std::string> signal_roles;
  std::vector<std::string> signal_names;
  std::vector<std::string> source_levels;
  std::vector<std::string> path_levels;
  std::vector<std::string> transform_types;
  std::vector<std::string> units;
  std::vector<double> tau_seconds;
  readIntegers(archive, signal_base + "candidate_id", &signal_candidate_ids);
  readStrings(archive, signal_base + "speed_level", &speed_levels);
  readStrings(archive, signal_base + "role", &signal_roles);
  readStrings(archive, signal_base + "signal_name", &signal_names);
  readStrings(archive, signal_base + "source_level", &source_levels);
  readStrings(archive, signal_base + "path_distance_source_level",
              &path_levels);
  readStrings(archive, signal_base + "transform_type", &transform_types);
  readStrings(archive, signal_base + "units", &units);
  readScalars(archive, signal_base + "tau_s", &tau_seconds);

  std::vector<int64_t> table_candidate_ids;
  std::vector<int64_t> table_signal_ids;
  std::vector<int64_t> starts;
  std::vector<int64_t> ends;
  std::vector<int64_t> core_starts;
  std::vector<int64_t> core_ends;
  std::vector<uint8_t> gap_censored;
  if (!readIntegers(archive, bouts_base + "candidate_id",
                    &table_candidate_ids) ||
      !readIntegers(archive, bouts_base + "signal_id", &table_signal_ids) ||
      !readIntegers(archive, bouts_base + "start_frame", &starts) ||
      !readIntegers(archive, bouts_base + "end_frame", &ends)) {
    return false;
  }
  readIntegers(archive, bouts_base + "core_start_frame", &core_starts);
  readIntegers(archive, bouts_base + "core_end_frame", &core_ends);
  readBooleans(archive, bouts_base + "gap_censored", &gap_censored);

  std::vector<int64_t> detector_signal_ids;
  readIntegers(archive, base + "signals/detector_signal_signal_ids",
               &detector_signal_ids);
  auto detector_matrix =
      openMatrixStore(archive, base + "signals/detector_signal_mm_s");
  auto detector_frames =
      openFrameStore(archive, base + "signals/frame_indices");
  const std::string default_level = stringValue(attributes, "default_level");
  const int64_t default_signal =
      integerValue(attributes, "default_signal_id", -1);
  bool loaded = false;
  for (size_t signal_row = 0; signal_row < signal_ids.size(); ++signal_row) {
    const int64_t signal_candidate =
        atOr(signal_candidate_ids, signal_row, selected_candidate);
    if (signal_candidate != selected_candidate ||
        signal_ids[signal_row] < std::numeric_limits<int32_t>::min() ||
        signal_ids[signal_row] > std::numeric_limits<int32_t>::max()) {
      continue;
    }
    const int32_t signal_id = static_cast<int32_t>(signal_ids[signal_row]);
    auto intervals =
        filteredIntervals(table_candidate_ids, table_signal_ids,
                          static_cast<int32_t>(selected_candidate), signal_id,
                          starts, ends, core_starts, core_ends, gap_censored);
    if (intervals.empty()) {
      continue;
    }

    CandidateStore candidate;
    auto& descriptor = candidate.descriptor;
    descriptor.run_name = run;
    descriptor.source_group = group;
    descriptor.speed_level = atOr(speed_levels, signal_row, std::string{});
    descriptor.signal_name = atOr(signal_names, signal_row, std::string{});
    if (descriptor.speed_level.empty()) {
      descriptor.speed_level = descriptor.signal_name;
    }
    descriptor.layout = "compact_tabular_v2";
    descriptor.compact_layout = true;
    descriptor.candidate_id = static_cast<int32_t>(selected_candidate);
    descriptor.signal_id = signal_id;
    descriptor.signal_role = atOr(signal_roles, signal_row, std::string{});
    descriptor.source_track_kinematics_run =
        stringValue(attributes, "source_track_kinematics_run");
    descriptor.track_id = static_cast<int32_t>(
        std::clamp<int64_t>(integerValue(attributes, "track_id", -1), -1,
                            std::numeric_limits<int32_t>::max()));
    descriptor.detection_method =
        atOr(candidate_methods, selected_candidate_row,
             stringValue(attributes, "detection_method"));
    descriptor.detection_signal_source_level =
        atOr(source_levels, signal_row, std::string{});
    descriptor.detection_signal_source_path =
        base + "signals/detector_signal_mm_s";
    descriptor.movement_metric_source_level =
        descriptor.detection_signal_source_level;
    descriptor.path_distance_source_level =
        atOr(path_levels, signal_row, std::string{});
    descriptor.detector_trace_label = "Detector response";
    descriptor.detector_trace_units =
        atOr(units, signal_row, std::string{"mm/s"});
    descriptor.threshold_mm = doubleValue(attributes, "threshold_mm");
    descriptor.exponential_tau_s = atOr(
        tau_seconds, signal_row, doubleValue(attributes, "exponential_tau_s"));
    descriptor.min_bout_duration_s =
        atOr(candidate_min_bout, selected_candidate_row,
             doubleValue(attributes, "min_bout_duration_s"));
    descriptor.min_gap_duration_s =
        atOr(candidate_min_gap, selected_candidate_row,
             doubleValue(attributes, "min_gap_duration_s"));
    descriptor.min_peak_prominence_mm_s =
        doubleValue(attributes, "min_peak_prominence_mm_s");
    descriptor.peak_width_rel_height =
        doubleValue(attributes, "peak_width_rel_height");
    descriptor.latest_run = latest;
    descriptor.default_level =
        default_signal == signal_id ||
        (!default_level.empty() && normalizedLevel(default_level) ==
                                       normalizedLevel(descriptor.speed_level));
    descriptor.bout_count = intervals.size();
    descriptor.key = crimson::timeline::makeSwimBoutCandidateKey(
        run, descriptor.candidate_id, descriptor.signal_id,
        descriptor.speed_level);
    candidate.intervals = std::move(intervals);

    if (detector_matrix) {
      const auto detector = std::find(detector_signal_ids.begin(),
                                      detector_signal_ids.end(), signal_id);
      if (detector != detector_signal_ids.end()) {
        const size_t row =
            static_cast<size_t>(detector - detector_signal_ids.begin());
        if (row < matrixRows(*detector_matrix)) {
          candidate.detector = std::make_shared<DetectorCache>();
          candidate.detector->matrix = *detector_matrix;
          candidate.detector->matrix_row = row;
          candidate.detector->frames = detector_frames;
          descriptor.detector_sample_count = matrixColumns(*detector_matrix);
          descriptor.has_detector_trace = descriptor.detector_sample_count != 0;
        }
      }
    }
    for (const auto& interval : candidate.intervals) {
      if (interval.end_frame >= 0) {
        *frame_count =
            std::max(*frame_count, static_cast<size_t>(interval.end_frame) + 1);
      }
    }
    if (candidate.detector) {
      *frame_count = std::max(*frame_count, descriptor.detector_sample_count);
    }
    descriptor.display_name =
        crimson::timeline::swimBoutCandidateLabel(descriptor);
    output->push_back(std::move(candidate));
    loaded = true;
  }
  return loaded;
}

std::vector<std::string> legacyLevels(const ArchiveContext::Impl& archive,
                                      const std::string& base) {
  std::vector<std::string> result;
  const std::filesystem::path run_path = archive.root_path / base;
  std::error_code error;
  if (!std::filesystem::is_directory(run_path, error)) {
    return result;
  }
  for (std::filesystem::directory_iterator iterator(run_path, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory(error)) {
      continue;
    }
    const std::string level = iterator->path().filename().string();
    const std::string bouts = base + "/" + level + "/bouts/";
    if (validName(level) &&
        (arrayMetadataExists(archive.root_path, bouts + "start_frame") ||
         arrayMetadataExists(archive.root_path, bouts + "end_frame"))) {
      result.push_back(level);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool loadLegacyRun(const ArchiveContext::Impl& archive,
                   const std::string& group, const std::string& run,
                   const json& attributes, bool latest,
                   std::vector<CandidateStore>* output, size_t* frame_count) {
  const std::string run_base = group + "/" + run;
  const std::string default_level = stringValue(attributes, "default_level");
  bool loaded = false;
  for (const auto& level : legacyLevels(archive, run_base)) {
    const std::string level_base = run_base + "/" + level + "/";
    const std::string bouts_base = level_base + "bouts/";
    std::vector<int64_t> starts;
    std::vector<int64_t> ends;
    if (!readIntegers(archive, bouts_base + "start_frame", &starts) ||
        !readIntegers(archive, bouts_base + "end_frame", &ends)) {
      continue;
    }
    const size_t count = std::min(starts.size(), ends.size());
    if (count == 0) {
      continue;
    }
    std::vector<int64_t> core_starts;
    std::vector<int64_t> core_ends;
    std::vector<uint8_t> gap_censored;
    readIntegers(archive, bouts_base + "core_start_frame", &core_starts);
    readIntegers(archive, bouts_base + "core_end_frame", &core_ends);
    readBooleans(archive, bouts_base + "gap_censored", &gap_censored);
    const json level_attributes =
        internal::ReadArchiveAttributes(archive, level_base)
            .value_or(json::object());

    CandidateStore candidate;
    auto& descriptor = candidate.descriptor;
    descriptor.source_group = group;
    descriptor.run_name = run;
    descriptor.speed_level = level;
    descriptor.layout = "hierarchical_v1";
    descriptor.source_track_kinematics_run =
        stringValue(attributes, "source_track_kinematics_run");
    descriptor.track_id = static_cast<int32_t>(
        std::clamp<int64_t>(integerValue(attributes, "track_id", -1), -1,
                            std::numeric_limits<int32_t>::max()));
    descriptor.detection_method = stringValue(attributes, "detection_method");
    descriptor.detection_signal_source_level =
        stringValue(attributes, "detection_signal_source_level");
    if (descriptor.detection_signal_source_level.empty()) {
      descriptor.detection_signal_source_level =
          stringValue(attributes, "exponential_source_level");
    }
    if (descriptor.detection_signal_source_level.empty()) {
      descriptor.detection_signal_source_level =
          stringValue(level_attributes, "source_speed_level");
    }
    descriptor.detection_signal_source_path =
        stringValue(attributes, "detection_signal_source_path");
    descriptor.movement_metric_source_level =
        stringValue(attributes, "movement_metric_source_level");
    descriptor.path_distance_source_level =
        stringValue(attributes, "path_distance_source_level");
    if (descriptor.path_distance_source_level.empty()) {
      descriptor.path_distance_source_level =
          stringValue(level_attributes, "path_distance_source_level");
    }
    descriptor.threshold_mm = doubleValue(attributes, "threshold_mm");
    descriptor.exponential_tau_s = doubleValue(attributes, "exponential_tau_s");
    if (!std::isfinite(descriptor.exponential_tau_s)) {
      descriptor.exponential_tau_s = doubleValue(level_attributes, "tau_s");
    }
    descriptor.min_bout_duration_s =
        doubleValue(attributes, "min_bout_duration_s");
    descriptor.min_gap_duration_s =
        doubleValue(attributes, "min_gap_duration_s");
    descriptor.min_peak_prominence_mm_s =
        doubleValue(attributes, "min_peak_prominence_mm_s");
    descriptor.peak_width_rel_height =
        doubleValue(attributes, "peak_width_rel_height");
    descriptor.latest_run = latest;
    descriptor.default_level =
        !default_level.empty() &&
        normalizedLevel(default_level) == normalizedLevel(level);
    const auto level_default = level_attributes.find("is_default_level");
    if (level_default != level_attributes.end() &&
        level_default->is_boolean()) {
      descriptor.default_level = level_default->get<bool>();
    }

    candidate.intervals.reserve(count);
    for (size_t row = 0; row < count; ++row) {
      candidate.intervals.push_back({row, starts[row], ends[row],
                                     atOr(core_starts, row, int64_t{-1}),
                                     atOr(core_ends, row, int64_t{-1}),
                                     atOr(gap_censored, row, uint8_t{0}) != 0});
      if (ends[row] >= 0) {
        *frame_count =
            std::max(*frame_count, static_cast<size_t>(ends[row]) + 1);
      }
    }
    descriptor.bout_count = candidate.intervals.size();
    descriptor.key = crimson::timeline::makeSwimBoutCandidateKey(
        run, -1, -1, descriptor.speed_level);

    auto detector =
        openScalarStore(archive, level_base + "detection_signal_mm_s");
    std::string detector_path = level_base + "detection_signal_mm_s";
    if (!detector) {
      detector = openScalarStore(archive, level_base + "speed_exponential_mm");
      detector_path = level_base + "speed_exponential_mm";
    }
    if (detector) {
      candidate.detector = std::make_shared<DetectorCache>();
      candidate.detector->scalar = *detector;
      candidate.detector->frames =
          openFrameStore(archive, level_base + "frame_indices");
      descriptor.detector_trace_label = "Detector response";
      descriptor.detector_trace_units = "mm/s";
      descriptor.detector_sample_count = rowCount(*detector);
      descriptor.has_detector_trace = descriptor.detector_sample_count != 0;
      if (descriptor.detection_signal_source_path.empty()) {
        descriptor.detection_signal_source_path = detector_path;
      }
      *frame_count = std::max(*frame_count, descriptor.detector_sample_count);
    }
    descriptor.display_name =
        crimson::timeline::swimBoutCandidateLabel(descriptor);
    output->push_back(std::move(candidate));
    loaded = true;
  }
  return loaded;
}

std::string globalDefaultCandidate(
    const std::vector<CandidateStore>& candidates) {
  if (candidates.empty()) {
    return {};
  }
  const CandidateStore* chosen = &candidates.front();
  for (const auto& candidate : candidates) {
    if (candidate.descriptor.latest_run && candidate.descriptor.default_level) {
      return candidate.descriptor.key;
    }
    if (chosen->descriptor.latest_run && !chosen->descriptor.default_level) {
      continue;
    }
    if (candidate.descriptor.latest_run || candidate.descriptor.default_level) {
      chosen = &candidate;
    }
  }
  return chosen->descriptor.key;
}

}  // namespace

std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository>
OpenSwimBoutTimelineRepository(const std::shared_ptr<ArchiveContext>& archive,
                               size_t frame_count_hint,
                               const std::string& requested_run,
                               std::string* error_message) {
  auto fail = [&](std::string message)
      -> std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository> {
    if (error_message != nullptr) {
      *error_message = std::move(message);
    }
    return nullptr;
  };
  if (!archive || !archive->impl_) {
    return fail("Archive context is unavailable");
  }
  const auto& impl = *archive->impl_;
  const std::string group = "analysis/swim_bout_runs";
  const std::string latest = latestRun(impl, group);
  const auto runs = runNames(impl, group, requested_run);
  if (runs.empty()) {
    return fail("No swim-bout runs were found");
  }

  std::vector<CandidateStore> candidates;
  size_t frame_count = frame_count_hint;
  for (const auto& run : runs) {
    const auto attributes =
        internal::ReadArchiveAttributes(impl, group + "/" + run);
    if (!attributes) {
      continue;
    }
    const bool is_latest = !latest.empty() && run == latest;
    const std::string layout = stringValue(*attributes, "layout");
    const bool compact =
        layout == "compact_tabular_v2" ||
        arrayMetadataExists(
            impl.root_path,
            group + "/" + run + "/indexes/candidates/candidate_id");
    if (compact) {
      loadCompactRun(impl, group, run, *attributes, is_latest, &candidates,
                     &frame_count);
    } else {
      loadLegacyRun(impl, group, run, *attributes, is_latest, &candidates,
                    &frame_count);
    }
  }
  if (candidates.empty()) {
    return fail("No readable swim-bout candidates were found");
  }
  if (frame_count == 0) {
    return fail("Swim-bout camera-frame count is unavailable");
  }

  crimson::timeline::SwimBoutTimelineDescriptor descriptor;
  descriptor.frame_count = frame_count;
  descriptor.default_candidate = globalDefaultCandidate(candidates);
  descriptor.candidates.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    descriptor.candidates.push_back(candidate.descriptor);
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return std::make_unique<TensorStoreRepository>(std::move(descriptor),
                                                 std::move(candidates));
}

}  // namespace crimson::zarr
