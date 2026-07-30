#include "zarr/tensorstore_keypoint_v2_repository.h"

#include "zarr/archive_context_internal.h"
#include "zarr/canonical_json.h"
#include "zarr/zarr_metadata_equivalence.h"

#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

namespace ts = tensorstore;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

enum class Extent {
  Rows,
  FrameBoundaries,
  Keypoints,
  Two,
  Four,
  Signature,
  One,
};

struct ArrayDeclaration {
  const char *path;
  const char *dtype;
  size_t rank;
  Extent second = Extent::One;
  Extent third = Extent::One;
};

constexpr ArrayDeclaration kRawDeclarations[] = {
    {"instance_key", "uint64", 1},
    {"source_crop_row_ids", "int64", 1},
    {"source_acquisition_frame_index", "int64", 1},
    {"frame_indices", "int64", 1},
    {"frame_row_offsets", "int64", 1},
    {"source_crop_row_signature", "uint8", 2, Extent::Signature},
    {"keypoint_row_signature", "uint8", 2, Extent::Signature},
    {"keypoints_roi", "float32", 3, Extent::Keypoints, Extent::Two},
    {"keypoints_img", "float32", 3, Extent::Keypoints, Extent::Two},
    {"keypoint_confidences", "float32", 2, Extent::Keypoints},
    {"keypoint_valid", "bool", 2, Extent::Keypoints},
    {"pose_confidence", "float32", 1},
    {"pose_bbox_xyxy_roi", "float32", 2, Extent::Four},
    {"pose_bbox_xyxy_img", "float32", 2, Extent::Four},
    {"pose_success", "bool", 1},
};

constexpr ArrayDeclaration kQualityDeclarations[] = {
    {"instance_key", "uint64", 1},
    {"source_keypoint_row_ids", "int64", 1},
    {"source_keypoint_row_signature", "uint8", 2, Extent::Signature},
    {"frame_indices", "int64", 1},
    {"frame_row_offsets", "int64", 1},
    {"keypoint_metric_values", "float32", 3, Extent::Keypoints, Extent::One},
    {"keypoint_metric_valid", "bool", 3, Extent::Keypoints, Extent::One},
    {"pose_metric_values", "float32", 2, Extent::One},
    {"pose_metric_valid", "bool", 2, Extent::One},
    {"keypoint_quality_flags", "uint16", 2, Extent::Keypoints},
    {"pose_quality_flags", "uint16", 1},
    {"proposed_keypoint_valid", "bool", 2, Extent::Keypoints},
    {"proposed_pose_usable", "bool", 1},
};

constexpr ArrayDeclaration kRefinedDeclarations[] = {
    {"instance_key", "uint64", 1},
    {"source_crop_row_ids", "int64", 1},
    {"source_acquisition_frame_index", "int64", 1},
    {"frame_indices", "int64", 1},
    {"frame_row_offsets", "int64", 1},
    {"source_crop_row_signature", "uint8", 2, Extent::Signature},
    {"keypoint_row_signature", "uint8", 2, Extent::Signature},
    {"keypoints_roi", "float32", 3, Extent::Keypoints, Extent::Two},
    {"keypoints_img", "float32", 3, Extent::Keypoints, Extent::Two},
    {"keypoint_confidences", "float32", 2, Extent::Keypoints},
    {"keypoint_valid", "bool", 2, Extent::Keypoints},
    {"pose_confidence", "float32", 1},
    {"pose_bbox_xyxy_roi", "float32", 2, Extent::Four},
    {"pose_bbox_xyxy_img", "float32", 2, Extent::Four},
    {"source_success", "bool", 1},
    {"refined_success", "bool", 1},
    {"keypoint_edit_flags", "bool", 2, Extent::Keypoints},
    {"flip_corrected", "bool", 1},
    {"confidence_valid", "bool", 1},
    {"geometry_valid", "bool", 1},
    {"usable_keypoints", "bool", 1},
    {"review_state_codes", "uint8", 1},
    {"reason_codes", "uint16", 1},
};

constexpr ArrayDeclaration kBodyFrameDeclarations[] = {
    {"instance_key", "uint64", 1},
    {"source_keypoint_row_ids", "int64", 1},
    {"source_keypoint_row_signature", "uint8", 2, Extent::Signature},
    {"frame_indices", "int64", 1},
    {"frame_row_offsets", "int64", 1},
    {"origin_xy", "float32", 2, Extent::Two},
    {"forward_axis_xy", "float32", 2, Extent::Two},
    {"left_axis_xy", "float32", 2, Extent::Two},
    {"axis_valid", "bool", 1},
    {"heading_deg", "float32", 1},
};

template <typename T, ts::DimensionIndex Rank>
using Store = ts::TensorStore<T, Rank>;

template <typename T, ts::DimensionIndex Rank>
using ArrayReadFuture = ts::Future<ts::SharedArray<T, Rank, ts::offset_origin>>;

struct RawStores {
  Store<uint64_t, 1> instance_key;
  Store<int64_t, 1> source_crop_row_ids;
  Store<int64_t, 1> source_acquisition_frame_index;
  Store<int64_t, 1> frame_indices;
  Store<int64_t, 1> frame_row_offsets;
  Store<uint8_t, 2> source_crop_row_signature;
  Store<uint8_t, 2> keypoint_row_signature;
  Store<float, 3> keypoints_roi;
  Store<float, 3> keypoints_img;
  Store<float, 2> keypoint_confidences;
  Store<bool, 2> keypoint_valid;
  Store<float, 1> pose_confidence;
  Store<float, 2> pose_bbox_xyxy_roi;
  Store<float, 2> pose_bbox_xyxy_img;
  Store<bool, 1> pose_success;
};

struct QualityStores {
  Store<uint64_t, 1> instance_key;
  Store<int64_t, 1> source_keypoint_row_ids;
  Store<uint8_t, 2> source_keypoint_row_signature;
  Store<int64_t, 1> frame_indices;
  Store<int64_t, 1> frame_row_offsets;
  Store<float, 3> keypoint_metric_values;
  Store<bool, 3> keypoint_metric_valid;
  Store<float, 2> pose_metric_values;
  Store<bool, 2> pose_metric_valid;
  Store<uint16_t, 2> keypoint_quality_flags;
  Store<uint16_t, 1> pose_quality_flags;
  Store<bool, 2> proposed_keypoint_valid;
  Store<bool, 1> proposed_pose_usable;
};

struct RefinedStores {
  Store<uint64_t, 1> instance_key;
  Store<int64_t, 1> source_crop_row_ids;
  Store<int64_t, 1> source_acquisition_frame_index;
  Store<int64_t, 1> frame_indices;
  Store<int64_t, 1> frame_row_offsets;
  Store<uint8_t, 2> source_crop_row_signature;
  Store<uint8_t, 2> keypoint_row_signature;
  Store<float, 3> keypoints_roi;
  Store<float, 3> keypoints_img;
  Store<float, 2> keypoint_confidences;
  Store<bool, 2> keypoint_valid;
  Store<float, 1> pose_confidence;
  Store<float, 2> pose_bbox_xyxy_roi;
  Store<float, 2> pose_bbox_xyxy_img;
  Store<bool, 1> source_success;
  Store<bool, 1> refined_success;
  Store<bool, 2> keypoint_edit_flags;
  Store<bool, 1> flip_corrected;
  Store<bool, 1> confidence_valid;
  Store<bool, 1> geometry_valid;
  Store<bool, 1> usable_keypoints;
  Store<uint8_t, 1> review_state_codes;
  Store<uint16_t, 1> reason_codes;
};

struct BodyFrameStores {
  Store<uint64_t, 1> instance_key;
  Store<int64_t, 1> source_keypoint_row_ids;
  Store<uint8_t, 2> source_keypoint_row_signature;
  Store<int64_t, 1> frame_indices;
  Store<int64_t, 1> frame_row_offsets;
  Store<float, 2> origin_xy;
  Store<float, 2> forward_axis_xy;
  Store<float, 2> left_axis_xy;
  Store<bool, 1> axis_valid;
  Store<float, 1> heading_deg;
};

void assignError(std::string *destination, std::string message) {
  if (destination) {
    *destination = std::move(message);
  }
}

double elapsedMilliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

const json *consolidatedEntry(const json &root, const std::string &path) {
  try {
    const auto &metadata = root.at("consolidated_metadata").at("metadata");
    const auto found = metadata.find(path);
    return found == metadata.end() ? nullptr : &*found;
  } catch (const json::exception &) {
    return nullptr;
  }
}

size_t extentValue(Extent extent, const KeypointV2ManifestSummary &summary) {
  switch (extent) {
  case Extent::Rows:
    return summary.row_count;
  case Extent::FrameBoundaries:
    return summary.frame_count + 1;
  case Extent::Keypoints:
    return summary.keypoint_count;
  case Extent::Two:
    return 2;
  case Extent::Four:
    return 4;
  case Extent::Signature:
    return 32;
  case Extent::One:
    return 1;
  }
  return 0;
}

std::vector<size_t> expectedShape(const ArrayDeclaration &declaration,
                                  const KeypointV2ManifestSummary &summary) {
  const size_t first = std::string_view(declaration.path) == "frame_row_offsets"
                           ? summary.frame_count + 1
                           : summary.row_count;
  std::vector<size_t> result = {first};
  if (declaration.rank >= 2) {
    result.push_back(extentValue(declaration.second, summary));
  }
  if (declaration.rank >= 3) {
    result.push_back(extentValue(declaration.third, summary));
  }
  return result;
}

bool normalizeRunGroup(json *metadata) {
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
         consolidation->at("metadata").is_object() &&
         consolidation->at("metadata").empty());
    if (!empty) {
      return false;
    }
    metadata->erase(consolidation);
  }
  auto &attributes = metadata->at("attributes");
  if (!attributes.is_object()) {
    return false;
  }
  attributes.erase("run_manifest");
  return true;
}

template <size_t N, typename Validator>
bool validateStageMetadata(const ArchiveContext::Impl &archive,
                           const std::string &base,
                           const ArrayDeclaration (&declarations)[N],
                           Validator validate_manifest,
                           KeypointV2ManifestSummary *summary, json *root_out,
                           KeypointV2RepositoryOpenMetrics *metrics,
                           std::string *error) {
  const auto root = internal::ReadArchiveJson(archive, "zarr.json");
  if (metrics) {
    ++metrics->root_metadata_reads;
  }
  if (!root || root->value("zarr_format", 0) != 3 ||
      root->value("node_type", "") != "group" ||
      root->at("consolidated_metadata").value("kind", "") != "inline" ||
      root->at("consolidated_metadata").value("must_understand", true)) {
    assignError(error, "Keypoint archive lacks exact inline Zarr v3 metadata");
    return false;
  }
  const auto *consolidated_group = consolidatedEntry(*root, base);
  const auto direct_group =
      internal::ReadArchiveJson(archive, base + "/zarr.json");
  if (metrics) {
    ++metrics->direct_metadata_reads;
  }
  if (!direct_group || !consolidated_group ||
      !internal::EquivalentDirectAndConsolidatedZarrNode(*direct_group,
                                                         *consolidated_group) ||
      !direct_group->contains("attributes") ||
      !direct_group->at("attributes").contains("run_manifest") ||
      !validate_manifest(direct_group->at("attributes").at("run_manifest"),
                         summary, error)) {
    if (error && error->empty()) {
      *error = "Keypoint run group metadata is invalid: " + base;
    }
    return false;
  }

  json normalized_declarations = json::object();
  json normalized_group = *direct_group;
  if (!normalizeRunGroup(&normalized_group)) {
    assignError(error, "Keypoint run group consolidation is invalid: " + base);
    return false;
  }
  normalized_declarations[""] = std::move(normalized_group);
  std::unordered_set<std::string> expected_paths;
  expected_paths.reserve(N + 1);
  expected_paths.insert(base);
  for (const auto &declaration : declarations) {
    const std::string path = base + "/" + declaration.path;
    expected_paths.insert(path);
    const auto *consolidated = consolidatedEntry(*root, path);
    const auto direct = internal::ReadArchiveJson(archive, path + "/zarr.json");
    if (metrics) {
      ++metrics->direct_metadata_reads;
    }
    if (!direct || !consolidated ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                           *consolidated) ||
        consolidated->value("zarr_format", 0) != 3 ||
        consolidated->value("node_type", "") != "array" ||
        consolidated->contains("consolidated_metadata") ||
        consolidated->value("data_type", "") != declaration.dtype ||
        !consolidated->at("shape").is_array()) {
      assignError(error, "Keypoint array declaration is invalid: " + path);
      return false;
    }
    const auto shape = consolidated->at("shape").get<std::vector<size_t>>();
    if (shape != expectedShape(declaration, *summary)) {
      assignError(error, "Keypoint array shape is invalid: " + path);
      return false;
    }
    normalized_declarations[declaration.path] = *direct;
  }
  const auto &all = root->at("consolidated_metadata").at("metadata");
  const std::string prefix = base + "/";
  for (auto item = all.begin(); item != all.end(); ++item) {
    if ((item.key() == base || item.key().rfind(prefix, 0) == 0) &&
        expected_paths.find(item.key()) == expected_paths.end()) {
      assignError(error, "Unexpected keypoint array or group: " + item.key());
      return false;
    }
  }
  const json digest_document = {
      {"scope",
       "exact_group_and_array_declarations_with_attributes_redacting_only_"
       "run_manifest"},
      {"declarations", std::move(normalized_declarations)}};
  if (CanonicalJsonSha256(digest_document) !=
      summary->metadata_declarations_digest) {
    assignError(error,
                "Keypoint metadata declarations digest mismatch: " + base);
    return false;
  }
  if (metrics) {
    metrics->consolidated_array_declarations += N;
  }
  *root_out = *root;
  return true;
}

template <typename T, ts::DimensionIndex Rank>
bool openExact(const ArchiveContext::Impl &archive, const json &root,
               const std::string &path, Store<T, Rank> *output,
               KeypointV2RepositoryOpenMetrics *metrics, std::string *error) {
  auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  const auto *metadata = consolidatedEntry(root, path);
  if (!spec || !metadata) {
    assignError(error, "Missing exact keypoint array metadata: " + path);
    return false;
  }
  (*spec)["metadata"] = *metadata;
  auto opened = ts::Open<T, Rank>(
                    *spec, ts::OpenMode::open | ts::OpenMode::assume_metadata,
                    ts::ReadWriteMode::read, archive.context)
                    .result();
  if (!opened.ok()) {
    assignError(error, path + ": " + opened.status().ToString());
    return false;
  }
  *output = *opened;
  if (metrics) {
    ++metrics->exact_handle_opens;
  }
  return true;
}

template <typename T, ts::DimensionIndex Rank>
ArrayReadFuture<T, Rank> issueRangeRead(const Store<T, Rank> &store,
                                        size_t first, size_t last) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  return ts::Read(store | ts::IdentityTransform(domain));
}

template <typename T, ts::DimensionIndex Rank>
bool collectRangeRead(ArrayReadFuture<T, Rank> *future, size_t rows,
                      size_t values_per_row, std::vector<T> *output,
                      std::string *error) {
  auto read = future->result();
  if (!read.ok() || read->rank() != Rank ||
      read->byte_strides().size() != Rank) {
    assignError(error, read.ok() ? "Keypoint read shape mismatch"
                                 : read.status().ToString());
    return false;
  }
  size_t actual_values_per_row = 1;
  for (ts::DimensionIndex dimension = 1; dimension < Rank; ++dimension) {
    actual_values_per_row *= static_cast<size_t>(read->shape()[dimension]);
  }
  if (actual_values_per_row != values_per_row) {
    assignError(error, "Keypoint read trailing extent mismatch");
    return false;
  }
  output->resize(rows * values_per_row);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < rows; ++row) {
    for (size_t value = 0; value < values_per_row; ++value) {
      ts::Index offset = static_cast<ts::Index>(row) * read->byte_strides()[0];
      size_t remainder = value;
      for (ts::DimensionIndex dimension = Rank - 1; dimension >= 1;
           --dimension) {
        const size_t extent = static_cast<size_t>(read->shape()[dimension]);
        const size_t coordinate = remainder % extent;
        remainder /= extent;
        offset += static_cast<ts::Index>(coordinate) *
                  read->byte_strides()[dimension];
        if (dimension == 1) {
          break;
        }
      }
      (*output)[row * values_per_row + value] =
          *reinterpret_cast<const T *>(origin + offset);
    }
  }
  return true;
}

template <typename T, ts::DimensionIndex Rank>
bool readRange(const Store<T, Rank> &store, size_t first, size_t last,
               size_t values_per_row, std::vector<T> *output,
               std::string *error) {
  if (!output || last < first ||
      last > static_cast<size_t>(store.domain().shape()[0])) {
    assignError(error, "Keypoint row range is invalid");
    return false;
  }
  if (first == last) {
    output->clear();
    return true;
  }
  auto future = issueRangeRead(store, first, last);
  return collectRangeRead(&future, last - first, values_per_row, output, error);
}

template <ts::DimensionIndex Rank>
bool collectBoolRangeRead(ArrayReadFuture<bool, Rank> *future, size_t rows,
                          size_t values_per_row, std::vector<uint8_t> *output,
                          std::string *error) {
  auto read = future->result();
  if (!read.ok() || read->rank() != Rank ||
      read->byte_strides().size() != Rank) {
    assignError(error, read.ok() ? "Keypoint boolean read shape mismatch"
                                 : read.status().ToString());
    return false;
  }
  size_t actual_values_per_row = 1;
  for (ts::DimensionIndex dimension = 1; dimension < Rank; ++dimension) {
    actual_values_per_row *= static_cast<size_t>(read->shape()[dimension]);
  }
  if (actual_values_per_row != values_per_row) {
    assignError(error, "Keypoint boolean trailing extent mismatch");
    return false;
  }
  output->resize(rows * values_per_row);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < rows; ++row) {
    for (size_t value = 0; value < values_per_row; ++value) {
      ts::Index offset = static_cast<ts::Index>(row) * read->byte_strides()[0];
      size_t remainder = value;
      for (ts::DimensionIndex dimension = Rank - 1; dimension >= 1;
           --dimension) {
        const size_t extent = static_cast<size_t>(read->shape()[dimension]);
        const size_t coordinate = remainder % extent;
        remainder /= extent;
        offset += static_cast<ts::Index>(coordinate) *
                  read->byte_strides()[dimension];
        if (dimension == 1) {
          break;
        }
      }
      output->at(row * values_per_row + value) =
          *reinterpret_cast<const bool *>(origin + offset) ? 1 : 0;
    }
  }
  return true;
}

template <ts::DimensionIndex Rank>
bool readBoolRange(const Store<bool, Rank> &store, size_t first, size_t last,
                   size_t values_per_row, std::vector<uint8_t> *output,
                   std::string *error) {
  if (!output || last < first ||
      last > static_cast<size_t>(store.domain().shape()[0])) {
    assignError(error, "Keypoint boolean row range is invalid");
    return false;
  }
  if (first == last) {
    output->clear();
    return true;
  }
  auto future = issueRangeRead(store, first, last);
  return collectBoolRangeRead(&future, last - first, values_per_row, output,
                              error);
}

template <typename T, ts::DimensionIndex Rank>
bool readAll(const Store<T, Rank> &store, size_t values_per_row,
             std::vector<T> *output, std::string *error) {
  return readRange(store, 0, static_cast<size_t>(store.domain().shape()[0]),
                   values_per_row, output, error);
}

template <ts::DimensionIndex Rank>
bool readAllBool(const Store<bool, Rank> &store, size_t values_per_row,
                 std::vector<uint8_t> *output, std::string *error) {
  return readBoolRange(store, 0, static_cast<size_t>(store.domain().shape()[0]),
                       values_per_row, output, error);
}

template <typename T>
bool equalVectors(const std::vector<T> &left, const std::vector<T> &right,
                  const char *label, std::string *error) {
  if (left != right) {
    assignError(error,
                std::string("Keypoint source binding mismatch: ") + label);
    return false;
  }
  return true;
}

bool openRawStores(const ArchiveContext::Impl &archive, const json &root,
                   const std::string &base, RawStores *stores,
                   KeypointV2RepositoryOpenMetrics *metrics,
                   std::string *error) {
#define OPEN_RAW(type, rank, member)                                           \
  if (!openExact<type, rank>(archive, root, base + "/" #member,                \
                             &stores->member, metrics, error))                 \
  return false
  OPEN_RAW(uint64_t, 1, instance_key);
  OPEN_RAW(int64_t, 1, source_crop_row_ids);
  OPEN_RAW(int64_t, 1, source_acquisition_frame_index);
  OPEN_RAW(int64_t, 1, frame_indices);
  OPEN_RAW(int64_t, 1, frame_row_offsets);
  OPEN_RAW(uint8_t, 2, source_crop_row_signature);
  OPEN_RAW(uint8_t, 2, keypoint_row_signature);
  OPEN_RAW(float, 3, keypoints_roi);
  OPEN_RAW(float, 3, keypoints_img);
  OPEN_RAW(float, 2, keypoint_confidences);
  OPEN_RAW(bool, 2, keypoint_valid);
  OPEN_RAW(float, 1, pose_confidence);
  OPEN_RAW(float, 2, pose_bbox_xyxy_roi);
  OPEN_RAW(float, 2, pose_bbox_xyxy_img);
  OPEN_RAW(bool, 1, pose_success);
#undef OPEN_RAW
  return true;
}

bool openQualityStores(const ArchiveContext::Impl &archive, const json &root,
                       const std::string &base, QualityStores *stores,
                       KeypointV2RepositoryOpenMetrics *metrics,
                       std::string *error) {
#define OPEN_QUALITY(type, rank, member)                                       \
  if (!openExact<type, rank>(archive, root, base + "/" #member,                \
                             &stores->member, metrics, error))                 \
  return false
  OPEN_QUALITY(uint64_t, 1, instance_key);
  OPEN_QUALITY(int64_t, 1, source_keypoint_row_ids);
  OPEN_QUALITY(uint8_t, 2, source_keypoint_row_signature);
  OPEN_QUALITY(int64_t, 1, frame_indices);
  OPEN_QUALITY(int64_t, 1, frame_row_offsets);
  OPEN_QUALITY(float, 3, keypoint_metric_values);
  OPEN_QUALITY(bool, 3, keypoint_metric_valid);
  OPEN_QUALITY(float, 2, pose_metric_values);
  OPEN_QUALITY(bool, 2, pose_metric_valid);
  OPEN_QUALITY(uint16_t, 2, keypoint_quality_flags);
  OPEN_QUALITY(uint16_t, 1, pose_quality_flags);
  OPEN_QUALITY(bool, 2, proposed_keypoint_valid);
  OPEN_QUALITY(bool, 1, proposed_pose_usable);
#undef OPEN_QUALITY
  return true;
}

bool openRefinedStores(const ArchiveContext::Impl &archive, const json &root,
                       const std::string &base, RefinedStores *stores,
                       KeypointV2RepositoryOpenMetrics *metrics,
                       std::string *error) {
#define OPEN_REFINED(type, rank, member)                                       \
  if (!openExact<type, rank>(archive, root, base + "/" #member,                \
                             &stores->member, metrics, error))                 \
  return false
  OPEN_REFINED(uint64_t, 1, instance_key);
  OPEN_REFINED(int64_t, 1, source_crop_row_ids);
  OPEN_REFINED(int64_t, 1, source_acquisition_frame_index);
  OPEN_REFINED(int64_t, 1, frame_indices);
  OPEN_REFINED(int64_t, 1, frame_row_offsets);
  OPEN_REFINED(uint8_t, 2, source_crop_row_signature);
  OPEN_REFINED(uint8_t, 2, keypoint_row_signature);
  OPEN_REFINED(float, 3, keypoints_roi);
  OPEN_REFINED(float, 3, keypoints_img);
  OPEN_REFINED(float, 2, keypoint_confidences);
  OPEN_REFINED(bool, 2, keypoint_valid);
  OPEN_REFINED(float, 1, pose_confidence);
  OPEN_REFINED(float, 2, pose_bbox_xyxy_roi);
  OPEN_REFINED(float, 2, pose_bbox_xyxy_img);
  OPEN_REFINED(bool, 1, source_success);
  OPEN_REFINED(bool, 1, refined_success);
  OPEN_REFINED(bool, 2, keypoint_edit_flags);
  OPEN_REFINED(bool, 1, flip_corrected);
  OPEN_REFINED(bool, 1, confidence_valid);
  OPEN_REFINED(bool, 1, geometry_valid);
  OPEN_REFINED(bool, 1, usable_keypoints);
  OPEN_REFINED(uint8_t, 1, review_state_codes);
  OPEN_REFINED(uint16_t, 1, reason_codes);
#undef OPEN_REFINED
  return true;
}

bool openBodyFrameStores(const ArchiveContext::Impl &archive, const json &root,
                         const std::string &base, BodyFrameStores *stores,
                         KeypointV2RepositoryOpenMetrics *metrics,
                         std::string *error) {
#define OPEN_BODY(type, rank, member)                                          \
  if (!openExact<type, rank>(archive, root, base + "/" #member,                \
                             &stores->member, metrics, error))                 \
  return false
  OPEN_BODY(uint64_t, 1, instance_key);
  OPEN_BODY(int64_t, 1, source_keypoint_row_ids);
  OPEN_BODY(uint8_t, 2, source_keypoint_row_signature);
  OPEN_BODY(int64_t, 1, frame_indices);
  OPEN_BODY(int64_t, 1, frame_row_offsets);
  OPEN_BODY(float, 2, origin_xy);
  OPEN_BODY(float, 2, forward_axis_xy);
  OPEN_BODY(float, 2, left_axis_xy);
  OPEN_BODY(bool, 1, axis_valid);
  OPEN_BODY(float, 1, heading_deg);
#undef OPEN_BODY
  return true;
}

struct FramePayload {
  std::vector<uint64_t> keys;
  std::vector<int64_t> frame_indices;
  std::vector<int64_t> source_crop_rows;
  std::vector<uint8_t> row_signatures;
  std::vector<float> points;
  std::vector<uint8_t> point_valid;
  std::vector<uint8_t> source_success;
  std::vector<uint8_t> refined_success;
  std::vector<uint8_t> edit_flags;
  std::vector<uint8_t> flip_corrected;
  std::vector<uint8_t> confidence_valid;
  std::vector<uint8_t> geometry_valid;
  std::vector<uint8_t> usable;
  std::vector<uint8_t> review_states;
  std::vector<uint16_t> reason_codes;
  std::vector<float> origins;
  std::vector<uint8_t> axis_valid;
  std::vector<float> headings;
  std::vector<uint64_t> body_keys;
  std::vector<int64_t> body_frame_indices;
  std::vector<int64_t> body_source_rows;
  std::vector<uint8_t> body_signatures;
};

struct FramePayloadFutures {
  ArrayReadFuture<uint64_t, 1> keys;
  ArrayReadFuture<int64_t, 1> frame_indices;
  ArrayReadFuture<int64_t, 1> source_crop_rows;
  ArrayReadFuture<uint8_t, 2> row_signatures;
  ArrayReadFuture<float, 3> points;
  ArrayReadFuture<bool, 2> point_valid;
  ArrayReadFuture<bool, 1> source_success;
  ArrayReadFuture<bool, 1> refined_success;
  ArrayReadFuture<bool, 2> edit_flags;
  ArrayReadFuture<bool, 1> flip_corrected;
  ArrayReadFuture<bool, 1> confidence_valid;
  ArrayReadFuture<bool, 1> geometry_valid;
  ArrayReadFuture<bool, 1> usable;
  ArrayReadFuture<uint8_t, 1> review_states;
  ArrayReadFuture<uint16_t, 1> reason_codes;
  ArrayReadFuture<uint64_t, 1> body_keys;
  ArrayReadFuture<int64_t, 1> body_frame_indices;
  ArrayReadFuture<int64_t, 1> body_source_rows;
  ArrayReadFuture<uint8_t, 2> body_signatures;
  ArrayReadFuture<float, 2> origins;
  ArrayReadFuture<bool, 1> axis_valid;
  ArrayReadFuture<float, 1> headings;
};

class TensorStoreKeypointV2Repository final : public KeypointV2Repository {
public:
  TensorStoreKeypointV2Repository(KeypointOverlayDescriptor overlay_descriptor,
                                  KeypointV2RepositoryDescriptor descriptor,
                                  RawStores raw, QualityStores quality,
                                  std::optional<RefinedStores> refined,
                                  BodyFrameStores body,
                                  std::vector<int64_t> raw_offsets,
                                  std::vector<int64_t> selected_offsets,
                                  std::vector<int64_t> body_offsets)
      : overlay_descriptor_(std::move(overlay_descriptor)),
        descriptor_(std::move(descriptor)), raw_(std::move(raw)),
        quality_(std::move(quality)), refined_(std::move(refined)),
        body_(std::move(body)), raw_offsets_(std::move(raw_offsets)),
        selected_offsets_(std::move(selected_offsets)),
        body_offsets_(std::move(body_offsets)) {}

  const KeypointOverlayDescriptor &descriptor() const override {
    return overlay_descriptor_;
  }

  const KeypointV2RepositoryDescriptor &v2Descriptor() const override {
    return descriptor_;
  }

  KeypointV2RepositoryAccessMetrics accessMetrics() const override {
    KeypointV2RepositoryAccessMetrics result;
    result.frame_requests = frame_requests_.load(std::memory_order_relaxed);
    result.rows_resolved = rows_resolved_.load(std::memory_order_relaxed);
    result.payload_read_calls =
        payload_read_calls_.load(std::memory_order_relaxed);
    result.payload_read_batches =
        payload_read_batches_.load(std::memory_order_relaxed);
    result.maximum_columns_per_batch =
        maximum_columns_per_batch_.load(std::memory_order_relaxed);
    result.quality_payload_read_calls =
        quality_payload_read_calls_.load(std::memory_order_relaxed);
    result.read_failures = read_failures_.load(std::memory_order_relaxed);
    return result;
  }

  RepositoryMemoryMetrics memoryMetrics() const override {
    RepositoryMemoryMetrics result;
    result.retained_index_bytes =
        (raw_offsets_.capacity() + selected_offsets_.capacity() +
         body_offsets_.capacity()) *
        sizeof(int64_t);
    return result;
  }

  KeypointOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    frame_requests_.fetch_add(1, std::memory_order_relaxed);
    KeypointOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 || static_cast<uint64_t>(camera_frame) >=
                                overlay_descriptor_.camera_frame_count) {
      result.status = KeypointOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0) {
      result.status = KeypointOverlayStatus::InvalidDimensions;
      return result;
    }
    const size_t frame = static_cast<size_t>(camera_frame);
    const auto &offsets = selectedOffsets();
    const size_t first = static_cast<size_t>(offsets[frame]);
    const size_t last = static_cast<size_t>(offsets[frame + 1]);
    if (first == last) {
      result.status = KeypointOverlayStatus::Missing;
      return result;
    }
    FramePayload payload;
    std::string error;
    if (!readFramePayload(first, last, &payload, &error)) {
      read_failures_.fetch_add(1, std::memory_order_relaxed);
      result.status = KeypointOverlayStatus::ReadFailed;
      result.error = std::move(error);
      return result;
    }
    const size_t rows = last - first;
    result.status = KeypointOverlayStatus::Mapped;
    result.detections.reserve(rows);
    const size_t keypoints = descriptor_.selected.keypoint_count;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (size_t local = 0; local < rows; ++local) {
      KeypointOverlayDetection detection;
      detection.instance_key = payload.keys[local];
      detection.detection_index = static_cast<int64_t>(first + local);
      detection.source_crop_row_id = payload.source_crop_rows[local];
      detection.refined_keypoints = descriptor_.refined;
      detection.source_success = payload.source_success[local] != 0;
      detection.refined_success = payload.refined_success[local] != 0;
      detection.keypoint_usable = payload.usable[local] != 0;
      detection.keypoint_flip_corrected = payload.flip_corrected[local] != 0;
      detection.confidence_valid = payload.confidence_valid[local] != 0;
      detection.geometry_valid = payload.geometry_valid[local] != 0;
      detection.review_state_code = payload.review_states[local];
      detection.reason_code = payload.reason_codes[local];
      detection.heading_from_body_frame = true;
      detection.keypoint_edit_flags.assign(
          payload.edit_flags.begin() +
              static_cast<ptrdiff_t>(local * keypoints),
          payload.edit_flags.begin() +
              static_cast<ptrdiff_t>((local + 1) * keypoints));
      detection.keypoints.reserve(keypoints);
      for (size_t point = 0; point < keypoints; ++point) {
        const size_t index = local * keypoints + point;
        if (!payload.point_valid[index]) {
          detection.keypoints.push_back({nan, nan});
        } else {
          detection.keypoints.push_back(
              {payload.points[index * 2], payload.points[index * 2 + 1]});
        }
      }
      const bool body_valid = payload.axis_valid[local] != 0;
      detection.heading_valid = body_valid &&
                                std::isfinite(payload.origins[local * 2]) &&
                                std::isfinite(payload.origins[local * 2 + 1]) &&
                                std::isfinite(payload.headings[local]);
      if (detection.heading_valid) {
        detection.heading_origin = KeypointOverlayPoint{
            payload.origins[local * 2], payload.origins[local * 2 + 1]};
        detection.heading_degrees = payload.headings[local];
      }
      result.detections.push_back(std::move(detection));
    }
    rows_resolved_.fetch_add(rows, std::memory_order_relaxed);
    return result;
  }

  bool validateQualityPayloadBindings(std::string *error) override {
    std::lock_guard<std::mutex> lock(quality_validation_mutex_);
    if (quality_validated_.load(std::memory_order_acquire)) {
      return true;
    }
    std::vector<int64_t> offsets;
    std::vector<int64_t> frames;
    std::vector<int64_t> source_rows;
    std::vector<uint64_t> keys;
    std::vector<uint8_t> signatures;
    std::vector<int64_t> raw_frames;
    std::vector<uint64_t> raw_keys;
    std::vector<uint8_t> raw_signatures;
    quality_payload_read_calls_.fetch_add(5, std::memory_order_relaxed);
    if (!readAll(raw_.frame_indices, 1, &raw_frames, error) ||
        !readAll(raw_.instance_key, 1, &raw_keys, error) ||
        !readAll(raw_.keypoint_row_signature, 32, &raw_signatures, error) ||
        !readAll(quality_.frame_row_offsets, 1, &offsets, error) ||
        !readAll(quality_.frame_indices, 1, &frames, error) ||
        !readAll(quality_.source_keypoint_row_ids, 1, &source_rows, error) ||
        !readAll(quality_.instance_key, 1, &keys, error) ||
        !readAll(quality_.source_keypoint_row_signature, 32, &signatures,
                 error)) {
      read_failures_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (!ValidateKeypointV2FrameIndex(raw_offsets_, raw_frames,
                                      descriptor_.raw.frame_count,
                                      descriptor_.raw.row_count, error) ||
        !ValidateKeypointV2InstanceKeys(raw_keys, descriptor_.raw.row_count,
                                        error) ||
        !ValidateKeypointV2FrameIndex(offsets, frames,
                                      descriptor_.quality.frame_count,
                                      descriptor_.quality.row_count, error) ||
        !equalVectors(offsets, raw_offsets_, "quality offsets", error) ||
        !equalVectors(frames, raw_frames, "quality frames", error) ||
        !equalVectors(keys, raw_keys, "quality instance keys", error) ||
        !equalVectors(signatures, raw_signatures, "quality signatures",
                      error)) {
      return false;
    }
    for (size_t row = 0; row < source_rows.size(); ++row) {
      if (source_rows[row] != static_cast<int64_t>(row)) {
        assignError(error,
                    "Quality source_keypoint_row_ids are not row-for-row");
        return false;
      }
    }
    quality_offsets_ = std::move(offsets);
    descriptor_.quality_offset_read_calls = 1;
    quality_validated_.store(true, std::memory_order_release);
    return true;
  }

private:
  const std::vector<int64_t> &selectedOffsets() const {
    return refined_ ? selected_offsets_ : raw_offsets_;
  }

  bool readFramePayload(size_t first, size_t last, FramePayload *payload,
                        std::string *error) const {
    const size_t rows = last - first;
    const size_t keypoints = descriptor_.selected.keypoint_count;
    FramePayloadFutures futures;
    futures.body_keys = issueRangeRead(body_.instance_key, first, last);
    futures.body_frame_indices =
        issueRangeRead(body_.frame_indices, first, last);
    futures.body_source_rows =
        issueRangeRead(body_.source_keypoint_row_ids, first, last);
    futures.body_signatures =
        issueRangeRead(body_.source_keypoint_row_signature, first, last);
    futures.origins = issueRangeRead(body_.origin_xy, first, last);
    futures.axis_valid = issueRangeRead(body_.axis_valid, first, last);
    futures.headings = issueRangeRead(body_.heading_deg, first, last);

    const auto recordBatch = [&](uint64_t columns) {
      payload_read_calls_.fetch_add(columns, std::memory_order_relaxed);
      payload_read_batches_.fetch_add(1, std::memory_order_relaxed);
      uint64_t current =
          maximum_columns_per_batch_.load(std::memory_order_relaxed);
      while (current < columns &&
             !maximum_columns_per_batch_.compare_exchange_weak(
                 current, columns, std::memory_order_relaxed)) {
      }
    };
    auto collectBody = [&] {
      return collectRangeRead(&futures.body_keys, rows, 1, &payload->body_keys,
                              error) &&
             collectRangeRead(&futures.body_frame_indices, rows, 1,
                              &payload->body_frame_indices, error) &&
             collectRangeRead(&futures.body_source_rows, rows, 1,
                              &payload->body_source_rows, error) &&
             collectRangeRead(&futures.body_signatures, rows, 32,
                              &payload->body_signatures, error) &&
             collectRangeRead(&futures.origins, rows, 2, &payload->origins,
                              error) &&
             collectBoolRangeRead(&futures.axis_valid, rows, 1,
                                  &payload->axis_valid, error) &&
             collectRangeRead(&futures.headings, rows, 1, &payload->headings,
                              error);
    };
    auto validatePageIdentity = [&] {
      if (payload->body_keys != payload->keys ||
          payload->body_frame_indices != payload->frame_indices ||
          payload->body_signatures != payload->row_signatures) {
        assignError(error, "Keypoint body-frame page identity mismatch");
        return false;
      }
      for (size_t local = 0; local < rows; ++local) {
        if (payload->frame_indices[local] !=
                static_cast<int64_t>(
                    std::upper_bound(selectedOffsets().begin(),
                                     selectedOffsets().end(),
                                     static_cast<int64_t>(first + local)) -
                    selectedOffsets().begin() - 1) ||
            payload->body_source_rows[local] !=
                static_cast<int64_t>(first + local)) {
          assignError(error, "Keypoint page row identity mismatch");
          return false;
        }
      }
      return true;
    };
    if (refined_) {
      futures.keys = issueRangeRead(refined_->instance_key, first, last);
      futures.frame_indices =
          issueRangeRead(refined_->frame_indices, first, last);
      futures.source_crop_rows =
          issueRangeRead(refined_->source_crop_row_ids, first, last);
      futures.row_signatures =
          issueRangeRead(refined_->keypoint_row_signature, first, last);
      futures.points = issueRangeRead(refined_->keypoints_img, first, last);
      futures.point_valid =
          issueRangeRead(refined_->keypoint_valid, first, last);
      futures.source_success =
          issueRangeRead(refined_->source_success, first, last);
      futures.refined_success =
          issueRangeRead(refined_->refined_success, first, last);
      futures.edit_flags =
          issueRangeRead(refined_->keypoint_edit_flags, first, last);
      futures.flip_corrected =
          issueRangeRead(refined_->flip_corrected, first, last);
      futures.confidence_valid =
          issueRangeRead(refined_->confidence_valid, first, last);
      futures.geometry_valid =
          issueRangeRead(refined_->geometry_valid, first, last);
      futures.usable = issueRangeRead(refined_->usable_keypoints, first, last);
      futures.review_states =
          issueRangeRead(refined_->review_state_codes, first, last);
      futures.reason_codes =
          issueRangeRead(refined_->reason_codes, first, last);
      recordBatch(22);
      return collectRangeRead(&futures.keys, rows, 1, &payload->keys, error) &&
             collectRangeRead(&futures.frame_indices, rows, 1,
                              &payload->frame_indices, error) &&
             collectRangeRead(&futures.source_crop_rows, rows, 1,
                              &payload->source_crop_rows, error) &&
             collectRangeRead(&futures.row_signatures, rows, 32,
                              &payload->row_signatures, error) &&
             collectRangeRead(&futures.points, rows, keypoints * 2,
                              &payload->points, error) &&
             collectBoolRangeRead(&futures.point_valid, rows, keypoints,
                                  &payload->point_valid, error) &&
             collectBoolRangeRead(&futures.source_success, rows, 1,
                                  &payload->source_success, error) &&
             collectBoolRangeRead(&futures.refined_success, rows, 1,
                                  &payload->refined_success, error) &&
             collectBoolRangeRead(&futures.edit_flags, rows, keypoints,
                                  &payload->edit_flags, error) &&
             collectBoolRangeRead(&futures.flip_corrected, rows, 1,
                                  &payload->flip_corrected, error) &&
             collectBoolRangeRead(&futures.confidence_valid, rows, 1,
                                  &payload->confidence_valid, error) &&
             collectBoolRangeRead(&futures.geometry_valid, rows, 1,
                                  &payload->geometry_valid, error) &&
             collectBoolRangeRead(&futures.usable, rows, 1, &payload->usable,
                                  error) &&
             collectRangeRead(&futures.review_states, rows, 1,
                              &payload->review_states, error) &&
             collectRangeRead(&futures.reason_codes, rows, 1,
                              &payload->reason_codes, error) &&
             collectBody() && validatePageIdentity();
    }
    futures.keys = issueRangeRead(raw_.instance_key, first, last);
    futures.frame_indices = issueRangeRead(raw_.frame_indices, first, last);
    futures.source_crop_rows =
        issueRangeRead(raw_.source_crop_row_ids, first, last);
    futures.row_signatures =
        issueRangeRead(raw_.keypoint_row_signature, first, last);
    futures.points = issueRangeRead(raw_.keypoints_img, first, last);
    futures.point_valid = issueRangeRead(raw_.keypoint_valid, first, last);
    futures.source_success = issueRangeRead(raw_.pose_success, first, last);
    recordBatch(14);
    if (!collectRangeRead(&futures.keys, rows, 1, &payload->keys, error) ||
        !collectRangeRead(&futures.frame_indices, rows, 1,
                          &payload->frame_indices, error) ||
        !collectRangeRead(&futures.source_crop_rows, rows, 1,
                          &payload->source_crop_rows, error) ||
        !collectRangeRead(&futures.row_signatures, rows, 32,
                          &payload->row_signatures, error) ||
        !collectRangeRead(&futures.points, rows, keypoints * 2,
                          &payload->points, error) ||
        !collectBoolRangeRead(&futures.point_valid, rows, keypoints,
                              &payload->point_valid, error) ||
        !collectBoolRangeRead(&futures.source_success, rows, 1,
                              &payload->source_success, error) ||
        !collectBody() || !validatePageIdentity()) {
      return false;
    }
    payload->refined_success = payload->source_success;
    payload->usable = payload->source_success;
    payload->edit_flags.assign(rows * keypoints, 0);
    payload->flip_corrected.assign(rows, 0);
    payload->confidence_valid = payload->source_success;
    payload->geometry_valid = payload->source_success;
    payload->review_states.assign(rows, 0);
    payload->reason_codes.assign(rows, 0);
    return true;
  }

  KeypointOverlayDescriptor overlay_descriptor_;
  mutable KeypointV2RepositoryDescriptor descriptor_;
  RawStores raw_;
  QualityStores quality_;
  std::optional<RefinedStores> refined_;
  BodyFrameStores body_;
  std::vector<int64_t> raw_offsets_;
  std::vector<int64_t> selected_offsets_;
  std::vector<int64_t> body_offsets_;
  std::vector<int64_t> quality_offsets_;
  std::mutex quality_validation_mutex_;
  std::atomic<bool> quality_validated_{false};
  mutable std::atomic<uint64_t> frame_requests_{0};
  mutable std::atomic<uint64_t> rows_resolved_{0};
  mutable std::atomic<uint64_t> payload_read_calls_{0};
  mutable std::atomic<uint64_t> payload_read_batches_{0};
  mutable std::atomic<uint64_t> maximum_columns_per_batch_{0};
  mutable std::atomic<uint64_t> quality_payload_read_calls_{0};
  mutable std::atomic<uint64_t> read_failures_{0};
};

bool validExpectedDigest(const std::string &expected, const std::string &actual,
                         const char *label, std::string *error) {
  if (!IsLowerSha256(expected) || expected != actual) {
    assignError(error, std::string(label) + " manifest digest mismatch");
    return false;
  }
  return true;
}

} // namespace

std::unique_ptr<KeypointV2Repository>
OpenKeypointV2Repository(const KeypointV2RepositoryOpenRequest &request,
                         std::string *error_message,
                         KeypointV2RepositoryOpenMetrics *open_metrics) {
  KeypointV2RepositoryOpenMetrics metrics;
  const auto all_started = Clock::now();
  if (!request.raw_archive || !request.quality_archive ||
      !request.body_frame_archive || request.raw_run.empty() ||
      request.quality_run.empty() || request.body_frame_run.empty()) {
    assignError(error_message,
                "Keypoint v2 requires explicit raw, quality, and body paths");
    return nullptr;
  }
  if (!request.allow_selector_ineligible) {
    assignError(error_message,
                "Selector-ineligible keypoint v2 artifacts require explicit "
                "benchmark permission");
    return nullptr;
  }
  const bool refined = !request.refined_run.empty();
  if (refined && !request.refined_archive) {
    assignError(error_message,
                "Explicit refined keypoint run lacks an archive");
    return nullptr;
  }

  const auto metadata_started = Clock::now();
  KeypointV2RepositoryDescriptor descriptor;
  descriptor.refined = refined;
  json raw_root;
  const std::string raw_base = "keypoints_runs/" + request.raw_run;
  if (!validateStageMetadata(
          *request.raw_archive->impl_, raw_base, kRawDeclarations,
          [&](const json &manifest, KeypointV2ManifestSummary *summary,
              std::string *error) {
            return ValidateRawKeypointV2RunManifest(manifest, request.raw_run,
                                                    summary, error);
          },
          &descriptor.raw, &raw_root, &metrics, error_message) ||
      !validExpectedDigest(request.expected_raw_manifest_digest,
                           descriptor.raw.manifest_digest, "Raw keypoint",
                           error_message)) {
    return nullptr;
  }
  json quality_root;
  const std::string quality_base =
      "keypoint_quality_runs/" + request.quality_run;
  if (!validateStageMetadata(
          *request.quality_archive->impl_, quality_base, kQualityDeclarations,
          [&](const json &manifest, KeypointV2ManifestSummary *summary,
              std::string *error) {
            return ValidateKeypointQualityV1RunManifest(
                manifest, request.quality_run, summary, error);
          },
          &descriptor.quality, &quality_root, &metrics, error_message) ||
      !validExpectedDigest(request.expected_quality_manifest_digest,
                           descriptor.quality.manifest_digest,
                           "Keypoint quality", error_message)) {
    return nullptr;
  }
  json selected_root;
  if (refined) {
    const std::string refined_base =
        "refined_keypoints_runs/" + request.refined_run;
    if (!validateStageMetadata(
            *request.refined_archive->impl_, refined_base, kRefinedDeclarations,
            [&](const json &manifest, KeypointV2ManifestSummary *summary,
                std::string *error) {
              return ValidateRefinedKeypointV2RunManifest(
                  manifest, request.refined_run, summary, error);
            },
            &descriptor.selected, &selected_root, &metrics, error_message) ||
        !validExpectedDigest(request.expected_refined_manifest_digest,
                             descriptor.selected.manifest_digest,
                             "Refined keypoint", error_message)) {
      return nullptr;
    }
  } else {
    descriptor.selected = descriptor.raw;
    selected_root = raw_root;
  }
  json body_root;
  const std::string body_base =
      "analysis/body_frame_runs/" + request.body_frame_run;
  if (!validateStageMetadata(
          *request.body_frame_archive->impl_, body_base, kBodyFrameDeclarations,
          [&](const json &manifest, KeypointV2ManifestSummary *summary,
              std::string *error) {
            return ValidateBodyFrameV1RunManifest(
                manifest, request.body_frame_run, summary, error);
          },
          &descriptor.body_frame, &body_root, &metrics, error_message) ||
      !validExpectedDigest(request.expected_body_frame_manifest_digest,
                           descriptor.body_frame.manifest_digest, "Body-frame",
                           error_message)) {
    return nullptr;
  }
  metrics.metadata_ms = elapsedMilliseconds(metadata_started);
  if (descriptor.quality.source_manifest_digest !=
          descriptor.raw.manifest_digest ||
      descriptor.quality.source_row_signatures_digest !=
          descriptor.raw.row_signatures_digest ||
      descriptor.selected.frame_count != descriptor.raw.frame_count ||
      descriptor.selected.row_count != descriptor.raw.row_count ||
      descriptor.selected.keypoint_count != descriptor.raw.keypoint_count ||
      (refined && (descriptor.selected.source_manifest_digest !=
                       descriptor.raw.manifest_digest ||
                   descriptor.selected.quality_source_manifest_digest !=
                       descriptor.quality.manifest_digest)) ||
      descriptor.body_frame.source_manifest_digest !=
          descriptor.selected.manifest_digest ||
      descriptor.body_frame.source_row_signatures_digest !=
          descriptor.selected.row_signatures_digest ||
      descriptor.body_frame.frame_count != descriptor.selected.frame_count ||
      descriptor.body_frame.row_count != descriptor.selected.row_count) {
    assignError(error_message,
                "Keypoint v2 cross-stage manifest bindings disagree");
    return nullptr;
  }

  const auto handles_started = Clock::now();
  RawStores raw_stores;
  QualityStores quality_stores;
  std::optional<RefinedStores> refined_stores;
  BodyFrameStores body_stores;
  if (!openRawStores(*request.raw_archive->impl_, raw_root, raw_base,
                     &raw_stores, &metrics, error_message) ||
      !openQualityStores(*request.quality_archive->impl_, quality_root,
                         quality_base, &quality_stores, &metrics,
                         error_message)) {
    return nullptr;
  }
  if (refined) {
    refined_stores.emplace();
    const std::string refined_base =
        "refined_keypoints_runs/" + request.refined_run;
    if (!openRefinedStores(*request.refined_archive->impl_, selected_root,
                           refined_base, &*refined_stores, &metrics,
                           error_message)) {
      return nullptr;
    }
  }
  if (!openBodyFrameStores(*request.body_frame_archive->impl_, body_root,
                           body_base, &body_stores, &metrics, error_message)) {
    return nullptr;
  }
  metrics.exact_handle_open_ms = elapsedMilliseconds(handles_started);

  const auto identity_started = Clock::now();
  std::vector<int64_t> raw_offsets;
  if (!readAll(raw_stores.frame_row_offsets, 1, &raw_offsets, error_message) ||
      !ValidateKeypointV2Offsets(raw_offsets, descriptor.raw.frame_count,
                                 descriptor.raw.row_count, error_message)) {
    return nullptr;
  }
  metrics.raw_offset_read_calls = 1;

  std::vector<int64_t> selected_offsets;
  if (refined) {
    if (!readAll(refined_stores->frame_row_offsets, 1, &selected_offsets,
                 error_message) ||
        !ValidateKeypointV2Offsets(
            selected_offsets, descriptor.selected.frame_count,
            descriptor.selected.row_count, error_message) ||
        !equalVectors(selected_offsets, raw_offsets, "refined offsets",
                      error_message)) {
      return nullptr;
    }
    metrics.selected_offset_read_calls = 1;
  }

  std::vector<int64_t> body_offsets;
  if (!readAll(body_stores.frame_row_offsets, 1, &body_offsets,
               error_message) ||
      !ValidateKeypointV2Offsets(
          body_offsets, descriptor.body_frame.frame_count,
          descriptor.body_frame.row_count, error_message) ||
      !equalVectors(body_offsets, refined ? selected_offsets : raw_offsets,
                    "body offsets", error_message)) {
    return nullptr;
  }
  metrics.body_frame_offset_read_calls = 1;

  if (request.deep_validate_identity) {
    std::vector<int64_t> raw_frames;
    std::vector<int64_t> raw_crop_rows;
    std::vector<int64_t> raw_acquisition_frames;
    std::vector<uint64_t> raw_keys;
    std::vector<uint8_t> raw_signatures;
    std::vector<uint8_t> raw_success;
    if (!readAll(raw_stores.frame_indices, 1, &raw_frames, error_message) ||
        !readAll(raw_stores.source_crop_row_ids, 1, &raw_crop_rows,
                 error_message) ||
        !readAll(raw_stores.source_acquisition_frame_index, 1,
                 &raw_acquisition_frames, error_message) ||
        !readAll(raw_stores.instance_key, 1, &raw_keys, error_message) ||
        !readAll(raw_stores.keypoint_row_signature, 32, &raw_signatures,
                 error_message) ||
        !readAllBool(raw_stores.pose_success, 1, &raw_success, error_message) ||
        !ValidateKeypointV2FrameIndex(
            raw_offsets, raw_frames, descriptor.raw.frame_count,
            descriptor.raw.row_count, error_message) ||
        !ValidateKeypointV2InstanceKeys(raw_keys, descriptor.raw.row_count,
                                        error_message)) {
      return nullptr;
    }

    std::vector<int64_t> selected_frames = raw_frames;
    std::vector<uint64_t> selected_keys = raw_keys;
    std::vector<uint8_t> selected_signatures = raw_signatures;
    if (refined) {
      std::vector<int64_t> selected_crop_rows;
      std::vector<int64_t> selected_acquisition_frames;
      std::vector<uint8_t> selected_source_success;
      if (!readAll(refined_stores->frame_indices, 1, &selected_frames,
                   error_message) ||
          !readAll(refined_stores->instance_key, 1, &selected_keys,
                   error_message) ||
          !readAll(refined_stores->keypoint_row_signature, 32,
                   &selected_signatures, error_message) ||
          !readAll(refined_stores->source_crop_row_ids, 1, &selected_crop_rows,
                   error_message) ||
          !readAll(refined_stores->source_acquisition_frame_index, 1,
                   &selected_acquisition_frames, error_message) ||
          !readAllBool(refined_stores->source_success, 1,
                       &selected_source_success, error_message) ||
          !ValidateKeypointV2FrameIndex(selected_offsets, selected_frames,
                                        descriptor.selected.frame_count,
                                        descriptor.selected.row_count,
                                        error_message) ||
          !ValidateKeypointV2InstanceKeys(
              selected_keys, descriptor.selected.row_count, error_message) ||
          !equalVectors(selected_frames, raw_frames, "refined frames",
                        error_message) ||
          !equalVectors(selected_keys, raw_keys, "refined instance keys",
                        error_message) ||
          !equalVectors(selected_crop_rows, raw_crop_rows, "refined crop rows",
                        error_message) ||
          !equalVectors(selected_acquisition_frames, raw_acquisition_frames,
                        "refined acquisition frames", error_message) ||
          !equalVectors(selected_source_success, raw_success,
                        "refined source success", error_message)) {
        return nullptr;
      }
    }

    std::vector<int64_t> body_frames;
    std::vector<int64_t> body_source_rows;
    std::vector<uint64_t> body_keys;
    std::vector<uint8_t> body_signatures;
    if (!readAll(body_stores.frame_indices, 1, &body_frames, error_message) ||
        !readAll(body_stores.source_keypoint_row_ids, 1, &body_source_rows,
                 error_message) ||
        !readAll(body_stores.instance_key, 1, &body_keys, error_message) ||
        !readAll(body_stores.source_keypoint_row_signature, 32,
                 &body_signatures, error_message) ||
        !ValidateKeypointV2FrameIndex(
            body_offsets, body_frames, descriptor.body_frame.frame_count,
            descriptor.body_frame.row_count, error_message) ||
        !equalVectors(body_frames, selected_frames, "body frames",
                      error_message) ||
        !equalVectors(body_keys, selected_keys, "body instance keys",
                      error_message) ||
        !equalVectors(body_signatures, selected_signatures, "body signatures",
                      error_message)) {
      return nullptr;
    }
    for (size_t row = 0; row < body_source_rows.size(); ++row) {
      if (body_source_rows[row] != static_cast<int64_t>(row)) {
        assignError(error_message,
                    "Body-frame source_keypoint_row_ids are not row-for-row");
        return nullptr;
      }
    }
  }
  metrics.identity_validation_ms = elapsedMilliseconds(identity_started);

  descriptor.consolidated_metadata = true;
  descriptor.stable_identity = true;
  descriptor.page_identity_validation = true;
  descriptor.deep_identity_validated = request.deep_validate_identity;
  descriptor.quality_payload_lazy = true;
  descriptor.raw_offset_read_calls = 1;
  descriptor.selected_offset_read_calls = refined ? 1 : 0;
  descriptor.quality_offset_read_calls = 0;
  descriptor.body_frame_offset_read_calls = 1;
  metrics.retained_offset_bytes =
      (raw_offsets.size() + selected_offsets.size() + body_offsets.size()) *
      sizeof(int64_t);
  metrics.total_ms = elapsedMilliseconds(all_started);
  if (open_metrics) {
    *open_metrics = metrics;
  }

  KeypointOverlayDescriptor overlay;
  overlay.source_group = refined ? "refined_keypoints_runs" : "keypoints_runs";
  overlay.run_name = descriptor.selected.run_id;
  overlay.coordinate_space = KeypointCoordinateSpace::Image;
  overlay.refined = refined;
  overlay.keypoint_labels = descriptor.raw.keypoint_labels;
  overlay.skeleton_edges = descriptor.raw.skeleton_edges;
  overlay.row_count = descriptor.selected.row_count;
  overlay.camera_frame_count = descriptor.selected.frame_count;
  return std::make_unique<TensorStoreKeypointV2Repository>(
      std::move(overlay), std::move(descriptor), std::move(raw_stores),
      std::move(quality_stores), std::move(refined_stores),
      std::move(body_stores), std::move(raw_offsets),
      std::move(selected_offsets), std::move(body_offsets));
}

} // namespace crimson::zarr
