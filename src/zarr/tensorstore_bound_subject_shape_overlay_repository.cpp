#include "zarr/tensorstore_bound_subject_shape_overlay_repository.h"

#include "zarr/archive_context_internal.h"
#include "zarr/canonical_json.h"
#include "zarr/shared_mask_frame_index.h"

#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace crimson::zarr {
namespace {

namespace ts = tensorstore;
using json = nlohmann::json;

void assignError(std::string *destination, std::string message) {
  if (destination) {
    *destination = std::move(message);
  }
}

bool validRunName(std::string_view value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string_view::npos &&
         value.find('\\') == std::string_view::npos;
}

std::string stringValue(const json &value, std::string_view key) {
  const auto found = value.find(std::string(key));
  return found != value.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

const json *objectAt(const json &value, std::string_view key) {
  const auto found = value.find(std::string(key));
  return found != value.end() && found->is_object() ? &*found : nullptr;
}

std::string stripGroup(std::string value, std::string_view group) {
  const std::string prefix = std::string(group) + "/";
  if (value.rfind(prefix, 0) == 0) {
    value.erase(0, prefix.size());
  }
  return value;
}

std::string manifestArrayDigest(const json &manifest, std::string_view path,
                                const std::vector<size_t> &shape,
                                std::string_view dtype) {
  try {
    const auto &entry = manifest.at("payload")
                            .at("logical_content")
                            .at("document")
                            .at("arrays")
                            .at(std::string(path));
    if (entry.at("shape").get<std::vector<size_t>>() != shape ||
        entry.value("dtype", "") != dtype ||
        entry.value("digest_algorithm", "") !=
            "sha256_c_contiguous_bytes_v1" ||
        !IsLowerSha256(entry.value("sha256", ""))) {
      return {};
    }
    return entry.at("sha256").get<std::string>();
  } catch (const json::exception &) {
    return {};
  }
}

std::string bindingArrayDigest(const json &binding, std::string_view path,
                               const std::vector<size_t> &shape,
                               std::string_view dtype) {
  try {
    const auto &entry = binding.at("row_arrays").at(std::string(path));
    if (entry.at("shape").get<std::vector<size_t>>() != shape ||
        entry.value("dtype", "") != dtype ||
        entry.value("digest_algorithm", "") !=
            "sha256_c_contiguous_bytes_v1" ||
        !IsLowerSha256(entry.value("sha256", ""))) {
      return {};
    }
    return entry.at("sha256").get<std::string>();
  } catch (const json::exception &) {
    return {};
  }
}

bool manifestPayloadMatches(const json &manifest,
                            const std::string &expected_digest) {
  try {
    return manifest.value("payload_digest", "") == expected_digest &&
           CanonicalJsonSha256(manifest.at("payload")) == expected_digest;
  } catch (const json::exception &) {
    return false;
  }
}

bool coordinateAuthorityMatches(const ArchiveContext::Impl &archive,
                                const std::string &path, size_t width,
                                size_t height, std::string_view profile,
                                std::string_view geometry_type,
                                std::string_view origin,
                                std::string_view overlay_status) {
  const auto attributes = internal::ReadArchiveAttributes(archive, path);
  if (!attributes ||
      !IsLowerSha256(stringValue(*attributes,
                                 "coordinate_descriptor_sha256"))) {
    return false;
  }
  const auto *descriptor = objectAt(*attributes, "coordinate_descriptor");
  const auto *extent = descriptor
                           ? objectAt(*descriptor, "reference_extent")
                           : nullptr;
  const auto *directions = descriptor
                               ? objectAt(*descriptor, "positive_directions")
                               : nullptr;
  const auto *overlay = descriptor
                            ? objectAt(*descriptor, "source_camera_overlay")
                            : nullptr;
  return descriptor && extent && directions && overlay &&
         CanonicalJsonSha256(*descriptor) ==
             stringValue(*attributes, "coordinate_descriptor_sha256") &&
         descriptor->value("schema_id", "") ==
             "palette.coordinate_descriptor" &&
         descriptor->value("schema_version", 0) == 2 &&
         descriptor->value("profile_id", "") == profile &&
         descriptor->value("space_id", "") == "source_camera_image_px" &&
         descriptor->value("geometry_type", "") == geometry_type &&
         descriptor->value("origin", "") == origin &&
         directions->value("x", "") == "right" &&
         directions->value("y", "") == "down" &&
         extent->value("width", size_t{0}) == width &&
         extent->value("height", size_t{0}) == height &&
         extent->value("units", "") == "px" &&
         overlay->value("status", "") == overlay_status;
}

bool headingAuthorityMatches(const ArchiveContext::Impl &archive,
                             const std::string &path, size_t rows,
                             const json &publication) {
  const auto attributes = internal::ReadArchiveAttributes(archive, path);
  if (!attributes) {
    return false;
  }
  const auto *semantics =
      objectAt(*attributes, "subject_shape_heading_semantics");
  const auto *heading = semantics ? objectAt(*semantics, "heading") : nullptr;
  const auto *axis = semantics ? objectAt(*semantics, "axis_valid") : nullptr;
  const auto *published = objectAt(publication, "heading_semantics");
  const std::string digest =
      stringValue(*attributes, "subject_shape_heading_semantics_sha256");
  try {
    return semantics && heading && axis && published && IsLowerSha256(digest) &&
           CanonicalJsonSha256(*semantics) == digest &&
           semantics->value("schema_id", "") ==
               "palette.subject_shape_row_bound_heading_semantics" &&
           semantics->value("schema_version", 0) == 1 &&
           semantics->value("units", "") == "deg" &&
           semantics->value("formula", "") ==
               "degrees(atan2(-forward_y, forward_x))" &&
           semantics->value("zero_direction", "") ==
               "source_camera_positive_x_right" &&
           semantics->value("positive_rotation", "") ==
               "counterclockwise_after_source_camera_y_flip" &&
           semantics->value("invalid_row_value", "") ==
               "nan_when_axis_valid_false" &&
           heading->value("relative_ref", "") == "body_frame/heading_deg" &&
           heading->value("dtype", "") == "<f4" &&
           heading->at("shape").get<std::vector<size_t>>() ==
               std::vector<size_t>{rows} &&
           axis->value("relative_ref", "") == "body_frame/axis_valid" &&
           axis->value("dtype", "") == "|b1" &&
           axis->at("shape").get<std::vector<size_t>>() ==
               std::vector<size_t>{rows} &&
           published->value("record_ref", "") ==
               "/" + path + "@subject_shape_heading_semantics" &&
           published->value("record_sha256", "") == digest;
  } catch (const json::exception &) {
    return false;
  }
}

template <typename T, ts::DimensionIndex Rank>
std::optional<ts::TensorStore<T, Rank>>
openExact(const ArchiveContext::Impl &archive, const std::string &path,
          SubjectShapeOverlayOpenMetrics *metrics, std::string *error) {
  const auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  if (!spec) {
    assignError(error, "Missing shape-v5 array: " + path);
    return std::nullopt;
  }
  auto opened = ts::Open<T, Rank>(*spec, ts::OpenMode::open,
                                  ts::ReadWriteMode::read, archive.context)
                    .result();
  if (!opened.ok()) {
    assignError(error, path + ": " + opened.status().ToString());
    return std::nullopt;
  }
  if (metrics) {
    ++metrics->exact_handle_opens;
  }
  return *opened;
}

template <typename T, ts::DimensionIndex Rank>
bool readRows(const ts::TensorStore<T, Rank> &store, size_t first, size_t last,
              size_t columns, std::vector<T> *output, std::string *error) {
  if (!output || last < first ||
      last > static_cast<size_t>(store.domain().shape()[0])) {
    assignError(error, "Shape-v5 row range is invalid");
    return false;
  }
  if (first == last) {
    output->clear();
    return true;
  }
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  auto read = ts::Read(store | ts::IdentityTransform(domain)).result();
  if (!read.ok() || read->rank() != Rank ||
      read->byte_strides().size() != Rank ||
      (Rank == 2 && static_cast<size_t>(read->shape()[1]) != columns) ||
      (Rank == 3 && static_cast<size_t>(read->shape()[2]) != columns)) {
    assignError(error, read.ok() ? "Shape-v5 read shape mismatch"
                                 : read.status().ToString());
    return false;
  }
  size_t trailing = 1;
  for (ts::DimensionIndex dimension = 1; dimension < Rank; ++dimension) {
    const size_t extent = static_cast<size_t>(read->shape()[dimension]);
    if (extent != 0 && trailing > std::numeric_limits<size_t>::max() / extent) {
      assignError(error, "Shape-v5 read extent overflows");
      return false;
    }
    trailing *= extent;
  }
  output->resize((last - first) * trailing);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < last - first; ++row) {
    if constexpr (Rank == 1) {
      (*output)[row] = *reinterpret_cast<const T *>(
          origin + static_cast<ts::Index>(row) * read->byte_strides()[0]);
    } else if constexpr (Rank == 2) {
      for (size_t column = 0; column < trailing; ++column) {
        (*output)[row * trailing + column] =
            *reinterpret_cast<const T *>(
                origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
                static_cast<ts::Index>(column) * read->byte_strides()[1]);
      }
    } else {
      const size_t middle = static_cast<size_t>(read->shape()[1]);
      const size_t inner = static_cast<size_t>(read->shape()[2]);
      for (size_t outer = 0; outer < middle; ++outer) {
        for (size_t inner_index = 0; inner_index < inner; ++inner_index) {
          (*output)[row * trailing + outer * inner + inner_index] =
              *reinterpret_cast<const T *>(
                  origin +
                  static_cast<ts::Index>(row) * read->byte_strides()[0] +
                  static_cast<ts::Index>(outer) * read->byte_strides()[1] +
                  static_cast<ts::Index>(inner_index) *
                      read->byte_strides()[2]);
        }
      }
    }
  }
  return true;
}

bool readBoolRows(const ts::TensorStore<bool, 1> &store, size_t first,
                  size_t last, std::vector<uint8_t> *output,
                  std::string *error) {
  if (!output || last < first ||
      last > static_cast<size_t>(store.domain().shape()[0])) {
    assignError(error, "Shape-v5 bool row range is invalid");
    return false;
  }
  if (first == last) {
    output->clear();
    return true;
  }
  ts::Box<1> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  auto read = ts::Read(store | ts::IdentityTransform(domain)).result();
  if (!read.ok() || read->rank() != 1 || read->byte_strides().size() != 1) {
    assignError(error, read.ok() ? "Shape-v5 bool read shape mismatch"
                                 : read.status().ToString());
    return false;
  }
  output->resize(last - first);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < output->size(); ++row) {
    (*output)[row] =
        *reinterpret_cast<const bool *>(
            origin + static_cast<ts::Index>(row) * read->byte_strides()[0])
            ? 1
            : 0;
  }
  return true;
}

struct GeometrySources {
  ts::TensorStore<bool, 1> body_valid;
  ts::TensorStore<bool, 1> axis_valid;
  ts::TensorStore<float, 1> heading;
  ts::TensorStore<float, 2> body_origin;
  ts::TensorStore<float, 2> forward_axis;
  ts::TensorStore<float, 2> left_axis;
  ts::TensorStore<bool, 1> snout_valid;
  ts::TensorStore<float, 2> snout;
  ts::TensorStore<bool, 1> tail_base_valid;
  ts::TensorStore<float, 2> tail_base;
  ts::TensorStore<float, 2> tail_tip;
  ts::TensorStore<bool, 1> caudal_valid;
  ts::TensorStore<float, 2> caudal;
  ts::TensorStore<bool, 1> centerline_valid;
  ts::TensorStore<bool, 1> centerline_reaches_snout;
  ts::TensorStore<float, 3> centerline;
  ts::TensorStore<bool, 1> bspline_valid;
  ts::TensorStore<float, 3> bspline;
  ts::TensorStore<float, 3> controls;
  ts::TensorStore<bool, 1> tail_sample_valid;
  ts::TensorStore<float, 3> tail_samples;
  ts::TensorStore<float, 3> tail_normals;
  size_t centerline_points = 0;
  size_t bspline_points = 0;
  size_t control_points = 0;
  size_t tail_points = 0;
};

struct GeometryPayload {
  std::vector<uint8_t> body_valid;
  std::vector<uint8_t> axis_valid;
  std::vector<float> heading;
  std::vector<float> body_origin;
  std::vector<float> forward_axis;
  std::vector<float> left_axis;
  std::vector<uint8_t> snout_valid;
  std::vector<float> snout;
  std::vector<uint8_t> tail_base_valid;
  std::vector<float> tail_base;
  std::vector<float> tail_tip;
  std::vector<uint8_t> caudal_valid;
  std::vector<float> caudal;
  std::vector<uint8_t> centerline_valid;
  std::vector<uint8_t> centerline_reaches_snout;
  std::vector<float> centerline;
  std::vector<uint8_t> bspline_valid;
  std::vector<float> bspline;
  std::vector<float> controls;
  std::vector<uint8_t> tail_sample_valid;
  std::vector<float> tail_samples;
  std::vector<float> tail_normals;
};

bool readGeometry(const GeometrySources &sources, size_t first, size_t last,
                  GeometryPayload *payload, std::string *error) {
  struct Result {
    bool ready = false;
    std::string error;
  };
  auto launch = [&](auto read) {
    return std::async(std::launch::async, [read = std::move(read)]() mutable {
      Result result;
      result.ready = read(&result.error);
      return result;
    });
  };
  std::vector<std::future<Result>> futures;
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.body_valid, first, last, &payload->body_valid,
                        e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.axis_valid, first, last, &payload->axis_valid,
                        e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.heading, first, last, 1, &payload->heading, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.body_origin, first, last, 2,
                    &payload->body_origin, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.forward_axis, first, last, 2,
                    &payload->forward_axis, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.left_axis, first, last, 2, &payload->left_axis, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.snout_valid, first, last,
                        &payload->snout_valid, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.snout, first, last, 2, &payload->snout, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.tail_base_valid, first, last,
                        &payload->tail_base_valid, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.tail_base, first, last, 2, &payload->tail_base, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.tail_tip, first, last, 2, &payload->tail_tip, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.caudal_valid, first, last,
                        &payload->caudal_valid, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.caudal, first, last, 2, &payload->caudal, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.centerline_valid, first, last,
                        &payload->centerline_valid, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.centerline_reaches_snout, first, last,
                        &payload->centerline_reaches_snout, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.centerline, first, last, 2,
                    &payload->centerline, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.bspline_valid, first, last,
                        &payload->bspline_valid, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.bspline, first, last, 2, &payload->bspline, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.controls, first, last, 2, &payload->controls, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readBoolRows(sources.tail_sample_valid, first, last,
                        &payload->tail_sample_valid, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.tail_samples, first, last, 2,
                    &payload->tail_samples, e);
  }));
  futures.push_back(launch([&](auto *e) {
    return readRows(sources.tail_normals, first, last, 2,
                    &payload->tail_normals, e);
  }));
  for (auto &future : futures) {
    const auto result = future.get();
    if (!result.ready) {
      assignError(error, result.error);
      return false;
    }
  }
  return true;
}

SubjectShapeOverlayPoint pointAt(const std::vector<float> &values,
                                 size_t row, size_t columns,
                                 size_t point = 0) {
  const size_t index = row * columns + point * 2;
  return {values[index], values[index + 1]};
}

std::vector<SubjectShapeOverlayPoint>
pointsAt(const std::vector<float> &values, size_t row, size_t point_count) {
  std::vector<SubjectShapeOverlayPoint> result;
  result.reserve(point_count);
  const size_t first = row * point_count * 2;
  for (size_t point = 0; point < point_count; ++point) {
    result.push_back(
        {values[first + point * 2], values[first + point * 2 + 1]});
  }
  return result;
}

bool finite(SubjectShapeOverlayPoint point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

bool allFinite(const std::vector<SubjectShapeOverlayPoint> &points) {
  return std::all_of(points.begin(), points.end(),
                     [](const auto point) { return finite(point); });
}

class BoundSubjectShapeOverlayRepository final
    : public SubjectShapeOverlayRepository {
public:
  BoundSubjectShapeOverlayRepository(
      SubjectShapeOverlayDescriptor descriptor, std::vector<int64_t> offsets,
      std::shared_ptr<const SharedMaskFrameIndex> shared_index,
      size_t max_observations_per_frame, uint64_t max_decoded_frame_bytes,
      uint64_t decoded_bytes_per_row, size_t roi_width, size_t roi_height,
      ts::TensorStore<int64_t, 1> shape_frames,
      ts::TensorStore<uint64_t, 1> shape_keys,
      ts::TensorStore<int64_t, 1> shape_crop_rows,
      ts::TensorStore<int64_t, 1> mask_frames,
      ts::TensorStore<uint64_t, 1> mask_keys,
      ts::TensorStore<int64_t, 1> mask_crop_rows,
      ts::TensorStore<float, 2> mask_crop_xywh, GeometrySources geometry)
      : descriptor_(std::move(descriptor)), offsets_(std::move(offsets)),
        shared_index_(std::move(shared_index)),
        max_observations_per_frame_(max_observations_per_frame),
        max_decoded_frame_bytes_(max_decoded_frame_bytes),
        decoded_bytes_per_row_(decoded_bytes_per_row),
        roi_width_(roi_width), roi_height_(roi_height),
        shape_frames_(std::move(shape_frames)),
        shape_keys_(std::move(shape_keys)),
        shape_crop_rows_(std::move(shape_crop_rows)),
        mask_frames_(std::move(mask_frames)), mask_keys_(std::move(mask_keys)),
        mask_crop_rows_(std::move(mask_crop_rows)),
        mask_crop_xywh_(std::move(mask_crop_xywh)),
        geometry_(std::move(geometry)) {}

  const SubjectShapeOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  SubjectShapeOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    SubjectShapeOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 || static_cast<uint64_t>(camera_frame) >=
                                descriptor_.camera_frame_count) {
      result.status = SubjectShapeOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0 ||
        static_cast<size_t>(full_frame_width) != descriptor_.coordinate_width ||
        static_cast<size_t>(full_frame_height) !=
            descriptor_.coordinate_height) {
      result.status = SubjectShapeOverlayStatus::InvalidDimensions;
      result.error = "Shape-v5 source-camera dimensions disagree";
      return result;
    }
    const size_t frame = static_cast<size_t>(camera_frame);
    const auto& offsets = shared_index_ ? shared_index_->offsets() : offsets_;
    const size_t first = static_cast<size_t>(offsets[frame]);
    const size_t last = static_cast<size_t>(offsets[frame + 1]);
    const size_t rows = last - first;
    if (rows == 0) {
      result.status = SubjectShapeOverlayStatus::Missing;
      return result;
    }
    if (rows > max_observations_per_frame_ ||
        decoded_bytes_per_row_ >
            max_decoded_frame_bytes_ / std::max<size_t>(1, rows)) {
      result.status = SubjectShapeOverlayStatus::ReadFailed;
      result.error = "Shape-v5 frame exceeds bounded read admission";
      return result;
    }

    std::vector<int64_t> shape_frames;
    std::vector<uint64_t> shape_keys;
    std::vector<int64_t> shape_crop_rows;
    std::vector<int64_t> mask_frames;
    std::vector<uint64_t> mask_keys;
    std::vector<int64_t> mask_crop_rows;
    std::vector<float> crop_xywh;
    struct ReadResult {
      bool ready = false;
      std::string error;
    };
    auto launch = [&](auto read) {
      return std::async(std::launch::async,
                        [read = std::move(read)]() mutable {
                          ReadResult read_result;
                          read_result.ready = read(&read_result.error);
                          return read_result;
                        });
    };
    std::vector<std::future<ReadResult>> mapping_reads;
    mapping_reads.push_back(launch([&](auto *e) {
      return readRows(shape_frames_, first, last, 1, &shape_frames, e);
    }));
    mapping_reads.push_back(launch([&](auto *e) {
      return readRows(shape_keys_, first, last, 1, &shape_keys, e);
    }));
    mapping_reads.push_back(launch([&](auto *e) {
      return readRows(shape_crop_rows_, first, last, 1, &shape_crop_rows, e);
    }));
    mapping_reads.push_back(launch([&](auto *e) {
      return readRows(mask_frames_, first, last, 1, &mask_frames, e);
    }));
    mapping_reads.push_back(launch([&](auto *e) {
      return readRows(mask_keys_, first, last, 1, &mask_keys, e);
    }));
    mapping_reads.push_back(launch([&](auto *e) {
      return readRows(mask_crop_rows_, first, last, 1, &mask_crop_rows, e);
    }));
    mapping_reads.push_back(launch([&](auto *e) {
      return readRows(mask_crop_xywh_, first, last, 4, &crop_xywh, e);
    }));
    for (auto &read : mapping_reads) {
      const auto read_result = read.get();
      if (!read_result.ready) {
        result.status = SubjectShapeOverlayStatus::ReadFailed;
        result.error = "Shape-v5 mapping read failed: " + read_result.error;
        return result;
      }
    }

    GeometryPayload geometry;
    if (!readGeometry(geometry_, first, last, &geometry, &result.error)) {
      result.status = SubjectShapeOverlayStatus::ReadFailed;
      result.error = "Shape-v5 geometry read failed: " + result.error;
      return result;
    }

    std::unordered_set<uint64_t> keys;
    keys.reserve(rows);
    result.detections.reserve(rows);
    for (size_t local = 0; local < rows; ++local) {
      const float *crop = crop_xywh.data() + local * 4;
      if (shape_frames[local] != camera_frame ||
          mask_frames[local] != camera_frame ||
          shape_keys[local] != mask_keys[local] ||
          shape_crop_rows[local] != mask_crop_rows[local] ||
          shape_crop_rows[local] < 0 ||
          !keys.insert(shape_keys[local]).second ||
          !std::all_of(crop, crop + 4,
                       [](float value) { return std::isfinite(value); }) ||
          crop[0] < 0.0f || crop[1] < 0.0f || crop[2] <= 0.0f ||
          crop[3] <= 0.0f ||
          crop[2] != static_cast<float>(roi_width_) ||
          crop[3] != static_cast<float>(roi_height_) ||
          crop[0] + crop[2] > descriptor_.coordinate_width ||
          crop[1] + crop[3] > descriptor_.coordinate_height) {
        result.status = SubjectShapeOverlayStatus::ReadFailed;
        result.error =
            "Shape-v5 frame/key/mask-row association is invalid at row " +
            std::to_string(first + local);
        result.detections.clear();
        return result;
      }

      SubjectShapeOverlayGeometry value;
      const bool axis_valid = geometry.axis_valid[local] != 0;
      value.body_frame_valid = geometry.body_valid[local] != 0;
      value.body_axis_valid = axis_valid;
      value.body_origin = pointAt(geometry.body_origin, local, 2);
      value.body_forward_axis = pointAt(geometry.forward_axis, local, 2);
      value.body_left_axis = pointAt(geometry.left_axis, local, 2);
      if (value.body_frame_valid && value.body_axis_valid &&
          std::isfinite(geometry.heading[local])) {
        value.heading_degrees = geometry.heading[local];
      }
      value.snout_tip_valid = geometry.snout_valid[local] != 0;
      value.snout_tip = pointAt(geometry.snout, local, 2);
      value.tail_base_valid = geometry.tail_base_valid[local] != 0;
      value.tail_base = pointAt(geometry.tail_base, local, 2);
      value.tail_tip = pointAt(geometry.tail_tip, local, 2);
      value.caudal_anchor_valid = geometry.caudal_valid[local] != 0;
      value.caudal_anchor = pointAt(geometry.caudal, local, 2);
      value.centerline_valid = geometry.centerline_valid[local] != 0;
      value.centerline_reaches_snout =
          geometry.centerline_reaches_snout[local] != 0;
      value.centerline =
          pointsAt(geometry.centerline, local, geometry_.centerline_points);
      value.bspline_valid = geometry.bspline_valid[local] != 0;
      value.bspline_sample =
          pointsAt(geometry.bspline, local, geometry_.bspline_points);
      value.bspline_control_points =
          pointsAt(geometry.controls, local, geometry_.control_points);
      value.tail_sample_valid = geometry.tail_sample_valid[local] != 0;
      value.tail_samples =
          pointsAt(geometry.tail_samples, local, geometry_.tail_points);
      value.tail_normals =
          pointsAt(geometry.tail_normals, local, geometry_.tail_points);

      const bool valid_body =
          (!value.body_frame_valid || finite(value.body_origin)) &&
          (!value.body_axis_valid ||
           (value.body_frame_valid && finite(value.body_forward_axis) &&
            finite(value.body_left_axis) &&
            value.heading_degrees.has_value())) &&
          (value.body_axis_valid || !value.heading_degrees.has_value());
      const bool valid_points =
          (!value.snout_tip_valid || finite(value.snout_tip)) &&
          (!value.tail_base_valid || finite(value.tail_base)) &&
          (!value.caudal_anchor_valid || finite(value.caudal_anchor)) &&
          (!value.centerline_valid || allFinite(value.centerline)) &&
          (!value.bspline_valid || allFinite(value.bspline_sample)) &&
          (!value.tail_sample_valid ||
           (allFinite(value.tail_samples) && allFinite(value.tail_normals)));
      if (!valid_body || !valid_points) {
        result.status = SubjectShapeOverlayStatus::ReadFailed;
        result.error = "Shape-v5 valid geometry contains non-finite values";
        result.detections.clear();
        return result;
      }

      SubjectShapeOverlayDetection detection;
      detection.shape_row = first + local;
      detection.source_crop_row_id = shape_crop_rows[local];
      detection.roi_x = crop[0];
      detection.roi_y = crop[1];
      detection.roi_width = crop[2];
      detection.roi_height = crop[3];
      detection.geometry = std::move(value);
      detection.instance_key = shape_keys[local];
      detection.instance_key_valid = true;
      result.detections.push_back(std::move(detection));
    }
    result.status = SubjectShapeOverlayStatus::Mapped;
    return result;
  }

  RepositoryMemoryMetrics memoryMetrics() const override {
    RepositoryMemoryMetrics metrics;
    metrics.retained_index_bytes = memory::vectorAllocationBytes(offsets_);
    return metrics;
  }

private:
  SubjectShapeOverlayDescriptor descriptor_;
  std::vector<int64_t> offsets_;
  std::shared_ptr<const SharedMaskFrameIndex> shared_index_;
  size_t max_observations_per_frame_ = 0;
  uint64_t max_decoded_frame_bytes_ = 0;
  uint64_t decoded_bytes_per_row_ = 0;
  size_t roi_width_ = 0;
  size_t roi_height_ = 0;
  ts::TensorStore<int64_t, 1> shape_frames_;
  ts::TensorStore<uint64_t, 1> shape_keys_;
  ts::TensorStore<int64_t, 1> shape_crop_rows_;
  ts::TensorStore<int64_t, 1> mask_frames_;
  ts::TensorStore<uint64_t, 1> mask_keys_;
  ts::TensorStore<int64_t, 1> mask_crop_rows_;
  ts::TensorStore<float, 2> mask_crop_xywh_;
  GeometrySources geometry_;
};

template <typename T, ts::DimensionIndex Rank>
bool exactShape(const ts::TensorStore<T, Rank> &store,
                const std::vector<size_t> &expected) {
  if (expected.size() != static_cast<size_t>(Rank)) {
    return false;
  }
  for (ts::DimensionIndex dimension = 0; dimension < Rank; ++dimension) {
    if (store.domain().shape()[dimension] !=
        static_cast<ts::Index>(expected[dimension])) {
      return false;
    }
  }
  return true;
}

} // namespace

std::unique_ptr<SubjectShapeOverlayRepository>
OpenBoundSubjectShapeOverlayRepository(
    const BoundSubjectShapeOverlayOpenRequest &request,
    std::string *error_message, SubjectShapeOverlayOpenMetrics *open_metrics) {
  SubjectShapeOverlayOpenMetrics metrics;
  metrics.maximum_decoded_frame_bytes = request.max_decoded_frame_bytes;
  if (!request.archive || !request.archive->impl_) {
    assignError(error_message, "Shape-v5 archive context is unavailable");
    return nullptr;
  }
  const auto &selection = request.selection;
  const auto shared_index = request.shared_mask_frame_index;
  if (shared_index &&
      (!shared_index->matches(request.archive, selection) ||
       !shared_index->admits(request.max_observations_per_frame))) {
    assignError(error_message,
                "Shared mask frame index binding or shape admission disagrees");
    return nullptr;
  }
  if (!selection.shape.valid || !selection.mask.valid ||
      !validRunName(selection.shape.run_id) ||
      !validRunName(selection.mask.run_id) ||
      selection.shape.group != "analysis/subject_shape_runs" ||
      selection.mask.group != "refined_subject_masks_runs" ||
      selection.shape.schema_id != "analysis.subject_shape_runs" ||
      selection.shape.schema_version != 5 ||
      selection.mask.schema_id !=
          "palette.stage.refined_subject_mask_dense_core" ||
      selection.mask.schema_version != 1 ||
      selection.shape.bound_source_run_id != selection.mask.run_id ||
      selection.shape.bound_source_payload_digest !=
          selection.mask.manifest_payload_digest ||
      !IsLowerSha256(selection.shape.identity_digest) ||
      !IsLowerSha256(selection.shape.manifest_payload_digest) ||
      !IsLowerSha256(selection.mask.manifest_payload_digest) ||
      !IsLowerSha256(selection.instance_key_digest) ||
      !IsLowerSha256(selection.acquisition_frame_digest) ||
      !IsLowerSha256(selection.frame_row_offsets_digest) ||
      selection.first_acquisition_frame != 0 || selection.frame_count == 0 ||
      selection.observation_count == 0 || selection.source_width == 0 ||
      selection.source_height == 0 ||
      selection.coordinate_surface_id != "source_camera_point_xy_v1" ||
      selection.coordinate_descriptor_profile !=
          "source_camera_image_px.top_left_y_down.v1" ||
      request.max_observations_per_frame == 0 ||
      request.max_decoded_frame_bytes == 0) {
    assignError(error_message,
                "Exact validated shape-v5/mask selection is required");
    return nullptr;
  }

  const auto &archive = *request.archive->impl_;
  const std::string shape_base =
      selection.shape.group + "/" + selection.shape.run_id;
  const std::string mask_base =
      selection.mask.group + "/" + selection.mask.run_id;
  const auto shape_attributes =
      internal::ReadArchiveAttributes(archive, shape_base);
  const auto mask_attributes =
      internal::ReadArchiveAttributes(archive, mask_base);
  if (!shape_attributes || !mask_attributes) {
    assignError(error_message,
                "Bound shape-v5 or mask metadata is unavailable");
    return nullptr;
  }
  const auto *binding = objectAt(*shape_attributes,
                                 "subject_shape_source_binding");
  const auto *publication = objectAt(*shape_attributes,
                                     "subject_shape_publication_manifest");
  const auto *derivation = objectAt(*shape_attributes,
                                    "subject_shape_coordinate_derivation");
  const auto *mask_manifest = objectAt(*mask_attributes, "run_manifest");
  if (!binding || !publication || !derivation || !mask_manifest ||
      stringValue(*shape_attributes, "schema_id") !=
          "analysis.subject_shape_runs" ||
      shape_attributes->value("schema_version", 0) != 5 ||
      stringValue(*shape_attributes, "palette_run_name") !=
          selection.shape.run_id ||
      stringValue(*shape_attributes, "palette_run_completion_status") !=
          "complete" ||
      !shape_attributes->value("stage_selector_eligible", false) ||
      stringValue(*shape_attributes, "row_axis") !=
          "recording_subject_mask_bundle_rows" ||
      stringValue(*shape_attributes, "source_refined_subject_masks_run") !=
          selection.mask.run_id ||
      stringValue(*shape_attributes, "coordinate_contract") !=
          "canonical_v2" ||
      stringValue(*shape_attributes, "coordinate_binding_status") !=
          "bound_canonical_v2" ||
      stringValue(*shape_attributes, "publication_manifest_sha256") !=
          selection.shape.identity_digest ||
      stringValue(*shape_attributes,
                  "subject_shape_publication_manifest_sha256") !=
          selection.shape.identity_digest ||
      CanonicalJsonSha256(*publication) != selection.shape.identity_digest ||
      stringValue(*shape_attributes, "subject_shape_source_binding_sha256") !=
          selection.shape.manifest_payload_digest ||
      CanonicalJsonSha256(*binding) !=
          selection.shape.manifest_payload_digest ||
      derivation->value("schema_id", "") !=
          "palette.subject_shape_coordinate_derivation" ||
      derivation->value("schema_version", 0) != 2 ||
      derivation->value("transform_direction", "") !=
          "roi_local_px_to_source_camera_image_px" ||
      derivation->value("transform_policy", "") !=
          "exact_translation_only_v1" ||
      derivation->value("roi_local_point_arrays_retained", true) ||
      mask_manifest->value("schema_id", "") !=
          "palette.subject_mask_core.run_manifest" ||
      mask_manifest->value("schema_version", 0) != 5 ||
      !manifestPayloadMatches(*mask_manifest,
                              selection.mask.manifest_payload_digest)) {
    assignError(error_message,
                "Bound shape-v5 publication or mask manifest disagrees with "
                "the immutable selection");
    return nullptr;
  }

  size_t roi_width = 0;
  size_t roi_height = 0;
  std::string crop_run;
  try {
    const auto &frame_axis = binding->at("frame_axis");
    const auto &extent = binding->at("source_camera_extent");
    const auto &roi = binding->at("roi_raster_extent");
    const auto &authorities = binding->at("authorities");
    const auto labels =
        binding->at("component_labels").get<std::vector<std::string>>();
    const std::string mask_path = selection.mask.group + "/" +
                                  selection.mask.run_id;
    crop_run = stripGroup(authorities.at("crop_run_path").get<std::string>(),
                         "crop_runs");
    roi_width = roi.at("width_px").get<size_t>();
    roi_height = roi.at("height_px").get<size_t>();
    if (binding->value("schema_id", "") !=
            "palette.subject_shape.recording_mask_bundle_source" ||
        binding->value("schema_version", 0) != 1 ||
        binding->value("source_kind", "") !=
            "recording_subject_mask_bundle_v3" ||
        binding->value("recording_identity", "") != selection.recording_id ||
        binding->value("camera_identity", "") != selection.camera_id ||
        binding->value("row_count", size_t{0}) !=
            selection.observation_count ||
        frame_axis.value("domain", "") !=
            "zero_based_acquisition_camera_frame" ||
        frame_axis.value("source_total_frames", size_t{0}) !=
            selection.frame_count ||
        frame_axis.value("frame_row_offsets_length", size_t{0}) !=
            selection.frame_count + 1 ||
        extent.value("width_px", size_t{0}) != selection.source_width ||
        extent.value("height_px", size_t{0}) != selection.source_height ||
        roi_width == 0 || roi_height == 0 ||
        labels != std::vector<std::string>({"subject_body", "eye_left",
                                            "eye_right", "swim_bladder"}) ||
        binding->at("coordinate_transform").value("profile", "") !=
            "rowwise_roi_to_source_camera_translation_v1" ||
        binding->at("coordinate_transform").value("size_policy", "") !=
            "source_crop_wh_must_equal_dense_roi_extent" ||
        authorities.value("refined_run_path", "") != mask_path ||
        authorities.value("refined_manifest_payload_digest", "") !=
            selection.mask.manifest_payload_digest ||
        !validRunName(crop_run) ||
        bindingArrayDigest(*binding, "instance_key",
                           {selection.observation_count}, "uint64") !=
            selection.instance_key_digest ||
        bindingArrayDigest(*binding, "source_acquisition_frame_index",
                           {selection.observation_count}, "int64") !=
            selection.acquisition_frame_digest ||
        bindingArrayDigest(*binding, "frame_row_offsets",
                           {selection.frame_count + 1}, "int64") !=
            selection.frame_row_offsets_digest ||
        manifestArrayDigest(*mask_manifest, "instance_key",
                            {selection.observation_count}, "uint64") !=
            selection.instance_key_digest ||
        manifestArrayDigest(*mask_manifest,
                            "source_acquisition_frame_index",
                            {selection.observation_count}, "int64") !=
            selection.acquisition_frame_digest ||
        manifestArrayDigest(*mask_manifest, "frame_row_offsets",
                            {selection.frame_count + 1}, "int64") !=
            selection.frame_row_offsets_digest ||
        bindingArrayDigest(*binding, "source_crop_row_ids",
                           {selection.observation_count}, "int64") !=
            manifestArrayDigest(*mask_manifest, "source_crop_row_ids",
                                {selection.observation_count}, "int64") ||
        bindingArrayDigest(*binding, "source_crop_xywh",
                           {selection.observation_count, 4}, "float32") !=
            manifestArrayDigest(*mask_manifest, "source_crop_xywh",
                                {selection.observation_count, 4}, "float32")) {
      assignError(error_message,
                  "Shape-v5 source binding and strict mask lineage disagree");
      return nullptr;
    }
  } catch (const json::exception &) {
    assignError(error_message, "Shape-v5 source binding is malformed");
    return nullptr;
  }

  auto shape_frames = openExact<int64_t, 1>(
      archive, shape_base + "/source_acquisition_frame_index", &metrics,
      error_message);
  auto shape_keys = openExact<uint64_t, 1>(
      archive, shape_base + "/instance_key", &metrics, error_message);
  auto shape_crop_rows = openExact<int64_t, 1>(
      archive, shape_base + "/source_crop_row_ids", &metrics, error_message);
  auto mask_frames = openExact<int64_t, 1>(
      archive, mask_base + "/source_acquisition_frame_index", &metrics,
      error_message);
  auto mask_keys = openExact<uint64_t, 1>(
      archive, mask_base + "/instance_key", &metrics, error_message);
  auto mask_crop_rows = openExact<int64_t, 1>(
      archive, mask_base + "/source_crop_row_ids", &metrics, error_message);
  auto mask_crop_xywh = openExact<float, 2>(
      archive, mask_base + "/source_crop_xywh", &metrics, error_message);
  std::optional<ts::TensorStore<int64_t, 1>> offsets;
  if (!shared_index) {
    offsets = openExact<int64_t, 1>(
        archive, mask_base + "/frame_row_offsets", &metrics, error_message);
  }

  const std::string body = shape_base + "/body_frame";
  const std::string subject = shape_base + "/components/subject_body";
  const std::string bladder = shape_base + "/components/swim_bladder";
  struct CoordinateAuthorityExpectation {
    std::string path;
    const char *profile;
    const char *geometry_type;
    const char *origin;
    const char *overlay_status;
  };
  const std::vector<CoordinateAuthorityExpectation> coordinate_authorities = {
      {body + "/origin_xy", "source_camera_image_px.top_left_y_down.v1",
       "point_xy", "top_left", "direct"},
      {body + "/forward_axis_xy",
       "source_camera_image_px.unit_vector_y_down.v1", "vector_xy",
       "not_applicable", "not_suitable"},
      {body + "/left_axis_xy",
       "source_camera_image_px.unit_vector_y_down.v1", "vector_xy",
       "not_applicable", "not_suitable"},
      {subject + "/snout_tip_xy",
       "source_camera_image_px.top_left_y_down.v1", "point_xy", "top_left",
       "direct"},
      {subject + "/tail_base_xy",
       "source_camera_image_px.top_left_y_down.v1", "point_xy", "top_left",
       "direct"},
      {subject + "/tail_tip_xy",
       "source_camera_image_px.top_left_y_down.v1", "point_xy", "top_left",
       "direct"},
      {bladder + "/caudal_contour_point_xy",
       "source_camera_image_px.top_left_y_down.v1", "point_xy", "top_left",
       "direct"},
      {subject + "/centerline_xy",
       "source_camera_image_px.top_left_y_down.v1", "polyline_xy", "top_left",
       "direct"},
      {subject + "/bspline_sample_xy",
       "source_camera_image_px.top_left_y_down.v1", "polyline_xy", "top_left",
       "direct"},
      {subject + "/bspline_control_points_xy",
       "source_camera_image_px.top_left_y_down.v1", "polyline_xy", "top_left",
       "direct"},
      {subject + "/tail_sample_xy",
       "source_camera_image_px.top_left_y_down.v1", "polyline_xy", "top_left",
       "direct"},
      {subject + "/tail_normal_xy",
       "source_camera_image_px.unit_vector_y_down.v1", "vector_sequence_xy",
       "not_applicable", "not_suitable"},
  };
  for (const auto &authority : coordinate_authorities) {
    if (!coordinateAuthorityMatches(
            archive, authority.path, selection.source_width,
            selection.source_height, authority.profile,
            authority.geometry_type, authority.origin,
            authority.overlay_status)) {
      assignError(error_message,
                  "Shape-v5 direct source-camera coordinate authority is "
                  "invalid: " +
                      authority.path);
      return nullptr;
    }
  }
  if (!headingAuthorityMatches(archive, body + "/heading_deg",
                               selection.observation_count, *publication)) {
    assignError(error_message,
                "Shape-v5 body heading authority is invalid or ambiguous");
    return nullptr;
  }
  auto body_valid =
      openExact<bool, 1>(archive, body + "/valid", &metrics, error_message);
  auto axis_valid = openExact<bool, 1>(archive, body + "/axis_valid", &metrics,
                                       error_message);
  auto heading = openExact<float, 1>(archive, body + "/heading_deg", &metrics,
                                     error_message);
  auto body_origin = openExact<float, 2>(archive, body + "/origin_xy", &metrics,
                                         error_message);
  auto forward = openExact<float, 2>(archive, body + "/forward_axis_xy",
                                     &metrics, error_message);
  auto left = openExact<float, 2>(archive, body + "/left_axis_xy", &metrics,
                                  error_message);
  auto snout_valid = openExact<bool, 1>(archive, subject + "/snout_tip_valid",
                                        &metrics, error_message);
  auto snout = openExact<float, 2>(archive, subject + "/snout_tip_xy", &metrics,
                                   error_message);
  auto tail_base_valid = openExact<bool, 1>(
      archive, subject + "/tail_base_valid", &metrics, error_message);
  auto tail_base = openExact<float, 2>(archive, subject + "/tail_base_xy",
                                       &metrics, error_message);
  auto tail_tip = openExact<float, 2>(archive, subject + "/tail_tip_xy",
                                      &metrics, error_message);
  auto caudal_valid = openExact<bool, 1>(
      archive, bladder + "/caudal_contour_valid", &metrics, error_message);
  auto caudal = openExact<float, 2>(
      archive, bladder + "/caudal_contour_point_xy", &metrics, error_message);
  auto centerline_valid = openExact<bool, 1>(
      archive, subject + "/centerline_valid", &metrics, error_message);
  auto reaches_snout = openExact<bool, 1>(
      archive, subject + "/centerline_reaches_snout", &metrics,
      error_message);
  auto centerline = openExact<float, 3>(
      archive, subject + "/centerline_xy", &metrics, error_message);
  auto bspline_valid = openExact<bool, 1>(
      archive, subject + "/bspline_valid", &metrics, error_message);
  auto bspline = openExact<float, 3>(
      archive, subject + "/bspline_sample_xy", &metrics, error_message);
  auto controls = openExact<float, 3>(
      archive, subject + "/bspline_control_points_xy", &metrics,
      error_message);
  auto tail_sample_valid = openExact<bool, 1>(
      archive, subject + "/tail_sample_valid", &metrics, error_message);
  auto tail_samples = openExact<float, 3>(
      archive, subject + "/tail_sample_xy", &metrics, error_message);
  auto tail_normals = openExact<float, 3>(
      archive, subject + "/tail_normal_xy", &metrics, error_message);

  if (!shape_frames || !shape_keys || !shape_crop_rows || !mask_frames ||
      !mask_keys || !mask_crop_rows || !mask_crop_xywh ||
      (!shared_index && !offsets) ||
      !body_valid || !axis_valid || !heading || !body_origin || !forward ||
      !left || !snout_valid || !snout || !tail_base_valid || !tail_base ||
      !tail_tip || !caudal_valid || !caudal || !centerline_valid ||
      !reaches_snout || !centerline || !bspline_valid || !bspline ||
      !controls || !tail_sample_valid || !tail_samples || !tail_normals) {
    return nullptr;
  }

  const size_t rows = selection.observation_count;
  auto vectorShape = [&](const auto &store) {
    return exactShape(store, {rows});
  };
  auto pointShape = [&](const auto &store) {
    return exactShape(store, {rows, 2});
  };
  auto sequenceShape = [&](const auto &store) {
    return store.domain().shape()[0] == static_cast<ts::Index>(rows) &&
           store.domain().shape()[1] > 0 && store.domain().shape()[2] == 2;
  };
  if (!vectorShape(*shape_frames) || !vectorShape(*shape_keys) ||
      !vectorShape(*shape_crop_rows) || !vectorShape(*mask_frames) ||
      !vectorShape(*mask_keys) || !vectorShape(*mask_crop_rows) ||
      !exactShape(*mask_crop_xywh, {rows, 4}) ||
      (!shared_index &&
       !exactShape(*offsets, {selection.frame_count + 1})) ||
      !vectorShape(*body_valid) || !vectorShape(*axis_valid) ||
      !vectorShape(*heading) || !pointShape(*body_origin) ||
      !pointShape(*forward) || !pointShape(*left) ||
      !vectorShape(*snout_valid) || !pointShape(*snout) ||
      !vectorShape(*tail_base_valid) || !pointShape(*tail_base) ||
      !pointShape(*tail_tip) || !vectorShape(*caudal_valid) ||
      !pointShape(*caudal) || !vectorShape(*centerline_valid) ||
      !vectorShape(*reaches_snout) || !sequenceShape(*centerline) ||
      !vectorShape(*bspline_valid) || !sequenceShape(*bspline) ||
      !sequenceShape(*controls) || !vectorShape(*tail_sample_valid) ||
      !sequenceShape(*tail_samples) || !sequenceShape(*tail_normals) ||
      tail_samples->domain().shape()[1] != tail_normals->domain().shape()[1]) {
    assignError(error_message, "Shape-v5 array extents are incompatible");
    return nullptr;
  }

  std::vector<int64_t> retained_offsets;
  if (!shared_index &&
      !readRows(*offsets, 0, selection.frame_count + 1, 1,
                &retained_offsets, error_message)) {
    return nullptr;
  }
  const auto& effective_offsets =
      shared_index ? shared_index->offsets() : retained_offsets;
  metrics.offset_read_calls = shared_index ? 0 : 1;
  metrics.retained_offset_bytes =
      retained_offsets.capacity() * sizeof(int64_t);
  metrics.borrowed_offset_bytes =
      shared_index ? shared_index->retainedBytes() : 0;
  size_t maximum_observations = 0;
  if (effective_offsets.size() != selection.frame_count + 1 ||
      effective_offsets.front() != 0 ||
      effective_offsets.back() != static_cast<int64_t>(rows) ||
      !std::is_sorted(effective_offsets.begin(), effective_offsets.end()) ||
      effective_offsets.back() < 0) {
    assignError(error_message, "Bound mask frame offsets are invalid");
    return nullptr;
  }
  for (size_t frame = 0; frame < selection.frame_count; ++frame) {
    const auto count = effective_offsets[frame + 1] - effective_offsets[frame];
    if (count < 0 ||
        static_cast<uint64_t>(count) > request.max_observations_per_frame) {
      assignError(error_message,
                  "Bound shape frame exceeds maximum observations policy");
      return nullptr;
    }
    maximum_observations =
        std::max(maximum_observations, static_cast<size_t>(count));
  }
  metrics.maximum_observations_per_frame = maximum_observations;

  const size_t centerline_points =
      static_cast<size_t>(centerline->domain().shape()[1]);
  const size_t bspline_points =
      static_cast<size_t>(bspline->domain().shape()[1]);
  const size_t control_points =
      static_cast<size_t>(controls->domain().shape()[1]);
  const size_t tail_points =
      static_cast<size_t>(tail_samples->domain().shape()[1]);
  GeometrySources geometry{
      std::move(*body_valid),       std::move(*axis_valid),
      std::move(*heading),          std::move(*body_origin),
      std::move(*forward),          std::move(*left),
      std::move(*snout_valid),      std::move(*snout),
      std::move(*tail_base_valid),  std::move(*tail_base),
      std::move(*tail_tip),         std::move(*caudal_valid),
      std::move(*caudal),           std::move(*centerline_valid),
      std::move(*reaches_snout),    std::move(*centerline),
      std::move(*bspline_valid),    std::move(*bspline),
      std::move(*controls),         std::move(*tail_sample_valid),
      std::move(*tail_samples),     std::move(*tail_normals),
      centerline_points,            bspline_points,
      control_points,               tail_points};
  const uint64_t point_count =
      7 + geometry.centerline_points + geometry.bspline_points +
      geometry.control_points + 2 * geometry.tail_points;
  if (point_count >
      (std::numeric_limits<uint64_t>::max() -
       sizeof(SubjectShapeOverlayGeometry)) /
          sizeof(SubjectShapeOverlayPoint)) {
    assignError(error_message, "Shape-v5 decoded-row accounting overflows");
    return nullptr;
  }
  const uint64_t decoded_bytes_per_row =
      sizeof(SubjectShapeOverlayGeometry) +
      point_count * sizeof(SubjectShapeOverlayPoint) + 7 * sizeof(int64_t) +
      4 * sizeof(float);
  if (maximum_observations != 0 &&
      decoded_bytes_per_row >
          request.max_decoded_frame_bytes / maximum_observations) {
    assignError(error_message,
                "Shape-v5 maximum frame exceeds decoded-byte policy");
    return nullptr;
  }

  SubjectShapeOverlayDescriptor descriptor;
  descriptor.source_group = selection.shape.group;
  descriptor.run_name = selection.shape.run_id;
  descriptor.source_refined_subject_masks_run = selection.mask.run_id;
  descriptor.source_crop_run = std::move(crop_run);
  descriptor.schema_id = selection.shape.schema_id;
  descriptor.schema_version = selection.shape.schema_version;
  descriptor.method = stringValue(*shape_attributes, "method");
  descriptor.method_version = shape_attributes->value("method_version", 0);
  descriptor.row_axis = stringValue(*shape_attributes, "row_axis");
  descriptor.head_endpoint_semantics =
      stringValue(*shape_attributes, "head_endpoint_semantics");
  descriptor.row_count = rows;
  descriptor.camera_frame_count = selection.frame_count;
  descriptor.coordinate_width = selection.source_width;
  descriptor.coordinate_height = selection.source_height;
  descriptor.centerline_point_count = geometry.centerline_points;
  descriptor.bspline_sample_point_count = geometry.bspline_points;
  descriptor.bspline_control_point_count = geometry.control_points;
  descriptor.tail_sample_point_count = geometry.tail_points;
  descriptor.geometry_in_source_camera_coordinates = true;
  descriptor.validated_instance_keys = true;
  descriptor.maximum_observations_per_frame = maximum_observations;
  descriptor.publication_identity_digest = selection.shape.identity_digest;
  descriptor.source_binding_digest = selection.shape.manifest_payload_digest;
  descriptor.source_mask_manifest_payload_digest =
      selection.mask.manifest_payload_digest;

  if (open_metrics) {
    *open_metrics = metrics;
  }
  return std::make_unique<BoundSubjectShapeOverlayRepository>(
      std::move(descriptor), std::move(retained_offsets),
      shared_index,
      request.max_observations_per_frame, request.max_decoded_frame_bytes,
      decoded_bytes_per_row, roi_width, roi_height, std::move(*shape_frames),
      std::move(*shape_keys), std::move(*shape_crop_rows),
      std::move(*mask_frames), std::move(*mask_keys),
      std::move(*mask_crop_rows), std::move(*mask_crop_xywh),
      std::move(geometry));
}

} // namespace crimson::zarr
