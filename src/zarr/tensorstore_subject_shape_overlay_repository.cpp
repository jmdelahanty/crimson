#include "zarr/tensorstore_subject_shape_overlay_repository.h"

#include "zarr/archive_context_internal.h"

#include <nlohmann/json.hpp>
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
#include <optional>
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

std::string LatestRun(const ArchiveContext::Impl &archive,
                      const std::string &group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char *, 5> keys = {
      "latest_complete", "latest_completed", "latest", "latest_success",
      "latest_subject_shape_run"};
  for (const char *key : keys) {
    const std::string value = StringValue(*attributes, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

bool ValidRunName(const std::string &run_name) {
  return !run_name.empty() && run_name != "." && run_name != ".." &&
         run_name.find('/') == std::string::npos;
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

struct BoolSource {
  bool available = false;
  bool byte_storage = false;
  ts::TensorStore<bool, 1> booleans;
  ts::TensorStore<uint8_t, 1> bytes;
};

BoolSource OpenBoolSource(const ArchiveContext::Impl &archive,
                          const std::string &path, size_t rows) {
  BoolSource source;
  if (auto store = OpenArray<bool, 1>(archive, path);
      store && store->domain().shape()[0] == static_cast<ts::Index>(rows)) {
    source.available = true;
    source.booleans = std::move(*store);
    return source;
  }
  if (auto store = OpenArray<uint8_t, 1>(archive, path);
      store && store->domain().shape()[0] == static_cast<ts::Index>(rows)) {
    source.available = true;
    source.byte_storage = true;
    source.bytes = std::move(*store);
  }
  return source;
}

struct PointSource {
  bool available = false;
  ts::TensorStore<float, 2> values;
};

PointSource OpenPointSource(const ArchiveContext::Impl &archive,
                            const std::string &path, size_t rows) {
  PointSource source;
  if (auto store = OpenArray<float, 2>(archive, path);
      store && store->domain().shape()[0] == static_cast<ts::Index>(rows) &&
      store->domain().shape()[1] >= 2) {
    source.available = true;
    source.values = std::move(*store);
  }
  return source;
}

struct SequenceSource {
  bool available = false;
  size_t point_count = 0;
  ts::TensorStore<float, 3> values;
};

SequenceSource OpenSequenceSource(const ArchiveContext::Impl &archive,
                                  const std::string &path, size_t rows) {
  SequenceSource source;
  if (auto store = OpenArray<float, 3>(archive, path);
      store && store->domain().shape()[0] == static_cast<ts::Index>(rows) &&
      store->domain().shape()[1] > 0 && store->domain().shape()[2] >= 2) {
    source.available = true;
    source.point_count = static_cast<size_t>(store->domain().shape()[1]);
    source.values = std::move(*store);
  }
  return source;
}

std::optional<std::vector<uint8_t>>
ReadBoolRange(const BoolSource &source, size_t start, size_t stop) {
  if (!source.available || stop <= start) {
    return std::nullopt;
  }
  std::vector<uint8_t> values(stop - start, 0);
  if (source.byte_storage) {
    auto slice = SliceFirstDimension(source.bytes,
                                     static_cast<ts::Index>(start),
                                     static_cast<ts::Index>(stop));
    auto read = ts::Read(slice).result();
    if (!read.ok() || read->rank() != 1 ||
        read->shape()[0] != static_cast<ts::Index>(values.size()) ||
        read->byte_strides().size() != 1) {
      return std::nullopt;
    }
    const auto *origin = reinterpret_cast<const uint8_t *>(
        read->byte_strided_origin_pointer().get());
    for (size_t index = 0; index < values.size(); ++index) {
      values[index] = *(origin + static_cast<ts::Index>(index) *
                                    read->byte_strides()[0]) != 0;
    }
    return values;
  }
  auto slice = SliceFirstDimension(source.booleans,
                                   static_cast<ts::Index>(start),
                                   static_cast<ts::Index>(stop));
  auto read = ts::Read(slice).result();
  if (!read.ok() || read->rank() != 1 ||
      read->shape()[0] != static_cast<ts::Index>(values.size()) ||
      read->byte_strides().size() != 1) {
    return std::nullopt;
  }
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t index = 0; index < values.size(); ++index) {
    values[index] =
        *reinterpret_cast<const bool *>(
            origin + static_cast<ts::Index>(index) *
                         read->byte_strides()[0])
            ? 1
            : 0;
  }
  return values;
}

std::optional<std::vector<SubjectShapeOverlayPoint>>
ReadPointRange(const PointSource &source, size_t start, size_t stop) {
  if (!source.available || stop <= start) {
    return std::nullopt;
  }
  auto slice = SliceFirstDimension(source.values,
                                   static_cast<ts::Index>(start),
                                   static_cast<ts::Index>(stop));
  auto read = ts::Read(slice).result();
  if (!read.ok() || read->rank() != 2 ||
      read->shape()[0] != static_cast<ts::Index>(stop - start) ||
      read->shape()[1] < 2 || read->byte_strides().size() != 2) {
    return std::nullopt;
  }
  const auto strides = read->byte_strides();
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  std::vector<SubjectShapeOverlayPoint> values(stop - start);
  for (size_t index = 0; index < values.size(); ++index) {
    const auto *value = origin + static_cast<ts::Index>(index) * strides[0];
    values[index].x = *reinterpret_cast<const float *>(value);
    values[index].y =
        *reinterpret_cast<const float *>(value + strides[1]);
  }
  return values;
}

std::optional<std::vector<std::vector<SubjectShapeOverlayPoint>>>
ReadSequenceRange(const SequenceSource &source, size_t start, size_t stop) {
  if (!source.available || stop <= start) {
    return std::nullopt;
  }
  auto slice = SliceFirstDimension(source.values,
                                   static_cast<ts::Index>(start),
                                   static_cast<ts::Index>(stop));
  auto read = ts::Read(slice).result();
  if (!read.ok() || read->rank() != 3 ||
      read->shape()[0] != static_cast<ts::Index>(stop - start) ||
      read->shape()[2] < 2 || read->byte_strides().size() != 3) {
    return std::nullopt;
  }
  const auto strides = read->byte_strides();
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  std::vector<std::vector<SubjectShapeOverlayPoint>> values(stop - start);
  for (size_t row = 0; row < values.size(); ++row) {
    values[row].reserve(static_cast<size_t>(read->shape()[1]));
    for (ts::Index point = 0; point < read->shape()[1]; ++point) {
      const auto *value = origin + static_cast<ts::Index>(row) * strides[0] +
                          point * strides[1];
      const double x = *reinterpret_cast<const float *>(value);
      const double y = *reinterpret_cast<const float *>(value + strides[2]);
      if (std::isfinite(x) && std::isfinite(y)) {
        values[row].push_back({x, y});
      }
    }
  }
  return values;
}

struct RowMetadata {
  size_t shape_row = 0;
  int64_t camera_frame = -1;
  int64_t detection_index = -1;
  int64_t source_refined_row_id = -1;
  int64_t source_crop_row_id = -1;
  double roi_x = 0.0;
  double roi_y = 0.0;
  double roi_width = 0.0;
  double roi_height = 0.0;
};

struct GeometrySources {
  BoolSource body_frame_valid;
  PointSource body_origin;
  PointSource body_forward_axis;
  PointSource body_left_axis;
  BoolSource snout_tip_valid;
  PointSource snout_tip;
  BoolSource tail_base_valid;
  PointSource tail_base;
  PointSource tail_tip;
  BoolSource caudal_anchor_valid;
  PointSource caudal_anchor;
  BoolSource centerline_valid;
  BoolSource centerline_reaches_snout;
  SequenceSource centerline;
  BoolSource bspline_valid;
  SequenceSource bspline_sample;
  SequenceSource bspline_control_points;
  BoolSource tail_sample_valid;
  SequenceSource tail_samples;
  SequenceSource tail_normals;
};

struct GeometryChunk {
  size_t start_row = 0;
  std::vector<SubjectShapeOverlayGeometry> rows;
};

class TensorStoreSubjectShapeOverlayRepository final
    : public SubjectShapeOverlayRepository {
public:
  TensorStoreSubjectShapeOverlayRepository(
      SubjectShapeOverlayDescriptor descriptor, std::vector<RowMetadata> rows,
      GeometrySources sources)
      : descriptor_(std::move(descriptor)), rows_(std::move(rows)),
        sources_(std::move(sources)) {
    for (size_t index = 0; index < rows_.size(); ++index) {
      rows_by_frame_[rows_[index].camera_frame].push_back(index);
    }
  }

  const SubjectShapeOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  SubjectShapeOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    SubjectShapeOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
      result.status = SubjectShapeOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = SubjectShapeOverlayStatus::InvalidDimensions;
      result.error = "Full-frame dimensions are invalid";
      return result;
    }
    const auto found = rows_by_frame_.find(camera_frame);
    if (found == rows_by_frame_.end()) {
      result.status = SubjectShapeOverlayStatus::Missing;
      return result;
    }

    result.status = SubjectShapeOverlayStatus::Mapped;
    result.detections.reserve(found->second.size());
    for (const size_t index : found->second) {
      const auto &row = rows_[index];
      SubjectShapeOverlayDetection detection{
          row.shape_row, row.detection_index, row.source_refined_row_id,
          row.source_crop_row_id, row.roi_x, row.roi_y, row.roi_width,
          row.roi_height, {}};
      if (!readGeometry(row.shape_row, &detection.geometry)) {
        result.status = SubjectShapeOverlayStatus::ReadFailed;
        result.error = "Subject-shape geometry row could not be read";
        result.detections.clear();
        return result;
      }
      result.detections.push_back(std::move(detection));
    }
    return result;
  }

private:
  bool readGeometry(size_t row, SubjectShapeOverlayGeometry *geometry) const {
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
      if (row >= chunk.start_row &&
          row - chunk.start_row < chunk.rows.size()) {
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
    const size_t stop =
        std::min(descriptor_.row_count, start + kRowsPerChunk);

    auto body_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.body_frame_valid, start, stop);
    });
    auto body_origin_future = std::async(std::launch::async, [&] {
      return ReadPointRange(sources_.body_origin, start, stop);
    });
    auto forward_future = std::async(std::launch::async, [&] {
      return ReadPointRange(sources_.body_forward_axis, start, stop);
    });
    auto left_future = std::async(std::launch::async, [&] {
      return ReadPointRange(sources_.body_left_axis, start, stop);
    });
    auto snout_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.snout_tip_valid, start, stop);
    });
    auto snout_future = std::async(std::launch::async, [&] {
      return ReadPointRange(sources_.snout_tip, start, stop);
    });
    auto tail_base_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.tail_base_valid, start, stop);
    });
    auto tail_base_future = std::async(std::launch::async, [&] {
      return ReadPointRange(sources_.tail_base, start, stop);
    });
    auto tail_tip_future = std::async(std::launch::async, [&] {
      return ReadPointRange(sources_.tail_tip, start, stop);
    });
    auto caudal_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.caudal_anchor_valid, start, stop);
    });
    auto caudal_future = std::async(std::launch::async, [&] {
      return ReadPointRange(sources_.caudal_anchor, start, stop);
    });
    auto centerline_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.centerline_valid, start, stop);
    });
    auto reaches_snout_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.centerline_reaches_snout, start, stop);
    });
    auto centerline_future = std::async(std::launch::async, [&] {
      return ReadSequenceRange(sources_.centerline, start, stop);
    });
    auto bspline_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.bspline_valid, start, stop);
    });
    auto bspline_future = std::async(std::launch::async, [&] {
      return ReadSequenceRange(sources_.bspline_sample, start, stop);
    });
    auto controls_future = std::async(std::launch::async, [&] {
      return ReadSequenceRange(sources_.bspline_control_points, start, stop);
    });
    auto tail_sample_valid_future = std::async(std::launch::async, [&] {
      return ReadBoolRange(sources_.tail_sample_valid, start, stop);
    });
    auto tail_samples_future = std::async(std::launch::async, [&] {
      return ReadSequenceRange(sources_.tail_samples, start, stop);
    });
    auto tail_normals_future = std::async(std::launch::async, [&] {
      return ReadSequenceRange(sources_.tail_normals, start, stop);
    });

    const auto body_valid = body_valid_future.get();
    const auto body_origin = body_origin_future.get();
    const auto forward = forward_future.get();
    const auto left = left_future.get();
    const auto snout_valid = snout_valid_future.get();
    const auto snout = snout_future.get();
    const auto tail_base_valid = tail_base_valid_future.get();
    const auto tail_base = tail_base_future.get();
    const auto tail_tip = tail_tip_future.get();
    const auto caudal_valid = caudal_valid_future.get();
    const auto caudal = caudal_future.get();
    const auto centerline_valid = centerline_valid_future.get();
    const auto reaches_snout = reaches_snout_future.get();
    const auto centerline = centerline_future.get();
    const auto bspline_valid = bspline_valid_future.get();
    const auto bspline = bspline_future.get();
    const auto controls = controls_future.get();
    const auto tail_sample_valid = tail_sample_valid_future.get();
    const auto tail_samples = tail_samples_future.get();
    const auto tail_normals = tail_normals_future.get();
    if (!body_valid || !body_origin || !forward || !left || !snout_valid ||
        !snout || !tail_base_valid || !tail_base || !tail_tip ||
        !caudal_valid || !caudal || !centerline_valid || !reaches_snout ||
        !centerline || !bspline_valid || !bspline || !controls ||
        !tail_sample_valid || !tail_samples || !tail_normals) {
      return false;
    }

    chunk->start_row = start;
    chunk->rows.resize(stop - start);
    for (size_t index = 0; index < chunk->rows.size(); ++index) {
      auto &geometry = chunk->rows[index];
      geometry.body_frame_valid = (*body_valid)[index] != 0;
      geometry.body_origin = (*body_origin)[index];
      geometry.body_forward_axis = (*forward)[index];
      geometry.body_left_axis = (*left)[index];
      geometry.snout_tip_valid = (*snout_valid)[index] != 0;
      geometry.snout_tip = (*snout)[index];
      geometry.tail_base_valid = (*tail_base_valid)[index] != 0;
      geometry.tail_base = (*tail_base)[index];
      geometry.tail_tip = (*tail_tip)[index];
      geometry.caudal_anchor_valid = (*caudal_valid)[index] != 0;
      geometry.caudal_anchor = (*caudal)[index];
      geometry.centerline_valid = (*centerline_valid)[index] != 0;
      geometry.centerline_reaches_snout = (*reaches_snout)[index] != 0;
      geometry.centerline = std::move((*centerline)[index]);
      geometry.bspline_valid = (*bspline_valid)[index] != 0;
      geometry.bspline_sample = std::move((*bspline)[index]);
      geometry.bspline_control_points = std::move((*controls)[index]);
      geometry.tail_sample_valid = (*tail_sample_valid)[index] != 0;
      geometry.tail_samples = std::move((*tail_samples)[index]);
      geometry.tail_normals = std::move((*tail_normals)[index]);
    }
    return true;
  }

  SubjectShapeOverlayDescriptor descriptor_;
  std::vector<RowMetadata> rows_;
  GeometrySources sources_;
  std::unordered_map<int64_t, std::vector<size_t>> rows_by_frame_;
  mutable std::mutex chunk_mutex_;
  mutable std::deque<GeometryChunk> chunks_;
};

} // namespace

std::unique_ptr<SubjectShapeOverlayRepository>
OpenSubjectShapeOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                  const std::string &requested_run,
                                  std::string *error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto &impl = *archive->impl_;
  constexpr const char *group = "analysis/subject_shape_runs";
  std::string run_name = requested_run;
  if (run_name.rfind(std::string(group) + "/", 0) == 0) {
    run_name.erase(0, std::string(group).size() + 1);
  }
  if (run_name.empty() || run_name == "latest") {
    run_name = LatestRun(impl, group);
  }
  if (!ValidRunName(run_name)) {
    internal::SetArchiveError(error_message,
                              "No valid subject-shape run is available");
    return nullptr;
  }
  const std::string run_base = std::string(group) + "/" + run_name;
  const auto run_attributes = internal::ReadArchiveAttributes(impl, run_base);
  if (!run_attributes) {
    internal::SetArchiveError(error_message,
                              "Subject-shape run attributes are unreadable");
    return nullptr;
  }

  SubjectShapeOverlayDescriptor descriptor;
  descriptor.source_group = group;
  descriptor.run_name = run_name;
  descriptor.source_refined_subject_masks_run =
      StringValue(*run_attributes, "source_refined_subject_masks_run");
  if (descriptor.source_refined_subject_masks_run.rfind(
          "refined_subject_masks_runs/", 0) == 0) {
    descriptor.source_refined_subject_masks_run.erase(
        0, std::string("refined_subject_masks_runs/").size());
  }
  descriptor.schema_id = StringValue(*run_attributes, "schema_id");
  descriptor.schema_version = IntegerValue(*run_attributes, "schema_version");
  descriptor.method = StringValue(*run_attributes, "method");
  descriptor.method_version = IntegerValue(*run_attributes, "method_version");
  descriptor.row_axis = StringValue(*run_attributes, "row_axis");
  descriptor.head_endpoint_semantics =
      StringValue(*run_attributes, "head_endpoint_semantics");
  if (descriptor.row_axis != "refined_subject_mask_rows") {
    internal::SetArchiveError(
        error_message,
        "Subject-shape row_axis must be refined_subject_mask_rows");
    return nullptr;
  }
  if (!ValidRunName(descriptor.source_refined_subject_masks_run)) {
    internal::SetArchiveError(
        error_message,
        "Subject-shape run lacks source_refined_subject_masks_run");
    return nullptr;
  }

  std::vector<int64_t> frames;
  std::vector<int64_t> detections;
  std::vector<int64_t> refined_rows;
  std::vector<int64_t> crop_rows;
  const std::string row_base = run_base + "/row_index";
  if (!ReadIntegers(impl, row_base + "/frame_indices", &frames) ||
      frames.empty() ||
      !ReadIntegers(impl, row_base + "/detection_indices", &detections) ||
      detections.size() != frames.size() ||
      !ReadIntegers(impl, row_base + "/source_refined_row_ids",
                    &refined_rows) ||
      refined_rows.size() != frames.size() ||
      !ReadIntegers(impl, row_base + "/source_crop_row_ids", &crop_rows) ||
      crop_rows.size() != frames.size()) {
    internal::SetArchiveError(error_message,
                              "Subject-shape row lineage is unreadable");
    return nullptr;
  }
  descriptor.row_count = frames.size();

  const std::string refined_base = "refined_subject_masks_runs/" +
                                   descriptor.source_refined_subject_masks_run;
  const auto refined_attributes =
      internal::ReadArchiveAttributes(impl, refined_base);
  std::vector<int64_t> refined_frames;
  std::vector<int64_t> refined_detections;
  std::vector<int64_t> refined_crop_rows;
  std::vector<int64_t> refined_source_row_ids;
  if (!refined_attributes ||
      !ReadIntegers(impl, refined_base + "/frame_indices", &refined_frames) ||
      !ReadIntegers(impl, refined_base + "/detection_indices",
                    &refined_detections) ||
      !ReadIntegers(impl, refined_base + "/source_crop_row_ids",
                    &refined_crop_rows) ||
      !ReadIntegers(impl, refined_base + "/source_refined_row_ids",
                    &refined_source_row_ids) ||
      refined_frames.size() != refined_detections.size() ||
      refined_frames.size() != refined_crop_rows.size() ||
      refined_frames.size() != refined_source_row_ids.size()) {
    internal::SetArchiveError(
        error_message, "Referenced refined-mask lineage is unreadable");
    return nullptr;
  }
  if (refined_frames.size() != frames.size()) {
    internal::SetArchiveError(
        error_message,
        "Subject-shape row count does not match the referenced refined-mask run");
    return nullptr;
  }
  descriptor.source_crop_run =
      StringValue(*refined_attributes, "source_crop_run");
  if (descriptor.source_crop_run.rfind("crop_runs/", 0) == 0) {
    descriptor.source_crop_run.erase(0, std::string("crop_runs/").size());
  }
  if (!ValidRunName(descriptor.source_crop_run)) {
    internal::SetArchiveError(error_message,
                              "Referenced refined-mask run lacks source_crop_run");
    return nullptr;
  }

  const std::string crop_base = "crop_runs/" + descriptor.source_crop_run;
  const auto crop_attributes = internal::ReadArchiveAttributes(impl, crop_base);
  std::vector<int64_t> source_frames;
  std::vector<int64_t> source_detections;
  std::vector<std::vector<double>> crop_offsets;
  if (!crop_attributes ||
      !ReadIntegers(impl, crop_base + "/frame_indices", &source_frames) ||
      !ReadIntegers(impl, crop_base + "/detection_indices",
                    &source_detections) ||
      !ReadNumericMatrix(impl, crop_base + "/roi_coordinates_full", 2,
                         &crop_offsets) ||
      source_frames.size() != source_detections.size() ||
      source_frames.size() != crop_offsets.size()) {
    internal::SetArchiveError(error_message,
                              "Source crop placement metadata is unreadable");
    return nullptr;
  }

  auto mask_store = OpenArray<uint8_t, 4>(impl, refined_base + "/masks_roi");
  if (!mask_store || mask_store->domain().shape()[0] !=
                         static_cast<ts::Index>(refined_frames.size()) ||
      mask_store->domain().shape()[2] <= 0 ||
      mask_store->domain().shape()[3] <= 0) {
    internal::SetArchiveError(
        error_message, "Referenced refined-mask coordinate dimensions are invalid");
    return nullptr;
  }
  descriptor.coordinate_height =
      static_cast<size_t>(mask_store->domain().shape()[2]);
  descriptor.coordinate_width =
      static_cast<size_t>(mask_store->domain().shape()[3]);
  double roi_width = 0.0;
  double roi_height = 0.0;
  if (!ReadRoiSize(*crop_attributes, &roi_width, &roi_height)) {
    roi_width = descriptor.coordinate_width;
    roi_height = descriptor.coordinate_height;
  }

  std::vector<RowMetadata> rows;
  rows.reserve(frames.size());
  size_t camera_frame_count = 0;
  for (size_t shape_row = 0; shape_row < frames.size(); ++shape_row) {
    if (shape_row >= refined_frames.size() || crop_rows[shape_row] < 0 ||
        static_cast<uint64_t>(crop_rows[shape_row]) >= source_frames.size()) {
      internal::SetArchiveError(
          error_message, "Subject-shape row lineage contains an out-of-range row");
      return nullptr;
    }
    const size_t crop_row = static_cast<size_t>(crop_rows[shape_row]);
    if (refined_frames[shape_row] != frames[shape_row] ||
        refined_detections[shape_row] != detections[shape_row] ||
        refined_source_row_ids[shape_row] != refined_rows[shape_row] ||
        refined_crop_rows[shape_row] != crop_rows[shape_row] ||
        source_frames[crop_row] != frames[shape_row] ||
        source_detections[crop_row] != detections[shape_row]) {
      internal::SetArchiveError(
          error_message, "Subject-shape row does not match its refined-mask and crop lineage");
      return nullptr;
    }
    if (frames[shape_row] < 0 ||
        !std::isfinite(crop_offsets[crop_row][0]) ||
        !std::isfinite(crop_offsets[crop_row][1])) {
      continue;
    }
    rows.push_back({shape_row, frames[shape_row], detections[shape_row],
                    refined_rows[shape_row],
                    static_cast<int64_t>(crop_row), crop_offsets[crop_row][0],
                    crop_offsets[crop_row][1], roi_width, roi_height});
    camera_frame_count = std::max(
        camera_frame_count, static_cast<size_t>(frames[shape_row]) + 1);
  }
  descriptor.camera_frame_count = camera_frame_count;
  if (rows.empty()) {
    internal::SetArchiveError(error_message,
                              "Subject-shape run has no verified placements");
    return nullptr;
  }

  const std::string body = run_base + "/body_frame";
  const std::string subject = run_base + "/components/subject_body";
  const std::string bladder = run_base + "/components/swim_bladder";
  GeometrySources sources;
  sources.body_frame_valid =
      OpenBoolSource(impl, body + "/valid", frames.size());
  sources.body_origin =
      OpenPointSource(impl, body + "/origin_xy", frames.size());
  sources.body_forward_axis =
      OpenPointSource(impl, body + "/forward_axis_xy", frames.size());
  sources.body_left_axis =
      OpenPointSource(impl, body + "/left_axis_xy", frames.size());
  sources.snout_tip_valid =
      OpenBoolSource(impl, subject + "/snout_tip_valid", frames.size());
  sources.snout_tip =
      OpenPointSource(impl, subject + "/snout_tip_xy", frames.size());
  sources.tail_base_valid =
      OpenBoolSource(impl, subject + "/tail_base_valid", frames.size());
  sources.tail_base =
      OpenPointSource(impl, subject + "/tail_base_xy", frames.size());
  sources.tail_tip =
      OpenPointSource(impl, subject + "/tail_tip_xy", frames.size());
  sources.caudal_anchor_valid =
      OpenBoolSource(impl, bladder + "/caudal_contour_valid", frames.size());
  sources.caudal_anchor = OpenPointSource(
      impl, bladder + "/caudal_contour_point_xy", frames.size());
  sources.centerline_valid =
      OpenBoolSource(impl, subject + "/centerline_valid", frames.size());
  sources.centerline_reaches_snout = OpenBoolSource(
      impl, subject + "/centerline_reaches_snout", frames.size());
  sources.centerline =
      OpenSequenceSource(impl, subject + "/centerline_xy", frames.size());
  sources.bspline_valid =
      OpenBoolSource(impl, subject + "/bspline_valid", frames.size());
  sources.bspline_sample = OpenSequenceSource(
      impl, subject + "/bspline_sample_xy", frames.size());
  sources.bspline_control_points = OpenSequenceSource(
      impl, subject + "/bspline_control_points_xy", frames.size());
  sources.tail_sample_valid =
      OpenBoolSource(impl, subject + "/tail_sample_valid", frames.size());
  sources.tail_samples = OpenSequenceSource(
      impl, subject + "/tail_sample_xy", frames.size());
  sources.tail_normals = OpenSequenceSource(
      impl, subject + "/tail_normal_xy", frames.size());

  const bool complete =
      sources.body_frame_valid.available && sources.body_origin.available &&
      sources.body_forward_axis.available && sources.body_left_axis.available &&
      sources.snout_tip_valid.available && sources.snout_tip.available &&
      sources.tail_base_valid.available && sources.tail_base.available &&
      sources.tail_tip.available && sources.caudal_anchor_valid.available &&
      sources.caudal_anchor.available && sources.centerline_valid.available &&
      sources.centerline_reaches_snout.available &&
      sources.centerline.available && sources.bspline_valid.available &&
      sources.bspline_sample.available &&
      sources.bspline_control_points.available &&
      sources.tail_sample_valid.available && sources.tail_samples.available &&
      sources.tail_normals.available;
  if (!complete) {
    internal::SetArchiveError(error_message,
                              "Subject-shape geometry arrays are incomplete");
    return nullptr;
  }
  descriptor.centerline_point_count = sources.centerline.point_count;
  descriptor.bspline_sample_point_count = sources.bspline_sample.point_count;
  descriptor.bspline_control_point_count =
      sources.bspline_control_points.point_count;
  descriptor.tail_sample_point_count = sources.tail_samples.point_count;
  if (sources.tail_normals.point_count != descriptor.tail_sample_point_count) {
    internal::SetArchiveError(
        error_message, "Subject-shape tail samples and normals do not align");
    return nullptr;
  }

  return std::make_unique<TensorStoreSubjectShapeOverlayRepository>(
      std::move(descriptor), std::move(rows), std::move(sources));
}

} // namespace crimson::zarr
