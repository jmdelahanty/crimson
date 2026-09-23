#include "zarr/tensorstore_subject_mask_overlay_repository.h"

#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "data_access_cache.h"
#include "zarr/archive_context_internal.h"
#include "zarr/canonical_json.h"
#include "zarr/subject_mask_sampled_contour_v1_contract.h"
#include "zarr/subject_mask_v1_contract.h"
#include "zarr/zarr_metadata_equivalence.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

using IntegerStore =
    std::variant<ts::TensorStore<int64_t, 1>, ts::TensorStore<uint64_t, 1>,
                 ts::TensorStore<int32_t, 1>, ts::TensorStore<uint32_t, 1>,
                 ts::TensorStore<int16_t, 1>, ts::TensorStore<uint16_t, 1>,
                 ts::TensorStore<int8_t, 1>, ts::TensorStore<uint8_t, 1>>;
using NumericMatrixStore =
    std::variant<ts::TensorStore<double, 2>, ts::TensorStore<float, 2>,
                 ts::TensorStore<int64_t, 2>, ts::TensorStore<int32_t, 2>>;

double ElapsedMilliseconds(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - started)
      .count();
}

void AddBytes(uint64_t* total, uint64_t count, uint64_t element_size) {
  if (total == nullptr || count == 0 || element_size == 0) {
    return;
  }
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  if (count > maximum / element_size ||
      *total > maximum - count * element_size) {
    *total = maximum;
    return;
  }
  *total += count * element_size;
}

template <typename T>
uint64_t VectorCapacityBytes(const std::vector<T>& values) {
  uint64_t total = 0;
  AddBytes(&total, values.capacity(), sizeof(T));
  return total;
}

uint64_t MatrixCapacityBytes(const std::vector<std::vector<double>>& values) {
  uint64_t total = VectorCapacityBytes(values);
  for (const auto& row : values) {
    AddBytes(&total, row.capacity(), sizeof(double));
  }
  return total;
}

template <typename Array>
uint64_t ArrayPayloadBytes(const Array& array, uint64_t element_size) {
  uint64_t count = 1;
  for (const auto extent : array.shape()) {
    if (extent <= 0) {
      return 0;
    }
    const uint64_t value = static_cast<uint64_t>(extent);
    if (count > std::numeric_limits<uint64_t>::max() / value) {
      return std::numeric_limits<uint64_t>::max();
    }
    count *= value;
  }
  uint64_t total = 0;
  AddBytes(&total, count, element_size);
  return total;
}

std::optional<json> MakeArraySpec(const ArchiveContext::Impl& archive,
                                  const std::string& path) {
  return internal::MakeReadOnlyArraySpec(archive, path);
}

template <typename T, size_t Rank>
std::optional<ts::TensorStore<T, Rank>>
OpenArray(const ArchiveContext::Impl &archive, const std::string &path) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return std::nullopt;
  }
  auto store = ts::Open<T, Rank>(*spec, ts::OpenMode::open,
                                 ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return std::nullopt;
  }
  return *store;
}

std::optional<IntegerStore>
OpenIntegerStore(const ArchiveContext::Impl &archive, const std::string &path) {
  if (auto store = OpenArray<int64_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  if (auto store = OpenArray<uint64_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  if (auto store = OpenArray<int32_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  if (auto store = OpenArray<uint32_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  if (auto store = OpenArray<int16_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  if (auto store = OpenArray<uint16_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  if (auto store = OpenArray<int8_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  if (auto store = OpenArray<uint8_t, 1>(archive, path)) {
    return IntegerStore{std::move(*store)};
  }
  return std::nullopt;
}

std::optional<NumericMatrixStore>
OpenNumericMatrixStore(const ArchiveContext::Impl &archive,
                       const std::string &path, size_t minimum_columns) {
  if (auto store = OpenArray<double, 2>(archive, path);
      store &&
      store->domain().shape()[1] >= static_cast<ts::Index>(minimum_columns)) {
    return NumericMatrixStore{std::move(*store)};
  }
  if (auto store = OpenArray<float, 2>(archive, path);
      store &&
      store->domain().shape()[1] >= static_cast<ts::Index>(minimum_columns)) {
    return NumericMatrixStore{std::move(*store)};
  }
  if (auto store = OpenArray<int64_t, 2>(archive, path);
      store &&
      store->domain().shape()[1] >= static_cast<ts::Index>(minimum_columns)) {
    return NumericMatrixStore{std::move(*store)};
  }
  if (auto store = OpenArray<int32_t, 2>(archive, path);
      store &&
      store->domain().shape()[1] >= static_cast<ts::Index>(minimum_columns)) {
    return NumericMatrixStore{std::move(*store)};
  }
  return std::nullopt;
}

template <typename T, ts::DimensionIndex Rank>
auto SliceFirstDimension(const ts::TensorStore<T, Rank>& store, ts::Index start,
                         ts::Index stop) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = start;
  domain.shape()[0] = stop - start;
  return store | ts::IdentityTransform(domain);
}

size_t IntegerRowCount(const IntegerStore& store) {
  return std::visit(
      [](const auto& typed) {
        const auto rows = typed.domain().shape()[0];
        return rows > 0 ? static_cast<size_t>(rows) : 0;
      },
      store);
}

size_t NumericMatrixRowCount(const NumericMatrixStore& store) {
  return std::visit(
      [](const auto& typed) {
        const auto rows = typed.domain().shape()[0];
        return rows > 0 ? static_cast<size_t>(rows) : 0;
      },
      store);
}

size_t IntegerChunkRows(const IntegerStore& store) {
  return std::visit(
      [](const auto& typed) {
        const auto layout = typed.chunk_layout();
        if (!layout.ok()) {
          return size_t{0};
        }
        const auto shape = layout->read_chunk_shape();
        return !shape.empty() && shape[0] > 0 ? static_cast<size_t>(shape[0])
                                              : size_t{0};
      },
      store);
}

bool ReadIntegerRange(const IntegerStore& store, size_t first, size_t last,
                      std::vector<int64_t>* output) {
  if (output == nullptr || last < first || last > IntegerRowCount(store)) {
    return false;
  }
  output->assign(last - first, 0);
  return std::visit(
      [&](const auto& typed) {
        auto read =
            ts::Read(SliceFirstDimension(typed, static_cast<ts::Index>(first),
                                         static_cast<ts::Index>(last)))
                .result();
        if (!read.ok() || read->rank() != 1 ||
            static_cast<size_t>(read->shape()[0]) != last - first ||
            read->byte_strides().size() != 1) {
          return false;
        }
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        for (size_t index = 0; index < output->size(); ++index) {
          const Source value = *reinterpret_cast<const Source*>(
              origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
          if constexpr (std::is_unsigned_v<Source>) {
            if (static_cast<uint64_t>(value) >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
              return false;
            }
          }
          (*output)[index] = static_cast<int64_t>(value);
        }
        return true;
      },
      store);
}

bool ReadNumericMatrixRange(const NumericMatrixStore& store, size_t first,
                            size_t last, size_t columns,
                            std::vector<double>* output) {
  if (output == nullptr || columns == 0 || last < first ||
      last > NumericMatrixRowCount(store)) {
    return false;
  }
  return std::visit(
      [&](const auto& typed) {
        auto read =
            ts::Read(SliceFirstDimension(typed, static_cast<ts::Index>(first),
                                         static_cast<ts::Index>(last)))
                .result();
        if (!read.ok() || read->rank() != 2 ||
            static_cast<size_t>(read->shape()[0]) != last - first ||
            static_cast<size_t>(read->shape()[1]) < columns ||
            read->byte_strides().size() != 2) {
          return false;
        }
        using Source = typename std::decay_t<decltype(typed)>::Element;
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        output->resize((last - first) * columns);
        for (size_t row = 0; row < last - first; ++row) {
          for (size_t column = 0; column < columns; ++column) {
            const auto* value = reinterpret_cast<const Source*>(
                origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
                static_cast<ts::Index>(column) * read->byte_strides()[1]);
            (*output)[row * columns + column] = static_cast<double>(*value);
          }
        }
        return true;
      },
      store);
}

template <typename T> size_t ReadChunkRows(const ts::TensorStore<T, 4> &store) {
  const auto layout = store.chunk_layout();
  if (!layout.ok()) {
    return 0;
  }
  const auto shape = layout->read_chunk_shape();
  if (shape.empty() || shape[0] <= 0 ||
      static_cast<uint64_t>(shape[0]) >
          static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return 0;
  }
  return static_cast<size_t>(shape[0]);
}

template <typename Source>
bool ReadIntegerVector(const ArchiveContext::Impl& archive,
                       const std::string& path, std::vector<int64_t>* output) {
  const auto store = OpenArray<Source, 1>(archive, path);
  if (!store) {
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
bool ReadBoolVector(const ArchiveContext::Impl& archive,
                    const std::string& path, std::vector<uint8_t>* output) {
  const auto store = OpenArray<Source, 1>(archive, path);
  if (!store) {
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
  const auto store = OpenArray<Source, 2>(archive, path);
  if (!store) {
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

std::string LatestRun(const ArchiveContext::Impl& archive,
                      const std::string& group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char*, 5> keys = {
      "latest", "latest_completed", "latest_complete", "latest_success",
      "refined_subject_mask_review_status_latest"};
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

std::vector<std::string> StringList(const json& attributes, const char* key) {
  std::vector<std::string> result;
  const auto found = attributes.find(key);
  if (found == attributes.end() || !found->is_array()) {
    return result;
  }
  for (const auto& value : *found) {
    if (value.is_string()) {
      result.push_back(value.get<std::string>());
    }
  }
  return result;
}

std::string StringValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

bool BoolValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_boolean() && found->get<bool>();
}

bool ReadRoiSize(const json& attributes, double* width, double* height) {
  const auto found = attributes.find("roi_size");
  if (found == attributes.end() || !found->is_array() || found->size() < 2 ||
      !(*found)[0].is_number() || !(*found)[1].is_number()) {
    return false;
  }
  *height = (*found)[0].get<double>();
  *width = (*found)[1].get<double>();
  return std::isfinite(*width) && std::isfinite(*height) && *width > 0.0 &&
         *height > 0.0;
}

std::string SafeComponentName(const std::string& label) {
  std::string result;
  result.reserve(label.size());
  for (const unsigned char value : label) {
    if (std::isalnum(value) || value == '_' || value == '-' || value == '.') {
      result.push_back(static_cast<char>(value));
    } else {
      result.push_back('_');
    }
  }
  return result.empty() ? "component" : result;
}

std::string RleComponentName(size_t index, const std::string& label,
                             bool padded) {
  std::ostringstream output;
  if (padded) {
    output << std::setw(2) << std::setfill('0') << index;
  } else {
    output << index;
  }
  output << '_' << SafeComponentName(label);
  return output.str();
}

struct RowMetadata {
  size_t mask_row = 0;
  int64_t camera_frame = -1;
  uint64_t instance_key = 0;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
};

struct SubjectMappingPage {
  size_t first_row = 0;
  std::vector<int64_t> frames;
  std::vector<int64_t> detections;
  std::vector<int64_t> source_crop_rows;
};

struct CropMappingPage {
  size_t first_row = 0;
  std::vector<int64_t> frames;
  std::vector<int64_t> detections;
  std::vector<double> offsets_xy;
};

uint64_t SubjectMappingPageBytes(const SubjectMappingPage& page) {
  return sizeof(page) + VectorCapacityBytes(page.frames) +
         VectorCapacityBytes(page.detections) +
         VectorCapacityBytes(page.source_crop_rows);
}

uint64_t CropMappingPageBytes(const CropMappingPage& page) {
  return sizeof(page) + VectorCapacityBytes(page.frames) +
         VectorCapacityBytes(page.detections) +
         VectorCapacityBytes(page.offsets_xy);
}

struct FrameCountIndex {
  static constexpr size_t kBlockFrames = 4096;

  std::vector<uint32_t> counts;
  std::vector<uint64_t> block_row_offsets;

  std::optional<std::pair<size_t, size_t>> rowsForFrame(int64_t frame) const {
    if (frame < 0 || static_cast<uint64_t>(frame) >= counts.size()) {
      return std::nullopt;
    }
    const size_t target = static_cast<size_t>(frame);
    const size_t block = target / kBlockFrames;
    if (block >= block_row_offsets.size()) {
      return std::nullopt;
    }
    uint64_t first = block_row_offsets[block];
    const size_t block_first = block * kBlockFrames;
    for (size_t index = block_first; index < target; ++index) {
      first += counts[index];
    }
    const uint64_t count = counts[target];
    if (first > std::numeric_limits<size_t>::max() ||
        count >
            std::numeric_limits<size_t>::max() - static_cast<size_t>(first)) {
      return std::nullopt;
    }
    return std::pair<size_t, size_t>{static_cast<size_t>(first),
                                     static_cast<size_t>(count)};
  }
};

struct FallbackFrameRow {
  int64_t frame = -1;
  size_t row = 0;
};

struct LazyMappingSource {
  LazyMappingSource(IntegerStore subject_frames_value,
                    std::optional<IntegerStore> subject_detections_value,
                    std::optional<IntegerStore> subject_crop_rows_value,
                    IntegerStore frame_counts_value,
                    IntegerStore crop_frames_value,
                    std::optional<IntegerStore> crop_detections_value,
                    NumericMatrixStore crop_offsets_value,
                    size_t subject_page_rows_value, size_t crop_page_rows_value,
                    double roi_width_value, double roi_height_value)
      : subject_frames(std::move(subject_frames_value)),
        subject_detections(std::move(subject_detections_value)),
        subject_crop_rows(std::move(subject_crop_rows_value)),
        frame_counts(std::move(frame_counts_value)),
        crop_frames(std::move(crop_frames_value)),
        crop_detections(std::move(crop_detections_value)),
        crop_offsets(std::move(crop_offsets_value)),
        subject_page_rows(std::max<size_t>(1, subject_page_rows_value)),
        crop_page_rows(std::max<size_t>(1, crop_page_rows_value)),
        roi_width(roi_width_value), roi_height(roi_height_value),
        subject_pages({8ULL * 1024ULL * 1024ULL, 0, 128}),
        crop_pages({16ULL * 1024ULL * 1024ULL, 0, 32}) {}

  IntegerStore subject_frames;
  std::optional<IntegerStore> subject_detections;
  std::optional<IntegerStore> subject_crop_rows;
  IntegerStore frame_counts;
  IntegerStore crop_frames;
  std::optional<IntegerStore> crop_detections;
  NumericMatrixStore crop_offsets;
  size_t subject_page_rows = 1;
  size_t crop_page_rows = 1;
  double roi_width = 0.0;
  double roi_height = 0.0;
  mutable std::mutex mutex;
  mutable bool index_initialized = false;
  mutable bool index_failed = false;
  mutable std::string index_error;
  mutable FrameCountIndex index;
  mutable bool use_fallback_frame_index = false;
  mutable bool fallback_frame_index_failed = false;
  mutable std::string fallback_frame_index_error;
  mutable std::vector<FallbackFrameRow> fallback_frame_rows;
  mutable crimson::data::ByteBudgetLruCache<
      size_t, std::shared_ptr<const SubjectMappingPage>>
      subject_pages;
  mutable crimson::data::ByteBudgetLruCache<
      size_t, std::shared_ptr<const CropMappingPage>>
      crop_pages;
};

enum class ContourKind : uint8_t { None, Sampled, Ragged };

struct ContourSource {
  ContourKind kind = ContourKind::None;
  bool sampled_valid_is_byte = false;
  ts::TensorStore<bool, 1> sampled_valid_bool;
  ts::TensorStore<uint8_t, 1> sampled_valid_byte;
  ts::TensorStore<float, 3> sampled_points;
  size_t sampled_point_count = 0;
  std::vector<int64_t> ragged_ptr;
  std::vector<int64_t> ragged_len;
  ts::TensorStore<float, 2> ragged_points;
};

using SampledPointReadFuture =
    decltype(ts::Read(std::declval<ts::TensorStore<float, 3>>()));
using SampledBoolReadFuture =
    decltype(ts::Read(std::declval<ts::TensorStore<bool, 1>>()));
using SampledByteReadFuture =
    decltype(ts::Read(std::declval<ts::TensorStore<uint8_t, 1>>()));

struct PendingSampledContourRead {
  const ContourSource* source = nullptr;
  size_t channel = 0;
  SampledPointReadFuture points;
  SampledBoolReadFuture valid_bool;
  SampledByteReadFuture valid_byte;
};

struct RleSource {
  bool available = false;
  ts::TensorStore<uint32_t, 1> counts;
  std::vector<int64_t> indptr;
  std::vector<uint8_t> present;
};

bool ReadSampledContour(const ContourSource& source, size_t row,
                        std::vector<SubjectMaskOverlayPoint>* output) {
  bool contour_valid = false;
  if (source.sampled_valid_is_byte) {
    auto valid_slice = SliceFirstDimension(source.sampled_valid_byte,
                                           static_cast<ts::Index>(row),
                                           static_cast<ts::Index>(row + 1));
    auto valid = ts::Read(valid_slice).result();
    if (!valid.ok() || valid->rank() != 1 || valid->shape()[0] != 1) {
      return false;
    }
    contour_valid = *static_cast<const uint8_t *>(
                        valid->byte_strided_origin_pointer()) != 0;
  } else {
    auto valid_slice = SliceFirstDimension(source.sampled_valid_bool,
                                           static_cast<ts::Index>(row),
                                           static_cast<ts::Index>(row + 1));
    auto valid = ts::Read(valid_slice).result();
    if (!valid.ok() || valid->rank() != 1 || valid->shape()[0] != 1) {
      return false;
    }
    contour_valid =
        *static_cast<const bool*>(valid->byte_strided_origin_pointer());
  }
  auto points_slice =
      SliceFirstDimension(source.sampled_points, static_cast<ts::Index>(row),
                          static_cast<ts::Index>(row + 1));
  auto points = ts::Read(points_slice).result();
  if (!points.ok() || points->rank() != 3 || points->shape()[0] != 1 ||
      points->shape()[1] < 2 || points->shape()[2] < 2) {
    return false;
  }
  if (!contour_valid) {
    return true;
  }
  const auto strides = points->byte_strides();
  if (strides.size() != 3) {
    return false;
  }
  const auto* origin = reinterpret_cast<const uint8_t*>(
      points->byte_strided_origin_pointer().get());
  const size_t count = static_cast<size_t>(points->shape()[1]);
  output->reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto* point = origin + static_cast<ts::Index>(index) * strides[1];
    const float x = *reinterpret_cast<const float*>(point);
    const float y = *reinterpret_cast<const float*>(point + strides[2]);
    if (std::isfinite(x) && std::isfinite(y)) {
      output->push_back({x, y});
    }
  }
  return true;
}

bool ReadRaggedContour(const ContourSource& source, size_t row,
                       std::vector<SubjectMaskOverlayPoint>* output) {
  if (row >= source.ragged_ptr.size() || row >= source.ragged_len.size() ||
      source.ragged_ptr[row] < 0 || source.ragged_len[row] <= 1) {
    return true;
  }
  const int64_t start = source.ragged_ptr[row];
  if (source.ragged_len[row] > std::numeric_limits<int64_t>::max() - start) {
    return false;
  }
  const int64_t stop = start + source.ragged_len[row];
  if (stop > source.ragged_points.domain().shape()[0]) {
    return false;
  }
  auto slice =
      SliceFirstDimension(source.ragged_points, static_cast<ts::Index>(start),
                          static_cast<ts::Index>(stop));
  auto points = ts::Read(slice).result();
  if (!points.ok() || points->rank() != 2 || points->shape()[1] < 2) {
    return false;
  }
  const auto strides = points->byte_strides();
  if (strides.size() != 2) {
    return false;
  }
  const auto* origin = reinterpret_cast<const uint8_t*>(
      points->byte_strided_origin_pointer().get());
  const size_t count = static_cast<size_t>(points->shape()[0]);
  output->reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto* point = origin + static_cast<ts::Index>(index) * strides[0];
    const float x = *reinterpret_cast<const float*>(point);
    const float y = *reinterpret_cast<const float*>(point + strides[1]);
    if (std::isfinite(x) && std::isfinite(y)) {
      output->push_back({x, y});
    }
  }
  return true;
}

struct CachedMaskComponent {
  std::vector<uint32_t> foreground_indices;
  std::vector<SubjectMaskOverlayPoint> contour;
};

struct CachedMaskRow {
  std::vector<CachedMaskComponent> components;
};

struct CachedMaskChunk {
  size_t chunk_id = 0;
  size_t first_row = 0;
  bool contour_read_failed = false;
  std::vector<CachedMaskRow> rows;
  uint64_t source_bytes_read = 0;
  uint64_t contour_source_bytes_read = 0;
  uint64_t dense_mask_payload_reads = 0;
  uint64_t contour_payload_reads = 0;
  uint64_t retained_bytes = 0;
  double read_ms = 0.0;
  double convert_ms = 0.0;
  double contour_load_ms = 0.0;
};

uint64_t CachedChunkRetainedBytes(const CachedMaskChunk& chunk) {
  uint64_t total = sizeof(chunk);
  AddBytes(&total, chunk.rows.capacity(), sizeof(CachedMaskRow));
  for (const auto& row : chunk.rows) {
    AddBytes(&total, row.components.capacity(), sizeof(CachedMaskComponent));
    for (const auto& component : row.components) {
      AddBytes(&total, component.foreground_indices.capacity(),
               sizeof(uint32_t));
      AddBytes(&total, component.contour.capacity(),
               sizeof(SubjectMaskOverlayPoint));
    }
  }
  return total;
}

class TensorStoreSubjectMaskOverlayRepository final
    : public SubjectMaskOverlayRepository {
 public:
  TensorStoreSubjectMaskOverlayRepository(
      SubjectMaskOverlayDescriptor descriptor, std::vector<RowMetadata> rows,
      std::vector<int64_t> frame_row_offsets,
      std::unique_ptr<LazyMappingSource> lazy_mapping,
      std::vector<uint8_t> available_channels,
      ts::TensorStore<uint8_t, 4> dense, ts::TensorStore<uint8_t, 4> bitpacked,
      std::vector<RleSource> rle, std::vector<ContourSource> contours,
      bool read_mask_pixels,
      SubjectMaskOverlayRepositoryMetrics opening_metrics,
      std::chrono::steady_clock::time_point open_started,
      SubjectMaskOverlayOpenOptions limits = {})
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)),
        frame_row_offsets_(std::move(frame_row_offsets)),
        lazy_mapping_(std::move(lazy_mapping)),
        available_channels_(std::move(available_channels)),
        dense_(std::move(dense)), bitpacked_(std::move(bitpacked)),
        rle_(std::move(rle)), contours_(std::move(contours)),
        read_mask_pixels_(read_mask_pixels),
        limits_(std::move(limits)),
        metrics_(std::move(opening_metrics)) {
    if (lazy_mapping_) {
      metrics_.lazy_mapping = true;
    } else if (frame_row_offsets_.empty()) {
      const auto index_started = std::chrono::steady_clock::now();
      for (size_t index = 0; index < rows_.size(); ++index) {
        rows_by_frame_[rows_[index].camera_frame].push_back(index);
      }
      metrics_.metadata_index_ms += ElapsedMilliseconds(index_started);
    }
    metrics_.metadata_retained_bytes += VectorCapacityBytes(rows_);
    metrics_.metadata_retained_bytes += VectorCapacityBytes(frame_row_offsets_);
    metrics_.metadata_retained_bytes +=
        VectorCapacityBytes(available_channels_);
    if (!lazy_mapping_ && frame_row_offsets_.empty()) {
      for (const auto& entry : rows_by_frame_) {
        AddBytes(&metrics_.metadata_retained_bytes, 1, sizeof(entry));
        metrics_.metadata_retained_bytes += VectorCapacityBytes(entry.second);
      }
    }
    for (const auto& source : rle_) {
      metrics_.metadata_retained_bytes += VectorCapacityBytes(source.indptr);
      metrics_.metadata_retained_bytes += VectorCapacityBytes(source.present);
    }
    for (const auto& source : contours_) {
      metrics_.metadata_retained_bytes +=
          VectorCapacityBytes(source.ragged_ptr);
      metrics_.metadata_retained_bytes +=
          VectorCapacityBytes(source.ragged_len);
    }
    if (!read_mask_pixels_) {
      chunk_rows_ = 0;
      for (const auto &source : contours_) {
        if (source.kind != ContourKind::Sampled) {
          continue;
        }
        const auto layout = source.sampled_points.chunk_layout();
        if (!layout.ok()) {
          continue;
        }
        const auto shape = layout->read_chunk_shape();
        if (!shape.empty() && shape[0] > 0) {
          const size_t rows = static_cast<size_t>(shape[0]);
          chunk_rows_ = chunk_rows_ == 0 ? rows : std::min(chunk_rows_, rows);
        }
      }
      cache_capacity_ = 8;
    } else if (descriptor_.storage == SubjectMaskStorage::Dense) {
      chunk_rows_ = ReadChunkRows(dense_);
      cache_capacity_ = 3;
    } else if (descriptor_.storage == SubjectMaskStorage::Bitpacked) {
      chunk_rows_ = ReadChunkRows(bitpacked_);
      cache_capacity_ = 8;
    } else {
      chunk_rows_ = 32;
      cache_capacity_ = 8;
    }
    if (chunk_rows_ == 0) {
      chunk_rows_ = 1;
    }
    descriptor_.storage_chunk_rows = chunk_rows_;
    if (limits_.max_read_rows != 0) {
      chunk_rows_ = std::min(chunk_rows_, limits_.max_read_rows);
    }
    chunk_count_ = descriptor_.row_count == 0
                       ? 0
                       : 1 + (descriptor_.row_count - 1) / chunk_rows_;
    if (!limits_.disable_prefetch) {
      prefetch_worker_ = std::thread([this] { runPrefetch(); });
    }
    metrics_.open_total_ms = ElapsedMilliseconds(open_started);
  }

  ~TensorStoreSubjectMaskOverlayRepository() override { stopPrefetch(); }

  const SubjectMaskOverlayDescriptor& descriptor() const override {
    return descriptor_;
  }

  SubjectMaskOverlayRepositoryMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto result = metrics_;
    result.cached_chunks = cache_.size();
    return result;
  }

  SubjectMaskOverlayResolution resolveCameraFrame(
      int64_t camera_frame, int full_frame_width,
      int full_frame_height) const override {
    std::unique_lock<std::mutex> interactive_lock(interactive_mutex_, std::defer_lock);
    if (limits_.serial_dense_channels) interactive_lock.lock();
    SubjectMaskOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
      result.status = SubjectMaskOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = SubjectMaskOverlayStatus::InvalidDimensions;
      result.error = "Full-frame dimensions are invalid";
      return result;
    }
    std::vector<RowMetadata> metadata_rows;
    if (lazy_mapping_) {
      std::string mapping_error;
      if (!resolveLazyRows(camera_frame, &metadata_rows, &mapping_error)) {
        result.status = SubjectMaskOverlayStatus::ReadFailed;
        result.error = std::move(mapping_error);
        return result;
      }
    } else if (!frame_row_offsets_.empty()) {
      const size_t frame = static_cast<size_t>(camera_frame);
      const size_t first = static_cast<size_t>(frame_row_offsets_[frame]);
      const size_t last = static_cast<size_t>(frame_row_offsets_[frame + 1]);
      if (limits_.max_observations_per_frame &&
          last - first > limits_.max_observations_per_frame) {
        result.status = SubjectMaskOverlayStatus::ReadFailed;
        result.error = "Subject-mask frame exceeds observation admission budget";
        return result;
      }
      metadata_rows.insert(metadata_rows.end(), rows_.begin() + first,
                           rows_.begin() + last);
    } else {
      const auto found = rows_by_frame_.find(camera_frame);
      if (found != rows_by_frame_.end()) {
        metadata_rows.reserve(found->second.size());
        for (const size_t row : found->second) {
          metadata_rows.push_back(rows_[row]);
        }
      }
    }
    if (metadata_rows.empty()) {
      result.status = SubjectMaskOverlayStatus::Missing;
      return result;
    }
    if (limits_.max_observations_per_frame &&
        metadata_rows.size() > limits_.max_observations_per_frame) {
      result.status = SubjectMaskOverlayStatus::ReadFailed;
      result.error = "Subject-mask frame exceeds observation admission budget";
      return result;
    }

    const size_t mask_pixels = descriptor_.mask_width * descriptor_.mask_height;
    result.status = SubjectMaskOverlayStatus::Mapped;
    result.detections.reserve(metadata_rows.size());
    for (const auto& metadata : metadata_rows) {
      const size_t chunk_id = metadata.mask_row / chunk_rows_;
      std::string read_error;
      const auto chunk = ensureChunk(chunk_id, false, &read_error);
      if (!chunk || metadata.mask_row < chunk->first_row ||
          metadata.mask_row - chunk->first_row >= chunk->rows.size()) {
        result.status = SubjectMaskOverlayStatus::ReadFailed;
        result.error =
            read_error.empty()
                ? "Subject-mask chunk did not contain its requested row"
                : std::move(read_error);
        result.detections.clear();
        return result;
      }
      queueAdjacentChunks(chunk_id);

      SubjectMaskOverlayDetection detection;
      detection.instance_key = metadata.instance_key;
      detection.detection_index = metadata.detection_index;
      detection.source_crop_row_id = metadata.source_crop_row_id;
      detection.roi_x = metadata.roi_x;
      detection.roi_y = metadata.roi_y;
      detection.roi_width = metadata.roi_width;
      detection.roi_height = metadata.roi_height;
      const auto& cached_row =
          chunk->rows[metadata.mask_row - chunk->first_row];
      if (chunk->contour_read_failed) {
        result.error = "Sampled contour cache payload read failed; mask fills remain available";
      }
      detection.components.resize(descriptor_.component_labels.size());
      for (size_t channel = 0; channel < detection.components.size();
           ++channel) {
        auto& component = detection.components[channel];
        component.label = descriptor_.component_labels[channel];
        component.channel_index = channel;
        component.mask_width = descriptor_.mask_width;
        component.mask_height = descriptor_.mask_height;
        if (channel >= cached_row.components.size()) {
          continue;
        }
        const auto& cached_component = cached_row.components[channel];
        component.present = !cached_component.foreground_indices.empty();
        if (component.present) {
          auto mask = std::make_shared<std::vector<uint8_t>>(mask_pixels, 0);
          for (const uint32_t index : cached_component.foreground_indices) {
            if (index >= mask_pixels) {
              result.status = SubjectMaskOverlayStatus::ReadFailed;
              result.error = "Cached mask index exceeds mask dimensions";
              result.detections.clear();
              return result;
            }
            (*mask)[index] = 255;
          }
          component.mask = std::move(mask);
        }
        component.contour = cached_component.contour;
        for (auto& point : component.contour) {
          point.x = metadata.roi_x +
                    point.x * metadata.roi_width / descriptor_.mask_width;
          point.y = metadata.roi_y +
                    point.y * metadata.roi_height / descriptor_.mask_height;
        }
      }
      result.detections.push_back(std::move(detection));
    }
    return result;
  }

 private:
  bool initializeLazyIndexLocked(std::string* error) const {
    if (lazy_mapping_->index_initialized) {
      return true;
    }
    if (lazy_mapping_->index_failed) {
      if (error) {
        *error = lazy_mapping_->index_error;
      }
      return false;
    }
    const auto started = std::chrono::steady_clock::now();
    std::vector<int64_t> source_counts;
    const bool read = ReadIntegerRange(
        lazy_mapping_->frame_counts, 0,
        IntegerRowCount(lazy_mapping_->frame_counts), &source_counts);
    FrameCountIndex candidate;
    uint64_t total_rows = 0;
    if (read) {
      candidate.counts.reserve(source_counts.size());
      candidate.block_row_offsets.reserve(
          (source_counts.size() + FrameCountIndex::kBlockFrames - 1) /
          FrameCountIndex::kBlockFrames);
      for (size_t frame = 0; frame < source_counts.size(); ++frame) {
        if (frame % FrameCountIndex::kBlockFrames == 0) {
          candidate.block_row_offsets.push_back(total_rows);
        }
        const int64_t count = source_counts[frame];
        if (count < 0 ||
            static_cast<uint64_t>(count) >
                std::numeric_limits<uint32_t>::max() ||
            static_cast<uint64_t>(count) >
                std::numeric_limits<uint64_t>::max() - total_rows) {
          lazy_mapping_->index_error =
              "Subject-mask frame_counts contains an invalid count";
          break;
        }
        candidate.counts.push_back(static_cast<uint32_t>(count));
        total_rows += static_cast<uint64_t>(count);
      }
    }
    if (!read || candidate.counts.size() != source_counts.size() ||
        total_rows != descriptor_.row_count) {
      if (lazy_mapping_->index_error.empty()) {
        lazy_mapping_->index_error =
            !read
                ? "Subject-mask frame_counts are unreadable"
                : "Subject-mask frame_counts do not sum to the mask row count";
      }
      lazy_mapping_->index_failed = true;
    } else {
      lazy_mapping_->index = std::move(candidate);
      lazy_mapping_->index_initialized = true;
    }
    const double elapsed_ms = ElapsedMilliseconds(started);
    const uint64_t source_bytes = VectorCapacityBytes(source_counts);
    const uint64_t retained_bytes =
        VectorCapacityBytes(lazy_mapping_->index.counts) +
        VectorCapacityBytes(lazy_mapping_->index.block_row_offsets);
    {
      std::lock_guard<std::mutex> metrics_lock(cache_mutex_);
      metrics_.frame_index_initialize_ms += elapsed_ms;
      metrics_.frame_index_rows_read += source_counts.size();
      metrics_.frame_index_source_bytes += source_bytes;
      metrics_.frame_index_retained_bytes = retained_bytes;
      metrics_.metadata_decoded_bytes += source_bytes;
      metrics_.metadata_retained_bytes += retained_bytes;
      if (lazy_mapping_->index_failed) {
        ++metrics_.mapping_initialize_failures;
      }
    }
    if (lazy_mapping_->index_failed && error) {
      *error = lazy_mapping_->index_error;
    }
    return lazy_mapping_->index_initialized;
  }

  bool initializeFallbackFrameIndexLocked(std::string* error) const {
    if (lazy_mapping_->use_fallback_frame_index) {
      return true;
    }
    if (lazy_mapping_->fallback_frame_index_failed) {
      if (error) {
        *error = lazy_mapping_->fallback_frame_index_error;
      }
      return false;
    }
    constexpr uint64_t kMaximumFallbackIndexBytes = 64ULL * 1024ULL * 1024ULL;
    if (descriptor_.row_count >
        kMaximumFallbackIndexBytes / sizeof(FallbackFrameRow)) {
      lazy_mapping_->fallback_frame_index_error =
          "Unordered subject-mask rows exceed the 64 MiB compatibility "
          "frame-index budget";
      lazy_mapping_->fallback_frame_index_failed = true;
    }

    const auto started = std::chrono::steady_clock::now();
    std::vector<int64_t> source_frames;
    if (!lazy_mapping_->fallback_frame_index_failed &&
        !ReadIntegerRange(lazy_mapping_->subject_frames, 0,
                          descriptor_.row_count, &source_frames)) {
      lazy_mapping_->fallback_frame_index_error =
          "Subject-mask frame_indices are unreadable for compatibility "
          "indexing";
      lazy_mapping_->fallback_frame_index_failed = true;
    }
    if (!lazy_mapping_->fallback_frame_index_failed) {
      lazy_mapping_->fallback_frame_rows.reserve(source_frames.size());
      for (size_t row = 0; row < source_frames.size(); ++row) {
        if (source_frames[row] >= 0) {
          lazy_mapping_->fallback_frame_rows.push_back(
              {source_frames[row], row});
        }
      }
      std::sort(
          lazy_mapping_->fallback_frame_rows.begin(),
          lazy_mapping_->fallback_frame_rows.end(),
          [](const FallbackFrameRow& left, const FallbackFrameRow& right) {
            return left.frame < right.frame ||
                   (left.frame == right.frame && left.row < right.row);
          });
      lazy_mapping_->use_fallback_frame_index = true;
    }
    const uint64_t source_bytes = VectorCapacityBytes(source_frames);
    const uint64_t retained_bytes =
        VectorCapacityBytes(lazy_mapping_->fallback_frame_rows);
    {
      std::lock_guard<std::mutex> metrics_lock(cache_mutex_);
      metrics_.frame_index_initialize_ms += ElapsedMilliseconds(started);
      metrics_.frame_index_rows_read += source_frames.size();
      metrics_.frame_index_source_bytes += source_bytes;
      metrics_.frame_index_retained_bytes += retained_bytes;
      metrics_.metadata_decoded_bytes += source_bytes;
      metrics_.metadata_retained_bytes += retained_bytes;
      if (lazy_mapping_->use_fallback_frame_index) {
        ++metrics_.fallback_frame_index_builds;
        metrics_.fallback_frame_index_rows +=
            lazy_mapping_->fallback_frame_rows.size();
      } else {
        ++metrics_.mapping_initialize_failures;
      }
    }
    if (lazy_mapping_->fallback_frame_index_failed && error) {
      *error = lazy_mapping_->fallback_frame_index_error;
    }
    return lazy_mapping_->use_fallback_frame_index;
  }

  bool lazyRowsForFrameLocked(int64_t camera_frame,
                              std::vector<size_t>* row_indices,
                              std::string* error) const {
    row_indices->clear();
    if (lazy_mapping_->use_fallback_frame_index) {
      const auto first = std::lower_bound(
          lazy_mapping_->fallback_frame_rows.begin(),
          lazy_mapping_->fallback_frame_rows.end(), camera_frame,
          [](const FallbackFrameRow& entry, int64_t frame) {
            return entry.frame < frame;
          });
      const auto last = std::upper_bound(
          first, lazy_mapping_->fallback_frame_rows.end(), camera_frame,
          [](int64_t frame, const FallbackFrameRow& entry) {
            return frame < entry.frame;
          });
      row_indices->reserve(static_cast<size_t>(last - first));
      for (auto entry = first; entry != last; ++entry) {
        row_indices->push_back(entry->row);
      }
      return true;
    }
    const auto range = lazy_mapping_->index.rowsForFrame(camera_frame);
    if (!range) {
      if (error) {
        *error = "Subject-mask frame_counts index is out of range";
      }
      return false;
    }
    row_indices->reserve(range->second);
    for (size_t row = range->first; row < range->first + range->second; ++row) {
      row_indices->push_back(row);
    }
    return true;
  }

  void updateMappingCacheMetricsLocked(uint64_t source_bytes,
                                       uint64_t subject_bytes,
                                       uint64_t crop_bytes, size_t evictions,
                                       double elapsed_ms) const {
    const auto& subject_metrics = lazy_mapping_->subject_pages.metrics();
    const auto& crop_metrics = lazy_mapping_->crop_pages.metrics();
    std::lock_guard<std::mutex> metrics_lock(cache_mutex_);
    ++metrics_.mapping_page_reads;
    metrics_.mapping_page_source_bytes += source_bytes;
    metrics_.subject_mapping_bytes += subject_bytes;
    metrics_.crop_mapping_bytes += crop_bytes;
    metrics_.mapping_page_evictions += evictions;
    metrics_.cached_mapping_bytes =
        subject_metrics.current_cpu_bytes + crop_metrics.current_cpu_bytes;
    metrics_.peak_cached_mapping_bytes =
        std::max(metrics_.peak_cached_mapping_bytes,
                 subject_metrics.peak_cpu_bytes + crop_metrics.peak_cpu_bytes);
    metrics_.maximum_mapping_page_read_ms =
        std::max(metrics_.maximum_mapping_page_read_ms, elapsed_ms);
  }

  std::shared_ptr<const SubjectMappingPage>
  subjectMappingPageLocked(size_t row, std::string *error) const {
    const size_t page_id = row / lazy_mapping_->subject_page_rows;
    if (auto cached = lazy_mapping_->subject_pages.findAndTouch(page_id)) {
      std::lock_guard<std::mutex> metrics_lock(cache_mutex_);
      ++metrics_.mapping_page_cache_hits;
      return *cached;
    }
    const size_t first = page_id * lazy_mapping_->subject_page_rows;
    const size_t last = std::min(descriptor_.row_count,
                                 first + lazy_mapping_->subject_page_rows);
    const auto started = std::chrono::steady_clock::now();
    auto page = std::make_shared<SubjectMappingPage>();
    page->first_row = first;
    if (!ReadIntegerRange(lazy_mapping_->subject_frames, first, last,
                          &page->frames)) {
      if (error) {
        *error = "Subject-mask frame mapping page is unreadable";
      }
      return nullptr;
    }
    if (lazy_mapping_->subject_detections) {
      if (!ReadIntegerRange(*lazy_mapping_->subject_detections, first, last,
                            &page->detections)) {
        if (error) {
          *error = "Subject-mask detection mapping page is unreadable";
        }
        return nullptr;
      }
    } else {
      page->detections.assign(last - first, -1);
    }
    if (lazy_mapping_->subject_crop_rows) {
      if (!ReadIntegerRange(*lazy_mapping_->subject_crop_rows, first, last,
                            &page->source_crop_rows)) {
        if (error) {
          *error = "Subject-mask crop-row mapping page is unreadable";
        }
        return nullptr;
      }
    } else {
      page->source_crop_rows.assign(last - first, -1);
    }
    const uint64_t bytes = SubjectMappingPageBytes(*page);
    auto admitted = lazy_mapping_->subject_pages.put(
        page_id, page, {bytes, 0}, crimson::data::RequestPriority::CurrentFrame,
        false);
    if (!admitted.admitted()) {
      if (error) {
        *error = "Subject-mask mapping page exceeds its cache budget";
      }
      return nullptr;
    }
    updateMappingCacheMetricsLocked(bytes, bytes, 0,
                                    admitted.evicted_keys.size(),
                                    ElapsedMilliseconds(started));
    return page;
  }

  std::shared_ptr<const CropMappingPage>
  cropMappingPageLocked(size_t row, std::string *error) const {
    const size_t page_id = row / lazy_mapping_->crop_page_rows;
    if (auto cached = lazy_mapping_->crop_pages.findAndTouch(page_id)) {
      std::lock_guard<std::mutex> metrics_lock(cache_mutex_);
      ++metrics_.mapping_page_cache_hits;
      return *cached;
    }
    const size_t crop_rows = IntegerRowCount(lazy_mapping_->crop_frames);
    const size_t first = page_id * lazy_mapping_->crop_page_rows;
    const size_t last =
        std::min(crop_rows, first + lazy_mapping_->crop_page_rows);
    const auto started = std::chrono::steady_clock::now();
    auto page = std::make_shared<CropMappingPage>();
    page->first_row = first;
    if (!ReadIntegerRange(lazy_mapping_->crop_frames, first, last,
                          &page->frames) ||
        !ReadNumericMatrixRange(lazy_mapping_->crop_offsets, first, last, 2,
                                &page->offsets_xy)) {
      if (error) {
        *error = "Source crop placement page is unreadable";
      }
      return nullptr;
    }
    if (lazy_mapping_->crop_detections) {
      if (!ReadIntegerRange(*lazy_mapping_->crop_detections, first, last,
                            &page->detections)) {
        if (error) {
          *error = "Source crop detection page is unreadable";
        }
        return nullptr;
      }
    } else {
      page->detections.assign(last - first, -1);
    }
    const uint64_t bytes = CropMappingPageBytes(*page);
    auto admitted = lazy_mapping_->crop_pages.put(
        page_id, page, {bytes, 0}, crimson::data::RequestPriority::CurrentFrame,
        false);
    if (!admitted.admitted()) {
      if (error) {
        *error = "Source crop mapping page exceeds its cache budget";
      }
      return nullptr;
    }
    updateMappingCacheMetricsLocked(bytes, 0, bytes,
                                    admitted.evicted_keys.size(),
                                    ElapsedMilliseconds(started));
    return page;
  }

  bool resolveLazyRows(int64_t camera_frame, std::vector<RowMetadata>* rows,
                       std::string* error) const {
    std::lock_guard<std::mutex> lock(lazy_mapping_->mutex);
    if (!initializeLazyIndexLocked(error)) {
      return false;
    }
    std::vector<size_t> row_indices;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!lazyRowsForFrameLocked(camera_frame, &row_indices, error)) {
        return false;
      }
      rows->clear();
      rows->reserve(row_indices.size());
      bool requires_fallback = false;
      for (const size_t row : row_indices) {
        const auto subject_page = subjectMappingPageLocked(row, error);
        if (!subject_page || row < subject_page->first_row ||
            row - subject_page->first_row >= subject_page->frames.size()) {
          return false;
        }
        const size_t subject_offset = row - subject_page->first_row;
        if (subject_page->frames[subject_offset] != camera_frame) {
          requires_fallback = !lazy_mapping_->use_fallback_frame_index;
          if (!requires_fallback) {
            if (error) {
              *error =
                  "Subject-mask compatibility frame index returned a "
                  "mismatched row";
            }
            return false;
          }
          break;
        }
        const int64_t source_crop_row =
            subject_page->source_crop_rows[subject_offset] >= 0
                ? subject_page->source_crop_rows[subject_offset]
                : static_cast<int64_t>(row);
        if (source_crop_row < 0 ||
            static_cast<uint64_t>(source_crop_row) >=
                IntegerRowCount(lazy_mapping_->crop_frames)) {
          if (error) {
            *error =
                "Subject-mask source_crop_row_ids contains an out-of-range row";
          }
          return false;
        }
        const size_t crop_row = static_cast<size_t>(source_crop_row);
        const auto crop_page = cropMappingPageLocked(crop_row, error);
        if (!crop_page || crop_row < crop_page->first_row ||
            crop_row - crop_page->first_row >= crop_page->frames.size()) {
          return false;
        }
        const size_t crop_offset = crop_row - crop_page->first_row;
        if (crop_page->frames[crop_offset] != camera_frame) {
          if (error) {
            *error =
                "Subject-mask source crop row does not match its camera "
                "frame";
          }
          return false;
        }
        const int64_t subject_detection =
            subject_page->detections[subject_offset];
        const int64_t crop_detection = crop_page->detections[crop_offset];
        if (subject_detection >= 0 && crop_detection >= 0 &&
            subject_detection != crop_detection) {
          if (error) {
            *error =
                "Subject-mask source crop row does not match its detection "
                "index";
          }
          return false;
        }
        const double x = crop_page->offsets_xy[crop_offset * 2];
        const double y = crop_page->offsets_xy[crop_offset * 2 + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) {
          continue;
        }
        rows->push_back(
            {row, camera_frame, 0,
             subject_detection >= 0 ? subject_detection : crop_detection,
             source_crop_row, x, y, lazy_mapping_->roi_width,
             lazy_mapping_->roi_height});
      }
      if (!requires_fallback) {
        return true;
      }
      if (!initializeFallbackFrameIndexLocked(error)) {
        return false;
      }
    }
    if (error) {
      *error = "Subject-mask frame mapping could not settle";
    }
    return false;
  }

  std::shared_ptr<const CachedMaskChunk>
  cachedChunkLocked(size_t chunk_id, bool count_hit) const {
    const auto found =
        std::find_if(cache_.begin(), cache_.end(), [&](const auto &entry) {
          return entry->chunk_id == chunk_id;
        });
    if (found == cache_.end()) {
      return nullptr;
    }
    auto result = *found;
    if (std::next(found) != cache_.end()) {
      cache_.erase(found);
      cache_.push_back(result);
    }
    if (count_hit) {
      ++metrics_.chunk_cache_hits;
    }
    return result;
  }

  std::shared_ptr<const CachedMaskChunk>
  ensureChunk(size_t chunk_id, bool prefetched, std::string *error) const {
    {
      std::unique_lock<std::mutex> lock(cache_mutex_);
      while (true) {
        if (auto cached = cachedChunkLocked(chunk_id, true)) {
          return cached;
        }
        if (stopping_) {
          if (error) {
            *error = "Subject-mask chunk cache is stopping";
          }
          return nullptr;
        }
        if (loading_chunks_.insert(chunk_id).second) {
          break;
        }
        cache_condition_.wait(lock, [&] {
          return stopping_ || loading_chunks_.count(chunk_id) == 0;
        });
      }
    }

    const auto started = std::chrono::steady_clock::now();
    std::string load_error;
    auto loaded = loadChunk(chunk_id, &load_error);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      loading_chunks_.erase(chunk_id);
      metrics_.maximum_chunk_load_ms =
          std::max(metrics_.maximum_chunk_load_ms, elapsed_ms);
      if (!loaded) {
        ++metrics_.chunk_load_failures;
      } else {
        if (prefetched) {
          ++metrics_.prefetched_chunk_loads;
        } else {
          ++metrics_.demand_chunk_loads;
        }
        metrics_.chunk_source_bytes_read += loaded->source_bytes_read;
        metrics_.contour_source_bytes_read += loaded->contour_source_bytes_read;
        metrics_.dense_mask_payload_reads += loaded->dense_mask_payload_reads;
        metrics_.contour_payload_reads += loaded->contour_payload_reads;
        metrics_.chunk_retained_bytes_produced += loaded->retained_bytes;
        metrics_.chunk_read_ms += loaded->read_ms;
        metrics_.chunk_convert_ms += loaded->convert_ms;
        metrics_.contour_load_ms += loaded->contour_load_ms;
        metrics_.maximum_chunk_read_ms =
            std::max(metrics_.maximum_chunk_read_ms, loaded->read_ms);
        metrics_.maximum_chunk_convert_ms =
            std::max(metrics_.maximum_chunk_convert_ms, loaded->convert_ms);
        metrics_.maximum_contour_load_ms =
            std::max(metrics_.maximum_contour_load_ms, loaded->contour_load_ms);
        cache_.push_back(loaded);
        metrics_.cached_payload_bytes += loaded->retained_bytes;
        while (cache_.size() > cache_capacity_ ||
               (limits_.max_cached_payload_bytes != 0 &&
                metrics_.cached_payload_bytes > limits_.max_cached_payload_bytes)) {
          metrics_.cached_payload_bytes -= std::min(
              metrics_.cached_payload_bytes, cache_.front()->retained_bytes);
          metrics_.evicted_payload_bytes += cache_.front()->retained_bytes;
          cache_.pop_front();
          ++metrics_.chunk_evictions;
        }
        metrics_.peak_cached_payload_bytes = std::max(
            metrics_.peak_cached_payload_bytes, metrics_.cached_payload_bytes);
        metrics_.peak_cached_chunks =
            std::max(metrics_.peak_cached_chunks, cache_.size());
      }
      cache_condition_.notify_all();
    }
    if (!loaded && error) {
      *error = std::move(load_error);
    }
    return loaded;
  }

  std::shared_ptr<CachedMaskChunk> loadChunk(size_t chunk_id,
                                             std::string* error) const {
    if (chunk_id >= chunk_count_) {
      *error = "Subject-mask chunk is out of range";
      return nullptr;
    }
    const size_t first = chunk_id * chunk_rows_;
    const size_t last = std::min(first + chunk_rows_, descriptor_.row_count);
    uint64_t worst_case_indices = 0;
    AddBytes(&worst_case_indices, last - first,
             descriptor_.component_labels.size() * descriptor_.mask_width *
                 descriptor_.mask_height * sizeof(uint32_t) * 2);
    if (limits_.max_cached_payload_bytes && read_mask_pixels_ &&
        worst_case_indices > limits_.max_cached_payload_bytes) {
      *error = "Subject-mask decode exceeds interactive admission budget";
      return nullptr;
    }
    auto chunk = std::make_shared<CachedMaskChunk>();
    chunk->chunk_id = chunk_id;
    chunk->first_row = first;
    chunk->rows.resize(last - first);
    for (auto& row : chunk->rows) {
      row.components.resize(descriptor_.component_labels.size());
    }

    bool loaded = !read_mask_pixels_;
    const auto mask_started = std::chrono::steady_clock::now();
    if (read_mask_pixels_) {
      switch (descriptor_.storage) {
      case SubjectMaskStorage::Dense:
        loaded = loadDenseChunk(first, last, chunk.get(), error);
        break;
      case SubjectMaskStorage::Bitpacked:
        loaded = loadBitpackedChunk(first, last, chunk.get(), error);
        break;
      case SubjectMaskStorage::Rle:
        loaded = loadRleChunk(first, last, chunk.get(), error);
        break;
      }
    }
    if (!loaded) {
      return nullptr;
    }
    chunk->convert_ms =
        std::max(0.0, ElapsedMilliseconds(mask_started) - chunk->read_ms);
    const auto contour_started = std::chrono::steady_clock::now();
    const bool contours_loaded = loadContourChunk(first, last, chunk.get());
    chunk->contour_load_ms = ElapsedMilliseconds(contour_started);
    if (!contours_loaded && !read_mask_pixels_) {
      if (error) {
        *error = "Required sampled-contour cache payload read failed";
      }
      return nullptr;
    }
    if (!contours_loaded) {
      chunk->contour_read_failed = true;
      for (auto& row : chunk->rows) {
        for (auto& component : row.components) component.contour.clear();
      }
    }
    chunk->retained_bytes = CachedChunkRetainedBytes(*chunk);
    if (limits_.max_cached_payload_bytes != 0 &&
        chunk->retained_bytes > limits_.max_cached_payload_bytes) {
      *error = "Subject-mask payload exceeds interactive byte budget";
      return nullptr;
    }
    return chunk;
  }

  bool loadDenseChannels(size_t first, size_t last, CachedMaskChunk* chunk,
                         std::string* error) const {
    // A narrow logical read still decodes physical Zarr chunks. Read channels
    // serially so the four large August channel chunks are not requested
    // concurrently by this repository. TensorStore's cache is separate.
    for (size_t channel = 0; channel < descriptor_.component_labels.size(); ++channel) {
      if (channel >= available_channels_.size() || !available_channels_[channel]) continue;
      ts::Box<4> domain(dense_.domain().box());
      domain.origin()[0] = static_cast<ts::Index>(first);
      domain.shape()[0] = static_cast<ts::Index>(last - first);
      domain.origin()[1] = static_cast<ts::Index>(channel);
      domain.shape()[1] = 1;
      const auto started = std::chrono::steady_clock::now();
      auto read = ts::Read(dense_ | ts::IdentityTransform(domain)).result();
      ++chunk->dense_mask_payload_reads;
      chunk->read_ms += ElapsedMilliseconds(started);
      if (!read.ok() || read->rank() != 4 ||
          read->shape()[0] != static_cast<ts::Index>(last - first) ||
          read->shape()[1] != 1 ||
          read->shape()[2] != static_cast<ts::Index>(descriptor_.mask_height) ||
          read->shape()[3] != static_cast<ts::Index>(descriptor_.mask_width)) {
        *error = read.ok() ? "Dense mask channel has an unexpected shape" : read.status().ToString();
        return false;
      }
      chunk->source_bytes_read += ArrayPayloadBytes(*read, sizeof(uint8_t));
      const auto strides = read->byte_strides();
      const auto* origin = reinterpret_cast<const uint8_t*>(read->byte_strided_origin_pointer().get());
      for (size_t row = 0; row < last - first; ++row) {
        auto& indices = chunk->rows[row].components[channel].foreground_indices;
        indices.reserve(256);
        for (size_t y = 0; y < descriptor_.mask_height; ++y) {
          for (size_t x = 0; x < descriptor_.mask_width; ++x) {
            if (*(origin + row * strides[0] + y * strides[2] + x * strides[3])) {
              indices.push_back(static_cast<uint32_t>(y * descriptor_.mask_width + x));
            }
          }
        }
      }
    }
    return true;
  }

  bool loadDenseChunk(size_t first, size_t last, CachedMaskChunk* chunk,
                      std::string* error) const {
    if (limits_.serial_dense_channels) {
      return loadDenseChannels(first, last, chunk, error);
    }
    const auto read_started = std::chrono::steady_clock::now();
    auto read =
        ts::Read(SliceFirstDimension(dense_, static_cast<ts::Index>(first),
                                     static_cast<ts::Index>(last)))
            .result();
    ++chunk->dense_mask_payload_reads;
    chunk->read_ms += ElapsedMilliseconds(read_started);
    if (!read.ok() || read->rank() != 4 ||
        static_cast<size_t>(read->shape()[0]) != last - first ||
        static_cast<size_t>(read->shape()[1]) <
            descriptor_.component_labels.size()) {
      *error = read.ok() ? "Dense mask chunk has an unexpected shape"
                         : read.status().ToString();
      return false;
    }
    chunk->source_bytes_read += ArrayPayloadBytes(*read, sizeof(uint8_t));
    const auto strides = read->byte_strides();
    if (strides.size() != 4) {
      *error = "Dense mask chunk has an unexpected stride rank";
      return false;
    }
    const auto* origin = reinterpret_cast<const uint8_t*>(
        read->byte_strided_origin_pointer().get());
    for (size_t local_row = 0; local_row < chunk->rows.size(); ++local_row) {
      for (size_t channel = 0; channel < descriptor_.component_labels.size();
           ++channel) {
        if (channel >= available_channels_.size() ||
            available_channels_[channel] == 0) {
          continue;
        }
        auto& indices =
            chunk->rows[local_row].components[channel].foreground_indices;
        indices.reserve(256);
        for (size_t y = 0; y < descriptor_.mask_height; ++y) {
          for (size_t x = 0; x < descriptor_.mask_width; ++x) {
            const auto* value = origin +
                                static_cast<ts::Index>(local_row) * strides[0] +
                                static_cast<ts::Index>(channel) * strides[1] +
                                static_cast<ts::Index>(y) * strides[2] +
                                static_cast<ts::Index>(x) * strides[3];
            if (*value != 0) {
              indices.push_back(
                  static_cast<uint32_t>(y * descriptor_.mask_width + x));
            }
          }
        }
      }
    }
    return true;
  }

  bool loadBitpackedChunk(size_t first, size_t last, CachedMaskChunk* chunk,
                          std::string* error) const {
    const auto read_started = std::chrono::steady_clock::now();
    auto read =
        ts::Read(SliceFirstDimension(bitpacked_, static_cast<ts::Index>(first),
                                     static_cast<ts::Index>(last)))
            .result();
    chunk->read_ms += ElapsedMilliseconds(read_started);
    if (!read.ok() || read->rank() != 4 ||
        static_cast<size_t>(read->shape()[0]) != last - first ||
        static_cast<size_t>(read->shape()[1]) <
            descriptor_.component_labels.size() ||
        static_cast<size_t>(read->shape()[2]) < descriptor_.mask_height ||
        static_cast<size_t>(read->shape()[3]) <
            (descriptor_.mask_width + 7) / 8) {
      *error = read.ok() ? "Bitpacked mask chunk has an unexpected shape"
                         : read.status().ToString();
      return false;
    }
    chunk->source_bytes_read += ArrayPayloadBytes(*read, sizeof(uint8_t));
    const auto strides = read->byte_strides();
    if (strides.size() != 4) {
      *error = "Bitpacked mask chunk has an unexpected stride rank";
      return false;
    }
    const auto* origin = reinterpret_cast<const uint8_t*>(
        read->byte_strided_origin_pointer().get());
    for (size_t local_row = 0; local_row < chunk->rows.size(); ++local_row) {
      for (size_t channel = 0; channel < descriptor_.component_labels.size();
           ++channel) {
        if (channel >= available_channels_.size() ||
            available_channels_[channel] == 0) {
          continue;
        }
        auto& indices =
            chunk->rows[local_row].components[channel].foreground_indices;
        indices.reserve(256);
        for (size_t y = 0; y < descriptor_.mask_height; ++y) {
          for (size_t x = 0; x < descriptor_.mask_width; ++x) {
            const auto* value = origin +
                                static_cast<ts::Index>(local_row) * strides[0] +
                                static_cast<ts::Index>(channel) * strides[1] +
                                static_cast<ts::Index>(y) * strides[2] +
                                static_cast<ts::Index>(x / 8) * strides[3];
            if (((*value >> (x % 8)) & 1U) != 0) {
              indices.push_back(
                  static_cast<uint32_t>(y * descriptor_.mask_width + x));
            }
          }
        }
      }
    }
    return true;
  }

  bool loadRleChunk(size_t first, size_t last, CachedMaskChunk* chunk,
                    std::string* error) const {
    const size_t total = descriptor_.mask_width * descriptor_.mask_height;
    for (size_t channel = 0; channel < descriptor_.component_labels.size();
         ++channel) {
      if (channel >= available_channels_.size() ||
          available_channels_[channel] == 0 || channel >= rle_.size() ||
          !rle_[channel].available) {
        continue;
      }
      const auto& source = rle_[channel];
      if (last >= source.indptr.size() || last > source.present.size() ||
          source.indptr[first] < 0 ||
          source.indptr[last] < source.indptr[first] ||
          source.indptr[last] > source.counts.domain().shape()[0]) {
        *error = "RLE chunk row pointers are invalid";
        return false;
      }
      const int64_t count_first = source.indptr[first];
      const int64_t count_last = source.indptr[last];
      std::vector<uint32_t> counts;
      if (count_last > count_first) {
        const auto read_started = std::chrono::steady_clock::now();
        auto read =
            ts::Read(SliceFirstDimension(source.counts,
                                         static_cast<ts::Index>(count_first),
                                         static_cast<ts::Index>(count_last)))
                .result();
        chunk->read_ms += ElapsedMilliseconds(read_started);
        if (!read.ok() || read->rank() != 1) {
          *error = read.ok() ? "RLE count chunk has an unexpected shape"
                             : read.status().ToString();
          return false;
        }
        chunk->source_bytes_read += ArrayPayloadBytes(*read, sizeof(uint32_t));
        const auto strides = read->byte_strides();
        if (strides.size() != 1) {
          *error = "RLE count chunk has an unexpected stride rank";
          return false;
        }
        const auto* origin = reinterpret_cast<const uint8_t*>(
            read->byte_strided_origin_pointer().get());
        counts.resize(static_cast<size_t>(count_last - count_first));
        for (size_t index = 0; index < counts.size(); ++index) {
          counts[index] = *reinterpret_cast<const uint32_t*>(
              origin + static_cast<ts::Index>(index) * strides[0]);
        }
      }
      for (size_t row = first; row < last; ++row) {
        if (source.present[row] == 0) {
          continue;
        }
        if (source.indptr[row] < count_first ||
            source.indptr[row + 1] < source.indptr[row] ||
            source.indptr[row + 1] > count_last) {
          *error = "RLE row pointer is invalid";
          return false;
        }
        auto& indices =
            chunk->rows[row - first].components[channel].foreground_indices;
        indices.reserve(256);
        size_t offset = 0;
        bool foreground = false;
        for (int64_t count_index = source.indptr[row];
             count_index < source.indptr[row + 1]; ++count_index) {
          const auto local = static_cast<size_t>(count_index - count_first);
          if (local >= counts.size()) {
            *error = "RLE count index exceeds the loaded chunk";
            return false;
          }
          const size_t count = counts[local];
          if (count > total - offset) {
            *error = "RLE count sum exceeds the mask dimensions";
            return false;
          }
          if (foreground) {
            for (size_t flat = offset; flat < offset + count; ++flat) {
              const size_t y = flat % descriptor_.mask_height;
              const size_t x = flat / descriptor_.mask_height;
              indices.push_back(
                  static_cast<uint32_t>(y * descriptor_.mask_width + x));
            }
          }
          offset += count;
          foreground = !foreground;
        }
        if (offset != total) {
          *error = "RLE count sum does not match the mask dimensions";
          return false;
        }
      }
    }
    return true;
  }

  bool loadContourChunk(size_t first, size_t last,
                        CachedMaskChunk* chunk) const {
    bool loaded = true;
    std::vector<PendingSampledContourRead> pending_sampled;
    pending_sampled.reserve(contours_.size());
    for (size_t channel = 0; channel < descriptor_.component_labels.size() &&
                             channel < contours_.size();
         ++channel) {
      const auto& source = contours_[channel];
      if (source.kind == ContourKind::Sampled) {
        PendingSampledContourRead pending;
        pending.source = &source;
        pending.channel = channel;
        pending.points = ts::Read(SliceFirstDimension(
            source.sampled_points, static_cast<ts::Index>(first),
            static_cast<ts::Index>(last)));
        pending.points.Force();
        if (source.sampled_valid_is_byte) {
          pending.valid_byte = ts::Read(SliceFirstDimension(
              source.sampled_valid_byte, static_cast<ts::Index>(first),
              static_cast<ts::Index>(last)));
          pending.valid_byte.Force();
        } else {
          pending.valid_bool = ts::Read(SliceFirstDimension(
              source.sampled_valid_bool, static_cast<ts::Index>(first),
              static_cast<ts::Index>(last)));
          pending.valid_bool.Force();
        }
        pending_sampled.push_back(std::move(pending));
      } else if (source.kind == ContourKind::Ragged) {
        loadRaggedContourChunk(source, first, last, channel, chunk);
      }
    }
    for (const auto& pending : pending_sampled) {
      loaded = loadSampledContourChunk(
                   *pending.source, first, last, pending.channel,
                   pending.points, pending.valid_bool, pending.valid_byte,
                   chunk) &&
               loaded;
    }
    return loaded;
  }

  bool loadSampledContourChunk(const ContourSource& source, size_t first,
                               size_t last, size_t channel,
                               const SampledPointReadFuture& points_future,
                               const SampledBoolReadFuture& valid_bool_future,
                               const SampledByteReadFuture& valid_byte_future,
                               CachedMaskChunk* chunk) const {
    const auto& points = points_future.result();
    ++chunk->contour_payload_reads;
    if (!points.ok() || points->rank() != 3 ||
        static_cast<size_t>(points->shape()[0]) != last - first ||
        points->shape()[1] < 2 || points->shape()[2] < 2) {
      return false;
    }
    const uint64_t point_bytes = ArrayPayloadBytes(*points, sizeof(float));
    chunk->source_bytes_read += point_bytes;
    chunk->contour_source_bytes_read += point_bytes;
    const auto point_strides = points->byte_strides();
    if (point_strides.size() != 3) {
      return false;
    }
    const auto* point_origin = reinterpret_cast<const uint8_t*>(
        points->byte_strided_origin_pointer().get());

    std::vector<uint8_t> valid(last - first, 0);
    if (source.sampled_valid_is_byte) {
      const auto& values = valid_byte_future.result();
      ++chunk->contour_payload_reads;
      if (!values.ok() || values->rank() != 1 ||
          static_cast<size_t>(values->shape()[0]) != valid.size()) {
        return false;
      }
      const uint64_t valid_bytes = ArrayPayloadBytes(*values, sizeof(uint8_t));
      chunk->source_bytes_read += valid_bytes;
      chunk->contour_source_bytes_read += valid_bytes;
      const auto strides = values->byte_strides();
      const auto* origin = reinterpret_cast<const uint8_t*>(
          values->byte_strided_origin_pointer().get());
      for (size_t index = 0; index < valid.size(); ++index) {
        valid[index] = *(origin + static_cast<ts::Index>(index) * strides[0]);
      }
    } else {
      const auto& values = valid_bool_future.result();
      ++chunk->contour_payload_reads;
      if (!values.ok() || values->rank() != 1 ||
          static_cast<size_t>(values->shape()[0]) != valid.size()) {
        return false;
      }
      const uint64_t valid_bytes = ArrayPayloadBytes(*values, sizeof(bool));
      chunk->source_bytes_read += valid_bytes;
      chunk->contour_source_bytes_read += valid_bytes;
      const auto strides = values->byte_strides();
      const auto* origin = reinterpret_cast<const uint8_t*>(
          values->byte_strided_origin_pointer().get());
      for (size_t index = 0; index < valid.size(); ++index) {
        valid[index] = *reinterpret_cast<const bool*>(
                           origin + static_cast<ts::Index>(index) * strides[0])
                           ? 1
                           : 0;
      }
    }
    const size_t point_count = static_cast<size_t>(points->shape()[1]);
    if (descriptor_.strict_v1 && point_count != source.sampled_point_count) {
      return false;
    }
    for (size_t local_row = 0; local_row < valid.size(); ++local_row) {
      if (valid[local_row] == 0) {
        continue;
      }
      auto& contour = chunk->rows[local_row].components[channel].contour;
      contour.reserve(point_count);
      for (size_t point_index = 0; point_index < point_count; ++point_index) {
        const auto* point =
            point_origin +
            static_cast<ts::Index>(local_row) * point_strides[0] +
            static_cast<ts::Index>(point_index) * point_strides[1];
        const float x = *reinterpret_cast<const float*>(point);
        const float y =
            *reinterpret_cast<const float*>(point + point_strides[2]);
        if (descriptor_.strict_v1 &&
            (!std::isfinite(x) || !std::isfinite(y))) {
          contour.clear();
          return false;
        }
        if (std::isfinite(x) && std::isfinite(y)) {
          contour.push_back({x, y});
        }
      }
    }
    return true;
  }

  void loadRaggedContourChunk(const ContourSource& source, size_t first,
                              size_t last, size_t channel,
                              CachedMaskChunk* chunk) const {
    int64_t point_first = std::numeric_limits<int64_t>::max();
    int64_t point_last = 0;
    for (size_t row = first; row < last; ++row) {
      if (row >= source.ragged_ptr.size() || row >= source.ragged_len.size() ||
          source.ragged_ptr[row] < 0 || source.ragged_len[row] <= 1 ||
          source.ragged_len[row] >
              std::numeric_limits<int64_t>::max() - source.ragged_ptr[row]) {
        continue;
      }
      point_first = std::min(point_first, source.ragged_ptr[row]);
      point_last =
          std::max(point_last, source.ragged_ptr[row] + source.ragged_len[row]);
    }
    if (point_first == std::numeric_limits<int64_t>::max() ||
        point_last <= point_first ||
        point_last > source.ragged_points.domain().shape()[0]) {
      return;
    }
    auto points =
        ts::Read(SliceFirstDimension(source.ragged_points,
                                     static_cast<ts::Index>(point_first),
                                     static_cast<ts::Index>(point_last)))
            .result();
    if (!points.ok() || points->rank() != 2 || points->shape()[1] < 2) {
      return;
    }
    chunk->source_bytes_read += ArrayPayloadBytes(*points, sizeof(float));
    const auto strides = points->byte_strides();
    if (strides.size() != 2) {
      return;
    }
    const auto* origin = reinterpret_cast<const uint8_t*>(
        points->byte_strided_origin_pointer().get());
    for (size_t row = first; row < last; ++row) {
      if (row >= source.ragged_ptr.size() || row >= source.ragged_len.size() ||
          source.ragged_ptr[row] < 0 || source.ragged_len[row] <= 1) {
        continue;
      }
      const int64_t start = source.ragged_ptr[row];
      const int64_t length = source.ragged_len[row];
      if (start < point_first || start + length > point_last) {
        continue;
      }
      auto& contour = chunk->rows[row - first].components[channel].contour;
      contour.reserve(static_cast<size_t>(length));
      for (int64_t index = 0; index < length; ++index) {
        const auto* point =
            origin +
            static_cast<ts::Index>(start + index - point_first) * strides[0];
        const float x = *reinterpret_cast<const float*>(point);
        const float y = *reinterpret_cast<const float*>(point + strides[1]);
        if (std::isfinite(x) && std::isfinite(y)) {
          contour.push_back({x, y});
        }
      }
    }
  }

  void queueAdjacentChunks(size_t chunk_id) const {
    bool forward = true;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      if (last_demand_chunk_) {
        if (chunk_id > *last_demand_chunk_) {
          prefetch_forward_ = true;
        } else if (chunk_id < *last_demand_chunk_) {
          prefetch_forward_ = false;
        }
      }
      last_demand_chunk_ = chunk_id;
      forward = prefetch_forward_;
    }
    for (size_t ahead = 1; ahead <= 2; ++ahead) {
      if (forward) {
        if (chunk_id > std::numeric_limits<size_t>::max() - ahead) {
          break;
        }
        queuePrefetch(chunk_id + ahead);
      } else {
        if (chunk_id < ahead) {
          break;
        }
        queuePrefetch(chunk_id - ahead);
      }
    }
  }

  void queuePrefetch(size_t chunk_id) const {
    if (limits_.disable_prefetch) return;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (stopping_ || chunk_id >= chunk_count_ ||
        loading_chunks_.count(chunk_id) != 0 ||
        queued_chunks_.count(chunk_id) != 0 ||
        cachedChunkLocked(chunk_id, false)) {
      return;
    }
    while (prefetch_queue_.size() >= 8) {
      queued_chunks_.erase(prefetch_queue_.front());
      prefetch_queue_.pop_front();
    }
    prefetch_queue_.push_back(chunk_id);
    queued_chunks_.insert(chunk_id);
    ++metrics_.prefetch_requests;
    cache_condition_.notify_all();
  }

  void runPrefetch() const {
    while (true) {
      size_t chunk_id = 0;
      {
        std::unique_lock<std::mutex> lock(cache_mutex_);
        cache_condition_.wait(
            lock, [&] { return stopping_ || !prefetch_queue_.empty(); });
        if (stopping_) {
          return;
        }
        chunk_id = prefetch_queue_.front();
        prefetch_queue_.pop_front();
        queued_chunks_.erase(chunk_id);
      }
      std::string ignored_error;
      ensureChunk(chunk_id, true, &ignored_error);
    }
  }

  void stopPrefetch() {
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      stopping_ = true;
      prefetch_queue_.clear();
      queued_chunks_.clear();
      cache_condition_.notify_all();
    }
    if (prefetch_worker_.joinable()) {
      prefetch_worker_.join();
    }
  }

  SubjectMaskOverlayDescriptor descriptor_;
  std::vector<RowMetadata> rows_;
  std::vector<int64_t> frame_row_offsets_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
  std::unique_ptr<LazyMappingSource> lazy_mapping_;
  std::vector<uint8_t> available_channels_;
  ts::TensorStore<uint8_t, 4> dense_;
  ts::TensorStore<uint8_t, 4> bitpacked_;
  std::vector<RleSource> rle_;
  std::vector<ContourSource> contours_;
  bool read_mask_pixels_ = true;
  SubjectMaskOverlayOpenOptions limits_;
  mutable std::mutex interactive_mutex_;
  size_t chunk_rows_ = 1;
  size_t chunk_count_ = 0;
  size_t cache_capacity_ = 3;
  mutable std::mutex cache_mutex_;
  mutable std::condition_variable cache_condition_;
  mutable std::deque<std::shared_ptr<const CachedMaskChunk>> cache_;
  mutable std::unordered_set<size_t> loading_chunks_;
  mutable std::deque<size_t> prefetch_queue_;
  mutable std::unordered_set<size_t> queued_chunks_;
  mutable SubjectMaskOverlayRepositoryMetrics metrics_;
  mutable std::optional<size_t> last_demand_chunk_;
  mutable bool prefetch_forward_ = true;
  mutable bool stopping_ = false;
  std::thread prefetch_worker_;
};

ContourSource OpenContour(const ArchiveContext::Impl& archive,
                          const std::string& run_base, const std::string& label,
                          size_t row_count, bool stale) {
  ContourSource source;
  if (stale) {
    return source;
  }
  const std::string component = run_base + "/components/" + label;
  const std::string sampled = component + "/sampled_contours";
  auto valid_bool = OpenArray<bool, 1>(archive, sampled + "/valid");
  auto valid_byte = valid_bool
                        ? std::optional<ts::TensorStore<uint8_t, 1>>()
                        : OpenArray<uint8_t, 1>(archive, sampled + "/valid");
  auto points = OpenArray<float, 3>(archive, sampled + "/points_xy");
  const bool valid_shape =
      (valid_bool && valid_bool->domain().shape()[0] == row_count) ||
      (valid_byte && valid_byte->domain().shape()[0] == row_count);
  if (valid_shape && points && points->domain().shape()[0] == row_count &&
      points->domain().shape()[1] > 1 && points->domain().shape()[2] >= 2) {
    source.kind = ContourKind::Sampled;
    if (valid_bool) {
      source.sampled_valid_bool = std::move(*valid_bool);
    } else {
      source.sampled_valid_is_byte = true;
      source.sampled_valid_byte = std::move(*valid_byte);
    }
    source.sampled_points = std::move(*points);
    source.sampled_point_count =
        static_cast<size_t>(source.sampled_points.domain().shape()[1]);
    return source;
  }

  std::vector<int64_t> ptr;
  std::vector<int64_t> len;
  auto ragged_points =
      OpenArray<float, 2>(archive, component + "/contours/points_xy");
  if (ReadIntegers(archive, component + "/contours/ptr", &ptr) &&
      ReadIntegers(archive, component + "/contours/len", &len) &&
      ptr.size() == row_count && len.size() == row_count && ragged_points &&
      ragged_points->domain().shape()[1] >= 2) {
    source.kind = ContourKind::Ragged;
    source.ragged_ptr = std::move(ptr);
    source.ragged_len = std::move(len);
    source.ragged_points = std::move(*ragged_points);
  }
  return source;
}

const json* ConsolidatedEntry(const json& root, const std::string& path) {
  try {
    const auto& metadata = root.at("consolidated_metadata").at("metadata");
    const auto found = metadata.find(path);
    return found == metadata.end() ? nullptr : &*found;
  } catch (const json::exception&) {
    return nullptr;
  }
}

bool NormalizeStrictGroup(json* metadata, const std::string& digest_scope,
                          bool redact_run_manifest) {
  if (!metadata || !metadata->is_object() ||
      metadata->value("node_type", "") != "group") {
    return false;
  }
  const auto consolidation = metadata->find("consolidated_metadata");
  if (consolidation != metadata->end()) {
    const bool empty =
        consolidation->is_null() ||
        (consolidation->is_object() && consolidation->size() == 3 &&
         consolidation->value("kind", "") == "inline" &&
         consolidation->value("must_understand", true) == false &&
         consolidation->contains("metadata") &&
         consolidation->at("metadata").is_object() &&
         consolidation->at("metadata").empty());
    if (!empty) {
      return false;
    }
    metadata->erase(consolidation);
  }
  if (redact_run_manifest) {
    auto attributes = metadata->find("attributes");
    if (attributes == metadata->end() || !attributes->is_object()) {
      return false;
    }
    attributes->erase("run_manifest");
    if (digest_scope ==
        "exact_run_group_and_array_declarations_redacting_manifest_lifecycle_"
        "and_transport_publication_attrs") {
      constexpr std::array<std::string_view, 9> kRedactedAttributes = {
          "status",
          "palette_run_completion_status",
          "palette_run_completed_at_utc",
          "atomic_publication_owner_uuid",
          "atomic_publication_tombstone",
          "cluster_output_staging",
          "publication_status",
          "subject_mask_bundle_selector_eligible",
          "run_manifest",
      };
      for (const auto name : kRedactedAttributes) {
        attributes->erase(std::string(name));
      }
    }
  }
  return true;
}

template <typename T, ts::DimensionIndex Rank>
bool OpenExactArray(const ArchiveContext::Impl& archive, const json& root,
                    const std::string& path,
                    ts::TensorStore<T, Rank>* output,
                    std::string* error_message) {
  auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  const auto* metadata = ConsolidatedEntry(root, path);
  if (!spec || !metadata) {
    internal::SetArchiveError(error_message,
                              "Missing strict subject-mask array: " + path);
    return false;
  }
  (*spec)["metadata"] = *metadata;
  auto opened =
      ts::Open<T, Rank>(*spec,
                        ts::OpenMode::open | ts::OpenMode::assume_metadata,
                        ts::ReadWriteMode::read, archive.context)
          .result();
  if (!opened.ok()) {
    internal::SetArchiveError(error_message,
                              path + ": " + opened.status().ToString());
    return false;
  }
  *output = std::move(*opened);
  return true;
}

bool NormalizeSampledContourCacheGroup(json *metadata, bool run_group) {
  if (!NormalizeStrictGroup(metadata, {}, false)) {
    return false;
  }
  if (!run_group) {
    return true;
  }
  auto attributes = metadata->find("attributes");
  if (attributes == metadata->end() || !attributes->is_object()) {
    return false;
  }
  constexpr std::array<std::string_view, 11> kRedacted = {
      "run_manifest",
      "status",
      "palette_run_completion_status",
      "palette_run_completed_at_utc",
      "palette_run_failed_at_utc",
      "palette_run_error",
      "atomic_publication_owner_uuid",
      "atomic_publication_tombstone",
      "cluster_output_staging",
      "publication_status",
      "subject_mask_bundle_selector_eligible"};
  for (const auto key : kRedacted) {
    attributes->erase(std::string(key));
  }
  return true;
}

bool ValidateSampledContourArrayPhysicalMetadata(
    const json &metadata,
    const SubjectMaskSampledContourV1ArrayDeclaration &declaration) {
  if (metadata.value("zarr_format", 0) != 3 ||
      metadata.value("node_type", "") != "array" ||
      metadata.contains("consolidated_metadata") ||
      metadata.value("data_type", "") != declaration.dtype ||
      !metadata.contains("shape") ||
      metadata.at("shape").get<std::vector<size_t>>() != declaration.shape ||
      !metadata.contains("codecs") || !metadata.at("codecs").is_array() ||
      metadata.at("codecs").size() != 1 ||
      metadata.at("codecs")[0].value("name", "") != "sharding_indexed") {
    return false;
  }
  const auto &sharding = metadata.at("codecs")[0].at("configuration");
  const size_t element_bytes = declaration.dtype == "int32" ? 4 : 1;
  const size_t row_bytes = declaration.field == "points_xy"
                               ? declaration.sample_count * 2 * sizeof(float)
                               : element_bytes;
  const size_t inner_rows = 131072 / row_bytes;
  const size_t maximum_outer_rows = 8388608 / row_bytes;
  const size_t logical_rows = declaration.shape.front();
  const size_t inner_chunks = (logical_rows + inner_rows - 1) / inner_rows;
  const size_t chunks_per_shard = std::min(
      inner_chunks, std::max<size_t>(1, maximum_outer_rows / inner_rows));
  const size_t outer_rows = inner_rows * chunks_per_shard;
  std::vector<size_t> expected_inner = declaration.shape;
  std::vector<size_t> expected_outer = declaration.shape;
  expected_inner[0] = inner_rows;
  expected_outer[0] = outer_rows;
  if (!metadata.contains("chunk_grid") ||
      metadata.at("chunk_grid").value("name", "") != "regular" ||
      metadata.at("chunk_grid")
              .at("configuration")
              .at("chunk_shape")
              .get<std::vector<size_t>>() != expected_outer ||
      sharding.at("chunk_shape").get<std::vector<size_t>>() != expected_inner ||
      sharding.value("index_location", "") != "end" ||
      !sharding.contains("codecs") || sharding.at("codecs").size() != 2 ||
      sharding.at("codecs")[0].value("name", "") != "bytes" ||
      sharding.at("codecs")[1].value("name", "") != "zstd" ||
      sharding.at("codecs")[1].at("configuration").value("level", -1) != 0 ||
      sharding.at("codecs")[1].at("configuration").value("checksum", true) ||
      !sharding.contains("index_codecs") ||
      sharding.at("index_codecs").size() != 2 ||
      sharding.at("index_codecs")[0].value("name", "") != "bytes" ||
      sharding.at("index_codecs")[0].at("configuration").value("endian", "") !=
          "little" ||
      sharding.at("index_codecs")[1].value("name", "") != "crc32c") {
    return false;
  }
  const auto &attributes = metadata.at("attributes");
  return attributes.value("benchmark_only", false) &&
         !attributes.value("selector_eligible", true) &&
         attributes.value("artifact_class", "") ==
             "subject_mask_derived_presentation_cache" &&
         attributes.value("authority", "") == "derived_from_dense_masks_roi" &&
         !attributes.value("authoritative_pixels", true) &&
         attributes.value("cache_kind", "") == "sampled_contours" &&
         attributes.value("component", "") == declaration.component &&
         attributes.value("field", "") == declaration.field &&
         attributes.value("storage_profile_id", "") ==
             "subject_mask_presentation_candidate_v1" &&
         attributes.value("codec_profile_id", "") == "zstd_fast_v1" &&
         attributes.value("write_mode", "") == "immutable";
}

bool ValidateSampledContourCacheMetadata(
    const ArchiveContext::Impl &archive, const std::string &run_base,
    const SubjectMaskSampledContourV1Summary &summary, json *root_output,
    std::string *error_message) {
  try {
    auto root = internal::ReadArchiveRunMetadata(archive, {run_base});
    if (!root || root->value("zarr_format", 0) != 3 ||
        root->value("node_type", "") != "group" ||
        !root->contains("consolidated_metadata") ||
        root->at("consolidated_metadata").value("kind", "") != "inline" ||
        root->at("consolidated_metadata").value("must_understand", true)) {
      internal::SetArchiveError(
          error_message,
          "Sampled-contour cache lacks exact inline Zarr v3 metadata");
      return false;
    }
    json declarations = json::object();
    std::unordered_set<std::string> expected_paths;
    std::vector<std::string> groups = {"", "components"};
    for (size_t index = 0; index < summary.component_labels.size(); ++index) {
      const std::string component =
          "components/" + summary.component_labels[index];
      groups.push_back(component);
      groups.push_back(component + "/sampled_contours");
    }
    for (const auto &relative : groups) {
      const std::string path =
          relative.empty() ? run_base : run_base + "/" + relative;
      expected_paths.insert(path);
      const auto direct =
          internal::ReadArchiveJson(archive, path + "/zarr.json");
      const auto *consolidated = ConsolidatedEntry(*root, path);
      if (!direct || !consolidated ||
          !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                             *consolidated)) {
        internal::SetArchiveError(
            error_message, "Sampled-contour group metadata differs: " + path);
        return false;
      }
      if (relative.find("/sampled_contours") != std::string::npos) {
        const auto &attributes = direct->at("attributes");
        const std::string component =
            relative.substr(std::string("components/").size(),
                            relative.rfind("/sampled_contours") -
                                std::string("components/").size());
        const auto label = std::find(summary.component_labels.begin(),
                                     summary.component_labels.end(), component);
        const size_t index = static_cast<size_t>(
            std::distance(summary.component_labels.begin(), label));
        if (label == summary.component_labels.end() ||
            attributes.value("schema_id", "") !=
                "sampled_component_contours_v1" ||
            attributes.value("coordinate_space", "") != "roi_pixels" ||
            attributes.value("point_order", "") != "xy" ||
            attributes.value("source_component", "") != component ||
            attributes.value("source_mask_run", "") != summary.source_run_id ||
            attributes.value("sample_count", size_t{0}) !=
                summary.component_sample_counts[index] ||
            attributes.value("surface_role", "") !=
                "canonical_derived_display_cache" ||
            attributes.value("authoritative_pixels", true)) {
          internal::SetArchiveError(
              error_message,
              "Sampled-contour component group semantics differ: " + path);
          return false;
        }
      }
      json normalized = *direct;
      if (!NormalizeSampledContourCacheGroup(&normalized, relative.empty())) {
        internal::SetArchiveError(error_message,
                                  "Sampled-contour group is not canonical");
        return false;
      }
      declarations[relative] = std::move(normalized);
    }
    for (const auto &declaration : summary.arrays) {
      const std::string path = run_base + "/" + declaration.path;
      expected_paths.insert(path);
      const auto direct =
          internal::ReadArchiveJson(archive, path + "/zarr.json");
      const auto *consolidated = ConsolidatedEntry(*root, path);
      if (!direct || !consolidated ||
          !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                             *consolidated) ||
          !ValidateSampledContourArrayPhysicalMetadata(*direct, declaration)) {
        internal::SetArchiveError(
            error_message, "Invalid sampled-contour array metadata: " + path);
        return false;
      }
      declarations[declaration.path] = *direct;
    }
    const auto &all = root->at("consolidated_metadata").at("metadata");
    const std::string prefix = run_base + "/";
    for (auto item = all.begin(); item != all.end(); ++item) {
      if ((item.key() == run_base || item.key().rfind(prefix, 0) == 0) &&
          expected_paths.find(item.key()) == expected_paths.end()) {
        internal::SetArchiveError(error_message,
                                  "Unexpected sampled-contour cache node: " +
                                      item.key());
        return false;
      }
    }
    if (CanonicalJsonSha256(declarations) != summary.metadata_digest) {
      internal::SetArchiveError(
          error_message,
          "Sampled-contour metadata declaration digest mismatch");
      return false;
    }
    *root_output = std::move(*root);
    return true;
  } catch (const json::exception &) {
    internal::SetArchiveError(
        error_message,
        "Sampled-contour direct/consolidated metadata is malformed");
    return false;
  }
}

bool OpenExactSampledContourCache(
    const ArchiveContext::Impl &archive, const std::string &run_base,
    const json &root, const SubjectMaskSampledContourV1Summary &summary,
    std::vector<ContourSource> *contours, std::string *error_message) {
  contours->clear();
  contours->reserve(summary.component_labels.size());
  for (size_t index = 0; index < summary.component_labels.size(); ++index) {
    const std::string base = run_base + "/components/" +
                             summary.component_labels[index] +
                             "/sampled_contours";
    ContourSource source;
    source.kind = ContourKind::Sampled;
    if (!OpenExactArray(archive, root, base + "/valid",
                        &source.sampled_valid_bool, error_message) ||
        !OpenExactArray(archive, root, base + "/points_xy",
                        &source.sampled_points, error_message)) {
      return false;
    }
    source.sampled_point_count = summary.component_sample_counts[index];
    contours->push_back(std::move(source));
  }
  return true;
}

template <typename T>
bool ReadExactVector(const ts::TensorStore<T, 1> &store, std::vector<T> *output,
                     std::string *error_message, const std::string &path) {
  auto read = ts::Read(store).result();
  if (!read.ok() || read->rank() != 1 || read->byte_strides().size() != 1) {
    internal::SetArchiveError(
        error_message,
        read.ok() ? "Strict subject-mask vector shape mismatch: " + path
                  : path + ": " + read.status().ToString());
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  output->resize(count);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      read->byte_strided_origin_pointer().get());
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = *reinterpret_cast<const T*>(
        origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
  }
  return true;
}

bool ReadExactBoolVector(const ts::TensorStore<bool, 1>& store,
                         std::vector<uint8_t>* output,
                         std::string* error_message,
                         const std::string& path) {
  auto read = ts::Read(store).result();
  if (!read.ok() || read->rank() != 1 || read->byte_strides().size() != 1) {
    internal::SetArchiveError(
        error_message,
        read.ok() ? "Strict subject-mask bool vector shape mismatch: " + path
                  : path + ": " + read.status().ToString());
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  output->resize(count);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      read->byte_strided_origin_pointer().get());
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] =
        *reinterpret_cast<const bool *>(origin + static_cast<ts::Index>(index) *
                                                     read->byte_strides()[0])
            ? 1
            : 0;
  }
  return true;
}

bool ReadExactFloatMatrix(const ts::TensorStore<float, 2>& store,
                          size_t columns, std::vector<float>* output,
                          std::string* error_message,
                          const std::string& path) {
  auto read = ts::Read(store).result();
  if (!read.ok() || read->rank() != 2 || read->byte_strides().size() != 2 ||
      static_cast<size_t>(read->shape()[1]) != columns) {
    internal::SetArchiveError(
        error_message,
        read.ok() ? "Strict subject-mask matrix shape mismatch: " + path
                  : path + ": " + read.status().ToString());
    return false;
  }
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  output->resize(rows * columns);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      (*output)[row * columns + column] = *reinterpret_cast<const float*>(
          origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
          static_cast<ts::Index>(column) * read->byte_strides()[1]);
    }
  }
  return true;
}

bool ValidateStrictMetadata(const ArchiveContext::Impl &archive,
                            const std::string &run_base,
                            const SubjectMaskV1ManifestSummary &summary,
                            json *root_output, std::string *error_message) {
  try {
    const auto root = internal::ReadArchiveRunMetadata(archive, {run_base});
    if (!root || root->value("zarr_format", 0) != 3 ||
        root->value("node_type", "") != "group" ||
        !root->contains("consolidated_metadata") ||
        root->at("consolidated_metadata").value("kind", "") != "inline" ||
        root->at("consolidated_metadata").value("must_understand", true)) {
      internal::SetArchiveError(
          error_message,
          "Subject-mask v1 archive lacks exact inline Zarr v3 metadata");
      return false;
    }

    const auto direct_group =
        internal::ReadArchiveJson(archive, run_base + "/zarr.json");
    const auto *consolidated_group = ConsolidatedEntry(*root, run_base);
    if (!direct_group || !consolidated_group ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(
            *direct_group, *consolidated_group)) {
      internal::SetArchiveError(
          error_message,
          "Subject-mask v1 run group direct/consolidated metadata differs");
      return false;
    }

    json declarations = json::object();
    json normalized_group = *direct_group;
    if (!NormalizeStrictGroup(&normalized_group, summary.metadata_digest_scope,
                              true)) {
      internal::SetArchiveError(error_message,
                                "Subject-mask v1 run group is not canonical");
      return false;
    }
    declarations[""] = std::move(normalized_group);
    std::unordered_set<std::string> expected_paths = {run_base,
                                                      run_base + "/metrics"};

    const std::string metrics_path = run_base + "/metrics";
    const auto direct_metrics =
        internal::ReadArchiveJson(archive, metrics_path + "/zarr.json");
    const auto *consolidated_metrics = ConsolidatedEntry(*root, metrics_path);
    if (!direct_metrics || !consolidated_metrics ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(
            *direct_metrics, *consolidated_metrics)) {
      internal::SetArchiveError(
          error_message,
          "Subject-mask v1 metrics group direct/consolidated metadata differs");
      return false;
    }

    for (const auto &declaration : kSubjectMaskV1ArrayDeclarations) {
      const std::string relative(declaration.path);
      const std::string path = run_base + "/" + relative;
      expected_paths.insert(path);
      const auto direct =
          internal::ReadArchiveJson(archive, path + "/zarr.json");
      const auto *consolidated = ConsolidatedEntry(*root, path);
      if (!direct || !consolidated ||
          !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                             *consolidated) ||
          consolidated->value("zarr_format", 0) != 3 ||
          consolidated->value("node_type", "") != "array" ||
          consolidated->contains("consolidated_metadata") ||
          consolidated->value("data_type", "") != declaration.dtype ||
          !consolidated->contains("shape") ||
          consolidated->at("shape").get<std::vector<size_t>>() !=
              ExpectedSubjectMaskV1Shape(declaration, summary)) {
        internal::SetArchiveError(
            error_message, "Invalid subject-mask v1 declaration: " + path);
        return false;
      }
      declarations[relative] = *direct;
    }

    const auto &all = root->at("consolidated_metadata").at("metadata");
    const std::string prefix = run_base + "/";
    for (auto item = all.begin(); item != all.end(); ++item) {
      if ((item.key() == run_base || item.key().rfind(prefix, 0) == 0) &&
          expected_paths.find(item.key()) == expected_paths.end()) {
        internal::SetArchiveError(
            error_message, "Unexpected subject-mask v1 node: " + item.key());
        return false;
      }
    }
    if (CanonicalJsonSha256(declarations) != summary.metadata_digest) {
      internal::SetArchiveError(
          error_message,
          "Subject-mask v1 metadata declaration digest mismatch");
      return false;
    }
    *root_output = std::move(*root);
    return true;
  } catch (const json::exception &) {
    internal::SetArchiveError(
        error_message,
        "Subject-mask v1 direct/consolidated metadata is malformed");
    return false;
  }
}

std::unique_ptr<SubjectMaskOverlayRepository>
OpenStrictSubjectMaskV1(const ArchiveContext::Impl &archive,
                        const std::string &run_name,
                        const std::string &run_base, const json &run_attributes,
                        const SubjectMaskOverlayOpenOptions &options,
                        const ArchiveContext::Impl *presentation_cache_archive,
                        std::chrono::steady_clock::time_point open_started,
                        std::string *error_message) {
  const auto manifest = run_attributes.find("run_manifest");
  SubjectMaskV1ManifestSummary summary;
  if (manifest == run_attributes.end() ||
      !ValidateSubjectMaskV1Manifest(*manifest, run_name, &summary,
                                     error_message)) {
    return nullptr;
  }
  if (!options.expected_manifest_payload_digest.empty() &&
      options.expected_manifest_payload_digest != summary.payload_digest) {
    internal::SetArchiveError(
        error_message,
        "Subject-mask v1 manifest payload digest does not match request");
    return nullptr;
  }
  if (!summary.selector_eligible && !options.allow_selector_ineligible) {
    internal::SetArchiveError(
        error_message,
        "Subject-mask v1 run is selector-ineligible; use an explicit "
        "benchmark request");
    return nullptr;
  }

  // Admit known mapping allocations before opening/reading full vectors. This
  // is not an RSS cap: archive JSON, TensorStore and allocator overhead remain
  // separately measured. Include temporary columns and key-validation space.
  uint64_t mapping_bytes = 0;
  AddBytes(&mapping_bytes, summary.row_count, sizeof(RowMetadata) + 40 + 64);
  AddBytes(&mapping_bytes, summary.frame_count + 1, 2 * sizeof(int64_t));
  if (options.max_mapping_bytes && mapping_bytes > options.max_mapping_bytes) {
    internal::SetArchiveError(error_message, "Subject-mask mappings exceed interactive admission budget");
    return nullptr;
  }

  SubjectMaskOverlayRepositoryMetrics opening_metrics;
  const auto catalog_started = std::chrono::steady_clock::now();
  json root;
  if (!ValidateStrictMetadata(archive, run_base, summary, &root,
                              error_message)) {
    return nullptr;
  }
  opening_metrics.catalog_ms = ElapsedMilliseconds(catalog_started);

  std::vector<ContourSource> presentation_contours;
  SubjectMaskSampledContourV1Summary cache_summary;
  if (options.presentation_cache_archive) {
    if (!options.allow_selector_ineligible ||
        options.presentation_cache_run.empty() ||
        options.expected_presentation_cache_manifest_payload_digest.empty() ||
        presentation_cache_archive == nullptr) {
      internal::SetArchiveError(
          error_message,
          "Sampled-contour cache requires an explicit selector-ineligible "
          "run and manifest digest");
      return nullptr;
    }
    std::string cache_run = options.presentation_cache_run;
    constexpr std::string_view cache_group = "subject_mask_cache_runs";
    if (cache_run.rfind(std::string(cache_group) + "/", 0) == 0) {
      cache_run.erase(0, cache_group.size() + 1);
    }
    if (!ValidRunName(cache_run)) {
      internal::SetArchiveError(error_message,
                                "Sampled-contour cache run id is invalid");
      return nullptr;
    }
    const std::string cache_base = std::string(cache_group) + "/" + cache_run;
    const auto &cache_archive = *presentation_cache_archive;
    const auto cache_attributes =
        internal::ReadArchiveAttributes(cache_archive, cache_base);
    if (!cache_attributes || !cache_attributes->contains("run_manifest") ||
        !ValidateSubjectMaskSampledContourV1Manifest(
            cache_attributes->at("run_manifest"), cache_run, *manifest, summary,
            &cache_summary, error_message) ||
        cache_summary.payload_digest !=
            options.expected_presentation_cache_manifest_payload_digest) {
      if (error_message && error_message->empty()) {
        *error_message =
            "Sampled-contour cache manifest digest does not match request";
      }
      return nullptr;
    }
    json cache_root;
    if (!ValidateSampledContourCacheMetadata(cache_archive, cache_base,
                                             cache_summary, &cache_root,
                                             error_message) ||
        !OpenExactSampledContourCache(cache_archive, cache_base, cache_root,
                                      cache_summary, &presentation_contours,
                                      error_message)) {
      return nullptr;
    }
  } else if (!options.presentation_cache_run.empty() ||
             !options.expected_presentation_cache_manifest_payload_digest
                  .empty() ||
             options.contour_only) {
    internal::SetArchiveError(error_message,
                              "Sampled-contour cache request is incomplete");
    return nullptr;
  }

  ts::TensorStore<int64_t, 1> source_crop_rows_store;
  ts::TensorStore<uint64_t, 1> instance_keys_store;
  ts::TensorStore<int64_t, 1> source_frames_store;
  ts::TensorStore<int64_t, 1> offsets_store;
  ts::TensorStore<float, 2> placements_store;
  ts::TensorStore<uint8_t, 4> masks_store;
  ts::TensorStore<bool, 1> available_store;
  ts::TensorStore<bool, 2> mask_present_store;
  ts::TensorStore<float, 2> area_store;
  ts::TensorStore<float, 3> centroid_store;
  ts::TensorStore<bool, 2> centroid_valid_store;
  ts::TensorStore<float, 3> bbox_store;
  ts::TensorStore<bool, 2> bbox_valid_store;
  const auto open = [&](const std::string& relative, auto* output) {
    return OpenExactArray(archive, root, run_base + "/" + relative, output,
                          error_message);
  };
  const auto storage_started = std::chrono::steady_clock::now();
  if (!open("source_crop_row_ids", &source_crop_rows_store) ||
      !open("instance_key", &instance_keys_store) ||
      !open("source_acquisition_frame_index", &source_frames_store) ||
      !open("frame_row_offsets", &offsets_store) ||
      !open("source_crop_xywh", &placements_store) ||
      !open("masks_roi", &masks_store) ||
      !open("available_channels", &available_store) ||
      !open("metrics/mask_present", &mask_present_store) ||
      !open("metrics/area_px", &area_store) ||
      !open("metrics/centroid_xy", &centroid_store) ||
      !open("metrics/centroid_valid", &centroid_valid_store) ||
      !open("metrics/bbox_xyxy", &bbox_store) ||
      !open("metrics/bbox_valid", &bbox_valid_store)) {
    return nullptr;
  }
  opening_metrics.storage_open_ms = ElapsedMilliseconds(storage_started);

  if (!options.contour_only && options.max_storage_chunk_bytes) {
    const auto layout = masks_store.chunk_layout();
    uint64_t bytes = 1;
    if (!layout.ok()) {
      internal::SetArchiveError(error_message, "Subject-mask physical chunk layout is unavailable");
      return nullptr;
    }
    for (const auto extent : layout->read_chunk_shape()) {
      if (extent <= 0 || bytes > options.max_storage_chunk_bytes / static_cast<uint64_t>(extent)) {
        internal::SetArchiveError(error_message, "Subject-mask physical chunk exceeds interactive decode budget");
        return nullptr;
      }
      bytes *= static_cast<uint64_t>(extent);
    }
  }

  const auto mapping_started = std::chrono::steady_clock::now();
  std::vector<int64_t> offsets;
  std::vector<int64_t> frames;
  std::vector<int64_t> source_crop_rows;
  std::vector<uint64_t> instance_keys;
  std::vector<float> placements;
  std::vector<uint8_t> available;
  if (!ReadExactVector(offsets_store, &offsets, error_message,
                       run_base + "/frame_row_offsets") ||
      !ReadExactVector(source_frames_store, &frames, error_message,
                       run_base + "/source_acquisition_frame_index") ||
      !ReadExactVector(source_crop_rows_store, &source_crop_rows, error_message,
                       run_base + "/source_crop_row_ids") ||
      !ReadExactVector(instance_keys_store, &instance_keys, error_message,
                       run_base + "/instance_key") ||
      !ReadExactFloatMatrix(placements_store, 4, &placements, error_message,
                            run_base + "/source_crop_xywh") ||
      !ReadExactBoolVector(available_store, &available, error_message,
                           run_base + "/available_channels")) {
    return nullptr;
  }
  opening_metrics.mapping_read_ms = ElapsedMilliseconds(mapping_started);
  opening_metrics.frame_offset_reads = 1;
  opening_metrics.frame_index_initialize_ms = opening_metrics.mapping_read_ms;
  opening_metrics.frame_index_rows_read = offsets.size();
  opening_metrics.frame_index_source_bytes = offsets.size() * sizeof(int64_t);
  opening_metrics.frame_index_retained_bytes =
      offsets.capacity() * sizeof(int64_t);

  if (offsets.size() != summary.frame_count + 1 ||
      frames.size() != summary.row_count ||
      source_crop_rows.size() != summary.row_count ||
      instance_keys.size() != summary.row_count ||
      placements.size() != summary.row_count * 4 ||
      available.size() != summary.channel_count || offsets.front() != 0 ||
      offsets.back() != static_cast<int64_t>(summary.row_count)) {
    internal::SetArchiveError(
        error_message, "Subject-mask v1 retained mapping shape mismatch");
    return nullptr;
  }
  if (!ValidateSubjectMaskV1FrameIndex(offsets, frames, summary.frame_count,
                                       summary.row_count, error_message) ||
      !ValidateSubjectMaskV1InstanceKeys(instance_keys, summary.row_count,
                                         error_message)) {
    return nullptr;
  }
  std::vector<RowMetadata> rows;
  rows.reserve(summary.row_count);
  for (size_t row = 0; row < summary.row_count; ++row) {
    const float x = placements[row * 4];
    const float y = placements[row * 4 + 1];
    const float width = placements[row * 4 + 2];
    const float height = placements[row * 4 + 3];
    if (source_crop_rows[row] < 0 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) || width <= 0.0f ||
        height <= 0.0f) {
      internal::SetArchiveError(
          error_message,
          "Subject-mask v1 contains invalid identity or placement data");
      return nullptr;
    }
    rows.push_back({row, frames[row], instance_keys[row], -1,
                    source_crop_rows[row], x, y, width, height});
  }

  SubjectMaskOverlayDescriptor descriptor;
  descriptor.cache_namespace = archive.root_path.string() + ":" + run_name + ":" + summary.payload_digest;
  descriptor.source_group = "refined_subject_masks_runs";
  descriptor.run_name = run_name;
  descriptor.source_crop_run = StringValue(run_attributes, "source_crop_run");
  descriptor.label_schema_id = "palette.subject_mask.component_registry";
  descriptor.storage = SubjectMaskStorage::Dense;
  descriptor.component_labels = summary.component_labels;
  descriptor.row_count = summary.row_count;
  descriptor.camera_frame_count = summary.frame_count;
  descriptor.mask_width = summary.mask_width;
  descriptor.mask_height = summary.mask_height;
  descriptor.strict_v1 = true;
  descriptor.run_manifest_payload_digest = summary.payload_digest;
  descriptor.contour_only = options.contour_only;
  if (options.presentation_cache_archive) {
    descriptor.presentation_cache_archive =
        options.presentation_cache_archive->rootPath().string();
    descriptor.presentation_cache_run = cache_summary.run_id;
    descriptor.presentation_cache_manifest_payload_digest =
        cache_summary.payload_digest;
  }
  if (descriptor.mask_width >
      std::numeric_limits<uint32_t>::max() / descriptor.mask_height) {
    internal::SetArchiveError(
        error_message,
        "Subject-mask v1 dimensions exceed sparse index representation");
    return nullptr;
  }

  opening_metrics.subject_mapping_bytes =
      frames.capacity() * sizeof(int64_t) +
      source_crop_rows.capacity() * sizeof(int64_t) +
      instance_keys.capacity() * sizeof(uint64_t) +
      placements.capacity() * sizeof(float);
  opening_metrics.metadata_decoded_bytes =
      opening_metrics.subject_mapping_bytes +
      offsets.capacity() * sizeof(int64_t) + available.capacity();
  opening_metrics.metadata_index_ms = 0.0;

  return std::make_unique<TensorStoreSubjectMaskOverlayRepository>(
      std::move(descriptor), std::move(rows), std::move(offsets), nullptr,
      std::move(available), std::move(masks_store),
      ts::TensorStore<uint8_t, 4>{}, std::vector<RleSource>{},
      std::move(presentation_contours), !options.contour_only,
      std::move(opening_metrics), open_started, options);
}

}  // namespace

std::unique_ptr<SubjectMaskOverlayRepository> OpenSubjectMaskOverlayRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const SubjectMaskOverlayOpenOptions& options,
    std::string* error_message) {
  const auto open_started = std::chrono::steady_clock::now();
  SubjectMaskOverlayRepositoryMetrics opening_metrics;
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto& impl = *archive->impl_;
  constexpr const char* group = "refined_subject_masks_runs";
  std::string run_name = options.requested_run;
  if (run_name.rfind(std::string(group) + "/", 0) == 0) {
    run_name.erase(0, std::string(group).size() + 1);
  }
  if (run_name.empty() || run_name == "latest") {
    run_name = LatestRun(impl, group);
  }
  if (!ValidRunName(run_name)) {
    internal::SetArchiveError(error_message,
                              "No valid refined subject-mask run is available");
    return nullptr;
  }
  const std::string run_base = std::string(group) + "/" + run_name;
  const auto run_attributes = internal::ReadArchiveAttributes(impl, run_base);
  if (!run_attributes) {
    internal::SetArchiveError(error_message,
                              "Subject-mask run attributes are unreadable");
    return nullptr;
  }
  const auto manifest = run_attributes->find("run_manifest");
  const bool strict_v1 =
      manifest != run_attributes->end() && manifest->is_object() &&
      manifest->value("schema_id", "") ==
          "palette.subject_mask_core.run_manifest";
  if (strict_v1) {
    const ArchiveContext::Impl *presentation_cache_archive =
        options.presentation_cache_archive
            ? options.presentation_cache_archive->impl_.get()
            : nullptr;
    return OpenStrictSubjectMaskV1(impl, run_name, run_base, *run_attributes,
                                   options, presentation_cache_archive,
                                   open_started, error_message);
  }
  if (options.require_strict_v1) {
    internal::SetArchiveError(
        error_message,
        "Requested subject-mask run is not strict subject-mask v1");
    return nullptr;
  }

  SubjectMaskOverlayDescriptor descriptor;
  descriptor.cache_namespace = archive->rootPath().string() + ":" + run_name;
  descriptor.source_group = group;
  descriptor.run_name = run_name;
  descriptor.source_crop_run = StringValue(*run_attributes, "source_crop_run");
  if (descriptor.source_crop_run.rfind("crop_runs/", 0) == 0) {
    descriptor.source_crop_run.erase(0, std::string("crop_runs/").size());
  }
  descriptor.label_schema_id = StringValue(*run_attributes, "label_schema_id");
  descriptor.component_labels = StringList(*run_attributes, "mask_labels");
  if (!ValidRunName(descriptor.source_crop_run) ||
      descriptor.component_labels.empty()) {
    internal::SetArchiveError(
        error_message, "Subject-mask run lacks source_crop_run or mask_labels");
    return nullptr;
  }
  opening_metrics.catalog_ms = ElapsedMilliseconds(open_started);

  const auto mapping_started = std::chrono::steady_clock::now();
  auto subject_frame_store =
      OpenIntegerStore(impl, run_base + "/frame_indices");
  if (!subject_frame_store || IntegerRowCount(*subject_frame_store) == 0) {
    internal::SetArchiveError(error_message,
                              "Subject-mask frame_indices are unreadable");
    return nullptr;
  }
  descriptor.row_count = IntegerRowCount(*subject_frame_store);
  auto frame_count_store = OpenIntegerStore(impl, run_base + "/frame_counts");
  const bool use_lazy_mapping =
      frame_count_store && IntegerRowCount(*frame_count_store) > 0;

  std::vector<int64_t> frames;
  std::vector<int64_t> detections;
  std::vector<int64_t> source_rows;
  const std::string crop_base = "crop_runs/" + descriptor.source_crop_run;
  const auto crop_attributes = internal::ReadArchiveAttributes(impl, crop_base);
  std::vector<int64_t> crop_frames;
  std::vector<int64_t> crop_detections;
  std::vector<std::vector<double>> crop_offsets;
  double roi_width = 0.0;
  double roi_height = 0.0;
  if (!crop_attributes) {
    internal::SetArchiveError(error_message,
                              "Source crop placement metadata is unreadable");
    return nullptr;
  }
  ReadRoiSize(*crop_attributes, &roi_width, &roi_height);

  std::unique_ptr<LazyMappingSource> lazy_mapping;
  if (use_lazy_mapping) {
    auto subject_detection_store =
        OpenIntegerStore(impl, run_base + "/detection_indices");
    if (subject_detection_store &&
        IntegerRowCount(*subject_detection_store) != descriptor.row_count) {
      subject_detection_store.reset();
    }
    auto subject_crop_row_store =
        OpenIntegerStore(impl, run_base + "/source_crop_row_ids");
    if (subject_crop_row_store &&
        IntegerRowCount(*subject_crop_row_store) != descriptor.row_count) {
      subject_crop_row_store.reset();
    }
    auto crop_frame_store =
        OpenIntegerStore(impl, crop_base + "/frame_indices");
    auto crop_detection_store =
        OpenIntegerStore(impl, crop_base + "/detection_indices");
    auto crop_offset_store =
        OpenNumericMatrixStore(impl, crop_base + "/roi_coordinates_full", 2);
    if (!crop_frame_store || IntegerRowCount(*crop_frame_store) == 0 ||
        !crop_offset_store ||
        NumericMatrixRowCount(*crop_offset_store) !=
            IntegerRowCount(*crop_frame_store)) {
      internal::SetArchiveError(error_message,
                                "Source crop placement metadata is unreadable");
      return nullptr;
    }
    if (crop_detection_store && IntegerRowCount(*crop_detection_store) !=
                                    IntegerRowCount(*crop_frame_store)) {
      crop_detection_store.reset();
    }
    descriptor.camera_frame_count = IntegerRowCount(*frame_count_store);
    const size_t subject_page_rows =
        std::max<size_t>(1, IntegerChunkRows(*subject_frame_store));
    const size_t crop_page_rows =
        std::max<size_t>(1, IntegerChunkRows(*crop_frame_store));
    lazy_mapping = std::make_unique<LazyMappingSource>(
        std::move(*subject_frame_store), std::move(subject_detection_store),
        std::move(subject_crop_row_store), std::move(*frame_count_store),
        std::move(*crop_frame_store), std::move(crop_detection_store),
        std::move(*crop_offset_store), subject_page_rows, crop_page_rows,
        roi_width, roi_height);
    opening_metrics.lazy_mapping = true;
  } else {
    auto column_started = std::chrono::steady_clock::now();
    const bool frames_read =
        ReadIntegers(impl, run_base + "/frame_indices", &frames);
    opening_metrics.frame_indices_ms = ElapsedMilliseconds(column_started);
    if (!frames_read || frames.size() != descriptor.row_count) {
      internal::SetArchiveError(error_message,
                                "Subject-mask frame_indices are unreadable");
      return nullptr;
    }
    column_started = std::chrono::steady_clock::now();
    const bool detections_read =
        ReadIntegers(impl, run_base + "/detection_indices", &detections);
    opening_metrics.detection_indices_ms = ElapsedMilliseconds(column_started);
    if (!detections_read || detections.size() != frames.size()) {
      detections.assign(frames.size(), -1);
    }
    column_started = std::chrono::steady_clock::now();
    const bool source_rows_read =
        ReadIntegers(impl, run_base + "/source_crop_row_ids", &source_rows);
    opening_metrics.source_crop_row_ids_ms =
        ElapsedMilliseconds(column_started);
    if (!source_rows_read || source_rows.size() != frames.size()) {
      source_rows.clear();
    }
    column_started = std::chrono::steady_clock::now();
    const bool crop_frames_read =
        ReadIntegers(impl, crop_base + "/frame_indices", &crop_frames);
    opening_metrics.crop_frame_indices_ms = ElapsedMilliseconds(column_started);
    column_started = std::chrono::steady_clock::now();
    const bool crop_offsets_read = ReadNumericMatrix(
        impl, crop_base + "/roi_coordinates_full", 2, &crop_offsets);
    opening_metrics.crop_coordinates_ms = ElapsedMilliseconds(column_started);
    if (!crop_frames_read || crop_frames.empty() || !crop_offsets_read ||
        crop_offsets.size() != crop_frames.size()) {
      internal::SetArchiveError(error_message,
                                "Source crop placement metadata is unreadable");
      return nullptr;
    }
    column_started = std::chrono::steady_clock::now();
    const bool crop_detections_read =
        ReadIntegers(impl, crop_base + "/detection_indices", &crop_detections);
    opening_metrics.crop_detection_indices_ms =
        ElapsedMilliseconds(column_started);
    if (!crop_detections_read || crop_detections.size() != crop_frames.size()) {
      crop_detections.assign(crop_frames.size(), -1);
    }
    opening_metrics.mapping_read_ms = ElapsedMilliseconds(mapping_started);
  }

  const auto storage_started = std::chrono::steady_clock::now();
  ts::TensorStore<uint8_t, 4> dense;
  ts::TensorStore<uint8_t, 4> bitpacked;
  std::vector<RleSource> rle(descriptor.component_labels.size());
  if (auto store = OpenArray<uint8_t, 4>(impl, run_base + "/masks_roi")) {
    const auto shape = store->domain().shape();
    if (shape[0] == static_cast<ts::Index>(descriptor.row_count) &&
        shape[1] >=
            static_cast<ts::Index>(descriptor.component_labels.size()) &&
        shape[2] > 0 && shape[3] > 0) {
      descriptor.storage = SubjectMaskStorage::Dense;
      descriptor.mask_height = static_cast<size_t>(shape[2]);
      descriptor.mask_width = static_cast<size_t>(shape[3]);
      dense = std::move(*store);
    }
  }
  if (descriptor.mask_width == 0) {
    const auto bitpacked_attributes =
        internal::ReadArchiveAttributes(impl, run_base + "/mask_bitpacked");
    auto store =
        OpenArray<uint8_t, 4>(impl, run_base + "/mask_bitpacked/masks_packed");
    if (bitpacked_attributes && store &&
        StringValue(*bitpacked_attributes, "schema_id") ==
            "palette_mask_bitpacked_binary_v1") {
      const auto logical = bitpacked_attributes->find("logical_shape");
      if (logical != bitpacked_attributes->end() && logical->is_array() &&
          logical->size() == 4 && (*logical)[0].is_number_integer() &&
          (*logical)[1].is_number_integer() &&
          (*logical)[2].is_number_integer() &&
          (*logical)[3].is_number_integer() &&
          (*logical)[0].get<int64_t>() ==
              static_cast<int64_t>(descriptor.row_count)) {
        const int64_t logical_channels = (*logical)[1].get<int64_t>();
        const int64_t logical_height = (*logical)[2].get<int64_t>();
        const int64_t logical_width = (*logical)[3].get<int64_t>();
        const auto physical = store->domain().shape();
        if (logical_channels >=
                static_cast<int64_t>(descriptor.component_labels.size()) &&
            logical_height > 0 && logical_width > 0 &&
            physical[0] == static_cast<ts::Index>(descriptor.row_count) &&
            physical[1] >= logical_channels && physical[2] >= logical_height &&
            physical[3] >= (logical_width + 7) / 8) {
          descriptor.storage = SubjectMaskStorage::Bitpacked;
          descriptor.mask_height = static_cast<size_t>(logical_height);
          descriptor.mask_width = static_cast<size_t>(logical_width);
          bitpacked = std::move(*store);
        }
      }
    }
  }
  if (descriptor.mask_width == 0) {
    const auto rle_attributes =
        internal::ReadArchiveAttributes(impl, run_base + "/mask_rle");
    if (rle_attributes && !BoolValue(*run_attributes, "mask_rle_stale") &&
        StringValue(*rle_attributes, "schema_id") ==
            "palette_mask_rle_binary_v1") {
      const auto shape = rle_attributes->find("encoded_shape_hw");
      if (shape != rle_attributes->end() && shape->is_array() &&
          shape->size() == 2 && (*shape)[0].is_number_integer() &&
          (*shape)[1].is_number_integer()) {
        const int64_t mask_height = (*shape)[0].get<int64_t>();
        const int64_t mask_width = (*shape)[1].get<int64_t>();
        if (mask_height <= 0 || mask_width <= 0 ||
            static_cast<uint64_t>(mask_height) >
                std::numeric_limits<size_t>::max() ||
            static_cast<uint64_t>(mask_width) >
                std::numeric_limits<size_t>::max() ||
            static_cast<size_t>(mask_width) >
                std::numeric_limits<size_t>::max() /
                    static_cast<size_t>(mask_height)) {
          internal::SetArchiveError(error_message,
                                    "RLE mask dimensions are invalid");
          return nullptr;
        }
        descriptor.storage = SubjectMaskStorage::Rle;
        descriptor.mask_height = static_cast<size_t>(mask_height);
        descriptor.mask_width = static_cast<size_t>(mask_width);
        for (size_t channel = 0; channel < descriptor.component_labels.size();
             ++channel) {
          std::string component_base;
          for (const bool padded : {true, false}) {
            const std::string candidate =
                run_base + "/mask_rle/components/" +
                RleComponentName(channel, descriptor.component_labels[channel],
                                 padded);
            if (internal::ReadArchiveAttributes(impl, candidate)) {
              component_base = candidate;
              break;
            }
          }
          if (component_base.empty()) {
            continue;
          }
          auto counts =
              OpenArray<uint32_t, 1>(impl, component_base + "/counts");
          if (!counts ||
              !ReadIntegers(impl, component_base + "/indptr",
                            &rle[channel].indptr) ||
              !ReadBools(impl, component_base + "/present",
                         &rle[channel].present) ||
              rle[channel].indptr.size() != descriptor.row_count + 1 ||
              rle[channel].present.size() != descriptor.row_count) {
            continue;
          }
          rle[channel].counts = std::move(*counts);
          rle[channel].available = true;
        }
      }
    }
  }
  if (descriptor.mask_width == 0 || descriptor.mask_height == 0) {
    internal::SetArchiveError(
        error_message,
        "Subject-mask run has no supported dense, bitpacked, or RLE storage");
    return nullptr;
  }
  if (descriptor.mask_width >
      std::numeric_limits<uint32_t>::max() / descriptor.mask_height) {
    internal::SetArchiveError(
        error_message,
        "Subject-mask dimensions exceed the sparse index representation");
    return nullptr;
  }
  if (roi_width <= 0.0 || roi_height <= 0.0) {
    roi_width = descriptor.mask_width;
    roi_height = descriptor.mask_height;
  }
  if (lazy_mapping) {
    lazy_mapping->roi_width = roi_width;
    lazy_mapping->roi_height = roi_height;
  }

  std::vector<uint8_t> available;
  if (!ReadBools(impl, run_base + "/available_channels", &available) ||
      available.size() != descriptor.component_labels.size()) {
    available.assign(descriptor.component_labels.size(), 1);
  }
  opening_metrics.storage_open_ms = ElapsedMilliseconds(storage_started);

  const auto index_started = std::chrono::steady_clock::now();
  std::vector<RowMetadata> rows;
  if (!lazy_mapping) {
    rows.reserve(frames.size());
    size_t camera_frame_count = 0;
    for (size_t index = 0; index < frames.size(); ++index) {
      size_t crop_row = index;
      if (!source_rows.empty()) {
        if (source_rows[index] < 0 ||
            static_cast<uint64_t>(source_rows[index]) >= crop_frames.size()) {
          internal::SetArchiveError(
              error_message,
              "Subject-mask source_crop_row_ids contains an out-of-range row");
          return nullptr;
        }
        crop_row = static_cast<size_t>(source_rows[index]);
      } else if (index >= crop_frames.size()) {
        internal::SetArchiveError(error_message,
                                  "Legacy subject-mask rows are not aligned "
                                  "with the source crop run");
        return nullptr;
      }
      if (crop_frames[crop_row] != frames[index]) {
        internal::SetArchiveError(
            error_message,
            "Subject-mask source crop row does not match its camera frame");
        return nullptr;
      }
      if (detections[index] >= 0 && crop_detections[crop_row] >= 0 &&
          detections[index] != crop_detections[crop_row]) {
        internal::SetArchiveError(
            error_message,
            "Subject-mask source crop row does not match its detection index");
        return nullptr;
      }
      if (frames[index] < 0 || !std::isfinite(crop_offsets[crop_row][0]) ||
          !std::isfinite(crop_offsets[crop_row][1])) {
        continue;
      }
      rows.push_back({index, frames[index], 0,
                      detections[index] >= 0 ? detections[index]
                                             : crop_detections[crop_row],
                      static_cast<int64_t>(crop_row), crop_offsets[crop_row][0],
                      crop_offsets[crop_row][1], roi_width, roi_height});
      camera_frame_count =
          std::max(camera_frame_count, static_cast<size_t>(frames[index]) + 1);
    }
    descriptor.camera_frame_count = camera_frame_count;
    if (rows.empty()) {
      internal::SetArchiveError(error_message,
                                "Subject-mask run has no verified placements");
      return nullptr;
    }
  }
  opening_metrics.metadata_index_ms = ElapsedMilliseconds(index_started);

  const auto contour_started = std::chrono::steady_clock::now();
  const bool contours_stale = BoolValue(*run_attributes, "contours_stale");
  std::vector<ContourSource> contours;
  contours.reserve(descriptor.component_labels.size());
  for (const auto& label : descriptor.component_labels) {
    contours.push_back(OpenContour(impl, run_base, label, descriptor.row_count,
                                   contours_stale));
  }
  opening_metrics.contour_open_ms = ElapsedMilliseconds(contour_started);

  opening_metrics.subject_mapping_bytes = VectorCapacityBytes(frames) +
                                          VectorCapacityBytes(detections) +
                                          VectorCapacityBytes(source_rows);
  opening_metrics.crop_mapping_bytes = VectorCapacityBytes(crop_frames) +
                                       VectorCapacityBytes(crop_detections) +
                                       MatrixCapacityBytes(crop_offsets);
  opening_metrics.metadata_decoded_bytes +=
      opening_metrics.subject_mapping_bytes;
  opening_metrics.metadata_decoded_bytes += opening_metrics.crop_mapping_bytes;
  opening_metrics.metadata_decoded_bytes += VectorCapacityBytes(available);
  for (const auto& source : rle) {
    opening_metrics.metadata_decoded_bytes +=
        VectorCapacityBytes(source.indptr);
    opening_metrics.metadata_decoded_bytes +=
        VectorCapacityBytes(source.present);
  }
  for (const auto& source : contours) {
    opening_metrics.metadata_decoded_bytes +=
        VectorCapacityBytes(source.ragged_ptr);
    opening_metrics.metadata_decoded_bytes +=
        VectorCapacityBytes(source.ragged_len);
  }

  return std::make_unique<TensorStoreSubjectMaskOverlayRepository>(
      std::move(descriptor), std::move(rows), std::vector<int64_t>{},
      std::move(lazy_mapping), std::move(available), std::move(dense),
      std::move(bitpacked), std::move(rle), std::move(contours), true,
      std::move(opening_metrics), open_started, options);
}

std::unique_ptr<SubjectMaskOverlayRepository>
OpenSubjectMaskOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 const std::string &requested_run,
                                 std::string *error_message) {
  SubjectMaskOverlayOpenOptions options;
  options.requested_run = requested_run;
  return OpenSubjectMaskOverlayRepository(archive, options, error_message);
}

}  // namespace crimson::zarr
