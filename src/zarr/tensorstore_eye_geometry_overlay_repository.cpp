#include "zarr/tensorstore_eye_geometry_overlay_repository.h"

#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zarr/archive_context_internal.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

std::optional<json> MakeArraySpec(const ArchiveContext::Impl &archive,
                                  const std::string &path) {
  return internal::MakeReadOnlyArraySpec(archive, path);
}

template <typename T, size_t Rank>
std::optional<ts::TensorStore<T, Rank>>
OpenArray(const ArchiveContext::Impl &archive, const std::string &path) {
  const auto spec = MakeArraySpec(archive, path);
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
auto SliceFirstDimension(const ts::TensorStore<T, Rank> &store, ts::Index start,
                         ts::Index stop) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = start;
  domain.shape()[0] = stop - start;
  return store | ts::IdentityTransform(domain);
}

std::string StringValue(const json &attributes, const char *key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

int IntegerValue(const json &attributes, const char *key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_number_integer()
             ? found->get<int>()
             : 0;
}

std::string MethodVersion(const json &attributes) {
  const auto found = attributes.find("method_version");
  if (found == attributes.end()) {
    return {};
  }
  return found->is_string() ? found->get<std::string>() : found->dump();
}

bool ValidRunName(const std::string &name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos;
}

std::string LatestRun(const ArchiveContext::Impl &archive,
                      const std::string &group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char *, 4> keys = {
      "latest_complete", "latest_completed", "latest", "latest_success"};
  for (const char *key : keys) {
    const auto value = StringValue(*attributes, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

template <typename Source>
bool ReadIntegerVector(const ArchiveContext::Impl &archive,
                       const std::string &path, std::vector<int64_t> *out) {
  const auto store = OpenArray<Source, 1>(archive, path);
  if (!store) {
    return false;
  }
  const auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const auto *values = static_cast<const Source *>(read->data());
  out->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*out)[index] = static_cast<int64_t>(values[index]);
  }
  return true;
}

bool ReadIntegers(const ArchiveContext::Impl &archive, const std::string &path,
                  std::vector<int64_t> *out) {
  return ReadIntegerVector<int64_t>(archive, path, out) ||
         ReadIntegerVector<int32_t>(archive, path, out) ||
         ReadIntegerVector<uint64_t>(archive, path, out) ||
         ReadIntegerVector<uint32_t>(archive, path, out);
}

template <typename Source>
bool ReadBoolVector(const ArchiveContext::Impl &archive,
                    const std::string &path, std::vector<uint8_t> *out) {
  const auto store = OpenArray<Source, 1>(archive, path);
  if (!store) {
    return false;
  }
  const auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const auto *values = static_cast<const Source *>(read->data());
  out->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*out)[index] = values[index] ? 1 : 0;
  }
  return true;
}

bool ReadBools(const ArchiveContext::Impl &archive, const std::string &path,
               std::vector<uint8_t> *out) {
  return ReadBoolVector<bool>(archive, path, out) ||
         ReadBoolVector<uint8_t>(archive, path, out) ||
         ReadBoolVector<uint16_t>(archive, path, out);
}

template <typename Source>
bool ReadMatrix(const ArchiveContext::Impl &archive, const std::string &path,
                size_t minimum_columns, std::vector<std::vector<double>> *out) {
  const auto store = OpenArray<Source, 2>(archive, path);
  if (!store) {
    return false;
  }
  const auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 2 ||
      read->shape()[1] < static_cast<ts::Index>(minimum_columns)) {
    return false;
  }
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  const size_t columns = static_cast<size_t>(read->shape()[1]);
  const auto *values = static_cast<const Source *>(read->data());
  out->assign(rows, std::vector<double>(columns));
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      (*out)[row][column] = static_cast<double>(values[row * columns + column]);
    }
  }
  return true;
}

bool ReadNumericMatrix(const ArchiveContext::Impl &archive,
                       const std::string &path, size_t minimum_columns,
                       std::vector<std::vector<double>> *out) {
  return ReadMatrix<float>(archive, path, minimum_columns, out) ||
         ReadMatrix<double>(archive, path, minimum_columns, out) ||
         ReadMatrix<int32_t>(archive, path, minimum_columns, out);
}

std::vector<std::string> ReadNames(const ArchiveContext::Impl &archive,
                                   const std::string &path) {
  std::vector<std::string> names;
  const auto store = OpenArray<uint8_t, 2>(archive, path);
  if (!store) {
    return names;
  }
  const auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 2) {
    return names;
  }
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  const size_t columns = static_cast<size_t>(read->shape()[1]);
  const auto *values = static_cast<const uint8_t *>(read->data());
  names.reserve(rows);
  for (size_t row = 0; row < rows; ++row) {
    const char *text = reinterpret_cast<const char *>(values + row * columns);
    size_t length = 0;
    while (length < columns && text[length] != '\0') {
      ++length;
    }
    names.emplace_back(text, length);
  }
  return names;
}

std::unordered_map<std::string, size_t>
AvailableChannels(const ArchiveContext::Impl &archive, const std::string &base,
                  const std::string &availability) {
  const auto names = ReadNames(archive, base + "/name");
  std::vector<uint8_t> available;
  ReadBools(archive, base + "/" + availability, &available);
  std::unordered_map<std::string, size_t> result;
  for (size_t index = 0; index < names.size(); ++index) {
    if (!names[index].empty() &&
        (available.empty() ||
         (index < available.size() && available[index] != 0))) {
      result[names[index]] = index;
    }
  }
  return result;
}

bool ReadRoiSize(const json &attributes, double *width, double *height) {
  const auto found = attributes.find("roi_size");
  if (found == attributes.end() || !found->is_array() || found->size() < 2 ||
      !(*found)[0].is_number() || !(*found)[1].is_number()) {
    return false;
  }
  *height = (*found)[0].get<double>();
  *width = (*found)[1].get<double>();
  return *width > 0.0 && *height > 0.0;
}

EyeGeometryAxis AxisFromEllipse(const std::vector<double> &ellipse,
                                bool major_axis) {
  EyeGeometryAxis axis;
  if (ellipse.size() < 5) {
    return axis;
  }
  const double cx = ellipse[0];
  const double cy = ellipse[1];
  const double length = major_axis ? ellipse[2] : ellipse[3];
  constexpr double kPi = 3.14159265358979323846;
  double radians = ellipse[4] * kPi / 180.0;
  if (!major_axis) {
    radians += kPi * 0.5;
  }
  if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(length) ||
      !std::isfinite(radians) || length <= 1e-5) {
    return axis;
  }
  const double dx = std::cos(radians) * length * 0.5;
  const double dy = std::sin(radians) * length * 0.5;
  axis.valid = true;
  axis.start = {cx - dx, cy - dy};
  axis.end = {cx + dx, cy + dy};
  return axis;
}

struct BoolArray {
  std::optional<ts::TensorStore<bool, 1>> booleans;
  std::optional<ts::TensorStore<uint8_t, 1>> bytes;

  bool available() const { return booleans.has_value() || bytes.has_value(); }
};

BoolArray OpenBoolArray(const ArchiveContext::Impl &archive,
                        const std::string &path, size_t rows) {
  BoolArray result;
  if (auto values = OpenArray<bool, 1>(archive, path);
      values && values->domain().shape()[0] == static_cast<ts::Index>(rows)) {
    result.booleans = std::move(*values);
  } else if (auto values = OpenArray<uint8_t, 1>(archive, path);
             values &&
             values->domain().shape()[0] == static_cast<ts::Index>(rows)) {
    result.bytes = std::move(*values);
  }
  return result;
}

template <typename Source>
std::optional<std::vector<uint8_t>>
ReadBoolRange(const ts::TensorStore<Source, 1> &store, size_t start,
              size_t stop) {
  if (stop <= start) {
    return std::nullopt;
  }
  const auto read =
      ts::Read(SliceFirstDimension(store, static_cast<ts::Index>(start),
                                   static_cast<ts::Index>(stop)))
          .result();
  if (!read.ok() || read->rank() != 1 ||
      read->shape()[0] != static_cast<ts::Index>(stop - start) ||
      read->byte_strides().size() != 1) {
    return std::nullopt;
  }
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  std::vector<uint8_t> values(stop - start);
  for (size_t index = 0; index < values.size(); ++index) {
    values[index] =
        *reinterpret_cast<const Source *>(
            origin + static_cast<ts::Index>(index) * read->byte_strides()[0])
            ? 1
            : 0;
  }
  return values;
}

std::optional<std::vector<uint8_t>> ReadBoolRange(const BoolArray &source,
                                                  size_t start, size_t stop) {
  if (source.booleans) {
    return ReadBoolRange(*source.booleans, start, stop);
  }
  if (source.bytes) {
    return ReadBoolRange(*source.bytes, start, stop);
  }
  return std::nullopt;
}

template <typename Source>
std::optional<std::vector<std::vector<double>>>
ReadMatrixRange(const ts::TensorStore<Source, 2> &store, size_t start,
                size_t stop) {
  if (stop <= start) {
    return std::nullopt;
  }
  const auto read =
      ts::Read(SliceFirstDimension(store, static_cast<ts::Index>(start),
                                   static_cast<ts::Index>(stop)))
          .result();
  if (!read.ok() || read->rank() != 2 ||
      read->shape()[0] != static_cast<ts::Index>(stop - start) ||
      read->byte_strides().size() != 2) {
    return std::nullopt;
  }
  const auto strides = read->byte_strides();
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  const size_t columns = static_cast<size_t>(read->shape()[1]);
  std::vector<std::vector<double>> values(stop - start,
                                          std::vector<double>(columns));
  for (size_t row = 0; row < values.size(); ++row) {
    for (size_t column = 0; column < columns; ++column) {
      const auto *value = origin + static_cast<ts::Index>(row) * strides[0] +
                          static_cast<ts::Index>(column) * strides[1];
      values[row][column] =
          static_cast<double>(*reinterpret_cast<const Source *>(value));
    }
  }
  return values;
}

std::optional<std::vector<std::vector<EyeGeometryPoint>>>
ReadVectorRange(const ts::TensorStore<float, 3> &store, size_t start,
                size_t stop) {
  if (stop <= start) {
    return std::nullopt;
  }
  const auto read =
      ts::Read(SliceFirstDimension(store, static_cast<ts::Index>(start),
                                   static_cast<ts::Index>(stop)))
          .result();
  if (!read.ok() || read->rank() != 3 ||
      read->shape()[0] != static_cast<ts::Index>(stop - start) ||
      read->shape()[2] < 2 || read->byte_strides().size() != 3) {
    return std::nullopt;
  }
  const auto strides = read->byte_strides();
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  const size_t channels = static_cast<size_t>(read->shape()[1]);
  std::vector<std::vector<EyeGeometryPoint>> values(
      stop - start, std::vector<EyeGeometryPoint>(channels));
  for (size_t row = 0; row < values.size(); ++row) {
    for (size_t channel = 0; channel < channels; ++channel) {
      const auto *value = origin + static_cast<ts::Index>(row) * strides[0] +
                          static_cast<ts::Index>(channel) * strides[1];
      values[row][channel] = {
          *reinterpret_cast<const float *>(value),
          *reinterpret_cast<const float *>(value + strides[2])};
    }
  }
  return values;
}

std::optional<size_t>
Channel(const std::unordered_map<std::string, size_t> &channels,
        std::initializer_list<const char *> names) {
  for (const char *name : names) {
    const auto found = channels.find(name);
    if (found != channels.end()) {
      return found->second;
    }
  }
  return std::nullopt;
}

struct Placement {
  size_t row = 0;
  int64_t camera_frame = -1;
  int64_t detection_index = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
};

struct LazySources {
  ts::TensorStore<float, 2> left_ellipse;
  ts::TensorStore<float, 2> right_ellipse;
  BoolArray left_ellipse_valid;
  BoolArray right_ellipse_valid;
  ts::TensorStore<float, 2> body_origin;
  ts::TensorStore<float, 2> body_forward;
  ts::TensorStore<float, 2> body_left;
  BoolArray body_valid;
  ts::TensorStore<float, 2> angles;
  ts::TensorStore<float, 3> vectors;
  ts::TensorStore<uint16_t, 2> qa;
  std::optional<size_t> left_eye_frame;
  std::optional<size_t> right_eye_frame;
  std::optional<size_t> vergence;
  std::optional<size_t> left_signed;
  std::optional<size_t> right_signed;
  std::optional<size_t> left_gaze;
  std::optional<size_t> right_gaze;
  std::optional<size_t> frame_valid;
  std::optional<size_t> left_valid;
  std::optional<size_t> right_valid;
};

struct GeometryChunk {
  size_t start_row = 0;
  std::vector<EyeGeometryOverlayDetection> rows;
};

class LazyRepository final : public EyeGeometryOverlayRepository {
public:
  LazyRepository(EyeGeometryOverlayDescriptor descriptor,
                 std::vector<Placement> placements, LazySources sources)
      : descriptor_(std::move(descriptor)), placements_(std::move(placements)),
        sources_(std::move(sources)) {
    for (size_t index = 0; index < placements_.size(); ++index) {
      rows_by_frame_[placements_[index].camera_frame].push_back(index);
    }
  }

  const EyeGeometryOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  EyeGeometryOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    EyeGeometryOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 || (descriptor_.camera_frame_count > 0 &&
                             static_cast<uint64_t>(camera_frame) >=
                                 descriptor_.camera_frame_count)) {
      result.status = EyeGeometryOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = EyeGeometryOverlayStatus::InvalidDimensions;
      return result;
    }
    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end()) {
      result.status = EyeGeometryOverlayStatus::Missing;
      return result;
    }
    for (const size_t placement_index : found->second) {
      const auto &placement = placements_[placement_index];
      EyeGeometryOverlayDetection detection;
      if (!readGeometry(placement.row, &detection)) {
        result.status = EyeGeometryOverlayStatus::ReadFailed;
        result.error =
            "Failed to read eye-geometry row " + std::to_string(placement.row);
        result.detections.clear();
        return result;
      }
      detection.eye_row = placement.row;
      detection.camera_frame = camera_frame;
      detection.detection_index = placement.detection_index;
      detection.source_crop_row_id = placement.source_crop_row_id;
      detection.roi_x = placement.roi_x;
      detection.roi_y = placement.roi_y;
      detection.roi_width = placement.roi_width;
      detection.roi_height = placement.roi_height;
      result.detections.push_back(std::move(detection));
    }
    result.status = result.detections.empty()
                        ? EyeGeometryOverlayStatus::Missing
                        : EyeGeometryOverlayStatus::Mapped;
    return result;
  }

  RepositoryMemoryMetrics memoryMetrics() const override {
    RepositoryMemoryMetrics metrics;
    metrics.retained_metadata_bytes =
        memory::vectorAllocationBytes(placements_);
    metrics.retained_index_bytes =
        memory::vectorMapAllocationLowerBound(rows_by_frame_);
    std::lock_guard<std::mutex> lock(chunk_mutex_);
    for (const auto &chunk : chunks_) {
      metrics.decoded_cache_bytes += memory::vectorAllocationBytes(chunk.rows);
    }
    return metrics;
  }

private:
  bool readGeometry(size_t row, EyeGeometryOverlayDetection *geometry) const {
    {
      std::lock_guard<std::mutex> lock(chunk_mutex_);
      for (const auto &chunk : chunks_) {
        if (row >= chunk.start_row &&
            row - chunk.start_row < chunk.rows.size()) {
          *geometry = chunk.rows[row - chunk.start_row];
          return true;
        }
      }
    }
    GeometryChunk loaded;
    if (!loadGeometryChunk(row, &loaded)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(chunk_mutex_);
    for (const auto &chunk : chunks_) {
      if (row >= chunk.start_row && row - chunk.start_row < chunk.rows.size()) {
        *geometry = chunk.rows[row - chunk.start_row];
        return true;
      }
    }
    chunks_.push_back(std::move(loaded));
    while (chunks_.size() > 2) {
      chunks_.pop_front();
    }
    const auto &chunk = chunks_.back();
    *geometry = chunk.rows[row - chunk.start_row];
    return true;
  }

  bool loadGeometryChunk(size_t row, GeometryChunk *chunk) const {
    constexpr size_t kRowsPerChunk = 256;
    const size_t start = row / kRowsPerChunk * kRowsPerChunk;
    const size_t stop = std::min(descriptor_.row_count, start + kRowsPerChunk);

    auto left_ellipse_future = std::async(std::launch::async, [&] {
      return ReadMatrixRange(sources_.left_ellipse, start, stop);
    });
    auto right_ellipse_future = std::async(std::launch::async, [&] {
      return ReadMatrixRange(sources_.right_ellipse, start, stop);
    });
    auto left_ellipse_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.left_ellipse_valid, start, stop);
    });
    auto right_ellipse_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.right_ellipse_valid, start, stop);
    });
    auto body_origin_future = std::async(std::launch::async, [&] {
      return ReadMatrixRange(sources_.body_origin, start, stop);
    });
    auto body_forward_future = std::async(std::launch::async, [&] {
      return ReadMatrixRange(sources_.body_forward, start, stop);
    });
    auto body_left_future = std::async(std::launch::async, [&] {
      return ReadMatrixRange(sources_.body_left, start, stop);
    });
    auto body_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.body_valid, start, stop);
    });
    auto angles_future = std::async(std::launch::async, [&] {
      return ReadMatrixRange(sources_.angles, start, stop);
    });
    auto vectors_future = std::async(std::launch::async, [&] {
      return ReadVectorRange(sources_.vectors, start, stop);
    });
    auto qa_future = std::async(std::launch::async, [&] {
      return ReadMatrixRange(sources_.qa, start, stop);
    });

    const auto left_ellipse = left_ellipse_future.get();
    const auto right_ellipse = right_ellipse_future.get();
    const auto left_ellipse_valid = left_ellipse_valid_future.get();
    const auto right_ellipse_valid = right_ellipse_valid_future.get();
    const auto body_origin = body_origin_future.get();
    const auto body_forward = body_forward_future.get();
    const auto body_left = body_left_future.get();
    const auto body_valid = body_valid_future.get();
    const auto angles = angles_future.get();
    const auto vectors = vectors_future.get();
    const auto qa = qa_future.get();
    if (!left_ellipse || !right_ellipse || !left_ellipse_valid ||
        !right_ellipse_valid || !body_origin || !body_forward || !body_left ||
        !body_valid || !angles || !vectors || !qa) {
      return false;
    }

    chunk->start_row = start;
    chunk->rows.resize(stop - start);
    for (size_t index = 0; index < chunk->rows.size(); ++index) {
      auto qa_value = [&](std::optional<size_t> channel, bool fallback) {
        return channel && *channel < (*qa)[index].size()
                   ? (*qa)[index][*channel] != 0.0
                   : fallback;
      };
      auto scalar = [&](std::optional<size_t> channel, double *value) {
        if (!channel || *channel >= (*angles)[index].size() ||
            !std::isfinite((*angles)[index][*channel])) {
          return false;
        }
        *value = (*angles)[index][*channel];
        return true;
      };
      auto &geometry = chunk->rows[index];
      geometry.frame_valid = qa_value(sources_.frame_valid, true);
      geometry.body_frame_valid = (*body_valid)[index] != 0;
      geometry.body_origin = {(*body_origin)[index][0],
                              (*body_origin)[index][1]};
      geometry.body_forward_axis = {(*body_forward)[index][0],
                                    (*body_forward)[index][1]};
      geometry.body_left_axis = {(*body_left)[index][0],
                                 (*body_left)[index][1]};
      const std::array<bool, 2> ellipse_valid = {
          (*left_ellipse_valid)[index] != 0,
          (*right_ellipse_valid)[index] != 0};
      const std::array<const std::vector<double> *, 2> ellipses = {
          &(*left_ellipse)[index], &(*right_ellipse)[index]};
      const std::array<std::optional<size_t>, 2> valid_channels = {
          sources_.left_valid, sources_.right_valid};
      const std::array<std::optional<size_t>, 2> frame_channels = {
          sources_.left_eye_frame, sources_.right_eye_frame};
      const std::array<std::optional<size_t>, 2> signed_channels = {
          sources_.left_signed, sources_.right_signed};
      const std::array<std::optional<size_t>, 2> gaze_channels = {
          sources_.left_gaze, sources_.right_gaze};
      for (size_t eye = 0; eye < 2; ++eye) {
        auto &target = geometry.eyes[eye];
        target.valid =
            geometry.frame_valid && qa_value(valid_channels[eye], true);
        if (ellipse_valid[eye]) {
          target.major_axis = AxisFromEllipse(*ellipses[eye], true);
          target.minor_axis = AxisFromEllipse(*ellipses[eye], false);
        }
        if (gaze_channels[eye] &&
            *gaze_channels[eye] < (*vectors)[index].size()) {
          target.gaze = (*vectors)[index][*gaze_channels[eye]];
          target.gaze_valid = std::isfinite(target.gaze.x) &&
                              std::isfinite(target.gaze.y) &&
                              std::hypot(target.gaze.x, target.gaze.y) > 1e-6;
        }
        target.signed_angle_valid =
            scalar(signed_channels[eye], &target.signed_angle_degrees);
        target.eye_frame_angle_valid =
            scalar(frame_channels[eye], &target.eye_frame_angle_degrees);
      }
      geometry.vergence_valid =
          scalar(sources_.vergence, &geometry.vergence_degrees);
      if (!geometry.vergence_valid && geometry.eyes[0].eye_frame_angle_valid &&
          geometry.eyes[1].eye_frame_angle_valid) {
        geometry.vergence_degrees = geometry.eyes[0].eye_frame_angle_degrees +
                                    geometry.eyes[1].eye_frame_angle_degrees;
        geometry.vergence_valid = true;
      }
    }
    return true;
  }

  EyeGeometryOverlayDescriptor descriptor_;
  std::vector<Placement> placements_;
  LazySources sources_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
  mutable std::mutex chunk_mutex_;
  mutable std::deque<GeometryChunk> chunks_;
};

} // namespace

std::unique_ptr<EyeGeometryOverlayRepository>
OpenEyeGeometryOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 const std::string &requested_run,
                                 std::string *error_message) {
  auto fail = [&](std::string message)
      -> std::unique_ptr<EyeGeometryOverlayRepository> {
    if (error_message != nullptr) {
      *error_message = std::move(message);
    }
    return nullptr;
  };
  if (!archive || !archive->impl_) {
    return fail("Archive context is unavailable");
  }
  const auto &impl = *archive->impl_;
  const std::string group = "analysis/eye_angle_runs";
  const std::string run =
      requested_run.empty() ? LatestRun(impl, group) : requested_run;
  if (!ValidRunName(run)) {
    return fail("No valid eye-angle run was selected");
  }
  const std::string base = group + "/" + run;
  const auto attributes = internal::ReadArchiveAttributes(impl, base);
  if (!attributes) {
    return fail("Eye-angle run metadata is unavailable: " + run);
  }
  const std::string refined_run =
      StringValue(*attributes, "source_refined_subject_masks_run");
  if (!ValidRunName(refined_run)) {
    return fail("Eye-angle run has no valid refined-mask lineage");
  }
  const std::string refined = "refined_subject_masks_runs/" + refined_run;
  const auto refined_attributes =
      internal::ReadArchiveAttributes(impl, refined);
  if (!refined_attributes) {
    return fail("Source refined-mask run is unavailable: " + refined_run);
  }
  const std::string crop_run =
      StringValue(*refined_attributes, "source_crop_run");
  if (!ValidRunName(crop_run)) {
    return fail("Source refined-mask run has no valid crop lineage");
  }
  const std::string crop = "crop_runs/" + crop_run;
  const auto crop_attributes = internal::ReadArchiveAttributes(impl, crop);
  double roi_width = 0.0;
  double roi_height = 0.0;
  if (!crop_attributes ||
      !ReadRoiSize(*crop_attributes, &roi_width, &roi_height)) {
    return fail("Source crop run has no valid roi_size");
  }

  std::vector<int64_t> eye_frames;
  std::vector<int64_t> refined_frames;
  std::vector<int64_t> source_crop_rows;
  std::vector<int64_t> crop_frames;
  std::vector<int64_t> crop_detections;
  std::vector<std::vector<double>> crop_coordinates;
  if (!ReadIntegers(impl, base + "/support/frame_indices", &eye_frames) ||
      !ReadIntegers(impl, refined + "/frame_indices", &refined_frames) ||
      !ReadIntegers(impl, refined + "/source_crop_row_ids",
                    &source_crop_rows) ||
      !ReadIntegers(impl, crop + "/frame_indices", &crop_frames) ||
      !ReadIntegers(impl, crop + "/detection_indices", &crop_detections) ||
      !ReadNumericMatrix(impl, crop + "/roi_coordinates_full", 2,
                         &crop_coordinates)) {
    return fail("Required eye geometry lineage indexes are unavailable");
  }
  const size_t rows = eye_frames.size();
  if (rows == 0 || refined_frames.size() != rows ||
      source_crop_rows.size() != rows ||
      crop_frames.size() != crop_detections.size() ||
      crop_frames.size() != crop_coordinates.size()) {
    return fail("Eye, refined-mask, and crop lineage counts do not match");
  }

  const auto angle_channels =
      AvailableChannels(impl, base + "/angle_channel_index", "roi_available");
  const auto vector_channels =
      AvailableChannels(impl, base + "/vector_channel_index", "roi_available");
  const auto qa_channels =
      AvailableChannels(impl, base + "/qa_channel_index", "roi_available");
  auto required_matrix =
      [&](const std::string &path,
          size_t columns) -> std::optional<ts::TensorStore<float, 2>> {
    auto store = OpenArray<float, 2>(impl, path);
    return store &&
                   store->domain().shape()[0] == static_cast<ts::Index>(rows) &&
                   store->domain().shape()[1] >= static_cast<ts::Index>(columns)
               ? std::move(store)
               : std::nullopt;
  };
  auto left_ellipse = required_matrix(
      refined + "/components/eye_left/geometry/ellipse_params", 5);
  auto right_ellipse = required_matrix(
      refined + "/components/eye_right/geometry/ellipse_params", 5);
  auto body_origin = required_matrix(base + "/support/body_frame/origin_xy", 2);
  auto body_forward =
      required_matrix(base + "/support/body_frame/forward_axis_xy", 2);
  auto body_left =
      required_matrix(base + "/support/body_frame/left_axis_xy", 2);
  auto angles = required_matrix(base + "/roi_angles", 1);
  auto vectors = OpenArray<float, 3>(impl, base + "/roi_vectors");
  auto qa = OpenArray<uint16_t, 2>(impl, base + "/roi_qa");
  BoolArray left_ellipse_valid = OpenBoolArray(
      impl, refined + "/components/eye_left/geometry/ellipse_success", rows);
  BoolArray right_ellipse_valid = OpenBoolArray(
      impl, refined + "/components/eye_right/geometry/ellipse_success", rows);
  BoolArray body_valid =
      OpenBoolArray(impl, base + "/support/body_frame/valid", rows);
  if (!left_ellipse || !right_ellipse || !left_ellipse_valid.available() ||
      !right_ellipse_valid.available() || !body_origin || !body_forward ||
      !body_left || !body_valid.available() || !angles || !vectors || !qa ||
      vectors->domain().shape()[0] != static_cast<ts::Index>(rows) ||
      vectors->domain().shape()[2] < 2 ||
      qa->domain().shape()[0] != static_cast<ts::Index>(rows)) {
    return fail(
        "Required lazy eye geometry arrays are unavailable or mismatched");
  }

  std::vector<Placement> placements;
  placements.reserve(rows);
  int64_t maximum_frame = -1;
  for (size_t row = 0; row < rows; ++row) {
    if (eye_frames[row] != refined_frames[row]) {
      return fail("Eye and refined-mask frame lineage mismatch at row " +
                  std::to_string(row));
    }
    const int64_t crop_row = source_crop_rows[row];
    if (crop_row < 0 || static_cast<size_t>(crop_row) >= crop_frames.size() ||
        crop_frames[crop_row] != eye_frames[row]) {
      return fail("Eye source crop lineage mismatch at row " +
                  std::to_string(row));
    }
    placements.push_back({row, eye_frames[row], crop_detections[crop_row],
                          crop_row, crop_coordinates[crop_row][0],
                          crop_coordinates[crop_row][1], roi_width,
                          roi_height});
    maximum_frame = std::max(maximum_frame, eye_frames[row]);
  }

  EyeGeometryOverlayDescriptor descriptor;
  descriptor.source_group = group;
  descriptor.run_name = run;
  descriptor.source_refined_subject_masks_run = refined_run;
  descriptor.source_crop_run = crop_run;
  descriptor.schema_id = StringValue(*attributes, "schema_id");
  descriptor.schema_version = IntegerValue(*attributes, "schema_version");
  descriptor.method = StringValue(*attributes, "method");
  descriptor.method_version = MethodVersion(*attributes);
  descriptor.row_axis = StringValue(*attributes, "row_axis");
  descriptor.layout = StringValue(*attributes, "layout");
  descriptor.row_count = rows;
  descriptor.camera_frame_count =
      maximum_frame >= 0 ? static_cast<size_t>(maximum_frame) + 1 : 0;
  descriptor.coordinate_width = static_cast<size_t>(roi_width);
  descriptor.coordinate_height = static_cast<size_t>(roi_height);
  if (error_message != nullptr) {
    error_message->clear();
  }
  LazySources sources;
  sources.left_ellipse = std::move(*left_ellipse);
  sources.right_ellipse = std::move(*right_ellipse);
  sources.left_ellipse_valid = std::move(left_ellipse_valid);
  sources.right_ellipse_valid = std::move(right_ellipse_valid);
  sources.body_origin = std::move(*body_origin);
  sources.body_forward = std::move(*body_forward);
  sources.body_left = std::move(*body_left);
  sources.body_valid = std::move(body_valid);
  sources.angles = std::move(*angles);
  sources.vectors = std::move(*vectors);
  sources.qa = std::move(*qa);
  sources.left_eye_frame = Channel(angle_channels, {"left_eye_angle_deg"});
  sources.right_eye_frame = Channel(angle_channels, {"right_eye_angle_deg"});
  sources.vergence = Channel(angle_channels, {"vergence_eye_angle_deg"});
  sources.left_signed = Channel(
      angle_channels, {"left_gaze_signed_deg", "left_minor_signed_deg"});
  sources.right_signed = Channel(
      angle_channels, {"right_gaze_signed_deg", "right_minor_signed_deg"});
  sources.left_gaze = Channel(vector_channels, {"left_gaze_xy"});
  sources.right_gaze = Channel(vector_channels, {"right_gaze_xy"});
  sources.frame_valid = Channel(qa_channels, {"valid_frame"});
  sources.left_valid = Channel(qa_channels, {"valid_left"});
  sources.right_valid = Channel(qa_channels, {"valid_right"});
  if (!sources.left_eye_frame || !sources.right_eye_frame ||
      !sources.left_gaze || !sources.right_gaze || !sources.frame_valid ||
      !sources.left_valid || !sources.right_valid) {
    return fail(
        "Eye-angle compact channels required for the overlay are missing");
  }
  auto within = [](std::optional<size_t> channel, ts::Index columns) {
    return !channel ||
           (columns >= 0 && *channel < static_cast<size_t>(columns));
  };
  if (!within(sources.left_eye_frame, sources.angles.domain().shape()[1]) ||
      !within(sources.right_eye_frame, sources.angles.domain().shape()[1]) ||
      !within(sources.vergence, sources.angles.domain().shape()[1]) ||
      !within(sources.left_signed, sources.angles.domain().shape()[1]) ||
      !within(sources.right_signed, sources.angles.domain().shape()[1]) ||
      !within(sources.left_gaze, sources.vectors.domain().shape()[1]) ||
      !within(sources.right_gaze, sources.vectors.domain().shape()[1]) ||
      !within(sources.frame_valid, sources.qa.domain().shape()[1]) ||
      !within(sources.left_valid, sources.qa.domain().shape()[1]) ||
      !within(sources.right_valid, sources.qa.domain().shape()[1])) {
    return fail("Eye-angle compact channel index exceeds its array shape");
  }
  return std::make_unique<LazyRepository>(
      std::move(descriptor), std::move(placements), std::move(sources));
}

} // namespace crimson::zarr
