#include "zarr/tensorstore_bound_keypoint_overlay_repository.h"

#include "zarr/archive_context_internal.h"
#include "zarr/canonical_json.h"
#include "zarr/keypoint_v2_contract.h"
#include "zarr/zarr_metadata_equivalence.h"

#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

namespace ts = tensorstore;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

enum class Extent { Rows, FrameBoundaries, Keypoints, Two, Four, Signature };

struct ArrayDeclaration {
  const char *path;
  const char *dtype;
  size_t rank;
  Extent second = Extent::Rows;
  Extent third = Extent::Rows;
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

template <typename T, ts::DimensionIndex Rank>
using Store = ts::TensorStore<T, Rank>;

template <typename T, ts::DimensionIndex Rank>
using ArrayReadFuture = ts::Future<ts::SharedArray<T, Rank, ts::offset_origin>>;

struct RawOverlayStores {
  Store<uint64_t, 1> instance_key;
  Store<int64_t, 1> source_crop_row_ids;
  Store<int64_t, 1> acquisition_frames;
  Store<int64_t, 1> frame_row_offsets;
  Store<float, 3> keypoints_img;
  Store<float, 2> keypoint_confidences;
  Store<bool, 2> keypoint_valid;
  Store<float, 1> pose_confidence;
  Store<bool, 1> pose_success;
};

struct FramePayloadFutures {
  ArrayReadFuture<uint64_t, 1> keys;
  ArrayReadFuture<int64_t, 1> crop_rows;
  ArrayReadFuture<int64_t, 1> acquisition_frames;
  ArrayReadFuture<float, 3> points;
  ArrayReadFuture<float, 2> confidences;
  ArrayReadFuture<bool, 2> valid;
  ArrayReadFuture<float, 1> pose_confidence;
  ArrayReadFuture<bool, 1> success;
};

struct FramePayload {
  std::vector<uint64_t> keys;
  std::vector<int64_t> crop_rows;
  std::vector<int64_t> acquisition_frames;
  std::vector<float> points;
  std::vector<float> confidences;
  std::vector<uint8_t> valid;
  std::vector<float> pose_confidence;
  std::vector<uint8_t> success;
};

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
  }
  return 0;
}

std::vector<size_t> expectedShape(const ArrayDeclaration &declaration,
                                  const KeypointV2ManifestSummary &summary) {
  std::vector<size_t> shape = {
      std::string_view(declaration.path) == "frame_row_offsets"
          ? summary.frame_count + 1
          : summary.row_count};
  if (declaration.rank >= 2) {
    shape.push_back(extentValue(declaration.second, summary));
  }
  if (declaration.rank >= 3) {
    shape.push_back(extentValue(declaration.third, summary));
  }
  return shape;
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
         !consolidation->value("must_understand", true) &&
         consolidation->at("metadata").is_object() &&
         consolidation->at("metadata").empty());
    if (!empty) {
      return false;
    }
    metadata->erase(consolidation);
  }
  auto attributes = metadata->find("attributes");
  if (attributes == metadata->end() || !attributes->is_object()) {
    return false;
  }
  attributes->erase("run_manifest");
  return true;
}

bool rawImageCoordinateContractValid(const json &manifest) {
  try {
    const auto &contract = manifest.at("payload").at("coordinate_contract");
    const auto &document = contract.at("document");
    if (contract.value("digest_algorithm", "") !=
            "sha256_canonical_json_v1" ||
        document.value("schema_id", "") !=
            "palette.array_coordinate_catalog" ||
        !IsLowerSha256(contract.value("digest", "")) ||
        CanonicalJsonSha256(document) != contract.value("digest", "")) {
      return false;
    }
    bool binding_found = false;
    for (const auto &binding : document.at("bindings")) {
      if (binding.value("array_contract_id", "") ==
          "palette.array.keypoints_img") {
        binding_found =
            binding.value("array_contract_version", 0) == 2 &&
            binding.value("surface_id", "") ==
                "source_camera_point_xy_v1" &&
            binding.value("semantic_role", "") ==
                "exact_derived_numeric_surface";
      }
    }
    bool surface_found = false;
    for (const auto &surface : document.at("surfaces")) {
      if (surface.value("surface_id", "") ==
          "source_camera_point_xy_v1") {
        surface_found =
            surface.value("domain_id", "") == "source_camera_image_px" &&
            surface.value("geometry_type", "") == "point_xy" &&
            surface.value("pixel_convention", "") == "continuous" &&
            surface.value("source_camera_mapping", "") ==
                "direct_source_camera_continuous_pixels" &&
            surface.value("descriptor_profile_id", "") ==
                "source_camera_image_px.top_left_y_down.v1" &&
            surface.value("descriptor_overlay_status", "") == "direct" &&
            surface.at("components") == json::array({"x", "y"}) &&
            surface.at("component_units") == json::array({"px", "px"});
      }
    }
    return binding_found && surface_found;
  } catch (const json::exception &) {
    return false;
  }
}

bool validateRawMetadata(const ArchiveContext::Impl &archive,
                         const CanonicalOverlaySelection &selection,
                         KeypointV2ManifestSummary *summary, json *root_out,
                         BoundKeypointOverlayOpenMetrics *metrics,
                         std::string *error) {
  const std::string base =
      selection.keypoints.group + "/" + selection.keypoints.run_id;
  const auto root = internal::ReadArchiveRunMetadata(archive, {base});
  if (metrics) {
    ++metrics->metadata_reads;
  }
  if (!root || root->value("zarr_format", 0) != 3 ||
      root->value("node_type", "") != "group" ||
      !root->contains("consolidated_metadata") ||
      !root->at("consolidated_metadata").is_object() ||
      root->at("consolidated_metadata").value("kind", "") != "inline" ||
      root->at("consolidated_metadata").value("must_understand", true) ||
      !root->at("consolidated_metadata").contains("metadata") ||
      !root->at("consolidated_metadata").at("metadata").is_object()) {
    assignError(error,
                "Bound keypoint archive lacks exact inline Zarr v3 metadata");
    return false;
  }
  const auto *consolidated_group = consolidatedEntry(*root, base);
  const auto direct_group =
      internal::ReadArchiveJson(archive, base + "/zarr.json");
  if (metrics) {
    ++metrics->metadata_reads;
  }
  if (!direct_group || !consolidated_group ||
      !internal::EquivalentDirectAndConsolidatedZarrNode(*direct_group,
                                                         *consolidated_group) ||
      !direct_group->contains("attributes") ||
      !direct_group->at("attributes").contains("run_manifest") ||
      !ValidateRawKeypointV2RunManifest(
          direct_group->at("attributes").at("run_manifest"),
          selection.keypoints.run_id, summary, error) ||
      !rawImageCoordinateContractValid(
          direct_group->at("attributes").at("run_manifest"))) {
    if (error && error->empty()) {
      *error = "Bound raw-v2 keypoint run metadata is invalid";
    }
    return false;
  }
  if (summary->manifest_digest != selection.keypoints.identity_digest ||
      summary->payload_digest !=
          selection.keypoints.manifest_payload_digest ||
      summary->selector_eligible != selection.keypoints.selector_eligible ||
      (!summary->selector_eligible &&
       !selection.keypoints.bound_selector_exception)) {
    assignError(error,
                "Bound raw-v2 keypoint manifest/eligibility identity changed");
    return false;
  }
  json declarations = json::object();
  json normalized_group = *direct_group;
  if (!normalizeRunGroup(&normalized_group)) {
    assignError(error, "Bound raw-v2 keypoint group is not canonical");
    return false;
  }
  declarations[""] = std::move(normalized_group);
  std::unordered_set<std::string> expected_paths;
  expected_paths.reserve(std::size(kRawDeclarations) + 1);
  expected_paths.insert(base);
  for (const auto &declaration : kRawDeclarations) {
    const std::string path = base + "/" + declaration.path;
    expected_paths.insert(path);
    const auto *consolidated = consolidatedEntry(*root, path);
    const auto direct = internal::ReadArchiveJson(archive, path + "/zarr.json");
    if (metrics) {
      ++metrics->metadata_reads;
    }
    if (!direct || !consolidated ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                           *consolidated) ||
        consolidated->value("zarr_format", 0) != 3 ||
        consolidated->value("node_type", "") != "array" ||
        consolidated->contains("consolidated_metadata") ||
        consolidated->value("data_type", "") != declaration.dtype ||
        !consolidated->contains("shape") ||
        consolidated->at("shape").get<std::vector<size_t>>() !=
            expectedShape(declaration, *summary)) {
      assignError(error, "Bound raw-v2 keypoint declaration is invalid: " +
                             path);
      return false;
    }
    declarations[declaration.path] = *direct;
  }
  const auto &all = root->at("consolidated_metadata").at("metadata");
  const std::string prefix = base + "/";
  for (auto item = all.begin(); item != all.end(); ++item) {
    if ((item.key() == base || item.key().rfind(prefix, 0) == 0) &&
        expected_paths.find(item.key()) == expected_paths.end()) {
      const std::string relative = item.key().substr(prefix.size());
      const bool coordinate_node =
          relative == "coordinate_frames" ||
          relative.rfind("coordinate_frames/", 0) == 0 ||
          relative == "coordinate_transforms" ||
          relative.rfind("coordinate_transforms/", 0) == 0;
      const bool successor_array =
          relative == "keypoints_norm" ||
          relative == "pose_bbox_xyxy_norm" ||
          relative == "source_crop_xywh";
      const auto direct =
          internal::ReadArchiveJson(archive, item.key() + "/zarr.json");
      if (metrics) {
        ++metrics->metadata_reads;
      }
      const auto attributes = item.value().find("attributes");
      const bool successor_declaration =
          successor_array && item.value().value("node_type", "") == "array" &&
          item.value().value("data_type", "") == "float32" &&
          attributes != item.value().end() && attributes->is_object() &&
          attributes->value("coordinate_successor_auxiliary", false) &&
          attributes->value("coordinate_successor_auxiliary_policy", "") ==
              "successor_owned_derived_coordinates_not_keypoint_v2_logical_"
              "payload_v2";
      if ((!coordinate_node && !successor_declaration) || !direct ||
          !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                             item.value()) ||
          (coordinate_node && item.value().value("node_type", "") != "group" &&
           item.value().value("node_type", "") != "array")) {
        assignError(error, "Unexpected or invalid bound raw-v2 keypoint node: " +
                               item.key());
        return false;
      }
    }
  }
  const json digest_document = {
      {"scope",
       "exact_group_and_array_declarations_with_attributes_redacting_only_"
       "run_manifest"},
      {"declarations", std::move(declarations)}};
  if (CanonicalJsonSha256(digest_document) !=
      summary->metadata_declarations_digest) {
    assignError(error,
                "Bound raw-v2 keypoint metadata declaration digest changed");
    return false;
  }
  *root_out = *root;
  return true;
}

template <typename T, ts::DimensionIndex Rank>
bool openExact(const ArchiveContext::Impl &archive, const json &root,
               const std::string &path, Store<T, Rank> *output,
               BoundKeypointOverlayOpenMetrics *metrics,
               std::string *error) {
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

bool openStores(const ArchiveContext::Impl &archive, const json &root,
                const std::string &base, RawOverlayStores *stores,
                BoundKeypointOverlayOpenMetrics *metrics,
                std::string *error) {
  return openExact(archive, root, base + "/instance_key",
                   &stores->instance_key, metrics, error) &&
         openExact(archive, root, base + "/source_crop_row_ids",
                   &stores->source_crop_row_ids, metrics, error) &&
         openExact(archive, root, base + "/source_acquisition_frame_index",
                   &stores->acquisition_frames, metrics, error) &&
         openExact(archive, root, base + "/frame_row_offsets",
                   &stores->frame_row_offsets, metrics, error) &&
         openExact(archive, root, base + "/keypoints_img",
                   &stores->keypoints_img, metrics, error) &&
         openExact(archive, root, base + "/keypoint_confidences",
                   &stores->keypoint_confidences, metrics, error) &&
         openExact(archive, root, base + "/keypoint_valid",
                   &stores->keypoint_valid, metrics, error) &&
         openExact(archive, root, base + "/pose_confidence",
                   &stores->pose_confidence, metrics, error) &&
         openExact(archive, root, base + "/pose_success",
                   &stores->pose_success, metrics, error);
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
      read->byte_strides().size() != Rank ||
      static_cast<size_t>(read->shape()[0]) != rows) {
    assignError(error, read.ok() ? "Bound keypoint read shape mismatch"
                                 : read.status().ToString());
    return false;
  }
  size_t actual_values_per_row = 1;
  for (ts::DimensionIndex dimension = 1; dimension < Rank; ++dimension) {
    actual_values_per_row *= static_cast<size_t>(read->shape()[dimension]);
  }
  if (actual_values_per_row != values_per_row) {
    assignError(error, "Bound keypoint read trailing extent mismatch");
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

template <ts::DimensionIndex Rank>
bool collectBoolRangeRead(ArrayReadFuture<bool, Rank> *future, size_t rows,
                          size_t values_per_row,
                          std::vector<uint8_t> *output,
                          std::string *error) {
  auto read = future->result();
  if (!read.ok() || read->rank() != Rank ||
      read->byte_strides().size() != Rank ||
      static_cast<size_t>(read->shape()[0]) != rows) {
    assignError(error, read.ok() ? "Bound keypoint boolean shape mismatch"
                                 : read.status().ToString());
    return false;
  }
  size_t actual_values_per_row = 1;
  for (ts::DimensionIndex dimension = 1; dimension < Rank; ++dimension) {
    actual_values_per_row *= static_cast<size_t>(read->shape()[dimension]);
  }
  if (actual_values_per_row != values_per_row) {
    assignError(error, "Bound keypoint boolean trailing extent mismatch");
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
          *reinterpret_cast<const bool *>(origin + offset) ? 1 : 0;
    }
  }
  return true;
}

bool readAllOffsets(const Store<int64_t, 1> &store,
                    std::vector<int64_t> *offsets,
                    std::string *error) {
  auto future = issueRangeRead(store, 0,
                               static_cast<size_t>(store.domain().shape()[0]));
  return collectRangeRead(&future,
                          static_cast<size_t>(store.domain().shape()[0]), 1,
                          offsets, error);
}

uint64_t decodedBytesPerRow(size_t keypoint_count) {
  return sizeof(uint64_t) + 2 * sizeof(int64_t) +
         keypoint_count * 2 * sizeof(float) +
         keypoint_count * sizeof(float) + keypoint_count * sizeof(bool) +
         sizeof(float) + sizeof(bool);
}

class TensorStoreBoundKeypointOverlayRepository final
    : public BoundKeypointOverlayRepository {
public:
  TensorStoreBoundKeypointOverlayRepository(
      KeypointOverlayDescriptor descriptor, RawOverlayStores stores,
      std::vector<int64_t> offsets, size_t max_rows_per_frame,
      uint64_t max_frame_payload_bytes)
      : descriptor_(std::move(descriptor)), stores_(std::move(stores)),
        offsets_(std::move(offsets)), max_rows_per_frame_(max_rows_per_frame),
        max_frame_payload_bytes_(max_frame_payload_bytes) {}

  const KeypointOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }

  RepositoryMemoryMetrics memoryMetrics() const override {
    RepositoryMemoryMetrics result;
    result.retained_index_bytes = offsets_.capacity() * sizeof(int64_t);
    return result;
  }

  BoundKeypointOverlayAccessMetrics accessMetrics() const override {
    BoundKeypointOverlayAccessMetrics result;
    result.frame_requests = frame_requests_.load(std::memory_order_relaxed);
    result.rows_resolved = rows_resolved_.load(std::memory_order_relaxed);
    result.payload_read_batches =
        payload_read_batches_.load(std::memory_order_relaxed);
    result.payload_read_calls =
        payload_read_calls_.load(std::memory_order_relaxed);
    result.decoded_payload_bytes =
        decoded_payload_bytes_.load(std::memory_order_relaxed);
    result.read_failures = read_failures_.load(std::memory_order_relaxed);
    return result;
  }

  KeypointOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    frame_requests_.fetch_add(1, std::memory_order_relaxed);
    KeypointOverlayResolution result;
    result.camera_frame = camera_frame;
    if (camera_frame < 0 ||
        static_cast<uint64_t>(camera_frame) >= descriptor_.camera_frame_count) {
      result.status = KeypointOverlayStatus::OutOfRange;
      return result;
    }
    if (full_frame_width <= 0 || full_frame_height <= 0 ||
        static_cast<size_t>(full_frame_width) != source_width_ ||
        static_cast<size_t>(full_frame_height) != source_height_) {
      result.status = KeypointOverlayStatus::InvalidDimensions;
      result.error = "Presented dimensions disagree with bound keypoint source";
      return result;
    }
    const size_t frame = static_cast<size_t>(camera_frame);
    const size_t first = static_cast<size_t>(offsets_[frame]);
    const size_t last = static_cast<size_t>(offsets_[frame + 1]);
    const size_t rows = last - first;
    if (rows == 0) {
      result.status = KeypointOverlayStatus::Missing;
      return result;
    }
    const uint64_t bytes = decodedBytesPerRow(keypoint_count_) * rows;
    if (rows > max_rows_per_frame_ || bytes > max_frame_payload_bytes_) {
      result.status = KeypointOverlayStatus::ReadFailed;
      result.error = "Bound keypoint frame exceeds configured read admission";
      read_failures_.fetch_add(1, std::memory_order_relaxed);
      return result;
    }

    FramePayloadFutures futures;
    futures.keys = issueRangeRead(stores_.instance_key, first, last);
    futures.crop_rows =
        issueRangeRead(stores_.source_crop_row_ids, first, last);
    futures.acquisition_frames =
        issueRangeRead(stores_.acquisition_frames, first, last);
    futures.points = issueRangeRead(stores_.keypoints_img, first, last);
    futures.confidences =
        issueRangeRead(stores_.keypoint_confidences, first, last);
    futures.valid = issueRangeRead(stores_.keypoint_valid, first, last);
    futures.pose_confidence =
        issueRangeRead(stores_.pose_confidence, first, last);
    futures.success = issueRangeRead(stores_.pose_success, first, last);
    payload_read_batches_.fetch_add(1, std::memory_order_relaxed);
    payload_read_calls_.fetch_add(8, std::memory_order_relaxed);

    FramePayload payload;
    std::string error;
    if (!collectRangeRead(&futures.keys, rows, 1, &payload.keys, &error) ||
        !collectRangeRead(&futures.crop_rows, rows, 1, &payload.crop_rows,
                          &error) ||
        !collectRangeRead(&futures.acquisition_frames, rows, 1,
                          &payload.acquisition_frames, &error) ||
        !collectRangeRead(&futures.points, rows, keypoint_count_ * 2,
                          &payload.points, &error) ||
        !collectRangeRead(&futures.confidences, rows, keypoint_count_,
                          &payload.confidences, &error) ||
        !collectBoolRangeRead(&futures.valid, rows, keypoint_count_,
                              &payload.valid, &error) ||
        !collectRangeRead(&futures.pose_confidence, rows, 1,
                          &payload.pose_confidence, &error) ||
        !collectBoolRangeRead(&futures.success, rows, 1, &payload.success,
                              &error)) {
      result.status = KeypointOverlayStatus::ReadFailed;
      result.error = std::move(error);
      read_failures_.fetch_add(1, std::memory_order_relaxed);
      return result;
    }
    std::unordered_set<uint64_t> frame_keys;
    frame_keys.reserve(rows);
    for (size_t local = 0; local < rows; ++local) {
      if (payload.acquisition_frames[local] != camera_frame ||
          !frame_keys.insert(payload.keys[local]).second) {
        result.status = KeypointOverlayStatus::ReadFailed;
        result.error =
            "Bound keypoint frame contains wrong-frame or duplicate identity";
        read_failures_.fetch_add(1, std::memory_order_relaxed);
        return result;
      }
    }

    result.status = KeypointOverlayStatus::Mapped;
    result.detections.reserve(rows);
    for (size_t local = 0; local < rows; ++local) {
      KeypointOverlayDetection detection;
      detection.instance_key = payload.keys[local];
      detection.instance_key_valid = true;
      detection.acquisition_frame = payload.acquisition_frames[local];
      detection.detection_index = static_cast<int64_t>(first + local);
      detection.source_crop_row_id = payload.crop_rows[local];
      detection.pose_confidence = payload.pose_confidence[local];
      detection.source_success = payload.success[local] != 0;
      detection.refined_keypoints = false;
      detection.heading_valid = false;
      detection.heading_from_body_frame = false;
      detection.keypoints.reserve(keypoint_count_);
      detection.keypoint_confidences.reserve(keypoint_count_);
      detection.keypoint_valid.reserve(keypoint_count_);
      for (size_t point = 0; point < keypoint_count_; ++point) {
        const size_t value = local * keypoint_count_ + point;
        detection.keypoints.push_back(
            {payload.points[value * 2], payload.points[value * 2 + 1]});
        detection.keypoint_confidences.push_back(payload.confidences[value]);
        detection.keypoint_valid.push_back(payload.valid[value]);
      }
      result.detections.push_back(std::move(detection));
    }
    rows_resolved_.fetch_add(rows, std::memory_order_relaxed);
    decoded_payload_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    return result;
  }

  void setSourceDimensions(size_t width, size_t height,
                           size_t keypoint_count) {
    source_width_ = width;
    source_height_ = height;
    keypoint_count_ = keypoint_count;
  }

private:
  KeypointOverlayDescriptor descriptor_;
  RawOverlayStores stores_;
  std::vector<int64_t> offsets_;
  size_t max_rows_per_frame_ = 0;
  uint64_t max_frame_payload_bytes_ = 0;
  size_t source_width_ = 0;
  size_t source_height_ = 0;
  size_t keypoint_count_ = 0;
  mutable std::atomic<uint64_t> frame_requests_{0};
  mutable std::atomic<uint64_t> rows_resolved_{0};
  mutable std::atomic<uint64_t> payload_read_batches_{0};
  mutable std::atomic<uint64_t> payload_read_calls_{0};
  mutable std::atomic<uint64_t> decoded_payload_bytes_{0};
  mutable std::atomic<uint64_t> read_failures_{0};
};

} // namespace

std::unique_ptr<BoundKeypointOverlayRepository>
OpenBoundKeypointOverlayRepository(
    const BoundKeypointOverlayOpenRequest &request,
    std::string *error_message,
    BoundKeypointOverlayOpenMetrics *open_metrics) {
  const auto started = Clock::now();
  BoundKeypointOverlayOpenMetrics metrics;
  auto finish = [&](auto result) {
    metrics.total_ms = elapsedMilliseconds(started);
    if (open_metrics) {
      *open_metrics = metrics;
    }
    return result;
  };
  auto fail = [&](std::string message)
      -> std::unique_ptr<BoundKeypointOverlayRepository> {
    assignError(error_message, std::move(message));
    return finish(std::unique_ptr<BoundKeypointOverlayRepository>{});
  };
  if (!request.archive || !request.archive->impl_) {
    return fail("Archive context is unavailable");
  }
  if (!request.selection.keypoints.valid) {
    return fail("Bound keypoint selection is unavailable: " +
                request.selection.keypoints.error);
  }
  if (request.selection.archive_identity !=
      request.archive->rootPath().lexically_normal().string()) {
    return fail("Bound keypoint selection belongs to another archive");
  }
  if (request.selection.keypoints.group != "keypoints_runs" ||
      request.selection.keypoints.schema_id !=
          "palette.stage.keypoint_observations" ||
      request.selection.keypoints.schema_version != 2 ||
      !validRunName(request.selection.keypoints.run_id) ||
      !request.selection.eye.valid ||
      request.selection.eye.group != "analysis/eye_angle_runs" ||
      request.selection.eye.schema_id != "analysis.eye_angle_runs" ||
      request.selection.eye.schema_version != 7 ||
      !validRunName(request.selection.eye.run_id) ||
      !request.selection.eye.selector_eligible ||
      request.selection.eye.bound_selector_exception ||
      !IsLowerSha256(request.selection.eye.identity_digest) ||
      request.selection.keypoints.bound_source_run_id !=
          request.selection.eye.run_id ||
      request.selection.frame_count == 0 ||
      request.selection.observation_count == 0 ||
      request.selection.source_width == 0 ||
      request.selection.source_height == 0 ||
      request.selection.coordinate_surface_id !=
          "source_camera_point_xy_v1" ||
      request.selection.coordinate_descriptor_profile !=
          "source_camera_image_px.top_left_y_down.v1" ||
      request.max_rows_per_frame == 0 ||
      request.max_frame_payload_bytes == 0) {
    return fail("Bound keypoint open request is invalid");
  }

  KeypointV2ManifestSummary summary;
  json root;
  if (!validateRawMetadata(*request.archive->impl_, request.selection,
                           &summary, &root, &metrics, error_message)) {
    return finish(std::unique_ptr<BoundKeypointOverlayRepository>{});
  }
  if (summary.frame_count != request.selection.frame_count ||
      summary.row_count != request.selection.observation_count ||
      summary.source_width != request.selection.source_width ||
      summary.source_height != request.selection.source_height ||
      summary.keypoint_labels != request.selection.keypoint_labels) {
    return fail("Bound keypoint manifest dimensions or labels changed");
  }
  const auto root_attributes = root.find("attributes");
  const json *source_video = nullptr;
  if (root_attributes != root.end() && root_attributes->is_object()) {
    const auto found = root_attributes->find("source_video_metadata");
    if (found != root_attributes->end() && found->is_object()) {
      source_video = &*found;
    }
  }
  if (root_attributes == root.end() || !root_attributes->is_object() ||
      !source_video ||
      root_attributes->value("recording_id", "") !=
          request.selection.recording_id ||
      root_attributes->value("camera_id", "") != request.selection.camera_id ||
      source_video->value("total_frames", size_t{0}) !=
          request.selection.frame_count ||
      source_video->value("width", size_t{0}) !=
          request.selection.source_width ||
      source_video->value("height", size_t{0}) !=
          request.selection.source_height) {
    return fail("Bound keypoint recording/frame authority changed");
  }

  RawOverlayStores stores;
  const std::string base = request.selection.keypoints.group + "/" +
                           request.selection.keypoints.run_id;
  if (!openStores(*request.archive->impl_, root, base, &stores, &metrics,
                  error_message)) {
    return finish(std::unique_ptr<BoundKeypointOverlayRepository>{});
  }
  std::vector<int64_t> offsets;
  if (!readAllOffsets(stores.frame_row_offsets, &offsets, error_message) ||
      !ValidateKeypointV2Offsets(offsets, summary.frame_count,
                                 summary.row_count, error_message)) {
    return finish(std::unique_ptr<BoundKeypointOverlayRepository>{});
  }
  metrics.offset_read_calls = 1;
  metrics.retained_offset_bytes = offsets.capacity() * sizeof(int64_t);
  const uint64_t row_bytes = decodedBytesPerRow(summary.keypoint_count);
  for (size_t frame = 0; frame < summary.frame_count; ++frame) {
    const size_t rows = static_cast<size_t>(offsets[frame + 1] - offsets[frame]);
    metrics.maximum_rows_in_frame =
        std::max(metrics.maximum_rows_in_frame, rows);
    if (rows > request.max_rows_per_frame ||
        (rows != 0 && row_bytes > request.max_frame_payload_bytes / rows)) {
      return fail("Bound keypoint frame index exceeds configured read admission");
    }
  }

  KeypointOverlayDescriptor descriptor;
  descriptor.source_group = request.selection.keypoints.group;
  descriptor.run_name = request.selection.keypoints.run_id;
  descriptor.recording_id = request.selection.recording_id;
  descriptor.manifest_digest = request.selection.keypoints.identity_digest;
  descriptor.manifest_payload_digest =
      request.selection.keypoints.manifest_payload_digest;
  descriptor.coordinate_authority =
      request.selection.coordinate_descriptor_profile;
  descriptor.coordinate_space = KeypointCoordinateSpace::Image;
  descriptor.refined = false;
  descriptor.keypoint_labels = summary.keypoint_labels;
  descriptor.skeleton_edges = summary.skeleton_edges;
  descriptor.row_count = summary.row_count;
  descriptor.camera_frame_count = summary.frame_count;
  descriptor.stable_instance_keys = true;
  auto repository =
      std::make_unique<TensorStoreBoundKeypointOverlayRepository>(
          std::move(descriptor), std::move(stores), std::move(offsets),
          request.max_rows_per_frame, request.max_frame_payload_bytes);
  repository->setSourceDimensions(summary.source_width, summary.source_height,
                                  summary.keypoint_count);
  if (error_message) {
    error_message->clear();
  }
  return finish(std::unique_ptr<BoundKeypointOverlayRepository>(
      std::move(repository)));
}

} // namespace crimson::zarr
