#include "zarr/tensorstore_analysis_series_timeline_repository.h"

#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "data_access_cache.h"
#include "zarr/archive_context_internal.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

using FrameStore =
    std::variant<ts::TensorStore<int64_t, 1>, ts::TensorStore<int32_t, 1>>;
using ScalarStore =
    std::variant<ts::TensorStore<float, 1>, ts::TensorStore<double, 1>>;
using Vec2Store =
    std::variant<ts::TensorStore<float, 2>, ts::TensorStore<double, 2>>;
using MaskStore =
    std::variant<ts::TensorStore<bool, 1>, ts::TensorStore<uint8_t, 1>>;

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

template <typename T, ts::DimensionIndex Rank>
auto sliceRows(const ts::TensorStore<T, Rank>& store, ts::Index start,
               ts::Index stop) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = start;
  domain.shape()[0] = stop - start;
  return store | ts::IdentityTransform(domain);
}

std::string stringValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

int integerValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_number_integer()
             ? found->get<int>()
             : 0;
}

bool validName(const std::string& value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string::npos;
}

std::string latestRun(const ArchiveContext::Impl& archive,
                      const std::string& group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char*, 4> keys = {
      "latest_complete", "latest_completed", "latest", "latest_success"};
  for (const char* key : keys) {
    const std::string value = stringValue(*attributes, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
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

std::optional<Vec2Store> openVec2Store(const ArchiveContext::Impl& archive,
                                       const std::string& path) {
  if (auto store = openArray<float, 2>(archive, path);
      store && store->domain().shape()[1] == 2) {
    return Vec2Store{std::move(*store)};
  }
  if (auto store = openArray<double, 2>(archive, path);
      store && store->domain().shape()[1] == 2) {
    return Vec2Store{std::move(*store)};
  }
  return std::nullopt;
}

std::optional<MaskStore> openMaskStore(const ArchiveContext::Impl& archive,
                                       const std::string& path) {
  if (auto store = openArray<bool, 1>(archive, path)) {
    return MaskStore{std::move(*store)};
  }
  if (auto store = openArray<uint8_t, 1>(archive, path)) {
    return MaskStore{std::move(*store)};
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

size_t rowCount(const Vec2Store& store) {
  return std::visit(
      [](const auto& value) {
        return value.domain().shape()[0] > 0
                   ? static_cast<size_t>(value.domain().shape()[0])
                   : 0;
      },
      store);
}

size_t rowCount(const MaskStore& store) {
  return std::visit(
      [](const auto& value) {
        return value.domain().shape()[0] > 0
                   ? static_cast<size_t>(value.domain().shape()[0])
                   : 0;
      },
      store);
}

size_t frameElementBytes(const FrameStore& store) {
  return std::visit(
      [](const auto& value) {
        using Element = typename std::decay_t<decltype(value)>::Element;
        return sizeof(Element);
      },
      store);
}

size_t scalarElementBytes(const ScalarStore& store) {
  return std::visit(
      [](const auto& value) {
        using Element = typename std::decay_t<decltype(value)>::Element;
        return sizeof(Element);
      },
      store);
}

size_t vec2ElementBytes(const Vec2Store& store) {
  return std::visit(
      [](const auto& value) {
        using Element = typename std::decay_t<decltype(value)>::Element;
        return sizeof(Element);
      },
      store);
}

template <typename Variant, typename Convert>
bool readRankOneRange(const Variant& store, size_t first, size_t last,
                      Convert convert) {
  return std::visit(
      [&](const auto& typed) {
        if (last < first ||
            last > static_cast<size_t>(typed.domain().shape()[0])) {
          return false;
        }
        const auto read =
            ts::Read(sliceRows(typed, static_cast<ts::Index>(first),
                               static_cast<ts::Index>(last)))
                .result();
        if (!read.ok() || read->rank() != 1 ||
            read->shape()[0] != static_cast<ts::Index>(last - first) ||
            read->byte_strides().size() != 1) {
          return false;
        }
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        for (size_t index = 0; index < last - first; ++index) {
          const auto* value = reinterpret_cast<const Source*>(
              origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
          convert(index, *value);
        }
        return true;
      },
      store);
}

bool readFrames(const FrameStore& store, size_t first, size_t last,
                std::vector<int64_t>* output) {
  output->assign(last - first, -1);
  return readRankOneRange(store, first, last, [&](size_t index, auto value) {
    (*output)[index] = static_cast<int64_t>(value);
  });
}

bool readScalars(const ScalarStore& store, size_t first, size_t last,
                 std::vector<double>* output) {
  output->assign(last - first, std::numeric_limits<double>::quiet_NaN());
  return readRankOneRange(store, first, last, [&](size_t index, auto value) {
    (*output)[index] = static_cast<double>(value);
  });
}

bool readMasks(const MaskStore& store, size_t first, size_t last,
               std::vector<uint8_t>* output) {
  output->assign(last - first, 0);
  return readRankOneRange(store, first, last, [&](size_t index, auto value) {
    (*output)[index] = value ? 1 : 0;
  });
}

bool readVec2Column(const Vec2Store& store, size_t first, size_t last,
                    size_t column, std::vector<double>* output) {
  if (column > 1) {
    return false;
  }
  output->assign(last - first, std::numeric_limits<double>::quiet_NaN());
  return std::visit(
      [&](const auto& typed) {
        if (last < first ||
            last > static_cast<size_t>(typed.domain().shape()[0])) {
          return false;
        }
        const auto read =
            ts::Read(sliceRows(typed, static_cast<ts::Index>(first),
                               static_cast<ts::Index>(last)))
                .result();
        if (!read.ok() || read->rank() != 2 || read->shape()[1] != 2 ||
            read->byte_strides().size() != 2) {
          return false;
        }
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        for (size_t row = 0; row < last - first; ++row) {
          const auto* value = reinterpret_cast<const Source*>(
              origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
              static_cast<ts::Index>(column) * read->byte_strides()[1]);
          (*output)[row] = static_cast<double>(*value);
        }
        return true;
      },
      store);
}

using ResidentNumeric = std::variant<std::vector<float>, std::vector<double>>;

uint64_t residentNumericBytes(const ResidentNumeric& values) {
  return std::visit(
      [](const auto& typed) {
        using Element = typename std::decay_t<decltype(typed)>::value_type;
        return static_cast<uint64_t>(typed.capacity()) * sizeof(Element);
      },
      values);
}

bool readResidentNumeric(const ResidentNumeric& values, size_t first,
                         size_t last, std::vector<double>* output) {
  if (last < first) {
    return false;
  }
  return std::visit(
      [&](const auto& typed) {
        if (last > typed.size()) {
          return false;
        }
        output->resize(last - first);
        std::transform(typed.begin() + static_cast<std::ptrdiff_t>(first),
                       typed.begin() + static_cast<std::ptrdiff_t>(last),
                       output->begin(),
                       [](auto value) { return static_cast<double>(value); });
        return true;
      },
      values);
}

bool readAllNativeScalars(const ScalarStore& store, ResidentNumeric* output) {
  return std::visit(
      [&](const auto& typed) {
        using Element = typename std::decay_t<decltype(typed)>::Element;
        const size_t count = static_cast<size_t>(typed.domain().shape()[0]);
        const auto read = ts::Read(typed).result();
        if (!read.ok() || read->rank() != 1 ||
            read->shape()[0] != static_cast<ts::Index>(count) ||
            read->byte_strides().size() != 1) {
          return false;
        }
        std::vector<Element> values(count);
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        for (size_t index = 0; index < count; ++index) {
          values[index] = *reinterpret_cast<const Element*>(
              origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
        }
        *output = std::move(values);
        return true;
      },
      store);
}

bool readAllNativeVec2Column(const Vec2Store& store, size_t column,
                             ResidentNumeric* output) {
  if (column > 1) {
    return false;
  }
  return std::visit(
      [&](const auto& typed) {
        using Element = typename std::decay_t<decltype(typed)>::Element;
        const size_t count = static_cast<size_t>(typed.domain().shape()[0]);
        const auto read =
            ts::Read(sliceRows(typed, 0, typed.domain().shape()[0])).result();
        if (!read.ok() || read->rank() != 2 || read->shape()[1] != 2 ||
            read->byte_strides().size() != 2) {
          return false;
        }
        std::vector<Element> values(count);
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        for (size_t row = 0; row < count; ++row) {
          values[row] = *reinterpret_cast<const Element*>(
              origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
              static_cast<ts::Index>(column) * read->byte_strides()[1]);
        }
        *output = std::move(values);
        return true;
      },
      store);
}

class FrameIndexBlocks {
 public:
  explicit FrameIndexBlocks(FrameStore store)
      : store_(std::move(store)), blocks_({2ULL * 1024ULL * 1024ULL, 0, 16}) {}

  std::optional<size_t> lowerBound(int64_t target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t first = 0;
    size_t last = rowCount(store_);
    while (first < last) {
      const size_t middle = first + (last - first) / 2;
      const auto value = at(middle);
      if (!value) {
        return std::nullopt;
      }
      if (*value < target) {
        first = middle + 1;
      } else {
        last = middle;
      }
    }
    return first;
  }

  crimson::timeline::AnalysisSeriesTimelineRepositoryMetrics metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = metrics_;
    result.cached_frame_index_bytes = blocks_.metrics().current_cpu_bytes;
    result.peak_cached_frame_index_bytes = blocks_.metrics().peak_cpu_bytes;
    return result;
  }

 private:
  static constexpr size_t kBlockRows = 16384;
  using Block = std::shared_ptr<const std::vector<int64_t>>;

  FrameStore store_;
  mutable std::mutex mutex_;
  mutable crimson::data::ByteBudgetLruCache<size_t, Block> blocks_;
  mutable crimson::timeline::AnalysisSeriesTimelineRepositoryMetrics metrics_;

  std::optional<int64_t> at(size_t row) const {
    const size_t block = row / kBlockRows;
    Block values;
    if (auto cached = blocks_.findAndTouch(block)) {
      values = *cached;
      ++metrics_.frame_index_cache_hits;
    } else {
      const size_t first = block * kBlockRows;
      const size_t last = std::min(rowCount(store_), first + kBlockRows);
      const auto started = std::chrono::steady_clock::now();
      auto loaded = std::make_shared<std::vector<int64_t>>();
      if (!readFrames(store_, first, last, loaded.get()) ||
          !std::is_sorted(loaded->begin(), loaded->end())) {
        return std::nullopt;
      }
      const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
      const uint64_t retained_bytes =
          loaded->capacity() * sizeof(std::vector<int64_t>::value_type);
      const auto admitted =
          blocks_.put(block, loaded, {retained_bytes, 0},
                      crimson::data::RequestPriority::CurrentFrame, false);
      if (!admitted.admitted()) {
        return std::nullopt;
      }
      values = std::move(loaded);
      ++metrics_.frame_index_block_reads;
      metrics_.frame_index_cache_evictions += admitted.evicted_keys.size();
      metrics_.frame_index_source_bytes +=
          static_cast<uint64_t>(last - first) * frameElementBytes(store_);
      metrics_.maximum_frame_index_read_ms =
          std::max(metrics_.maximum_frame_index_read_ms, elapsed_ms);
    }
    const size_t offset = row - block * kBlockRows;
    return values && offset < values->size()
               ? std::optional<int64_t>((*values)[offset])
               : std::nullopt;
  }
};

struct FieldStore {
  crimson::timeline::AnalysisSeriesTraceDescriptor descriptor;
  std::optional<ScalarStore> scalar;
  std::optional<Vec2Store> vec2;
  size_t vec2_column = 0;
  std::optional<MaskStore> mask;
};

struct ResidentField {
  ResidentNumeric values;
  std::optional<std::vector<uint8_t>> mask;
};

struct ResidentSource {
  std::vector<int64_t> frames;
  std::optional<ResidentNumeric> times;
  std::vector<ResidentField> fields;

  uint64_t retainedBytes() const {
    uint64_t result = static_cast<uint64_t>(frames.capacity()) *
                      sizeof(std::vector<int64_t>::value_type);
    if (times) {
      result += residentNumericBytes(*times);
    }
    for (const auto& field : fields) {
      result += residentNumericBytes(field.values);
      if (field.mask) {
        result += field.mask->capacity() * sizeof(uint8_t);
      }
    }
    return result;
  }
};

struct SourceStore {
  crimson::timeline::AnalysisSeriesSourceDescriptor descriptor;
  FrameStore frames;
  std::optional<ScalarStore> times;
  std::vector<FieldStore> fields;
  std::shared_ptr<FrameIndexBlocks> frame_index;
  std::shared_ptr<const ResidentSource> resident;
};

uint64_t preloadCandidateBytes(const SourceStore& source) {
  const uint64_t rows = rowCount(source.frames);
  uint64_t result = rows * sizeof(int64_t);
  if (source.times) {
    result += rows * scalarElementBytes(*source.times);
  }
  for (const auto& field : source.fields) {
    if (field.scalar) {
      result += rows * scalarElementBytes(*field.scalar);
    } else if (field.vec2) {
      result += rows * vec2ElementBytes(*field.vec2);
    }
    if (field.mask) {
      result += rows * sizeof(uint8_t);
    }
  }
  return result;
}

std::shared_ptr<const ResidentSource> preloadSource(const SourceStore& source) {
  auto resident = std::make_shared<ResidentSource>();
  if (!readFrames(source.frames, 0, rowCount(source.frames),
                  &resident->frames) ||
      !std::is_sorted(resident->frames.begin(), resident->frames.end())) {
    return nullptr;
  }
  if (source.times) {
    ResidentNumeric times;
    if (!readAllNativeScalars(*source.times, &times)) {
      return nullptr;
    }
    resident->times = std::move(times);
  }
  resident->fields.reserve(source.fields.size());
  for (const auto& field : source.fields) {
    ResidentField retained;
    bool ready = false;
    if (field.scalar) {
      ready = readAllNativeScalars(*field.scalar, &retained.values);
    } else if (field.vec2) {
      ready = readAllNativeVec2Column(*field.vec2, field.vec2_column,
                                      &retained.values);
    }
    if (!ready) {
      return nullptr;
    }
    if (field.mask) {
      std::vector<uint8_t> mask;
      if (!readMasks(*field.mask, 0, rowCount(*field.mask), &mask)) {
        return nullptr;
      }
      retained.mask = std::move(mask);
    }
    resident->fields.push_back(std::move(retained));
  }
  return resident;
}

class TensorStoreRepository final
    : public crimson::timeline::AnalysisSeriesTimelineRepository {
 public:
  TensorStoreRepository(
      crimson::timeline::AnalysisSeriesTimelineDescriptor descriptor,
      std::vector<SourceStore> sources,
      crimson::data::SmallSeriesPreloadPolicy preload_policy)
      : descriptor_(std::move(descriptor)) {
    for (auto& source : sources) {
      if (!source.frame_index) {
        source.frame_index = std::make_shared<FrameIndexBlocks>(source.frames);
      }
      sources_[source.descriptor.key] = std::move(source);
    }
    const auto selected = sources_.find(descriptor_.default_source);
    if (selected != sources_.end() && preload_policy.enabled()) {
      metrics_.preload_candidate_bytes =
          preloadCandidateBytes(selected->second);
      if (preload_policy.admits(metrics_.preload_candidate_bytes)) {
        const auto started = std::chrono::steady_clock::now();
        selected->second.resident = preloadSource(selected->second);
        metrics_.preload_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
        if (selected->second.resident) {
          metrics_.default_source_preloaded = true;
          metrics_.preloaded_retained_bytes =
              selected->second.resident->retainedBytes();
        }
      }
    }
  }

  const crimson::timeline::AnalysisSeriesTimelineDescriptor& descriptor()
      const override {
    return descriptor_;
  }

  crimson::timeline::AnalysisSeriesTimelineWindow resolveWindow(
      const crimson::timeline::AnalysisSeriesTimelineRequest& request)
      const override {
    auto failure = [&](crimson::timeline::AnalysisSeriesTimelineStatus status,
                       std::string error) {
      crimson::timeline::AnalysisSeriesTimelineWindow result;
      result.request = request;
      result.status = status;
      result.error = std::move(error);
      return result;
    };
    const auto found = sources_.find(request.source_key);
    if (found == sources_.end()) {
      return failure(
          crimson::timeline::AnalysisSeriesTimelineStatus::InvalidRequest,
          "Analysis-series source is unavailable");
    }
    if (request.first_frame < 0 || request.last_frame < request.first_frame ||
        request.last_frame >= static_cast<int64_t>(descriptor_.frame_count)) {
      return failure(
          crimson::timeline::AnalysisSeriesTimelineStatus::OutOfRange,
          "Analysis-series frame window is out of range");
    }

    const auto& source = found->second;
    std::optional<size_t> first_row;
    std::optional<size_t> last_row;
    if (source.resident) {
      first_row = static_cast<size_t>(
          std::lower_bound(source.resident->frames.begin(),
                           source.resident->frames.end(), request.first_frame) -
          source.resident->frames.begin());
      last_row =
          static_cast<size_t>(std::lower_bound(source.resident->frames.begin(),
                                               source.resident->frames.end(),
                                               request.last_frame + 1) -
                              source.resident->frames.begin());
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      ++metrics_.preloaded_window_resolves;
    } else {
      first_row = source.frame_index->lowerBound(request.first_frame);
      last_row = source.frame_index->lowerBound(request.last_frame + 1);
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      ++metrics_.paged_window_resolves;
    }
    if (!first_row || !last_row) {
      return failure(
          crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed,
          "Failed to resolve sparse analysis-series frame bounds");
    }
    if (*last_row <= *first_row) {
      return failure(crimson::timeline::AnalysisSeriesTimelineStatus::Missing,
                     "No source rows map to the requested frame window");
    }

    std::vector<int64_t> frames;
    if (source.resident) {
      frames.assign(source.resident->frames.begin() +
                        static_cast<std::ptrdiff_t>(*first_row),
                    source.resident->frames.begin() +
                        static_cast<std::ptrdiff_t>(*last_row));
    } else if (!readFrames(source.frames, *first_row, *last_row, &frames)) {
      return failure(
          crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed,
          "Failed to read analysis-series frame indices");
    }
    std::vector<double> times;
    if (source.resident && source.resident->times &&
        !readResidentNumeric(*source.resident->times, *first_row, *last_row,
                             &times)) {
      return failure(
          crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed,
          "Failed to read preloaded analysis-series sample times");
    }
    if (!source.resident && source.times &&
        !readScalars(*source.times, *first_row, *last_row, &times)) {
      return failure(
          crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed,
          "Failed to read analysis-series sample times");
    }

    std::vector<crimson::timeline::AnalysisSeriesTimelineFieldSeries> fields;
    fields.reserve(source.fields.size());
    for (size_t field_index = 0; field_index < source.fields.size();
         ++field_index) {
      const auto& field = source.fields[field_index];
      crimson::timeline::AnalysisSeriesTimelineFieldSeries values;
      values.key = field.descriptor.key;
      bool read_ok = false;
      if (source.resident && field_index < source.resident->fields.size()) {
        read_ok =
            readResidentNumeric(source.resident->fields[field_index].values,
                                *first_row, *last_row, &values.values);
      } else if (field.scalar) {
        read_ok =
            readScalars(*field.scalar, *first_row, *last_row, &values.values);
      } else if (field.vec2) {
        read_ok = readVec2Column(*field.vec2, *first_row, *last_row,
                                 field.vec2_column, &values.values);
      }
      if (!read_ok) {
        return failure(
            crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed,
            "Failed to read analysis-series trace: " + field.descriptor.key);
      }
      if (source.resident && field_index < source.resident->fields.size() &&
          source.resident->fields[field_index].mask) {
        const auto& mask = *source.resident->fields[field_index].mask;
        for (size_t row = 0; row < values.values.size(); ++row) {
          const size_t source_row = *first_row + row;
          if (source_row >= mask.size() || mask[source_row] == 0) {
            values.values[row] = std::numeric_limits<double>::quiet_NaN();
          }
        }
      } else if (field.mask) {
        std::vector<uint8_t> mask;
        if (!readMasks(*field.mask, *first_row, *last_row, &mask)) {
          return failure(
              crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed,
              "Failed to read analysis-series validity mask");
        }
        for (size_t row = 0; row < values.values.size() && row < mask.size();
             ++row) {
          if (mask[row] == 0) {
            values.values[row] = std::numeric_limits<double>::quiet_NaN();
          }
        }
      }
      fields.push_back(std::move(values));
    }
    return crimson::timeline::buildAnalysisSeriesTimelineWindow(
        descriptor_, request, frames, times, fields);
  }

  crimson::timeline::AnalysisSeriesTimelineRepositoryMetrics metrics()
      const override {
    crimson::timeline::AnalysisSeriesTimelineRepositoryMetrics result;
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      result = metrics_;
    }
    std::unordered_set<const FrameIndexBlocks*> visited;
    for (const auto& source : sources_) {
      const auto* index = source.second.frame_index.get();
      if (!index || !visited.insert(index).second) {
        continue;
      }
      const auto source_metrics = index->metrics();
      result.frame_index_block_reads += source_metrics.frame_index_block_reads;
      result.frame_index_cache_hits += source_metrics.frame_index_cache_hits;
      result.frame_index_cache_evictions +=
          source_metrics.frame_index_cache_evictions;
      result.frame_index_source_bytes +=
          source_metrics.frame_index_source_bytes;
      result.cached_frame_index_bytes +=
          source_metrics.cached_frame_index_bytes;
      result.peak_cached_frame_index_bytes +=
          source_metrics.peak_cached_frame_index_bytes;
      result.maximum_frame_index_read_ms =
          std::max(result.maximum_frame_index_read_ms,
                   source_metrics.maximum_frame_index_read_ms);
    }
    return result;
  }

 private:
  crimson::timeline::AnalysisSeriesTimelineDescriptor descriptor_;
  std::unordered_map<std::string, SourceStore> sources_;
  mutable std::mutex metrics_mutex_;
  mutable crimson::timeline::AnalysisSeriesTimelineRepositoryMetrics metrics_;
};

std::optional<ScalarStore> openFirstScalar(
    const ArchiveContext::Impl& archive,
    const std::vector<std::string>& candidates) {
  for (const auto& path : candidates) {
    if (auto store = openScalarStore(archive, path)) {
      return store;
    }
  }
  return std::nullopt;
}

std::vector<int64_t> readAllFrames(const FrameStore& store) {
  std::vector<int64_t> result;
  readFrames(store, 0, rowCount(store), &result);
  return result;
}

size_t resolvedFrameCount(const FrameStore& frames, size_t hint) {
  const size_t count = rowCount(frames);
  if (count == 0) {
    return hint;
  }
  std::vector<int64_t> last;
  if (!readFrames(frames, count - 1, count, &last) || last.empty() ||
      last.front() < 0) {
    return hint;
  }
  return std::max(hint, static_cast<size_t>(last.front()) + 1);
}

std::string titleCase(std::string value) {
  if (!value.empty() && value.front() >= 'a' && value.front() <= 'z') {
    value.front() = static_cast<char>(value.front() - 'a' + 'A');
  }
  return value;
}

void appendScalarField(SourceStore* source, const std::string& key,
                       const std::string& display_name,
                       const std::string& units, const std::string& row_key,
                       crimson::timeline::AnalysisSeriesTraceRole role,
                       bool default_visible, ScalarStore store,
                       std::optional<MaskStore> mask = std::nullopt) {
  source->descriptor.traces.push_back(
      {key, display_name, units, row_key, role, default_visible});
  FieldStore field;
  field.descriptor = source->descriptor.traces.back();
  field.scalar = std::move(store);
  field.mask = std::move(mask);
  source->fields.push_back(std::move(field));
}

void appendVec2Field(SourceStore* source, const std::string& key,
                     const std::string& display_name, const std::string& units,
                     const std::string& row_key,
                     crimson::timeline::AnalysisSeriesTraceRole role,
                     bool default_visible, const Vec2Store& store,
                     size_t column) {
  source->descriptor.traces.push_back(
      {key, display_name, units, row_key, role, default_visible});
  FieldStore field;
  field.descriptor = source->descriptor.traces.back();
  field.vec2 = store;
  field.vec2_column = column;
  source->fields.push_back(std::move(field));
}

std::vector<std::string> speedCandidates(const std::string& track_base,
                                         const std::string& level,
                                         const std::string& units) {
  return {track_base + "/movement/speed/" + level + "/" + units,
          track_base + "/speed_" + level + "_" + units};
}

}  // namespace

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenMotionSeriesTimelineRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const MotionSeriesTimelineOpenRequest& request,
    std::string* error_message,
    crimson::data::SmallSeriesPreloadPolicy preload_policy) {
  auto fail = [&](std::string message)
      -> std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository> {
    if (error_message != nullptr) {
      *error_message = std::move(message);
    }
    return nullptr;
  };
  if (!archive || !archive->impl_) {
    return fail("Archive context is unavailable");
  }
  if ((!request.scope.empty() && !validName(request.scope)) ||
      (!request.run_name.empty() && !validName(request.run_name)) ||
      request.track_id < -1) {
    return fail("Exact motion timeline source identity is invalid");
  }
  const auto& impl = *archive->impl_;
  std::vector<std::pair<std::string, std::string>> scopes;
  if (!request.run_name.empty()) {
    const std::string scope = request.scope.empty() ? "offline" : request.scope;
    scopes.emplace_back(scope, scope);
  } else {
    scopes = {{"offline", "offline"},
              {"online_refined", "online_refined"},
              {"online", "online"}};
  }
  const std::array<const char*, 4> levels = {"filtered", "smoothed", "raw",
                                             "averaged"};
  std::vector<SourceStore> sources;
  size_t resolved_frame_count = request.frame_count_hint;

  for (const auto& [scope_name, category_name] : scopes) {
    const std::string scope =
        "analysis/track_kinematics_runs/" + scope_name;
    const std::string run = request.run_name.empty()
                                ? latestRun(impl, scope)
                                : request.run_name;
    if (!validName(run)) {
      continue;
    }
    const std::string run_base = scope + "/" + run;

    if (!request.run_name.empty()) {
      const auto run_attributes =
          internal::ReadArchiveAttributes(impl, run_base);
      if (!run_attributes ||
          stringValue(*run_attributes, "schema_id") !=
              "analysis.track_kinematics_runs" ||
          integerValue(*run_attributes, "schema_version") != 1) {
        return fail("Selected motion run is not track-kinematics schema 1");
      }
    }

    auto track_ids_store = openFrameStore(impl, run_base + "/track_ids");
    std::vector<int64_t> track_ids = track_ids_store
                                         ? readAllFrames(*track_ids_store)
                                         : std::vector<int64_t>{0};
    if (track_ids.empty()) {
      track_ids.push_back(0);
    }
    for (int64_t track_id : track_ids) {
      if (track_id < 0 ||
          (request.track_id >= 0 && track_id != request.track_id)) {
        continue;
      }
      const std::string track_name = "id_" + std::to_string(track_id);
      const std::string track_base = run_base + "/tracks/" + track_name;
      auto frames = openFrameStore(
          impl, track_base + "/source_acquisition_frame_index");
      if (!frames && !request.require_source_identity) {
        frames = openFrameStore(impl, track_base + "/frame_indices");
      }
      if (!frames || rowCount(*frames) == 0) {
        continue;
      }
      if (request.require_source_identity) {
        const auto track_sample_key =
            openArray<int64_t, 2>(impl, track_base + "/track_sample_key");
        const auto source_instance_key =
            makeArraySpec(impl, track_base + "/source_instance_key");
        if (!track_sample_key ||
            track_sample_key->domain().shape()[0] !=
                static_cast<ts::Index>(rowCount(*frames)) ||
            track_sample_key->domain().shape()[1] != 2 ||
            !source_instance_key) {
          return fail("Selected motion track lacks exact row identity arrays");
        }
      }
      resolved_frame_count = std::max(
          resolved_frame_count,
          resolvedFrameCount(*frames, request.frame_count_hint));
      auto times = openScalarStore(impl, track_base + "/time_seconds");
      if (times && rowCount(*times) != rowCount(*frames)) {
        return fail("Motion frame and time arrays have different row counts");
      }
      const auto heading_mask =
          openMaskStore(impl, track_base + "/keypoint_success");
      const auto heading_raw =
          openScalarStore(impl, track_base + "/heading_degrees");
      const auto heading_smoothed =
          openScalarStore(impl, track_base + "/smoothed_heading_degrees");
      auto positions = openVec2Store(impl, track_base + "/positions_mm");
      std::string position_units = "mm";
      if (!positions) {
        positions = openVec2Store(impl, track_base + "/positions_px");
        position_units = "px";
      }
      auto frame_index = std::make_shared<FrameIndexBlocks>(*frames);

      for (const char* level_name : levels) {
        const std::string level = level_name;
        auto primary =
            openFirstScalar(impl, speedCandidates(track_base, level, "mm"));
        std::string speed_units = "mm/s";
        if (!primary) {
          primary =
              openFirstScalar(impl, speedCandidates(track_base, level, "px"));
          speed_units = "px/s";
        }
        if (!primary || rowCount(*primary) != rowCount(*frames)) {
          continue;
        }

        SourceStore source;
        source.frames = *frames;
        source.frame_index = frame_index;
        source.times = times;
        source.descriptor.key = scope_name + "/" + run + "/" +
                                track_name + "/" + level;
        source.descriptor.display_name =
            titleCase(level) + " | track " + std::to_string(track_id);
        source.descriptor.source_group = scope;
        source.descriptor.run_name = run;
        source.descriptor.category =
            "track_kinematics/" + category_name;
        source.descriptor.track_id = track_name;
        source.descriptor.variant = level;
        source.descriptor.sample_count = rowCount(*frames);
        appendScalarField(
            &source, "speed_" + level, titleCase(level) + " Speed", speed_units,
            "speed", crimson::timeline::AnalysisSeriesTraceRole::PrimarySpeed,
            true, std::move(*primary));

        if (level != "raw") {
          auto secondary =
              openFirstScalar(impl, speedCandidates(track_base, "raw", "mm"));
          std::string secondary_units = "mm/s";
          if (!secondary) {
            secondary =
                openFirstScalar(impl, speedCandidates(track_base, "raw", "px"));
            secondary_units = "px/s";
          }
          if (secondary && rowCount(*secondary) == rowCount(*frames)) {
            appendScalarField(
                &source, "speed_raw", "Raw Speed", secondary_units, "speed",
                crimson::timeline::AnalysisSeriesTraceRole::SecondarySpeed,
                false, std::move(*secondary));
          }
        }
        if (heading_raw && rowCount(*heading_raw) == rowCount(*frames)) {
          appendScalarField(
              &source, "heading_raw", "Raw Heading", "deg", "heading",
              crimson::timeline::AnalysisSeriesTraceRole::HeadingRaw, false,
              *heading_raw, heading_mask);
        }
        if (heading_smoothed &&
            rowCount(*heading_smoothed) == rowCount(*frames)) {
          appendScalarField(
              &source, "heading_smoothed", "Smoothed Heading", "deg", "heading",
              crimson::timeline::AnalysisSeriesTraceRole::HeadingSmoothed, true,
              *heading_smoothed, heading_mask);
        }
        if (positions && rowCount(*positions) == rowCount(*frames)) {
          appendVec2Field(&source, "position_x", "X Position", position_units,
                          "position",
                          crimson::timeline::AnalysisSeriesTraceRole::PositionX,
                          true, *positions, 0);
          appendVec2Field(&source, "position_y", "Y Position", position_units,
                          "position",
                          crimson::timeline::AnalysisSeriesTraceRole::PositionY,
                          true, *positions, 1);
        }
        sources.push_back(std::move(source));
      }
    }
  }

  if (sources.empty()) {
    return fail("No maintained track-kinematics timeline sources were found");
  }
  crimson::timeline::AnalysisSeriesTimelineDescriptor descriptor;
  descriptor.kind = crimson::timeline::AnalysisSeriesKind::Motion;
  descriptor.title = "Motion";
  descriptor.frame_count = resolved_frame_count;
  descriptor.default_source = sources.front().descriptor.key;
  for (const auto& source : sources) {
    descriptor.sources.push_back(source.descriptor);
  }
  if (descriptor.frame_count == 0) {
    return fail("Motion timeline camera-frame count is unavailable");
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return std::make_unique<TensorStoreRepository>(
      std::move(descriptor), std::move(sources), preload_policy);
}

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenMotionSeriesTimelineRepository(
    const std::shared_ptr<ArchiveContext>& archive, size_t frame_count_hint,
    std::string* error_message,
    crimson::data::SmallSeriesPreloadPolicy preload_policy) {
  MotionSeriesTimelineOpenRequest request;
  request.frame_count_hint = frame_count_hint;
  request.scope.clear();
  return OpenMotionSeriesTimelineRepository(archive, request, error_message,
                                            preload_policy);
}

std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
OpenTailKinematicsTimelineRepository(
    const std::shared_ptr<ArchiveContext>& archive, size_t frame_count_hint,
    const std::string& requested_run, std::string* error_message,
    crimson::data::SmallSeriesPreloadPolicy preload_policy) {
  auto fail = [&](std::string message)
      -> std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository> {
    if (error_message != nullptr) {
      *error_message = std::move(message);
    }
    return nullptr;
  };
  if (!archive || !archive->impl_) {
    return fail("Archive context is unavailable");
  }
  const auto& impl = *archive->impl_;
  const std::string group = "analysis/tail_kinematics_runs";
  const std::string run =
      requested_run.empty() ? latestRun(impl, group) : requested_run;
  if (!validName(run)) {
    return fail("No valid tail-kinematics timeline run was selected");
  }
  const std::string base = group + "/" + run;
  if (!internal::ReadArchiveAttributes(impl, base)) {
    return fail("Tail-kinematics metadata is unavailable: " + run);
  }
  auto frames = openFrameStore(impl, base + "/frame_index");
  if (!frames) {
    frames = openFrameStore(impl, base + "/row_to_frame");
  }
  if (!frames || rowCount(*frames) == 0) {
    return fail("Tail-kinematics camera-frame mapping is unavailable");
  }

  SourceStore source;
  source.frames = *frames;
  source.frame_index = std::make_shared<FrameIndexBlocks>(*frames);
  source.times = openScalarStore(impl, base + "/time_seconds");
  source.descriptor.key = run;
  source.descriptor.display_name = run;
  source.descriptor.source_group = group;
  source.descriptor.run_name = run;
  source.descriptor.category = "tail_kinematics";
  source.descriptor.variant = "scalar_traces";
  source.descriptor.sample_count = rowCount(*frames);

  auto append = [&](const char* path, const char* display, const char* units,
                    const char* row_key,
                    crimson::timeline::AnalysisSeriesTraceRole role,
                    bool visible) {
    auto values = openScalarStore(impl, base + "/" + path);
    if (values && rowCount(*values) == rowCount(*frames)) {
      appendScalarField(&source, path, display, units, row_key, role, visible,
                        std::move(*values));
    }
  };
  append("tail_tip_angle_deg", "Tail Tip Angle", "deg", "tail_angle",
         crimson::timeline::AnalysisSeriesTraceRole::TailTipAngle, true);
  append("max_abs_tail_angle_deg", "Max Abs Tail Angle", "deg", "tail_angle",
         crimson::timeline::AnalysisSeriesTraceRole::MaxAbsTailAngle, true);
  append("tail_tip_lateral_deflection_px", "Tail Tip Lateral Deflection", "px",
         "tail_deflection",
         crimson::timeline::AnalysisSeriesTraceRole::TailTipLateralDeflection,
         true);
  append("max_abs_tail_curvature_px_inv", "Max Abs Tail Curvature", "px^-1",
         "tail_curvature",
         crimson::timeline::AnalysisSeriesTraceRole::MaxAbsTailCurvature,
         false);
  if (source.fields.empty()) {
    return fail("No maintained tail-kinematics scalar traces were found");
  }

  crimson::timeline::AnalysisSeriesTimelineDescriptor descriptor;
  descriptor.kind = crimson::timeline::AnalysisSeriesKind::TailKinematics;
  descriptor.title = "Tail Kinematics";
  descriptor.default_source = run;
  descriptor.frame_count = resolvedFrameCount(*frames, frame_count_hint);
  descriptor.sources.push_back(source.descriptor);
  if (descriptor.frame_count == 0) {
    return fail("Tail-kinematics camera-frame count is unavailable");
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return std::make_unique<TensorStoreRepository>(
      std::move(descriptor), std::vector<SourceStore>{std::move(source)},
      preload_policy);
}

}  // namespace crimson::zarr
