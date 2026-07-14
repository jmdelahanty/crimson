#include "zarr/tensorstore_subject_mask_overlay_repository.h"

#include "zarr/archive_context_internal.h"

#include <nlohmann/json.hpp>
#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

std::optional<json> MakeArraySpec(const ArchiveContext::Impl &archive,
                                  const std::string &path) {
  auto store_spec = archive.store.spec();
  if (!store_spec.ok()) {
    return std::nullopt;
  }
  auto kvstore_json = store_spec->ToJson();
  if (!kvstore_json.ok()) {
    return std::nullopt;
  }
  return json{{"driver", "zarr3"}, {"kvstore", *kvstore_json}, {"path", path}};
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

template <typename T, ts::DimensionIndex Rank>
auto SliceFirstDimension(const ts::TensorStore<T, Rank> &store,
                         ts::Index start, ts::Index stop) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = start;
  domain.shape()[0] = stop - start;
  return store | ts::IdentityTransform(domain);
}

template <typename Source>
bool ReadIntegerVector(const ArchiveContext::Impl &archive,
                       const std::string &path, std::vector<int64_t> *output) {
  const auto store = OpenArray<Source, 1>(archive, path);
  if (!store) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source *values = static_cast<const Source *>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = static_cast<int64_t>(values[index]);
  }
  return true;
}

bool ReadIntegers(const ArchiveContext::Impl &archive, const std::string &path,
                  std::vector<int64_t> *output) {
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
bool ReadBoolVector(const ArchiveContext::Impl &archive,
                    const std::string &path, std::vector<uint8_t> *output) {
  const auto store = OpenArray<Source, 1>(archive, path);
  if (!store) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source *values = static_cast<const Source *>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = values[index] ? 1 : 0;
  }
  return true;
}

bool ReadBools(const ArchiveContext::Impl &archive, const std::string &path,
               std::vector<uint8_t> *output) {
  return ReadBoolVector<bool>(archive, path, output) ||
         ReadBoolVector<uint8_t>(archive, path, output) ||
         ReadBoolVector<int8_t>(archive, path, output) ||
         ReadBoolVector<int32_t>(archive, path, output);
}

template <typename Source>
bool ReadMatrix(const ArchiveContext::Impl &archive, const std::string &path,
                size_t minimum_columns,
                std::vector<std::vector<double>> *output) {
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
  const Source *values = static_cast<const Source *>(read->data());
  output->assign(rows, std::vector<double>(columns));
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      (*output)[row][column] =
          static_cast<double>(values[row * columns + column]);
    }
  }
  return true;
}

bool ReadNumericMatrix(const ArchiveContext::Impl &archive,
                       const std::string &path, size_t minimum_columns,
                       std::vector<std::vector<double>> *output) {
  return ReadMatrix<double>(archive, path, minimum_columns, output) ||
         ReadMatrix<float>(archive, path, minimum_columns, output) ||
         ReadMatrix<int64_t>(archive, path, minimum_columns, output) ||
         ReadMatrix<int32_t>(archive, path, minimum_columns, output);
}

std::string LatestRun(const ArchiveContext::Impl &archive,
                      const std::string &group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char *, 5> keys = {
      "latest", "latest_completed", "latest_complete", "latest_success",
      "refined_subject_mask_review_status_latest"};
  for (const char *key : keys) {
    const auto found = attributes->find(key);
    if (found != attributes->end() && found->is_string() &&
        !found->get_ref<const std::string &>().empty()) {
      return found->get<std::string>();
    }
  }
  return {};
}

bool ValidRunName(const std::string &run_name) {
  return !run_name.empty() && run_name != "." && run_name != ".." &&
         run_name.find('/') == std::string::npos;
}

std::vector<std::string> StringList(const json &attributes, const char *key) {
  std::vector<std::string> result;
  const auto found = attributes.find(key);
  if (found == attributes.end() || !found->is_array()) {
    return result;
  }
  for (const auto &value : *found) {
    if (value.is_string()) {
      result.push_back(value.get<std::string>());
    }
  }
  return result;
}

std::string StringValue(const json &attributes, const char *key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

bool BoolValue(const json &attributes, const char *key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_boolean() && found->get<bool>();
}

bool ReadRoiSize(const json &attributes, double *width, double *height) {
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

std::string SafeComponentName(const std::string &label) {
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

std::string RleComponentName(size_t index, const std::string &label,
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
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
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

struct RleSource {
  bool available = false;
  ts::TensorStore<uint32_t, 1> counts;
  std::vector<int64_t> indptr;
  std::vector<uint8_t> present;
};

bool ReadSampledContour(const ContourSource &source, size_t row,
                        std::vector<SubjectMaskOverlayPoint> *output) {
  bool contour_valid = false;
  if (source.sampled_valid_is_byte) {
    auto valid_slice = SliceFirstDimension(
        source.sampled_valid_byte, static_cast<ts::Index>(row),
        static_cast<ts::Index>(row + 1));
    auto valid = ts::Read(valid_slice).result();
    if (!valid.ok() || valid->rank() != 1 || valid->shape()[0] != 1) {
      return false;
    }
    contour_valid = *static_cast<const uint8_t *>(
                        valid->byte_strided_origin_pointer()) != 0;
  } else {
    auto valid_slice = SliceFirstDimension(
        source.sampled_valid_bool, static_cast<ts::Index>(row),
        static_cast<ts::Index>(row + 1));
    auto valid = ts::Read(valid_slice).result();
    if (!valid.ok() || valid->rank() != 1 || valid->shape()[0] != 1) {
      return false;
    }
    contour_valid =
        *static_cast<const bool *>(valid->byte_strided_origin_pointer());
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
  const auto *origin = reinterpret_cast<const uint8_t *>(
      points->byte_strided_origin_pointer().get());
  const size_t count = static_cast<size_t>(points->shape()[1]);
  output->reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto *point = origin + static_cast<ts::Index>(index) * strides[1];
    const float x = *reinterpret_cast<const float *>(point);
    const float y = *reinterpret_cast<const float *>(point + strides[2]);
    if (std::isfinite(x) && std::isfinite(y)) {
      output->push_back({x, y});
    }
  }
  return true;
}

bool ReadRaggedContour(const ContourSource &source, size_t row,
                       std::vector<SubjectMaskOverlayPoint> *output) {
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
  auto slice = SliceFirstDimension(source.ragged_points,
                                   static_cast<ts::Index>(start),
                                   static_cast<ts::Index>(stop));
  auto points = ts::Read(slice).result();
  if (!points.ok() || points->rank() != 2 || points->shape()[1] < 2) {
    return false;
  }
  const auto strides = points->byte_strides();
  if (strides.size() != 2) {
    return false;
  }
  const auto *origin = reinterpret_cast<const uint8_t *>(
      points->byte_strided_origin_pointer().get());
  const size_t count = static_cast<size_t>(points->shape()[0]);
  output->reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto *point = origin + static_cast<ts::Index>(index) * strides[0];
    const float x = *reinterpret_cast<const float *>(point);
    const float y = *reinterpret_cast<const float *>(point + strides[1]);
    if (std::isfinite(x) && std::isfinite(y)) {
      output->push_back({x, y});
    }
  }
  return true;
}

class TensorStoreSubjectMaskOverlayRepository final
    : public SubjectMaskOverlayRepository {
public:
  TensorStoreSubjectMaskOverlayRepository(
      SubjectMaskOverlayDescriptor descriptor, std::vector<RowMetadata> rows,
      std::vector<uint8_t> available_channels,
      ts::TensorStore<uint8_t, 4> dense, ts::TensorStore<uint8_t, 4> bitpacked,
      std::vector<RleSource> rle, std::vector<ContourSource> contours)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)),
        available_channels_(std::move(available_channels)),
        dense_(std::move(dense)), bitpacked_(std::move(bitpacked)),
        rle_(std::move(rle)), contours_(std::move(contours)) {
    for (size_t index = 0; index < rows_.size(); ++index) {
      rows_by_frame_[rows_[index].camera_frame].push_back(index);
    }
  }

  const SubjectMaskOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  SubjectMaskOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
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
    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end()) {
      result.status = SubjectMaskOverlayStatus::Missing;
      return result;
    }

    result.status = SubjectMaskOverlayStatus::Mapped;
    result.detections.reserve(found->second.size());
    for (const size_t row_index : found->second) {
      SubjectMaskOverlayDetection detection;
      const auto &metadata = rows_[row_index];
      detection.detection_index = metadata.detection_index;
      detection.source_crop_row_id = metadata.source_crop_row_id;
      detection.roi_x = metadata.roi_x;
      detection.roi_y = metadata.roi_y;
      detection.roi_width = metadata.roi_width;
      detection.roi_height = metadata.roi_height;
      detection.components.resize(descriptor_.component_labels.size());
      for (size_t channel = 0; channel < detection.components.size();
           ++channel) {
        auto &component = detection.components[channel];
        component.label = descriptor_.component_labels[channel];
        component.channel_index = channel;
        component.mask_width = descriptor_.mask_width;
        component.mask_height = descriptor_.mask_height;
      }

      std::string read_error;
      if (!readMasks(metadata.mask_row, &detection.components, &read_error)) {
        result.status = SubjectMaskOverlayStatus::ReadFailed;
        result.error = std::move(read_error);
        result.detections.clear();
        return result;
      }
      for (size_t channel = 0; channel < detection.components.size();
           ++channel) {
        if (channel >= contours_.size()) {
          continue;
        }
        auto &points = detection.components[channel].contour;
        bool contour_ok = true;
        if (contours_[channel].kind == ContourKind::Sampled) {
          contour_ok = ReadSampledContour(contours_[channel], metadata.mask_row,
                                          &points);
        } else if (contours_[channel].kind == ContourKind::Ragged) {
          contour_ok =
              ReadRaggedContour(contours_[channel], metadata.mask_row, &points);
        }
        if (!contour_ok) {
          points.clear();
        }
        for (auto &point : points) {
          point.x += metadata.roi_x;
          point.y += metadata.roi_y;
        }
      }
      result.detections.push_back(std::move(detection));
    }
    return result;
  }

private:
  bool readMasks(size_t row,
                 std::vector<SubjectMaskOverlayComponent> *components,
                 std::string *error) const {
    switch (descriptor_.storage) {
    case SubjectMaskStorage::Dense:
      return readDense(row, components, error);
    case SubjectMaskStorage::Bitpacked:
      return readBitpacked(row, components, error);
    case SubjectMaskStorage::Rle:
      return readRle(row, components, error);
    }
    *error = "Unsupported subject-mask storage";
    return false;
  }

  bool readDense(size_t row,
                 std::vector<SubjectMaskOverlayComponent> *components,
                 std::string *error) const {
    auto slice = SliceFirstDimension(dense_, static_cast<ts::Index>(row),
                                     static_cast<ts::Index>(row + 1));
    auto read = ts::Read(slice).result();
    if (!read.ok() || read->rank() != 4 || read->shape()[0] != 1 ||
        static_cast<size_t>(read->shape()[1]) < components->size()) {
      *error = read.ok() ? "Dense mask row has an unexpected shape"
                         : read.status().ToString();
      return false;
    }
    const auto strides = read->byte_strides();
    if (strides.size() != 4) {
      *error = "Dense mask row has an unexpected stride rank";
      return false;
    }
    const auto *origin = reinterpret_cast<const uint8_t *>(
        read->byte_strided_origin_pointer().get());
    for (size_t channel = 0; channel < components->size(); ++channel) {
      if (channel >= available_channels_.size() ||
          available_channels_[channel] == 0) {
        continue;
      }
      auto mask = std::make_shared<std::vector<uint8_t>>(
          descriptor_.mask_width * descriptor_.mask_height, 0);
      bool present = false;
      for (size_t y = 0; y < descriptor_.mask_height; ++y) {
        for (size_t x = 0; x < descriptor_.mask_width; ++x) {
          const auto *value = origin +
                              static_cast<ts::Index>(channel) * strides[1] +
                              static_cast<ts::Index>(y) * strides[2] +
                              static_cast<ts::Index>(x) * strides[3];
          const uint8_t binary = *value == 0 ? 0 : 255;
          (*mask)[y * descriptor_.mask_width + x] = binary;
          present = present || binary != 0;
        }
      }
      (*components)[channel].present = present;
      if (present) {
        (*components)[channel].mask = std::move(mask);
      }
    }
    return true;
  }

  bool readBitpacked(size_t row,
                     std::vector<SubjectMaskOverlayComponent> *components,
                     std::string *error) const {
    auto slice = SliceFirstDimension(bitpacked_, static_cast<ts::Index>(row),
                                     static_cast<ts::Index>(row + 1));
    auto read = ts::Read(slice).result();
    if (!read.ok() || read->rank() != 4 || read->shape()[0] != 1 ||
        static_cast<size_t>(read->shape()[1]) < components->size()) {
      *error = read.ok() ? "Bitpacked mask row has an unexpected shape"
                         : read.status().ToString();
      return false;
    }
    const auto strides = read->byte_strides();
    if (strides.size() != 4) {
      *error = "Bitpacked mask row has an unexpected stride rank";
      return false;
    }
    const auto *origin = reinterpret_cast<const uint8_t *>(
        read->byte_strided_origin_pointer().get());
    const size_t packed_width = static_cast<size_t>(read->shape()[3]);
    for (size_t channel = 0; channel < components->size(); ++channel) {
      if (channel >= available_channels_.size() ||
          available_channels_[channel] == 0) {
        continue;
      }
      auto mask = std::make_shared<std::vector<uint8_t>>(
          descriptor_.mask_width * descriptor_.mask_height, 0);
      bool present = false;
      for (size_t y = 0; y < descriptor_.mask_height; ++y) {
        for (size_t x = 0; x < descriptor_.mask_width; ++x) {
          const size_t packed_x = x / 8;
          const auto *packed_value =
              origin + static_cast<ts::Index>(channel) * strides[1] +
              static_cast<ts::Index>(y) * strides[2] +
              static_cast<ts::Index>(packed_x) * strides[3];
          const uint8_t packed = *packed_value;
          const uint8_t binary = ((packed >> (x % 8)) & 1U) != 0 ? 255 : 0;
          (*mask)[y * descriptor_.mask_width + x] = binary;
          present = present || binary != 0;
        }
      }
      (*components)[channel].present = present;
      if (present) {
        (*components)[channel].mask = std::move(mask);
      }
    }
    return true;
  }

  bool readRle(size_t row, std::vector<SubjectMaskOverlayComponent> *components,
               std::string *error) const {
    const size_t total = descriptor_.mask_width * descriptor_.mask_height;
    for (size_t channel = 0; channel < components->size(); ++channel) {
      if (channel >= available_channels_.size() ||
          available_channels_[channel] == 0 || channel >= rle_.size() ||
          !rle_[channel].available || row >= rle_[channel].present.size() ||
          rle_[channel].present[row] == 0) {
        continue;
      }
      const auto &source = rle_[channel];
      if (row + 1 >= source.indptr.size() || source.indptr[row] < 0 ||
          source.indptr[row + 1] < source.indptr[row]) {
        *error = "RLE row pointer is invalid";
        return false;
      }
      const int64_t start = source.indptr[row];
      const int64_t stop = source.indptr[row + 1];
      if (stop > source.counts.domain().shape()[0]) {
        *error = "RLE row pointer exceeds the count array";
        return false;
      }
      auto slice = SliceFirstDimension(source.counts,
                                       static_cast<ts::Index>(start),
                                       static_cast<ts::Index>(stop));
      auto read = ts::Read(slice).result();
      if (!read.ok() || read->rank() != 1) {
        *error = read.ok() ? "RLE count row has an unexpected shape"
                           : read.status().ToString();
        return false;
      }
      const auto strides = read->byte_strides();
      if (strides.size() != 1) {
        *error = "RLE count row has an unexpected stride rank";
        return false;
      }
      const auto *origin = reinterpret_cast<const uint8_t *>(
          read->byte_strided_origin_pointer().get());
      const size_t count_count = static_cast<size_t>(read->shape()[0]);
      auto mask = std::make_shared<std::vector<uint8_t>>(total, 0);
      size_t offset = 0;
      bool foreground = false;
      bool present = false;
      for (size_t index = 0; index < count_count; ++index) {
        const size_t count = *reinterpret_cast<const uint32_t *>(
            origin + static_cast<ts::Index>(index) * strides[0]);
        if (offset + count > total) {
          *error = "RLE count sum exceeds the mask dimensions";
          return false;
        }
        if (foreground) {
          for (size_t flat = offset; flat < offset + count; ++flat) {
            const size_t y = flat % descriptor_.mask_height;
            const size_t x = flat / descriptor_.mask_height;
            (*mask)[y * descriptor_.mask_width + x] = 255;
          }
          present = present || count != 0;
        }
        offset += count;
        foreground = !foreground;
      }
      if (offset != total) {
        *error = "RLE count sum does not match the mask dimensions";
        return false;
      }
      (*components)[channel].present = present;
      if (present) {
        (*components)[channel].mask = std::move(mask);
      }
    }
    return true;
  }

  SubjectMaskOverlayDescriptor descriptor_;
  std::vector<RowMetadata> rows_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
  std::vector<uint8_t> available_channels_;
  ts::TensorStore<uint8_t, 4> dense_;
  ts::TensorStore<uint8_t, 4> bitpacked_;
  std::vector<RleSource> rle_;
  std::vector<ContourSource> contours_;
};

ContourSource OpenContour(const ArchiveContext::Impl &archive,
                          const std::string &run_base, const std::string &label,
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

} // namespace

std::unique_ptr<SubjectMaskOverlayRepository>
OpenSubjectMaskOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 const std::string &requested_run,
                                 std::string *error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto &impl = *archive->impl_;
  constexpr const char *group = "refined_subject_masks_runs";
  std::string run_name = requested_run;
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

  SubjectMaskOverlayDescriptor descriptor;
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

  std::vector<int64_t> frames;
  std::vector<int64_t> detections;
  std::vector<int64_t> source_rows;
  if (!ReadIntegers(impl, run_base + "/frame_indices", &frames) ||
      frames.empty()) {
    internal::SetArchiveError(error_message,
                              "Subject-mask frame_indices are unreadable");
    return nullptr;
  }
  descriptor.row_count = frames.size();
  if (!ReadIntegers(impl, run_base + "/detection_indices", &detections) ||
      detections.size() != frames.size()) {
    detections.assign(frames.size(), -1);
  }
  if (!ReadIntegers(impl, run_base + "/source_crop_row_ids", &source_rows) ||
      source_rows.size() != frames.size()) {
    source_rows.clear();
  }

  const std::string crop_base = "crop_runs/" + descriptor.source_crop_run;
  const auto crop_attributes = internal::ReadArchiveAttributes(impl, crop_base);
  std::vector<int64_t> crop_frames;
  std::vector<int64_t> crop_detections;
  std::vector<std::vector<double>> crop_offsets;
  if (!crop_attributes ||
      !ReadIntegers(impl, crop_base + "/frame_indices", &crop_frames) ||
      crop_frames.empty() ||
      !ReadNumericMatrix(impl, crop_base + "/roi_coordinates_full", 2,
                         &crop_offsets) ||
      crop_offsets.size() != crop_frames.size()) {
    internal::SetArchiveError(error_message,
                              "Source crop placement metadata is unreadable");
    return nullptr;
  }
  if (!ReadIntegers(impl, crop_base + "/detection_indices", &crop_detections) ||
      crop_detections.size() != crop_frames.size()) {
    crop_detections.assign(crop_frames.size(), -1);
  }

  double roi_width = 0.0;
  double roi_height = 0.0;
  ReadRoiSize(*crop_attributes, &roi_width, &roi_height);

  ts::TensorStore<uint8_t, 4> dense;
  ts::TensorStore<uint8_t, 4> bitpacked;
  std::vector<RleSource> rle(descriptor.component_labels.size());
  if (auto store = OpenArray<uint8_t, 4>(impl, run_base + "/masks_roi")) {
    const auto shape = store->domain().shape();
    if (shape[0] == static_cast<ts::Index>(frames.size()) &&
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
          (*logical)[0].get<int64_t>() == static_cast<int64_t>(frames.size())) {
        const int64_t logical_channels = (*logical)[1].get<int64_t>();
        const int64_t logical_height = (*logical)[2].get<int64_t>();
        const int64_t logical_width = (*logical)[3].get<int64_t>();
        const auto physical = store->domain().shape();
        if (logical_channels >=
                static_cast<int64_t>(descriptor.component_labels.size()) &&
            logical_height > 0 && logical_width > 0 &&
            physical[0] == static_cast<ts::Index>(frames.size()) &&
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
              rle[channel].indptr.size() != frames.size() + 1 ||
              rle[channel].present.size() != frames.size()) {
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
  if (roi_width <= 0.0 || roi_height <= 0.0) {
    roi_width = descriptor.mask_width;
    roi_height = descriptor.mask_height;
  }

  std::vector<uint8_t> available;
  if (!ReadBools(impl, run_base + "/available_channels", &available) ||
      available.size() != descriptor.component_labels.size()) {
    available.assign(descriptor.component_labels.size(), 1);
  }

  std::vector<RowMetadata> rows;
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
      internal::SetArchiveError(
          error_message,
          "Legacy subject-mask rows are not aligned with the source crop run");
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
    rows.push_back(
        {index, frames[index],
         detections[index] >= 0 ? detections[index] : crop_detections[crop_row],
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

  const bool contours_stale = BoolValue(*run_attributes, "contours_stale");
  std::vector<ContourSource> contours;
  contours.reserve(descriptor.component_labels.size());
  for (const auto &label : descriptor.component_labels) {
    contours.push_back(
        OpenContour(impl, run_base, label, frames.size(), contours_stale));
  }

  return std::make_unique<TensorStoreSubjectMaskOverlayRepository>(
      std::move(descriptor), std::move(rows), std::move(available),
      std::move(dense), std::move(bitpacked), std::move(rle),
      std::move(contours));
}

} // namespace crimson::zarr
