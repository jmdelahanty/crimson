#include "zarr/tensorstore_canonical_detection_repository.h"

#include "zarr/archive_context_internal.h"
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
};

constexpr ArrayDeclaration kDeclarations[] = {
    {"instances/frame_indices", "int32", 1, 1},
    {"instances/source_acquisition_frame_index", "int64", 1, 1},
    {"instances/instance_key", "uint64", 1, 1},
    {"instances/bbox_norm_coords", "float32", 2, 4},
    {"instances/bbox_img_xyxy", "float32", 2, 4},
    {"instances/centers_img_xy", "float32", 2, 2},
    {"instances/scores", "float32", 1, 1},
    {"instances/class_ids", "int32", 1, 1},
    {"instances/frame_row_offsets", "int64", 1, 1},
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

bool validateBoxContract(const json &logical_schema, std::string *error) {
  try {
    const auto &contracts =
        logical_schema.at("array_contracts").at("contracts");
    for (const auto &contract : contracts) {
      if (contract.value("schema_id", "") !=
          "palette.array.detection.bbox_norm_coords") {
        continue;
      }
      const auto axes =
          contract.at("axis_names").get<std::vector<std::string>>();
      if (contract.value("coordinate_space", "") !=
              "source_camera_normalized" ||
          axes != std::vector<std::string>({"instance", "cxcywh"})) {
        assignError(error, "Canonical bbox_norm_coords coordinate contract is "
                           "incompatible");
        return false;
      }
      return true;
    }
  } catch (const json::exception &) {
  }
  assignError(error,
              "Canonical bbox_norm_coords coordinate contract is missing");
  return false;
}

bool validateSchema(const json &root, const std::string &base,
                    CanonicalDetectionDescriptor *descriptor,
                    size_t *declaration_count, std::string *error) {
  try {
    if (root.at("zarr_format") != 3 || root.at("node_type") != "group" ||
        root.at("consolidated_metadata").at("kind") != "inline") {
      assignError(error,
                  "Archive root does not provide inline Zarr v3 consolidated "
                  "metadata");
      return false;
    }
    const auto *run_metadata = consolidatedEntry(root, base);
    const auto *instances_metadata =
        consolidatedEntry(root, base + "/instances");
    if (!run_metadata || !instances_metadata ||
        run_metadata->value("node_type", "") != "group" ||
        instances_metadata->value("node_type", "") != "group") {
      assignError(error, "Requested canonical detection run is missing");
      return false;
    }
    const auto &logical_schema =
        run_metadata->at("attributes").at("logical_schema");
    if (logical_schema.value("schema_id", "") !=
            "palette.stage.canonical_detection" ||
        logical_schema.value("schema_version", 0) != 1 ||
        logical_schema.value("layout", "") !=
            "sparse_instances_with_frame_row_offsets_v1" ||
        logical_schema.value("instance_group", "") != "instances" ||
        !validateBoxContract(logical_schema, error)) {
      if (error && error->empty()) {
        *error = "Canonical detection logical schema is incompatible";
      }
      return false;
    }
    const auto &dimensions = logical_schema.at("dimensions");
    const size_t frame_count = dimensions.at("n_frames").get<size_t>();
    const size_t row_count = dimensions.at("n_instances").get<size_t>();
    if (frame_count == 0 ||
        dimensions.at("n_frame_boundaries").get<size_t>() != frame_count + 1) {
      assignError(error, "Canonical detection dimensions are invalid");
      return false;
    }
    descriptor->camera_frame_count = frame_count;
    descriptor->row_count = row_count;
    descriptor->source_width = dimensions.value("source_width", size_t{0});
    descriptor->source_height = dimensions.value("source_height", size_t{0});

    size_t validated = 0;
    for (const auto &declaration : kDeclarations) {
      const std::string path = base + "/" + declaration.relative_path;
      const auto *metadata = consolidatedEntry(root, path);
      if (!metadata) {
        assignError(error, "Missing consolidated array declaration: " + path);
        return false;
      }
      const auto shape = metadata->at("shape").get<std::vector<size_t>>();
      const size_t expected_rows =
          std::string_view(declaration.relative_path) ==
                  "instances/frame_row_offsets"
              ? frame_count + 1
              : row_count;
      if (metadata->value("node_type", "") != "array" ||
          metadata->contains("consolidated_metadata") ||
          metadata->value("data_type", "") != declaration.dtype ||
          shape.size() != declaration.rank || shape[0] != expected_rows ||
          (declaration.rank == 2 && shape[1] != declaration.columns)) {
        assignError(error, "Consolidated array schema mismatch: " + path);
        return false;
      }
      ++validated;
    }
    *declaration_count = validated;
    descriptor->consolidated_metadata = true;
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid canonical detection metadata: " +
                           std::string(exception.what()));
    return false;
  }
}

bool validateDirectGroups(const ArchiveContext::Impl &archive, const json &root,
                          const std::string &base, size_t *read_count,
                          std::string *error) {
  for (const std::string &path : {base, base + "/instances"}) {
    const auto *consolidated = consolidatedEntry(root, path);
    const auto direct = internal::ReadArchiveJson(archive, path + "/zarr.json");
    if (read_count) {
      ++*read_count;
    }
    if (!direct || !consolidated ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                           *consolidated)) {
      assignError(
          error,
          "Canonical direct and consolidated group metadata disagree: " + path);
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
    assignError(error, "Missing exact array metadata: " + path);
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
    assignError(error, "Canonical detection row range is invalid");
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
      (Rank == 2 && static_cast<size_t>(read->shape()[1]) != columns)) {
    assignError(error, read.ok() ? "Canonical detection read shape mismatch"
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

class TensorStoreCanonicalDetectionRepository final
    : public CanonicalDetectionRepository {
public:
  TensorStoreCanonicalDetectionRepository(
      CanonicalDetectionDescriptor descriptor, std::vector<int64_t> offsets,
      ts::TensorStore<float, 2> boxes, ts::TensorStore<float, 1> scores,
      ts::TensorStore<int32_t, 1> classes)
      : descriptor_(std::move(descriptor)), offsets_(std::move(offsets)),
        boxes_(std::move(boxes)), scores_(std::move(scores)),
        classes_(std::move(classes)) {}

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
    CanonicalDetectionUiRows paged_rows;
    if (!resident) {
      paged_rows = readStorageUiRows(first_row, last_row);
      if (!paged_rows.ready()) {
        page.status = paged_rows.status;
        page.error = paged_rows.error;
        recordPageMetrics(page, 0, elapsedMilliseconds(started), false);
        return page;
      }
    }

    const auto &boxes =
        resident ? resident->bbox_norm_coords : paged_rows.bbox_norm_coords;
    const auto &scores = resident ? resident->scores : paged_rows.scores;
    const auto &classes = resident ? resident->class_ids : paged_rows.class_ids;

    const size_t row_count = last_row - first_row;
    page.frames.reserve(last_frame - first_frame + 1);
    for (size_t frame = first_frame; frame <= last_frame; ++frame) {
      CanonicalDetectionFrame resolved;
      resolved.camera_frame = static_cast<int64_t>(frame);
      const size_t frame_first = static_cast<size_t>(offsets_[frame]);
      const size_t frame_last = static_cast<size_t>(offsets_[frame + 1]);
      resolved.detections.reserve(frame_last - frame_first);
      for (size_t row = frame_first; row < frame_last; ++row) {
        const size_t local = row - first_row;
        CanonicalDetection detection;
        detection.row_index = static_cast<int64_t>(row);
        const size_t value_row = resident ? row : local;
        std::copy_n(boxes.data() + value_row * 4, 4,
                    detection.normalized_cxcywh.begin());
        detection.score = scores[value_row];
        detection.class_id = classes[value_row];
        resolved.detections.push_back(std::move(detection));
      }
      page.frames.push_back(std::move(resolved));
    }
    page.decoded_bytes = row_count * kUiDecodedBytesPerRow;
    page.status = CanonicalDetectionPageStatus::Ready;
    recordPageMetrics(page, row_count, elapsedMilliseconds(started),
                      resident != nullptr);
    return page;
  }

  uint64_t decodedUiColumnBytes() const override {
    if (descriptor_.row_count >
        std::numeric_limits<uint64_t>::max() / kUiDecodedBytesPerRow) {
      return std::numeric_limits<uint64_t>::max();
    }
    return descriptor_.row_count * kUiDecodedBytesPerRow;
  }

  std::vector<CanonicalDetectionUiResidencyChunk>
  planUiResidency(uint64_t maximum_chunk_decoded_bytes) const override {
    std::vector<CanonicalDetectionUiResidencyChunk> chunks;
    if (maximum_chunk_decoded_bytes == 0 || offsets_.size() < 2) {
      return chunks;
    }
    const size_t maximum_rows = static_cast<size_t>(std::max<uint64_t>(
        1, maximum_chunk_decoded_bytes / kUiDecodedBytesPerRow));
    const size_t frame_count = descriptor_.camera_frame_count;
    size_t first_frame = 0;
    while (first_frame < frame_count) {
      const size_t first_row = static_cast<size_t>(offsets_[first_frame]);
      const size_t target_row =
          first_row > descriptor_.row_count -
                          std::min(descriptor_.row_count, maximum_rows)
              ? descriptor_.row_count
              : std::min(descriptor_.row_count, first_row + maximum_rows);
      const auto search_first = offsets_.begin() + first_frame + 1;
      const auto upper = std::upper_bound(search_first, offsets_.end(),
                                          static_cast<int64_t>(target_row));
      size_t boundary =
          upper == search_first
              ? first_frame + 1
              : static_cast<size_t>(std::distance(offsets_.begin(), upper) - 1);
      boundary = std::min(frame_count, std::max(first_frame + 1, boundary));
      chunks.push_back({static_cast<int64_t>(first_frame),
                        static_cast<int64_t>(boundary - 1), first_row,
                        static_cast<size_t>(offsets_[boundary])});
      first_frame = boundary;
    }
    return chunks;
  }

  CanonicalDetectionUiRows
  readUiRowsForResidency(size_t first_row,
                         size_t last_row_exclusive) const override {
    const auto started = Clock::now();
    auto rows = readStorageUiRows(first_row, last_row_exclusive);
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.residency_chunk_reads;
    metrics_.ui_field_reads += 3;
    metrics_.maximum_residency_chunk_read_ms = std::max(
        metrics_.maximum_residency_chunk_read_ms, elapsedMilliseconds(started));
    if (rows.ready()) {
      metrics_.residency_rows_read += last_row_exclusive - first_row;
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
    if (!columns || !columns->valid(descriptor_.row_count)) {
      assignError(error, "Canonical resident UI column extents are invalid");
      return false;
    }
    for (size_t row = 0; row < descriptor_.row_count; ++row) {
      if (!validUiRow(columns->bbox_norm_coords.data() + row * 4,
                      columns->scores[row], columns->class_ids[row])) {
        assignError(error,
                    "Canonical resident UI value contract failed at row " +
                        std::to_string(row));
        return false;
      }
    }
    std::lock_guard<std::mutex> publication_lock(resident_publication_mutex_);
    if (std::atomic_load_explicit(&resident_columns_,
                                  std::memory_order_acquire)) {
      assignError(error, "Canonical resident UI columns are already published");
      return false;
    }
    std::atomic_store_explicit(&resident_columns_, std::move(columns),
                               std::memory_order_release);
    const auto resident = std::atomic_load_explicit(&resident_columns_,
                                                    std::memory_order_acquire);
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    ++metrics_.resident_publications;
    metrics_.resident_retained_bytes = resident ? resident->retainedBytes() : 0;
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
    auto result = metrics_;
    result.peak_concurrent_ui_field_reads = peak_active_fields_.load();
    return result;
  }

private:
  static constexpr uint64_t kUiDecodedBytesPerRow = 6 * sizeof(uint32_t);

  static bool validUiRow(const float *box, float score, int32_t class_id) {
    return std::all_of(box, box + 4,
                       [](float value) { return std::isfinite(value); }) &&
           box[2] > 0.0f && box[3] > 0.0f && std::isfinite(score) &&
           score >= 0.0f && score <= 1.0f && class_id >= 0 && class_id <= 65535;
  }

  CanonicalDetectionUiRows readStorageUiRows(size_t first_row,
                                             size_t last_row) const {
    CanonicalDetectionUiRows rows;
    rows.first_row = first_row;
    rows.last_row_exclusive = last_row;
    if (last_row < first_row || last_row > descriptor_.row_count) {
      rows.status = CanonicalDetectionPageStatus::OutOfRange;
      rows.error = "Canonical detection row range is out of range";
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
    auto boxes_future = std::async(std::launch::async, [&] {
      return readField([&](std::string *error) {
        return readRows(boxes_, first_row, last_row, 4, &rows.bbox_norm_coords,
                        error);
      });
    });
    auto scores_future = std::async(std::launch::async, [&] {
      return readField([&](std::string *error) {
        return readRows(scores_, first_row, last_row, 1, &rows.scores, error);
      });
    });
    auto classes_future = std::async(std::launch::async, [&] {
      return readField([&](std::string *error) {
        return readRows(classes_, first_row, last_row, 1, &rows.class_ids,
                        error);
      });
    });
    const auto boxes_result = boxes_future.get();
    const auto scores_result = scores_future.get();
    const auto classes_result = classes_future.get();
    if (!boxes_result.ready || !scores_result.ready || !classes_result.ready) {
      rows.status = CanonicalDetectionPageStatus::ReadFailed;
      rows.error = !boxes_result.ready    ? boxes_result.error
                   : !scores_result.ready ? scores_result.error
                                          : classes_result.error;
      return rows;
    }
    const size_t row_count = last_row - first_row;
    for (size_t row = 0; row < row_count; ++row) {
      if (!validUiRow(rows.bbox_norm_coords.data() + row * 4, rows.scores[row],
                      rows.class_ids[row])) {
        rows.status = CanonicalDetectionPageStatus::ReadFailed;
        rows.error = "Canonical detection value contract failed at row " +
                     std::to_string(first_row + row);
        rows.bbox_norm_coords.clear();
        rows.scores.clear();
        rows.class_ids.clear();
        return rows;
      }
    }
    rows.decoded_bytes = row_count * kUiDecodedBytesPerRow;
    rows.status = CanonicalDetectionPageStatus::Ready;
    return rows;
  }

  void recordPageMetrics(const CanonicalDetectionPage &page, size_t rows,
                         double elapsed_ms, bool resident) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.range_reads;
    if (resident) {
      ++metrics_.resident_range_reads;
    } else {
      ++metrics_.paged_range_reads;
      metrics_.ui_field_reads += 3;
    }
    metrics_.maximum_range_read_ms =
        std::max(metrics_.maximum_range_read_ms, elapsed_ms);
    metrics_.peak_concurrent_ui_field_reads = std::max(
        metrics_.peak_concurrent_ui_field_reads, peak_active_fields_.load());
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
  ts::TensorStore<float, 2> boxes_;
  ts::TensorStore<float, 1> scores_;
  ts::TensorStore<int32_t, 1> classes_;
  std::shared_ptr<const CanonicalDetectionResidentUiColumns> resident_columns_;
  mutable std::mutex resident_publication_mutex_;
  mutable std::atomic<size_t> active_fields_{0};
  mutable std::atomic<size_t> peak_active_fields_{0};
  mutable std::mutex metrics_mutex_;
  mutable CanonicalDetectionRepositoryMetrics metrics_;
};

} // namespace

std::unique_ptr<CanonicalDetectionRepository> OpenCanonicalDetectionRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const std::string &requested_run, std::string *error_message,
    CanonicalDetectionRepositoryOpenMetrics *open_metrics) {
  CanonicalDetectionRepositoryOpenMetrics metrics;
  const auto all_started = Clock::now();
  if (!archive || !archive->impl_) {
    assignError(error_message, "Archive context is unavailable");
    return nullptr;
  }
  if (requested_run.empty() || requested_run.find('/') != std::string::npos ||
      requested_run == "." || requested_run == "..") {
    assignError(error_message,
                "An exact canonical detection run name is required");
    return nullptr;
  }
  const std::string base = "detect_runs/" + requested_run;
  CanonicalDetectionDescriptor descriptor;
  descriptor.source_group = "detect_runs";
  descriptor.run_name = requested_run;
  descriptor.instance_group = "instances";

  const auto root_started = Clock::now();
  const auto root = internal::ReadArchiveJson(*archive->impl_, "zarr.json");
  metrics.root_metadata_ms = elapsedMilliseconds(root_started);
  metrics.root_metadata_reads = 1;
  if (!root || !validateSchema(*root, base, &descriptor,
                               &metrics.consolidated_array_declarations,
                               error_message)) {
    return nullptr;
  }
  if (!validateDirectGroups(*archive->impl_, *root, base,
                            &metrics.direct_group_metadata_reads,
                            error_message)) {
    return nullptr;
  }

  const auto handles_started = Clock::now();
  auto boxes =
      openExact<float, 2>(*archive->impl_, *root,
                          base + "/instances/bbox_norm_coords", error_message);
  if (boxes) {
    ++metrics.exact_handle_opens;
  }
  auto scores = openExact<float, 1>(*archive->impl_, *root,
                                    base + "/instances/scores", error_message);
  if (scores) {
    ++metrics.exact_handle_opens;
  }
  auto classes = openExact<int32_t, 1>(
      *archive->impl_, *root, base + "/instances/class_ids", error_message);
  if (classes) {
    ++metrics.exact_handle_opens;
  }
  auto offsets = openExact<int64_t, 1>(*archive->impl_, *root,
                                       base + "/instances/frame_row_offsets",
                                       error_message);
  if (offsets) {
    ++metrics.exact_handle_opens;
  }
  metrics.exact_handle_open_ms = elapsedMilliseconds(handles_started);
  if (!boxes || !scores || !classes || !offsets) {
    return nullptr;
  }

  const auto offset_started = Clock::now();
  std::vector<int64_t> retained_offsets;
  if (!readRows(*offsets, 0, descriptor.camera_frame_count + 1, 1,
                &retained_offsets, error_message)) {
    return nullptr;
  }
  metrics.offset_read_ms = elapsedMilliseconds(offset_started);
  metrics.offset_read_calls = 1;
  metrics.retained_offset_bytes = retained_offsets.size() * sizeof(int64_t);
  if (retained_offsets.size() != descriptor.camera_frame_count + 1 ||
      retained_offsets.front() != 0 ||
      retained_offsets.back() != static_cast<int64_t>(descriptor.row_count) ||
      !std::is_sorted(retained_offsets.begin(), retained_offsets.end()) ||
      retained_offsets.back() < 0) {
    assignError(error_message, "Canonical frame_row_offsets invariants failed");
    return nullptr;
  }
  descriptor.offset_read_calls = 1;
  descriptor.retained_offset_bytes = metrics.retained_offset_bytes;
  metrics.total_ms = elapsedMilliseconds(all_started);
  if (open_metrics) {
    *open_metrics = metrics;
  }
  return std::make_unique<TensorStoreCanonicalDetectionRepository>(
      std::move(descriptor), std::move(retained_offsets), std::move(*boxes),
      std::move(*scores), std::move(*classes));
}

} // namespace crimson::zarr
