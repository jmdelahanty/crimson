#include "zarr/tensorstore_refined_detection_repository.h"

#include "zarr/archive_context_internal.h"
#include "zarr/refined_detection_contract.h"
#include "zarr/zarr_metadata_equivalence.h"

#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace crimson::zarr {
namespace {

namespace ts = tensorstore;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

double elapsedMilliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

void assignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

struct ArrayDeclaration {
  const char *relative_path;
  const char *dtype;
  size_t rank;
  size_t columns;
  bool source_table;
  bool offset;
  bool clipped;
};

constexpr ArrayDeclaration kDeclarations[] = {
    {"instances/frame_indices", "int32", 1, 1, false, false, false},
    {"instances/source_acquisition_frame_index", "int64", 1, 1, false, false,
     false},
    {"instances/instance_key", "uint64", 1, 1, false, false, false},
    {"instances/refined_row_ids", "int64", 1, 1, false, false, false},
    {"instances/bbox_norm_coords", "float32", 2, 4, false, false, false},
    {"instances/bbox_img_xyxy", "float32", 2, 4, false, false, false},
    {"instances/centers_img_xy", "float32", 2, 2, false, false, false},
    {"instances/scores", "float32", 1, 1, false, false, false},
    {"instances/score_valid", "bool", 1, 1, false, false, false},
    {"instances/class_ids", "int32", 1, 1, false, false, false},
    {"instances/source_kind_codes", "uint8", 1, 1, false, false, false},
    {"instances/manual_edit_flags", "bool", 1, 1, false, false, false},
    {"instances/source_detect_row_index", "int64", 1, 1, false, false, false},
    {"instances/reason_codes", "uint16", 1, 1, false, false, false},
    {"instances/frame_row_offsets", "int64", 1, 1, false, true, false},
    {"source_detections/source_detect_row_index", "int64", 1, 1, true, false,
     false},
    {"source_detections/frame_indices", "int32", 1, 1, true, false, false},
    {"source_detections/source_acquisition_frame_index", "int64", 1, 1, true,
     false, false},
    {"source_detections/instance_key", "uint64", 1, 1, true, false, false},
    {"source_detections/bbox_norm_coords", "float32", 2, 4, true, false, false},
    {"source_detections/bbox_img_xyxy", "float32", 2, 4, true, false, false},
    {"source_detections/centers_img_xy", "float32", 2, 2, true, false, false},
    {"source_detections/scores", "float32", 1, 1, true, false, false},
    {"source_detections/class_ids", "int32", 1, 1, true, false, false},
    {"source_detections/decision_codes", "uint8", 1, 1, true, false, false},
    {"source_detections/resolved_refined_row_id", "int64", 1, 1, true, false,
     false},
    {"source_detections/reason_codes", "uint16", 1, 1, true, false, false},
    {"source_detections/frame_row_offsets", "int64", 1, 1, true, true, false},
    {"instances/source_recording_frame_ids", "int64", 1, 1, false, false, true},
    {"instances/source_clip_indices", "int32", 1, 1, false, false, true},
    {"instances/source_clip_local_frame_indices", "int32", 1, 1, false, false,
     true},
    {"instances/source_clip_detect_row_index", "int64", 1, 1, false, false,
     true},
    {"instances/source_refined_row_ids", "int64", 1, 1, false, false, true},
    {"source_detections/source_recording_frame_ids", "int64", 1, 1, true, false,
     true},
    {"source_detections/source_clip_indices", "int32", 1, 1, true, false, true},
    {"source_detections/source_clip_local_frame_indices", "int32", 1, 1, true,
     false, true},
    {"source_detections/source_clip_detect_row_index", "int64", 1, 1, true,
     false, true},
    {"source_detections/source_resolved_refined_row_id", "int64", 1, 1, true,
     false, true},
};

const json *consolidatedEntry(const json &root, const std::string &path) {
  try {
    const auto &metadata = root.at("consolidated_metadata").at("metadata");
    const auto found = metadata.find(path);
    return found == metadata.end() ? nullptr : &*found;
  } catch (const json::exception &) {
    return nullptr;
  }
}

bool validateDeclarations(const json &root, const std::string &base,
                          const RefinedDetectionManifestSummary &manifest,
                          size_t *count, std::string *error) {
  try {
    if (root.at("zarr_format") != 3 || root.at("node_type") != "group" ||
        root.at("consolidated_metadata").at("kind") != "inline") {
      assignError(error, "Archive lacks inline Zarr v3 consolidated metadata");
      return false;
    }
    const bool clipped =
        manifest.lineage_profile == "clipped_recording_snapshot";
    const auto *instances = consolidatedEntry(root, base + "/instances");
    const auto *source = consolidatedEntry(root, base + "/source_detections");
    if (!instances || !source || instances->value("node_type", "") != "group" ||
        source->value("node_type", "") != "group") {
      assignError(error, "Refined detection table groups are missing");
      return false;
    }
    size_t validated = 0;
    for (const auto &declaration : kDeclarations) {
      if (declaration.clipped && !clipped) {
        continue;
      }
      const std::string path = base + "/" + declaration.relative_path;
      const auto *metadata = consolidatedEntry(root, path);
      const size_t rows = declaration.offset ? manifest.frame_count + 1
                          : declaration.source_table
                              ? manifest.source_detection_count
                              : manifest.instance_count;
      if (!metadata || metadata->value("node_type", "") != "array" ||
          metadata->contains("consolidated_metadata") ||
          metadata->value("data_type", "") != declaration.dtype) {
        assignError(error, "Missing or mistyped refined array: " + path);
        return false;
      }
      const auto shape = metadata->at("shape").get<std::vector<size_t>>();
      if (shape.size() != declaration.rank || shape[0] != rows ||
          (declaration.rank == 2 && shape[1] != declaration.columns)) {
        assignError(error, "Refined array shape mismatch: " + path);
        return false;
      }
      ++validated;
    }
    *count = validated;
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid refined consolidated metadata: " +
                           std::string(exception.what()));
    return false;
  }
}

bool validateDirectGroups(const ArchiveContext::Impl &archive, const json &root,
                          const std::string &base, size_t *read_count,
                          std::string *error) {
  for (const std::string &path :
       {base, base + "/instances", base + "/source_detections"}) {
    const auto *consolidated = consolidatedEntry(root, path);
    const auto direct = internal::ReadArchiveJson(archive, path + "/zarr.json");
    if (read_count) {
      ++*read_count;
    }
    if (!direct || !consolidated ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                           *consolidated)) {
      assignError(error,
                  "Refined direct and consolidated group metadata disagree: " +
                      path);
      return false;
    }
  }
  return true;
}

template <typename T, ts::DimensionIndex Rank>
std::optional<ts::TensorStore<T, Rank>>
openExact(const ArchiveContext::Impl &archive, const json &root,
          const std::string &path, std::string *error) {
  auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  const auto *metadata = consolidatedEntry(root, path);
  if (!spec || !metadata) {
    assignError(error, "Missing exact refined array metadata: " + path);
    return std::nullopt;
  }
  (*spec)["metadata"] = *metadata;
  auto opened = ts::Open<T, Rank>(
                    *spec, ts::OpenMode::open | ts::OpenMode::assume_metadata,
                    ts::ReadWriteMode::read, archive.context)
                    .result();
  if (!opened.ok()) {
    assignError(error, path + ": " + opened.status().ToString());
    return std::nullopt;
  }
  return *opened;
}

template <typename T, ts::DimensionIndex Rank>
bool readRows(const ts::TensorStore<T, Rank> &store, size_t first, size_t last,
              size_t columns, std::vector<T> *output, std::string *error) {
  if (!output || last < first ||
      last > static_cast<size_t>(store.domain().shape()[0])) {
    assignError(error, "Refined detection row range is invalid");
    return false;
  }
  if (first == last) {
    output->clear();
    return true;
  }
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  const auto read = ts::Read(store | ts::IdentityTransform(domain)).result();
  if (!read.ok() || read->rank() != Rank ||
      (Rank == 2 && static_cast<size_t>(read->shape()[1]) != columns)) {
    assignError(error, read.ok() ? "Refined detection read shape mismatch"
                                 : read.status().ToString());
    return false;
  }
  output->resize((last - first) * columns);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < last - first; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      ts::Index offset = static_cast<ts::Index>(row) * read->byte_strides()[0];
      if constexpr (Rank == 2) {
        offset += static_cast<ts::Index>(column) * read->byte_strides()[1];
      }
      (*output)[row * columns + column] =
          *reinterpret_cast<const T *>(origin + offset);
    }
  }
  return true;
}

bool readBoolRows(const ts::TensorStore<bool, 1> &store, size_t first,
                  size_t last, std::vector<uint8_t> *output,
                  std::string *error) {
  if (!output || last < first ||
      last > static_cast<size_t>(store.domain().shape()[0])) {
    assignError(error, "Refined detection boolean row range is invalid");
    return false;
  }
  if (first == last) {
    output->clear();
    return true;
  }
  ts::Box<1> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  const auto read = ts::Read(store | ts::IdentityTransform(domain)).result();
  if (!read.ok()) {
    assignError(error, read.status().ToString());
    return false;
  }
  output->resize(last - first);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < last - first; ++row) {
    (*output)[row] =
        *reinterpret_cast<const bool *>(origin + static_cast<ts::Index>(row) *
                                                     read->byte_strides()[0])
            ? 1
            : 0;
  }
  return true;
}

class TensorStoreRefinedDetectionRepository final
    : public CanonicalDetectionRepository {
public:
  TensorStoreRefinedDetectionRepository(
      CanonicalDetectionDescriptor descriptor, std::vector<int64_t> offsets,
      ts::TensorStore<int32_t, 1> frames,
      ts::TensorStore<uint64_t, 1> instance_keys,
      ts::TensorStore<int64_t, 1> refined_rows,
      ts::TensorStore<int64_t, 1> source_rows, ts::TensorStore<float, 2> boxes,
      ts::TensorStore<float, 1> scores, ts::TensorStore<bool, 1> score_valid,
      ts::TensorStore<int32_t, 1> classes,
      ts::TensorStore<uint8_t, 1> source_kinds,
      ts::TensorStore<bool, 1> manual_flags, size_t source_detection_count)
      : descriptor_(std::move(descriptor)), offsets_(std::move(offsets)),
        frames_(std::move(frames)), instance_keys_(std::move(instance_keys)),
        refined_rows_(std::move(refined_rows)),
        source_rows_(std::move(source_rows)), boxes_(std::move(boxes)),
        scores_(std::move(scores)), score_valid_(std::move(score_valid)),
        classes_(std::move(classes)), source_kinds_(std::move(source_kinds)),
        manual_flags_(std::move(manual_flags)),
        source_detection_count_(source_detection_count) {}

  const CanonicalDetectionDescriptor &descriptor() const override {
    return descriptor_;
  }

  CanonicalDetectionPage
  resolveCameraFrameRange(int64_t first_camera_frame,
                          int64_t last_camera_frame) const override {
    const auto started = Clock::now();
    CanonicalDetectionPage page;
    page.first_camera_frame = first_camera_frame;
    page.last_camera_frame = last_camera_frame;
    if (first_camera_frame < 0 || last_camera_frame < first_camera_frame ||
        static_cast<uint64_t>(last_camera_frame) >=
            descriptor_.camera_frame_count) {
      page.status = CanonicalDetectionPageStatus::OutOfRange;
      return page;
    }
    const size_t first_frame = static_cast<size_t>(first_camera_frame);
    const size_t last_frame = static_cast<size_t>(last_camera_frame);
    const size_t first_row = static_cast<size_t>(offsets_[first_frame]);
    const size_t last_row = static_cast<size_t>(offsets_[last_frame + 1]);
    const auto resident = std::atomic_load_explicit(&resident_columns_,
                                                    std::memory_order_acquire);
    CanonicalDetectionUiRows paged;
    if (!resident) {
      paged = readStorageRows(first_row, last_row, first_frame, last_frame);
      if (!paged.ready()) {
        page.error = paged.error;
        page.status = paged.status;
        recordPage(page, 0, elapsedMilliseconds(started), false);
        return page;
      }
    }
    const auto &boxes =
        resident ? resident->bbox_norm_coords : paged.bbox_norm_coords;
    const auto &scores = resident ? resident->scores : paged.scores;
    const auto &classes = resident ? resident->class_ids : paged.class_ids;
    const auto &keys = resident ? resident->instance_keys : paged.instance_keys;
    const auto &refined_ids =
        resident ? resident->refined_row_ids : paged.refined_row_ids;
    const auto &source_rows = resident ? resident->source_detect_row_indices
                                       : paged.source_detect_row_indices;
    const auto &kinds =
        resident ? resident->source_kind_codes : paged.source_kind_codes;
    const auto &score_flags =
        resident ? resident->score_valid : paged.score_valid;
    const auto &manual_flags =
        resident ? resident->manual_edit_flags : paged.manual_edit_flags;
    page.frames.reserve(last_frame - first_frame + 1);
    for (size_t frame = first_frame; frame <= last_frame; ++frame) {
      CanonicalDetectionFrame resolved;
      resolved.camera_frame = static_cast<int64_t>(frame);
      const size_t begin = static_cast<size_t>(offsets_[frame]);
      const size_t end = static_cast<size_t>(offsets_[frame + 1]);
      resolved.detections.reserve(end - begin);
      for (size_t row = begin; row < end; ++row) {
        const size_t index = resident ? row : row - first_row;
        CanonicalDetection detection;
        detection.row_index = static_cast<int64_t>(row);
        detection.instance_key = keys[index];
        detection.refined_row_id = refined_ids[index];
        detection.source_detect_row_index = source_rows[index];
        detection.source_kind_code = kinds[index];
        detection.score_valid = score_flags[index] != 0;
        detection.manual_edit = manual_flags[index] != 0;
        std::copy_n(boxes.data() + index * 4, 4,
                    detection.normalized_cxcywh.begin());
        detection.score = scores[index];
        detection.class_id = classes[index];
        resolved.detections.push_back(std::move(detection));
      }
      page.frames.push_back(std::move(resolved));
    }
    const size_t rows = last_row - first_row;
    page.decoded_bytes = rows * kDecodedBytesPerRow;
    page.status = CanonicalDetectionPageStatus::Ready;
    recordPage(page, rows, elapsedMilliseconds(started), resident != nullptr);
    return page;
  }

  uint64_t decodedUiColumnBytes() const override {
    if (descriptor_.row_count >
        std::numeric_limits<uint64_t>::max() / kDecodedBytesPerRow) {
      return std::numeric_limits<uint64_t>::max();
    }
    return descriptor_.row_count * kDecodedBytesPerRow;
  }

  std::vector<CanonicalDetectionUiResidencyChunk>
  planUiResidency(uint64_t maximum_chunk_decoded_bytes) const override {
    std::vector<CanonicalDetectionUiResidencyChunk> chunks;
    if (maximum_chunk_decoded_bytes == 0 || offsets_.size() < 2) {
      return chunks;
    }
    const size_t maximum_rows = static_cast<size_t>(std::max<uint64_t>(
        1, maximum_chunk_decoded_bytes / kDecodedBytesPerRow));
    size_t first_frame = 0;
    while (first_frame < descriptor_.camera_frame_count) {
      const size_t first_row = static_cast<size_t>(offsets_[first_frame]);
      const size_t target =
          std::min(descriptor_.row_count, first_row + maximum_rows);
      const auto upper =
          std::upper_bound(offsets_.begin() + first_frame + 1, offsets_.end(),
                           static_cast<int64_t>(target));
      size_t boundary = static_cast<size_t>(std::max<std::ptrdiff_t>(
          first_frame + 1, std::distance(offsets_.begin(), upper) - 1));
      boundary = std::min(descriptor_.camera_frame_count, boundary);
      chunks.push_back({static_cast<int64_t>(first_frame),
                        static_cast<int64_t>(boundary - 1), first_row,
                        static_cast<size_t>(offsets_[boundary])});
      first_frame = boundary;
    }
    return chunks;
  }

  CanonicalDetectionUiRows
  readUiRowsForResidency(size_t first_row, size_t last_row) const override {
    const auto started = Clock::now();
    if (last_row < first_row || last_row > descriptor_.row_count) {
      CanonicalDetectionUiRows result;
      result.status = CanonicalDetectionPageStatus::OutOfRange;
      result.error = "Refined detection residency range is invalid";
      return result;
    }
    const auto first_offset = std::upper_bound(offsets_.begin(), offsets_.end(),
                                               static_cast<int64_t>(first_row));
    const auto last_offset = std::lower_bound(offsets_.begin(), offsets_.end(),
                                              static_cast<int64_t>(last_row));
    const size_t first_frame =
        first_offset == offsets_.begin()
            ? 0
            : static_cast<size_t>(
                  std::distance(offsets_.begin(), first_offset) - 1);
    const size_t last_frame =
        last_offset == offsets_.begin()
            ? 0
            : std::min(descriptor_.camera_frame_count - 1,
                       static_cast<size_t>(
                           std::distance(offsets_.begin(), last_offset)));
    auto rows = readStorageRows(first_row, last_row, first_frame, last_frame);
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.residency_chunk_reads;
    metrics_.ui_field_reads += 10;
    metrics_.maximum_residency_chunk_read_ms = std::max(
        metrics_.maximum_residency_chunk_read_ms, elapsedMilliseconds(started));
    if (rows.ready()) {
      metrics_.residency_rows_read += last_row - first_row;
      metrics_.residency_decoded_bytes += rows.decoded_bytes;
    } else {
      ++metrics_.failed_reads;
      metrics_.last_error = rows.error;
    }
    return rows;
  }

  bool publishResidentUiColumns(
      std::shared_ptr<const CanonicalDetectionResidentUiColumns> columns,
      std::string *error) override {
    if (!columns || !columns->valid(descriptor_.row_count, true) ||
        !validateRows(*columns, 0, descriptor_.row_count, 0,
                      descriptor_.camera_frame_count - 1, error, true)) {
      if (error && error->empty()) {
        *error = "Refined resident column contract is invalid";
      }
      return false;
    }
    std::lock_guard<std::mutex> lock(resident_publication_mutex_);
    if (std::atomic_load_explicit(&resident_columns_,
                                  std::memory_order_acquire)) {
      assignError(error, "Refined resident columns are already published");
      return false;
    }
    std::atomic_store_explicit(&resident_columns_, std::move(columns),
                               std::memory_order_release);
    const auto resident = std::atomic_load_explicit(&resident_columns_,
                                                    std::memory_order_acquire);
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    ++metrics_.resident_publications;
    metrics_.resident_retained_bytes = resident->retainedBytes();
    return true;
  }

  bool residentUiColumnsReady() const override {
    return std::atomic_load_explicit(&resident_columns_,
                                     std::memory_order_acquire) != nullptr;
  }

  uint64_t residentUiColumnBytes() const override {
    const auto resident = std::atomic_load_explicit(&resident_columns_,
                                                    std::memory_order_acquire);
    return resident ? resident->retainedBytes() : 0;
  }

  CanonicalDetectionRepositoryMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto copy = metrics_;
    copy.peak_concurrent_ui_field_reads = peak_active_fields_.load();
    return copy;
  }

private:
  static constexpr uint64_t kDecodedBytesPerRow =
      4 * sizeof(float) + sizeof(float) + sizeof(int32_t) + sizeof(uint64_t) +
      2 * sizeof(int64_t) + 3 * sizeof(uint8_t);

  template <typename Columns>
  bool validateRows(const Columns &rows, size_t first_row, size_t last_row,
                    size_t first_frame, size_t last_frame, std::string *error,
                    bool global_identity) const {
    std::unordered_set<uint64_t> keys;
    std::unordered_set<int64_t> ids;
    std::unordered_set<int64_t> raw_source_rows;
    keys.reserve(last_row - first_row);
    ids.reserve(last_row - first_row);
    raw_source_rows.reserve(last_row - first_row);
    size_t expected_frame = first_frame;
    size_t previous_frame = std::numeric_limits<size_t>::max();
    int64_t previous_refined_id = -1;
    for (size_t row = first_row; row < last_row; ++row) {
      const size_t local = row - first_row;
      while (expected_frame < last_frame + 1 &&
             row >= static_cast<size_t>(offsets_[expected_frame + 1])) {
        ++expected_frame;
      }
      if constexpr (std::is_same_v<Columns, CanonicalDetectionUiRows>) {
        if (local >= rows.frame_indices.size() ||
            rows.frame_indices[local] != static_cast<int32_t>(expected_frame)) {
          assignError(error,
                      "Refined frame_row_offsets disagree with frame_indices");
          return false;
        }
      }
      const float *box = rows.bbox_norm_coords.data() + local * 4;
      const bool score_is_valid = rows.score_valid[local] != 0;
      const bool manual = rows.source_kind_codes[local] == 3;
      if (!std::all_of(box, box + 4,
                       [](float value) { return std::isfinite(value); }) ||
          box[2] <= 0.0f || box[3] <= 0.0f || box[0] - box[2] * 0.5f < 0.0f ||
          box[1] - box[3] * 0.5f < 0.0f || box[0] + box[2] * 0.5f > 1.0f ||
          box[1] + box[3] * 0.5f > 1.0f || rows.refined_row_ids[local] < 0 ||
          (rows.source_kind_codes[local] != 1 && !manual) ||
          (manual && rows.source_detect_row_indices[local] != -1) ||
          (!manual &&
           (rows.source_detect_row_indices[local] < 0 ||
            static_cast<size_t>(rows.source_detect_row_indices[local]) >=
                source_detection_count_)) ||
          rows.class_ids[local] < 0 || rows.class_ids[local] > 65535 ||
          !std::isfinite(rows.scores[local]) ||
          (score_is_valid &&
           (rows.scores[local] < 0.0f || rows.scores[local] > 1.0f)) ||
          (!score_is_valid && rows.scores[local] != 0.0f) ||
          (manual && (score_is_valid || rows.manual_edit_flags[local] == 0)) ||
          (!manual && !score_is_valid)) {
        assignError(error, "Refined instance semantic contract failed at row " +
                               std::to_string(row));
        return false;
      }
      if (!keys.insert(rows.instance_keys[local]).second ||
          !ids.insert(rows.refined_row_ids[local]).second ||
          (!manual &&
           !raw_source_rows.insert(rows.source_detect_row_indices[local])
                .second)) {
        assignError(
            error, global_identity
                       ? "Refined instance identity is not globally unique"
                       : "Refined instance identity repeats within read range");
        return false;
      }
      if (expected_frame == previous_frame &&
          rows.refined_row_ids[local] <= previous_refined_id) {
        assignError(error, "Refined rows are not ordered by refined_row_id");
        return false;
      }
      previous_frame = expected_frame;
      previous_refined_id = rows.refined_row_ids[local];
    }
    return true;
  }

  CanonicalDetectionUiRows readStorageRows(size_t first_row, size_t last_row,
                                           size_t first_frame,
                                           size_t last_frame) const {
    CanonicalDetectionUiRows rows;
    rows.first_row = first_row;
    rows.last_row_exclusive = last_row;
    if (last_row < first_row || last_row > descriptor_.row_count) {
      rows.status = CanonicalDetectionPageStatus::OutOfRange;
      rows.error = "Refined detection row range is out of range";
      return rows;
    }
    struct FieldResult {
      bool ready = false;
      std::string error;
    };
    auto readField = [this](auto &&read) {
      const size_t active = active_fields_.fetch_add(1) + 1;
      size_t peak = peak_active_fields_.load();
      while (active > peak &&
             !peak_active_fields_.compare_exchange_weak(peak, active)) {
      }
      FieldResult result;
      result.ready = read(&result.error);
      active_fields_.fetch_sub(1);
      return result;
    };
    std::vector<std::future<FieldResult>> futures;
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(frames_, first_row, last_row, 1, &rows.frame_indices,
                        e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(instance_keys_, first_row, last_row, 1,
                        &rows.instance_keys, e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(refined_rows_, first_row, last_row, 1,
                        &rows.refined_row_ids, e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(source_rows_, first_row, last_row, 1,
                        &rows.source_detect_row_indices, e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(boxes_, first_row, last_row, 4, &rows.bbox_norm_coords,
                        e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(scores_, first_row, last_row, 1, &rows.scores, e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readBoolRows(score_valid_, first_row, last_row,
                            &rows.score_valid, e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(classes_, first_row, last_row, 1, &rows.class_ids, e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readRows(source_kinds_, first_row, last_row, 1,
                        &rows.source_kind_codes, e);
      });
    }));
    futures.push_back(std::async(std::launch::async, [&] {
      return readField([&](auto *e) {
        return readBoolRows(manual_flags_, first_row, last_row,
                            &rows.manual_edit_flags, e);
      });
    }));
    for (auto &future : futures) {
      const auto result = future.get();
      if (!result.ready) {
        rows.status = CanonicalDetectionPageStatus::ReadFailed;
        rows.error = result.error;
        return rows;
      }
    }
    if (!validateRows(rows, first_row, last_row, first_frame, last_frame,
                      &rows.error, false)) {
      rows.status = CanonicalDetectionPageStatus::ReadFailed;
      return rows;
    }
    rows.decoded_bytes = (last_row - first_row) * kDecodedBytesPerRow;
    rows.status = CanonicalDetectionPageStatus::Ready;
    return rows;
  }

  void recordPage(const CanonicalDetectionPage &page, size_t rows,
                  double elapsed_ms, bool resident) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.range_reads;
    if (resident)
      ++metrics_.resident_range_reads;
    else {
      ++metrics_.paged_range_reads;
      metrics_.ui_field_reads += 10;
    }
    metrics_.maximum_range_read_ms =
        std::max(metrics_.maximum_range_read_ms, elapsed_ms);
    if (page.status == CanonicalDetectionPageStatus::Ready) {
      metrics_.resolved_frames += page.frames.size();
      metrics_.resolved_rows += rows;
    } else if (page.status == CanonicalDetectionPageStatus::ReadFailed) {
      ++metrics_.failed_reads;
      metrics_.last_error = page.error;
    }
  }

  CanonicalDetectionDescriptor descriptor_;
  std::vector<int64_t> offsets_;
  ts::TensorStore<int32_t, 1> frames_;
  ts::TensorStore<uint64_t, 1> instance_keys_;
  ts::TensorStore<int64_t, 1> refined_rows_;
  ts::TensorStore<int64_t, 1> source_rows_;
  ts::TensorStore<float, 2> boxes_;
  ts::TensorStore<float, 1> scores_;
  ts::TensorStore<bool, 1> score_valid_;
  ts::TensorStore<int32_t, 1> classes_;
  ts::TensorStore<uint8_t, 1> source_kinds_;
  ts::TensorStore<bool, 1> manual_flags_;
  std::shared_ptr<const CanonicalDetectionResidentUiColumns> resident_columns_;
  mutable std::mutex resident_publication_mutex_;
  mutable std::atomic<size_t> active_fields_{0};
  mutable std::atomic<size_t> peak_active_fields_{0};
  mutable std::mutex metrics_mutex_;
  mutable CanonicalDetectionRepositoryMetrics metrics_;
  size_t source_detection_count_ = 0;
};

} // namespace

std::unique_ptr<CanonicalDetectionRepository> OpenRefinedDetectionRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const std::string &requested_run,
    const RefinedDetectionRepositoryOpenOptions &options,
    std::string *error_message,
    RefinedDetectionRepositoryOpenMetrics *open_metrics) {
  RefinedDetectionRepositoryOpenMetrics metrics;
  const auto all_started = Clock::now();
  if (!archive || !archive->impl_) {
    assignError(error_message, "Archive context is unavailable");
    return nullptr;
  }
  if (requested_run.empty() || requested_run.find('/') != std::string::npos ||
      requested_run == "." || requested_run == "..") {
    assignError(error_message, "An exact refined detection run is required");
    return nullptr;
  }
  const std::string base = "refined_detect_runs/" + requested_run;
  const auto root_started = Clock::now();
  const auto root = internal::ReadArchiveJson(*archive->impl_, "zarr.json");
  metrics.root_metadata_ms = elapsedMilliseconds(root_started);
  metrics.root_metadata_reads = 1;
  if (!root) {
    assignError(error_message, "Archive root metadata is unreadable");
    return nullptr;
  }
  const auto *run_metadata = consolidatedEntry(*root, base);
  if (!run_metadata || run_metadata->value("node_type", "") != "group") {
    assignError(error_message, "Requested refined detection run is missing");
    return nullptr;
  }
  metrics.direct_run_metadata_reads = 1;
  if (!validateDirectGroups(*archive->impl_, *root, base,
                            &metrics.direct_group_metadata_reads,
                            error_message)) {
    return nullptr;
  }
  const auto manifest_started = Clock::now();
  RefinedDetectionManifestSummary manifest;
  try {
    if (!run_metadata->at("attributes").contains("run_manifest") ||
        !ValidateRefinedDetectionRunManifest(
            run_metadata->at("attributes").at("run_manifest"), requested_run,
            options.allow_selector_ineligible, &manifest, error_message)) {
      return nullptr;
    }
  } catch (const json::exception &exception) {
    assignError(error_message, "Refined run_manifest is missing: " +
                                   std::string(exception.what()));
    return nullptr;
  }
  metrics.manifest_validation_ms = elapsedMilliseconds(manifest_started);
  if (!options.expected_manifest_digest.empty() &&
      options.expected_manifest_digest != manifest.payload_digest) {
    assignError(error_message,
                "Refined authority and run_manifest digests disagree");
    return nullptr;
  }
  if (!validateDeclarations(*root, base, manifest,
                            &metrics.consolidated_array_declarations,
                            error_message)) {
    return nullptr;
  }

  CanonicalDetectionDescriptor descriptor;
  descriptor.surface_kind = DetectionSurfaceKind::RefinedSnapshotV1;
  descriptor.source_group = "refined_detect_runs";
  descriptor.run_name = requested_run;
  descriptor.instance_group = "instances";
  descriptor.run_manifest_digest = manifest.payload_digest;
  descriptor.row_count = manifest.instance_count;
  descriptor.camera_frame_count = manifest.frame_count;
  descriptor.source_width = manifest.source_width;
  descriptor.source_height = manifest.source_height;
  descriptor.consolidated_metadata = true;
  descriptor.stable_identity = true;
  descriptor.source_audit_lazy = true;
  descriptor.authority_approved = options.authority_approved;

  const std::string instances = base + "/instances/";
  const auto handles_started = Clock::now();
  auto frames = openExact<int32_t, 1>(
      *archive->impl_, *root, instances + "frame_indices", error_message);
  auto keys = openExact<uint64_t, 1>(*archive->impl_, *root,
                                     instances + "instance_key", error_message);
  auto refined = openExact<int64_t, 1>(
      *archive->impl_, *root, instances + "refined_row_ids", error_message);
  auto source_rows = openExact<int64_t, 1>(
      *archive->impl_, *root, instances + "source_detect_row_index",
      error_message);
  auto boxes = openExact<float, 2>(
      *archive->impl_, *root, instances + "bbox_norm_coords", error_message);
  auto scores = openExact<float, 1>(*archive->impl_, *root,
                                    instances + "scores", error_message);
  auto score_valid = openExact<bool, 1>(
      *archive->impl_, *root, instances + "score_valid", error_message);
  auto classes = openExact<int32_t, 1>(*archive->impl_, *root,
                                       instances + "class_ids", error_message);
  auto kinds = openExact<uint8_t, 1>(
      *archive->impl_, *root, instances + "source_kind_codes", error_message);
  auto manual = openExact<bool, 1>(
      *archive->impl_, *root, instances + "manual_edit_flags", error_message);
  auto offsets = openExact<int64_t, 1>(
      *archive->impl_, *root, instances + "frame_row_offsets", error_message);
  metrics.exact_handle_open_ms = elapsedMilliseconds(handles_started);
  if (!frames || !keys || !refined || !source_rows || !boxes || !scores ||
      !score_valid || !classes || !kinds || !manual || !offsets) {
    return nullptr;
  }
  metrics.exact_handle_opens = 11;

  const auto offset_started = Clock::now();
  std::vector<int64_t> retained_offsets;
  if (!readRows(*offsets, 0, manifest.frame_count + 1, 1, &retained_offsets,
                error_message)) {
    return nullptr;
  }
  metrics.offset_read_ms = elapsedMilliseconds(offset_started);
  metrics.offset_read_calls = 1;
  metrics.retained_offset_bytes = retained_offsets.size() * sizeof(int64_t);
  if (retained_offsets.size() != manifest.frame_count + 1 ||
      retained_offsets.front() != 0 ||
      retained_offsets.back() !=
          static_cast<int64_t>(manifest.instance_count) ||
      retained_offsets.back() < 0 ||
      !std::is_sorted(retained_offsets.begin(), retained_offsets.end())) {
    assignError(error_message, "Refined frame_row_offsets invariants failed");
    return nullptr;
  }
  descriptor.offset_read_calls = 1;
  descriptor.retained_offset_bytes = metrics.retained_offset_bytes;
  metrics.total_ms = elapsedMilliseconds(all_started);
  if (open_metrics) {
    *open_metrics = metrics;
  }
  return std::make_unique<TensorStoreRefinedDetectionRepository>(
      std::move(descriptor), std::move(retained_offsets), std::move(*frames),
      std::move(*keys), std::move(*refined), std::move(*source_rows),
      std::move(*boxes), std::move(*scores), std::move(*score_valid),
      std::move(*classes), std::move(*kinds), std::move(*manual),
      manifest.source_detection_count);
}

} // namespace crimson::zarr
