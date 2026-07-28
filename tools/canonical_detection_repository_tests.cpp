#include "canonical_detection_buffer.h"
#include "read_only_overlay_scene.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_detection_overlay_scene_adapter.h"
#include "zarr/detection_repository_selection.h"
#include "zarr/refined_detection_contract.h"
#include "zarr/tensorstore_canonical_detection_repository.h"
#include "zarr/tensorstore_refined_detection_repository.h"
#include "zarr/zarr_metadata_equivalence.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace ts = tensorstore;
using json = nlohmann::json;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':'    \
                << __LINE__ << '\n';                                           \
      return false;                                                            \
    }                                                                          \
  } while (false)

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-canonical-detection-" + std::to_string(seed) + "-" +
               std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    path_.clear();
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class BlockingCanonicalDetectionRepository final
    : public crimson::zarr::CanonicalDetectionRepository {
public:
  BlockingCanonicalDetectionRepository() {
    descriptor_.source_group = "detect_runs";
    descriptor_.run_name = "blocking_fixture";
    descriptor_.instance_group = "instances";
    descriptor_.row_count = 4;
    descriptor_.camera_frame_count = 4;
    descriptor_.source_width = 100;
    descriptor_.source_height = 80;
    descriptor_.retained_offset_bytes = 5 * sizeof(int64_t);
    descriptor_.offset_read_calls = 1;
    descriptor_.consolidated_metadata = true;
  }

  const crimson::zarr::CanonicalDetectionDescriptor &
  descriptor() const override {
    return descriptor_;
  }

  crimson::zarr::CanonicalDetectionPage
  resolveCameraFrameRange(int64_t first, int64_t last) const override {
    crimson::zarr::CanonicalDetectionPage page;
    page.first_camera_frame = first;
    page.last_camera_frame = last;
    if (first < 0 || last < first || last >= 4) {
      page.status = crimson::zarr::CanonicalDetectionPageStatus::OutOfRange;
      return page;
    }
    for (int64_t frame = first; frame <= last; ++frame) {
      crimson::zarr::CanonicalDetectionFrame resolved;
      resolved.camera_frame = frame;
      resolved.detections.push_back(detection(static_cast<size_t>(frame)));
      page.frames.push_back(std::move(resolved));
    }
    page.decoded_bytes = page.frames.size() * 24;
    page.status = crimson::zarr::CanonicalDetectionPageStatus::Ready;
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.range_reads;
    ++metrics_.paged_range_reads;
    metrics_.resolved_frames += page.frames.size();
    metrics_.resolved_rows += page.frames.size();
    metrics_.ui_field_reads += 3;
    return page;
  }

  uint64_t decodedUiColumnBytes() const override { return 4 * 24; }

  std::vector<crimson::zarr::CanonicalDetectionUiResidencyChunk>
  planUiResidency(uint64_t maximum_chunk_decoded_bytes) const override {
    if (maximum_chunk_decoded_bytes == 0) {
      return {};
    }
    return {{0, 3, 0, 4}};
  }

  crimson::zarr::CanonicalDetectionUiRows
  readUiRowsForResidency(size_t first, size_t last) const override {
    crimson::zarr::CanonicalDetectionUiRows rows;
    rows.first_row = first;
    rows.last_row_exclusive = last;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      preload_started_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return release_preload_; });
    }
    if (last < first || last > 4) {
      rows.status = crimson::zarr::CanonicalDetectionPageStatus::OutOfRange;
      return rows;
    }
    for (size_t row = first; row < last; ++row) {
      const auto value = detection(row);
      rows.bbox_norm_coords.insert(rows.bbox_norm_coords.end(),
                                   value.normalized_cxcywh.begin(),
                                   value.normalized_cxcywh.end());
      rows.scores.push_back(value.score);
      rows.class_ids.push_back(value.class_id);
    }
    rows.decoded_bytes = (last - first) * 24;
    rows.status = crimson::zarr::CanonicalDetectionPageStatus::Ready;
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.residency_chunk_reads;
    metrics_.residency_rows_read += last - first;
    metrics_.residency_decoded_bytes += rows.decoded_bytes;
    metrics_.ui_field_reads += 3;
    return rows;
  }

  bool publishResidentUiColumns(
      std::shared_ptr<const crimson::zarr::CanonicalDetectionResidentUiColumns>
          columns,
      std::string *error) override {
    if (!columns || !columns->valid(4)) {
      if (error) {
        *error = "Invalid blocking resident columns";
      }
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    resident_ = std::move(columns);
    ++metrics_.resident_publications;
    metrics_.resident_retained_bytes = resident_->retainedBytes();
    return true;
  }

  bool residentUiColumnsReady() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return resident_ != nullptr;
  }

  uint64_t residentUiColumnBytes() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return resident_ ? resident_->retainedBytes() : 0;
  }

  crimson::zarr::CanonicalDetectionRepositoryMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
  }

  bool waitForPreloadStart(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] { return preload_started_; });
  }

  void releasePreload() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_preload_ = true;
    condition_.notify_all();
  }

private:
  static crimson::zarr::CanonicalDetection detection(size_t row) {
    crimson::zarr::CanonicalDetection value;
    value.row_index = static_cast<int64_t>(row);
    value.normalized_cxcywh = {0.2f + static_cast<float>(row) * 0.1f, 0.5f,
                               0.1f, 0.2f};
    value.score = 0.9f - static_cast<float>(row) * 0.1f;
    value.class_id = static_cast<int32_t>(row + 1);
    return value;
  }

  crimson::zarr::CanonicalDetectionDescriptor descriptor_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable bool preload_started_ = false;
  mutable bool release_preload_ = false;
  std::shared_ptr<const crimson::zarr::CanonicalDetectionResidentUiColumns>
      resident_;
  mutable crimson::zarr::CanonicalDetectionRepositoryMetrics metrics_;
};

bool writeJson(const std::filesystem::path &path, const json &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value.dump(2) << '\n';
  return output.good();
}

std::optional<json> readJson(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  try {
    return json::parse(input);
  } catch (const json::exception &) {
    return std::nullopt;
  }
}

template <typename T, size_t Rank>
bool writeArray(const std::filesystem::path &root, const std::string &path,
                const std::string &data_type,
                const std::array<ts::Index, Rank> &shape,
                const std::vector<T> &values) {
  size_t element_count = 1;
  json shape_json = json::array();
  json chunk_shape = json::array();
  for (const auto extent : shape) {
    element_count *= static_cast<size_t>(extent);
    shape_json.push_back(extent);
    chunk_shape.push_back(std::max<ts::Index>(1, extent));
  }
  if (element_count != values.size()) {
    return false;
  }
  json bytes_codec = {{"name", "bytes"}};
  if (sizeof(T) > 1) {
    bytes_codec["configuration"] = {{"endian", "little"}};
  }
  json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata",
       {{"shape", shape_json},
        {"data_type", data_type},
        {"chunk_grid",
         {{"name", "regular"},
          {"configuration", {{"chunk_shape", chunk_shape}}}}},
        {"chunk_key_encoding",
         {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", std::is_same_v<T, bool> ? json(false) : json(0)},
        {"codecs", json::array({bytes_codec})}}},
  };
  auto store =
      ts::Open<T, Rank>(spec, ts::OpenMode::open | ts::OpenMode::create,
                        ts::ReadWriteMode::read_write)
          .result();
  if (!store.ok()) {
    std::cerr << "Failed to create " << path << ": " << store.status() << '\n';
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

json logicalSchema() {
  return {
      {"schema_id", "palette.stage.canonical_detection"},
      {"schema_version", 1},
      {"stage", "detect"},
      {"layout", "sparse_instances_with_frame_row_offsets_v1"},
      {"base_path", "detect_runs/<run>"},
      {"instance_group", "instances"},
      {"dimensions",
       {{"n_frames", 4},
        {"n_instances", 3},
        {"n_frame_boundaries", 5},
        {"source_width", 100},
        {"source_height", 80}}},
      {"array_contracts",
       {{"schema_id", "palette.array_contract_catalog"},
        {"schema_version", 1},
        {"contracts",
         json::array(
             {{{"schema_id", "palette.array.detection.bbox_norm_coords"},
               {"schema_version", 1},
               {"axis_names", {"instance", "cxcywh"}},
               {"coordinate_space", "source_camera_normalized"}}})}}},
  };
}

bool buildFixture(const std::filesystem::path &root) {
  constexpr const char *run = "fixture_run";
  const std::string base = std::string("detect_runs/") + run;
  const json detect_group = {
      {"zarr_format", 3}, {"node_type", "group"}, {"attributes", {}}};
  const json run_group = {{"zarr_format", 3},
                          {"node_type", "group"},
                          {"consolidated_metadata", nullptr},
                          {"attributes",
                           {{"benchmark_only", true},
                            {"selector_eligible", false},
                            {"logical_schema", logicalSchema()}}}};
  const json instances_group = {{"zarr_format", 3},
                                {"node_type", "group"},
                                {"consolidated_metadata", nullptr},
                                {"attributes", {}}};
  CHECK(writeJson(root / "detect_runs/zarr.json", detect_group));
  CHECK(writeJson(root / base / "zarr.json", run_group));
  CHECK(writeJson(root / base / "instances/zarr.json", instances_group));

  const std::string instances = base + "/instances/";
  CHECK((writeArray<int32_t, 1>(root, instances + "frame_indices", "int32", {3},
                                {1, 2, 2})));
  CHECK((writeArray<int64_t, 1>(root,
                                instances + "source_acquisition_frame_index",
                                "int64", {3}, {1, 2, 2})));
  CHECK((writeArray<uint64_t, 1>(root, instances + "instance_key", "uint64",
                                 {3}, {101, 202, 203})));
  CHECK((writeArray<float, 2>(root, instances + "bbox_norm_coords", "float32",
                              {3, 4},
                              {0.20f, 0.25f, 0.10f, 0.20f, 0.50f, 0.50f, 0.20f,
                               0.25f, 0.75f, 0.60f, 0.10f, 0.10f})));
  CHECK((
      writeArray<float, 2>(root, instances + "bbox_img_xyxy", "float32", {3, 4},
                           {15, 12, 25, 28, 40, 30, 60, 50, 70, 44, 80, 52})));
  CHECK((writeArray<float, 2>(root, instances + "centers_img_xy", "float32",
                              {3, 2}, {20, 20, 50, 40, 75, 48})));
  CHECK((writeArray<float, 1>(root, instances + "scores", "float32", {3},
                              {0.9f, 0.8f, 0.7f})));
  CHECK((writeArray<int32_t, 1>(root, instances + "class_ids", "int32", {3},
                                {4, 5, 6})));
  CHECK((writeArray<int64_t, 1>(root, instances + "frame_row_offsets", "int64",
                                {5}, {0, 0, 1, 3, 3})));

  json root_metadata = {
      {"zarr_format", 3},
      {"node_type", "group"},
      {"attributes", {{"benchmark_only", true}}},
      {"consolidated_metadata",
       {{"kind", "inline"},
        {"must_understand", false},
        {"metadata", json::object()}}},
  };
  auto &consolidated = root_metadata["consolidated_metadata"]["metadata"];
  for (const std::string path :
       {std::string("detect_runs"), base, base + "/instances"}) {
    const auto metadata = readJson(root / path / "zarr.json");
    CHECK(metadata.has_value());
    consolidated[path] = *metadata;
    if (path == base || path == base + "/instances") {
      consolidated[path]["consolidated_metadata"] = {
          {"kind", "inline"},
          {"must_understand", false},
          {"metadata", json::object()},
      };
    }
  }
  for (const std::string name :
       {"frame_indices", "source_acquisition_frame_index", "instance_key",
        "bbox_norm_coords", "bbox_img_xyxy", "centers_img_xy", "scores",
        "class_ids", "frame_row_offsets"}) {
    const std::string path = instances + name;
    const auto metadata = readJson(root / path / "zarr.json");
    CHECK(metadata.has_value());
    consolidated[path] = *metadata;
  }
  return writeJson(root / "zarr.json", root_metadata);
}

json reasonRegistry(const std::string &registry_id) {
  json payload = {{"schema_id", "palette.refined_detection.reason_registry"},
                  {"schema_version", 1},
                  {"registry_id", registry_id},
                  {"codes", {{"0", "none"}}}};
  json result = payload;
  result["digest_algorithm"] = "sha256_canonical_json_v1";
  result["digest"] = crimson::zarr::CanonicalJsonSha256(payload);
  return result;
}

json refinedRunManifest(const std::string &run, bool selector_eligible) {
  json logical = {
      {"schema_id", "palette.stage.refined_detection"},
      {"schema_version", 1},
      {"stage", "refined_detect"},
      {"layout",
       "immutable_sparse_instances_with_source_audit_and_frame_row_offsets_v1"},
      {"base_path", "refined_detect_runs/<run>"},
      {"dimensions",
       {{"n_frames", 4},
        {"n_instances", 6},
        {"n_source_detections", 3},
        {"n_frame_boundaries", 5},
        {"source_width", 100},
        {"source_height", 80},
        {"lineage_profile", "full_acquisition"}}},
      {"clipped_binding", nullptr},
      {"bindings", json::array()},
      {"forbidden_legacy_bindings", json::array()},
      {"array_contracts", json::object()},
      {"code_maps", json::object()},
      {"invariants", json::object()},
  };
  json payload = {
      {"run_id", run},
      {"stage", "refined_detect"},
      {"publication",
       {{"completion_contract", "palette.zarr_run_completion.v1"},
        {"completion_status", "complete"},
        {"stage_selector_eligible", selector_eligible},
        {"metadata_state", "direct_and_consolidated_validated"},
        {"metadata_declarations_digest_scope",
         "normalized_group_and_array_declarations_excluding_attributes"},
        {"metadata_declarations_digest_algorithm", "sha256_canonical_json_v1"},
        {"metadata_declarations_digest", std::string(64, 'a')}}},
      {"logical_schema", logical},
      {"storage_plan", json::object()},
      {"snapshot_lineage", json::object()},
      {"source_detection", json::object()},
      {"reason_registries",
       {{"instances", reasonRegistry("instances.reason_codes.v1")},
        {"source_detections",
         reasonRegistry("source_detections.reason_codes.v1")}}},
  };
  return {{"schema_id", "palette.refined_detection.run_manifest"},
          {"schema_version", 1},
          {"persisted_attribute", "run_manifest"},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"payload_digest", crimson::zarr::CanonicalJsonSha256(payload)},
          {"payload", payload}};
}

json refinedAuthority(const std::string &run,
                      const std::string &manifest_digest) {
  json payload = {{"run_id", run},
                  {"run_manifest_digest", manifest_digest},
                  {"review_state", "approved"},
                  {"review_method", "headless_fixture"},
                  {"intended_use", "analysis"},
                  {"approved_by", "test"},
                  {"approved_at_utc", "2026-07-27T00:00:00Z"},
                  {"git_sha", nullptr},
                  {"note", ""}};
  return {{"schema_id", "palette.refined_detection.authoritative_selection"},
          {"schema_version", 1},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"payload_digest", crimson::zarr::CanonicalJsonSha256(payload)},
          {"payload", payload}};
}

bool buildRefinedFixture(
    const std::filesystem::path &root, bool selector_eligible = true,
    std::vector<int64_t> instance_offsets = {0, 2, 2, 3, 6},
    std::vector<uint64_t> instance_keys = {101, 501, 303, 404, 502, 503}) {
  constexpr const char *run = "refined_fixture";
  const std::string base = std::string("refined_detect_runs/") + run;
  const json manifest = refinedRunManifest(run, selector_eligible);
  const json parent = {
      {"zarr_format", 3},
      {"node_type", "group"},
      {"attributes",
       {{"authoritative_run", run},
        {"authoritative_run_provenance",
         refinedAuthority(run, manifest.at("payload_digest"))}}}};
  const json run_group = {{"zarr_format", 3},
                          {"node_type", "group"},
                          {"consolidated_metadata", nullptr},
                          {"attributes", {{"run_manifest", manifest}}}};
  const json empty_group = {{"zarr_format", 3},
                            {"node_type", "group"},
                            {"consolidated_metadata", nullptr},
                            {"attributes", {}}};
  CHECK(writeJson(root / "refined_detect_runs/zarr.json", parent));
  CHECK(writeJson(root / base / "zarr.json", run_group));
  CHECK(writeJson(root / base / "instances/zarr.json", empty_group));
  CHECK(writeJson(root / base / "source_detections/zarr.json", empty_group));

  const std::string instances = base + "/instances/";
  CHECK((writeArray<int32_t, 1>(root, instances + "frame_indices", "int32", {6},
                                {0, 0, 2, 3, 3, 3})));
  CHECK((writeArray<int64_t, 1>(root,
                                instances + "source_acquisition_frame_index",
                                "int64", {6}, {0, 0, 2, 3, 3, 3})));
  CHECK((writeArray<uint64_t, 1>(root, instances + "instance_key", "uint64",
                                 {6}, instance_keys)));
  CHECK((writeArray<int64_t, 1>(root, instances + "refined_row_ids", "int64",
                                {6}, {10, 11, 12, 13, 14, 15})));
  const std::vector<float> boxes = {0.2f, 0.25f, 0.1f,  0.2f, 0.5f,  0.5f,
                                    0.2f, 0.2f,  0.75f, 0.6f, 0.1f,  0.1f,
                                    0.3f, 0.3f,  0.2f,  0.2f, 0.32f, 0.32f,
                                    0.2f, 0.2f,  0.8f,  0.8f, 0.1f,  0.1f};
  CHECK((writeArray<float, 2>(root, instances + "bbox_norm_coords", "float32",
                              {6, 4}, boxes)));
  CHECK((
      writeArray<float, 2>(root, instances + "bbox_img_xyxy", "float32", {6, 4},
                           {15, 12, 25, 28, 40, 32, 60, 48, 70, 44, 80, 52,
                            20, 16, 40, 32, 22, 18, 42, 34, 75, 60, 85, 68})));
  CHECK((writeArray<float, 2>(
      root, instances + "centers_img_xy", "float32", {6, 2},
      {20, 20, 50, 40, 75, 48, 30, 24, 32, 26, 80, 64})));
  CHECK((writeArray<float, 1>(root, instances + "scores", "float32", {6},
                              {0.9f, 0.0f, 0.7f, 0.6f, 0.0f, 0.0f})));
  CHECK((writeArray<bool, 1>(root, instances + "score_valid", "bool", {6},
                             {true, false, true, true, false, false})));
  CHECK((writeArray<int32_t, 1>(root, instances + "class_ids", "int32", {6},
                                {4, 4, 6, 6, 6, 7})));
  CHECK((writeArray<uint8_t, 1>(root, instances + "source_kind_codes", "uint8",
                                {6}, {1, 3, 1, 1, 3, 3})));
  CHECK((writeArray<bool, 1>(root, instances + "manual_edit_flags", "bool", {6},
                             {false, true, false, false, true, true})));
  CHECK((writeArray<int64_t, 1>(root, instances + "source_detect_row_index",
                                "int64", {6}, {0, -1, 1, 2, -1, -1})));
  CHECK((writeArray<uint16_t, 1>(root, instances + "reason_codes", "uint16",
                                 {6}, {0, 0, 0, 0, 0, 0})));
  CHECK((writeArray<int64_t, 1>(root, instances + "frame_row_offsets", "int64",
                                {5}, instance_offsets)));

  const std::string source = base + "/source_detections/";
  CHECK((writeArray<int64_t, 1>(root, source + "source_detect_row_index",
                                "int64", {3}, {0, 1, 2})));
  CHECK((writeArray<int32_t, 1>(root, source + "frame_indices", "int32", {3},
                                {0, 2, 3})));
  CHECK((writeArray<int64_t, 1>(root, source + "source_acquisition_frame_index",
                                "int64", {3}, {0, 2, 3})));
  CHECK((writeArray<uint64_t, 1>(root, source + "instance_key", "uint64", {3},
                                 {101, 303, 404})));
  CHECK((writeArray<float, 2>(root, source + "bbox_norm_coords", "float32",
                              {3, 4},
                              {0.2f, 0.25f, 0.1f, 0.2f, 0.75f, 0.6f, 0.1f, 0.1f,
                               0.3f, 0.3f, 0.2f, 0.2f})));
  CHECK(
      (writeArray<float, 2>(root, source + "bbox_img_xyxy", "float32", {3, 4},
                            {15, 12, 25, 28, 70, 44, 80, 52, 20, 16, 40, 32})));
  CHECK((writeArray<float, 2>(root, source + "centers_img_xy", "float32",
                              {3, 2}, {20, 20, 75, 48, 30, 24})));
  CHECK((writeArray<float, 1>(root, source + "scores", "float32", {3},
                              {0.9f, 0.7f, 0.6f})));
  CHECK((writeArray<int32_t, 1>(root, source + "class_ids", "int32", {3},
                                {4, 6, 6})));
  CHECK((writeArray<uint8_t, 1>(root, source + "decision_codes", "uint8", {3},
                                {0, 0, 0})));
  CHECK((writeArray<int64_t, 1>(root, source + "resolved_refined_row_id",
                                "int64", {3}, {10, 12, 13})));
  CHECK((writeArray<uint16_t, 1>(root, source + "reason_codes", "uint16", {3},
                                 {0, 0, 0})));
  CHECK((writeArray<int64_t, 1>(root, source + "frame_row_offsets", "int64",
                                {5}, {0, 1, 1, 2, 3})));

  json root_metadata = {{"zarr_format", 3},
                        {"node_type", "group"},
                        {"attributes", {}},
                        {"consolidated_metadata",
                         {{"kind", "inline"},
                          {"must_understand", false},
                          {"metadata", json::object()}}}};
  auto &consolidated = root_metadata["consolidated_metadata"]["metadata"];
  for (const std::string path :
       {std::string("refined_detect_runs"), base, base + "/instances",
        base + "/source_detections"}) {
    const auto metadata = readJson(root / path / "zarr.json");
    CHECK(metadata.has_value());
    consolidated[path] = *metadata;
    if (path == base || path == base + "/instances" ||
        path == base + "/source_detections") {
      consolidated[path]["consolidated_metadata"] = {
          {"kind", "inline"},
          {"must_understand", false},
          {"metadata", json::object()},
      };
    }
  }
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(root / base)) {
    if (!entry.is_regular_file() || entry.path().filename() != "zarr.json") {
      continue;
    }
    const auto relative =
        std::filesystem::relative(entry.path().parent_path(), root)
            .generic_string();
    if (consolidated.contains(relative)) {
      continue;
    }
    const auto metadata = readJson(entry.path());
    CHECK(metadata.has_value());
    consolidated[relative] = *metadata;
  }
  return writeJson(root / "zarr.json", root_metadata);
}

bool testZarrMetadataEquivalence() {
  const json group = {
      {"zarr_format", 3}, {"node_type", "group"}, {"attributes", {}}};
  json null_group = group;
  null_group["consolidated_metadata"] = nullptr;
  json empty_group = group;
  empty_group["consolidated_metadata"] = {
      {"kind", "inline"},
      {"must_understand", false},
      {"metadata", json::object()},
  };
  CHECK(crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      group, null_group));
  CHECK(crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      null_group, empty_group));

  json nonempty = empty_group;
  nonempty["consolidated_metadata"]["metadata"]["child"] = group;
  CHECK(!crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      null_group, nonempty));
  json wrong_kind = empty_group;
  wrong_kind["consolidated_metadata"]["kind"] = "external";
  CHECK(!crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      null_group, wrong_kind));
  json malformed = empty_group;
  malformed["consolidated_metadata"].erase("must_understand");
  CHECK(!crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      null_group, malformed));
  json changed_group = empty_group;
  changed_group["attributes"]["changed"] = true;
  CHECK(!crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      null_group, changed_group));

  const json array = {{"zarr_format", 3},
                      {"node_type", "array"},
                      {"data_type", "int32"},
                      {"shape", {1}}};
  CHECK(crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      array, array));
  json array_with_envelope = array;
  array_with_envelope["consolidated_metadata"] = nullptr;
  CHECK(!crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      array_with_envelope, array_with_envelope));
  CHECK(!crimson::zarr::internal::EquivalentDirectAndConsolidatedZarrNode(
      array, array_with_envelope));
  return true;
}

bool testRefinedRepositoryAndSelection() {
  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto archive_root = temporary.path() / "analysis.zarr";
  std::filesystem::create_directories(archive_root);
  CHECK(buildRefinedFixture(archive_root));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(archive_root, &error);
  CHECK(archive != nullptr);

  crimson::zarr::RefinedDetectionRepositoryOpenMetrics open_metrics;
  auto repository = crimson::zarr::OpenRefinedDetectionRepository(
      archive, "refined_fixture", {}, &error, &open_metrics);
  CHECK(repository != nullptr);
  CHECK(error.empty());
  CHECK(open_metrics.consolidated_array_declarations == 28);
  CHECK(open_metrics.direct_group_metadata_reads == 3);
  CHECK(open_metrics.exact_handle_opens == 11);
  CHECK(open_metrics.source_audit_handle_opens == 0);
  CHECK(open_metrics.offset_read_calls == 1);
  const auto &descriptor = repository->descriptor();
  CHECK(descriptor.surface_kind ==
        crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1);
  CHECK(descriptor.stable_identity);
  CHECK(descriptor.source_audit_lazy);
  CHECK(!descriptor.authority_approved);

  auto page = repository->resolveCameraFrameRange(0, 3);
  CHECK(page.status == crimson::zarr::CanonicalDetectionPageStatus::Ready);
  CHECK(page.frames[0].detections.size() == 2);
  CHECK(page.frames[1].detections.empty());
  CHECK(page.frames[2].detections.size() == 1);
  CHECK(page.frames[3].detections.size() == 3);
  CHECK(page.frames[0].detections[0].instance_key == 101);
  CHECK(page.frames[0].detections[1].instance_key == 501);
  CHECK(page.frames[0].detections[1].refined_row_id == 11);
  CHECK(page.frames[0].detections[1].source_detect_row_index == -1);
  CHECK(page.frames[0].detections[1].source_kind_code == 3);
  CHECK(!page.frames[0].detections[1].score_valid);
  CHECK(page.frames[0].detections[1].manual_edit);
  const auto input = crimson::zarr::makeCanonicalDetectionOverlaySceneInput(
      descriptor, page.frames[0], 0, 0, 0, 100, 80);
  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(scene.primitives.size() == 2);
  CHECK(scene.primitives[1].instance_key == 501);
  CHECK(scene.primitives[1].refined_row_id == 11);
  CHECK(scene.primitives[1].source_kind_code == 3);
  CHECK(page.frames[3].detections[0].source_kind_code == 1);
  CHECK(page.frames[3].detections[1].source_kind_code == 3);
  CHECK(page.frames[3].detections[2].source_kind_code == 3);
  CHECK(page.frames[3].detections[0].class_id ==
        page.frames[3].detections[1].class_id);

  auto chunks = repository->planUiResidency(1024);
  CHECK(!chunks.empty());
  auto rows = repository->readUiRowsForResidency(0, 6);
  CHECK(rows.ready());
  auto resident =
      std::make_shared<crimson::zarr::CanonicalDetectionResidentUiColumns>();
  resident->bbox_norm_coords = std::move(rows.bbox_norm_coords);
  resident->scores = std::move(rows.scores);
  resident->class_ids = std::move(rows.class_ids);
  resident->instance_keys = std::move(rows.instance_keys);
  resident->refined_row_ids = std::move(rows.refined_row_ids);
  resident->source_detect_row_indices =
      std::move(rows.source_detect_row_indices);
  resident->source_kind_codes = std::move(rows.source_kind_codes);
  resident->score_valid = std::move(rows.score_valid);
  resident->manual_edit_flags = std::move(rows.manual_edit_flags);
  CHECK(repository->publishResidentUiColumns(resident, &error));
  const auto before = repository->metrics().ui_field_reads;
  page = repository->resolveCameraFrameRange(0, 3);
  CHECK(page.status == crimson::zarr::CanonicalDetectionPageStatus::Ready);
  CHECK(repository->metrics().ui_field_reads == before);
  CHECK(page.frames[0].detections[1].instance_key == 501);
  CHECK(page.frames[3].detections.size() == 3);
  CHECK(page.frames[3].detections[2].instance_key == 503);

  crimson::zarr::DetectionRepositorySelectionRequest request;
  request.explicit_refined_run = "refined_fixture";
  crimson::zarr::DetectionRepositorySelectionMetrics selection_metrics;
  auto selected = crimson::zarr::OpenSelectedDetectionRepository(
      archive, request, &error, &selection_metrics);
  CHECK(selected != nullptr);
  CHECK(selection_metrics.kind ==
        crimson::zarr::DetectionRepositorySelectionKind::ExplicitRefinedV1);
  CHECK(!selected->descriptor().authority_approved);

  request = {};
  selected = crimson::zarr::OpenSelectedDetectionRepository(
      archive, request, &error, &selection_metrics);
  CHECK(selected != nullptr);
  CHECK(selection_metrics.kind ==
        crimson::zarr::DetectionRepositorySelectionKind::
            ApprovedAuthoritativeRefinedV1);
  CHECK(selected->descriptor().authority_approved);
  return true;
}

bool testRefinedFailClosedValidation() {
  std::string error;
  crimson::zarr::RefinedDetectionManifestSummary manifest_summary;
  auto manifest = refinedRunManifest("refined_fixture", true);
  manifest["payload"]["logical_schema"]["dimensions"]["n_frames"] = 0;
  manifest["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(manifest["payload"]);
  CHECK(!crimson::zarr::ValidateRefinedDetectionRunManifest(
      manifest, "refined_fixture", false, &manifest_summary, &error));

  error.clear();
  manifest = refinedRunManifest("refined_fixture", true);
  manifest["payload"]["reason_registries"]["instances"]["codes"]["0"] =
      "tampered";
  manifest["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(manifest["payload"]);
  CHECK(!crimson::zarr::ValidateRefinedDetectionRunManifest(
      manifest, "refined_fixture", false, &manifest_summary, &error));

  error.clear();
  auto authority = refinedAuthority("refined_fixture", std::string(64, 'a'));
  authority["payload"]["intended_use"] = "unknown";
  authority["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(authority["payload"]);
  CHECK(!crimson::zarr::ValidateRefinedDetectionAuthority(
      "refined_fixture", authority, nullptr, &error));

  {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "ineligible.zarr";
    std::filesystem::create_directories(root);
    CHECK(buildRefinedFixture(root, false));
    auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
    CHECK(archive != nullptr);
    CHECK(crimson::zarr::OpenRefinedDetectionRepository(
              archive, "refined_fixture", {}, &error) == nullptr);
    crimson::zarr::RefinedDetectionRepositoryOpenOptions benchmark;
    benchmark.allow_selector_ineligible = true;
    error.clear();
    CHECK(crimson::zarr::OpenRefinedDetectionRepository(
              archive, "refined_fixture", benchmark, &error) != nullptr);
  }

  {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "bad_offsets.zarr";
    std::filesystem::create_directories(root);
    CHECK(buildRefinedFixture(root, true, {0, 1, 1, 3, 6}));
    auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
    CHECK(archive != nullptr);
    auto repository = crimson::zarr::OpenRefinedDetectionRepository(
        archive, "refined_fixture", {}, &error);
    CHECK(repository != nullptr);
    CHECK(repository->resolveCameraFrameRange(0, 3).status ==
          crimson::zarr::CanonicalDetectionPageStatus::ReadFailed);
  }

  {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "duplicate_keys.zarr";
    std::filesystem::create_directories(root);
    CHECK(buildRefinedFixture(root, true, {0, 2, 2, 3, 6},
                              {101, 101, 303, 404, 502, 503}));
    auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
    CHECK(archive != nullptr);
    auto repository = crimson::zarr::OpenRefinedDetectionRepository(
        archive, "refined_fixture", {}, &error);
    CHECK(repository != nullptr);
    CHECK(repository->resolveCameraFrameRange(0, 3).status ==
          crimson::zarr::CanonicalDetectionPageStatus::ReadFailed);
  }
  return true;
}

bool testRepositoryAndOverlay() {
  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto archive_root = temporary.path() / "analysis.zarr";
  std::filesystem::create_directories(archive_root);
  CHECK(buildFixture(archive_root));

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(archive_root, &error);
  CHECK(archive != nullptr);
  CHECK(crimson::zarr::OpenCanonicalDetectionRepository(archive, "missing_run",
                                                        &error) == nullptr);
  CHECK(!error.empty());

  crimson::zarr::DetectionRepositorySelectionRequest selection;
  selection.explicit_refined_run = "missing_refined";
  selection.canonical_raw_run = "fixture_run";
  selection.raw_fallback_policy = crimson::zarr::DetectionRawFallbackPolicy::
      AllowOnlyWhenNoRefinedAuthority;
  error.clear();
  CHECK(crimson::zarr::OpenSelectedDetectionRepository(archive, selection,
                                                       &error) == nullptr);
  CHECK(!error.empty());
  selection.explicit_refined_run.clear();
  error.clear();
  auto raw_selected = crimson::zarr::OpenSelectedDetectionRepository(
      archive, selection, &error);
  CHECK(raw_selected != nullptr);
  CHECK(raw_selected->descriptor().surface_kind ==
        crimson::zarr::DetectionSurfaceKind::CanonicalRawV1);

  error.clear();
  crimson::zarr::CanonicalDetectionRepositoryOpenMetrics open_metrics;
  auto repository = crimson::zarr::OpenCanonicalDetectionRepository(
      archive, "fixture_run", &error, &open_metrics);
  CHECK(repository != nullptr);
  CHECK(error.empty());
  CHECK(open_metrics.root_metadata_reads == 1);
  CHECK(open_metrics.direct_group_metadata_reads == 2);
  CHECK(open_metrics.consolidated_array_declarations == 9);
  CHECK(open_metrics.exact_handle_opens == 4);
  CHECK(open_metrics.fallback_metadata_reads == 0);
  CHECK(open_metrics.fallback_dtype_opens == 0);
  CHECK(open_metrics.offset_read_calls == 1);
  CHECK(open_metrics.retained_offset_bytes == 5 * sizeof(int64_t));

  const auto &descriptor = repository->descriptor();
  CHECK(descriptor.source_group == "detect_runs");
  CHECK(descriptor.run_name == "fixture_run");
  CHECK(descriptor.row_count == 3);
  CHECK(descriptor.camera_frame_count == 4);
  CHECK(descriptor.source_width == 100);
  CHECK(descriptor.source_height == 80);
  CHECK(descriptor.ready());

  const auto page = repository->resolveCameraFrameRange(0, 3);
  CHECK(page.status == crimson::zarr::CanonicalDetectionPageStatus::Ready);
  CHECK(page.frames.size() == 4);
  CHECK(page.frames[0].detections.empty());
  CHECK(page.frames[1].detections.size() == 1);
  CHECK(page.frames[2].detections.size() == 2);
  CHECK(page.frames[3].detections.empty());
  CHECK(page.frames[2].detections[0].class_id == 5);
  CHECK(std::abs(page.frames[2].detections[1].score - 0.7f) < 1e-6f);
  CHECK(repository->resolveCameraFrameRange(-1, 0).status ==
        crimson::zarr::CanonicalDetectionPageStatus::OutOfRange);

  const auto input = crimson::zarr::makeCanonicalDetectionOverlaySceneInput(
      descriptor, page.frames[1], 0, 1, 0, 100, 80);
  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(scene.ready());
  CHECK(scene.count(crimson::overlay::CameraOverlayLayer::BoundingBoxes) == 1);
  CHECK(scene.primitives.front().points.size() == 5);
  CHECK(std::abs(scene.primitives.front().points[0].x - 15.0) < 1e-6);
  CHECK(std::abs(scene.primitives.front().points[0].y - 12.0) < 1e-6);
  CHECK(crimson::overlay::buildReadOnlyOverlayScene(
            crimson::zarr::makeCanonicalDetectionOverlaySceneInput(
                descriptor, page.frames[1], 0, 2, 0, 100, 80))
            .ready() == false);
  return true;
}

bool testPageBuffer() {
  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto archive_root = temporary.path() / "analysis.zarr";
  std::filesystem::create_directories(archive_root);
  CHECK(buildFixture(archive_root));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(archive_root, &error);
  CHECK(archive != nullptr);
  auto repository = crimson::zarr::OpenCanonicalDetectionRepository(
      archive, "fixture_run", &error);
  CHECK(repository != nullptr);

  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1);
  CanonicalDetectionBuffer buffer(scheduler, archive_root.string());
  CHECK(buffer.open(std::move(repository), 2, 2, &error));

  CanonicalDetectionResidencyPolicy ineligible;
  ineligible.maximum_resident_bytes = 1;
  ineligible.maximum_chunk_decoded_bytes = 24;
  CHECK(buffer.startUiResidency(ineligible, &error));
  CHECK(buffer.residencyMetrics().state ==
        CanonicalDetectionResidencyState::Ineligible);

  CHECK(buffer.requestFrame(1, true, &error));
  CHECK(buffer.waitForFrame(1, std::chrono::seconds(2)));
  const auto frame_one = buffer.frame(1);
  CHECK(frame_one != nullptr);
  CHECK(frame_one->detections.size() == 1);
  CHECK(buffer.requestFrame(1, false, &error));
  CHECK(buffer.requestFrame(3, true, &error));
  CHECK(buffer.waitForFrame(3, std::chrono::seconds(2)));
  CHECK(buffer.frame(3) != nullptr);
  scheduler->waitUntilIdle();
  const auto metrics = buffer.metrics();
  const auto repository_metrics = buffer.repositoryMetrics();
  CHECK(metrics.requests == 3);
  CHECK(metrics.cache_hits >= 1);
  CHECK(metrics.demand_pages >= 2);
  CHECK(metrics.lead_pages >= 1);
  CHECK(metrics.resolved_pages >= 2);
  CHECK(metrics.failed_pages == 0);
  CHECK(metrics.peak_cached_pages <= 2);
  CHECK(repository_metrics.range_reads >= 2);
  CHECK(repository_metrics.failed_reads == 0);
  CHECK(repository_metrics.ui_field_reads ==
        repository_metrics.range_reads * 3);

  const auto resident = canonicalDetectionProductionResidencyPolicy();
  CHECK(resident.enabled());
  CHECK(resident.maximum_resident_bytes == 64ULL * 1024ULL * 1024ULL);
  CHECK(resident.maximum_chunk_decoded_bytes == 512ULL * 1024ULL);
  CHECK(resident.admits(resident.maximum_resident_bytes));
  CHECK(!resident.admits(resident.maximum_resident_bytes + 1));
  CHECK(buffer.startUiResidency(resident, &error));
  CHECK(buffer.waitForUiResidency(std::chrono::seconds(2)));
  const auto residency = buffer.residencyMetrics();
  CHECK(residency.state == CanonicalDetectionResidencyState::Ready);
  CHECK(residency.decoded_hot_bytes == 3 * 24);
  CHECK(residency.decoded_source_bytes == 3 * 24);
  CHECK(residency.retained_bytes == 3 * 24);
  CHECK(residency.completed_chunks == residency.planned_chunks);
  CHECK(residency.publications == 1);
  CHECK(std::string(canonicalDetectionResidencyStateName(residency.state)) ==
        "ready");

  const auto before_resident_request = buffer.repositoryMetrics();
  CHECK(buffer.requestFrame(2, true, &error));
  CHECK(buffer.waitForFrame(2, std::chrono::seconds(2)));
  scheduler->waitUntilIdle();
  const auto resident_frame = buffer.frame(2);
  CHECK(resident_frame != nullptr);
  CHECK(resident_frame->detections.size() == 2);
  CHECK(resident_frame->detections[0].class_id == 5);
  const auto after_resident_request = buffer.repositoryMetrics();
  CHECK(after_resident_request.ui_field_reads ==
        before_resident_request.ui_field_reads);
  CHECK(after_resident_request.resident_range_reads >
        before_resident_request.resident_range_reads);
  CHECK(after_resident_request.resident_publications == 1);
  CHECK(after_resident_request.resident_retained_bytes == 3 * 24);
  buffer.close();
  scheduler->shutdown();
  return true;
}

bool testDemandOvertakesResidencyAndCancellation() {
  {
    auto scheduler =
        std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
    auto repository = std::make_unique<BlockingCanonicalDetectionRepository>();
    auto *repository_view = repository.get();
    CanonicalDetectionBuffer buffer(scheduler, "blocking-demand");
    std::string error;
    CHECK(buffer.open(std::move(repository), 2, 2, &error));
    CanonicalDetectionResidencyPolicy policy;
    policy.maximum_resident_bytes = 1024;
    policy.maximum_chunk_decoded_bytes = 1024;
    CHECK(buffer.startUiResidency(policy, &error));
    CHECK(repository_view->waitForPreloadStart(std::chrono::seconds(1)));

    const auto demand_started = std::chrono::steady_clock::now();
    CHECK(buffer.requestFrame(2, true, &error));
    CHECK(buffer.waitForFrame(2, std::chrono::milliseconds(200)));
    const auto demand_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - demand_started)
            .count();
    CHECK(demand_ms < 200.0);
    CHECK(buffer.frame(2) != nullptr);

    repository_view->releasePreload();
    CHECK(buffer.waitForUiResidency(std::chrono::seconds(1)));
    CHECK(buffer.residencyMetrics().state ==
          CanonicalDetectionResidencyState::Ready);
    CHECK(repository_view->residentUiColumnsReady());
    buffer.close();
    scheduler->shutdown();
  }

  {
    auto scheduler =
        std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
    auto repository = std::make_unique<BlockingCanonicalDetectionRepository>();
    auto *repository_view = repository.get();
    CanonicalDetectionBuffer buffer(scheduler, "blocking-cancel");
    std::string error;
    CHECK(buffer.open(std::move(repository), 2, 2, &error));
    CanonicalDetectionResidencyPolicy policy;
    policy.maximum_resident_bytes = 1024;
    policy.maximum_chunk_decoded_bytes = 1024;
    CHECK(buffer.startUiResidency(policy, &error));
    CHECK(repository_view->waitForPreloadStart(std::chrono::seconds(1)));

    std::thread cancel([&] { buffer.cancelUiResidency(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    repository_view->releasePreload();
    cancel.join();
    const auto cancelled = buffer.residencyMetrics();
    CHECK(cancelled.state == CanonicalDetectionResidencyState::Cancelled);
    CHECK(cancelled.publications == 0);
    CHECK(cancelled.stale_chunks >= 1);
    CHECK(!repository_view->residentUiColumnsReady());
    buffer.close();
    scheduler->shutdown();
  }
  {
    auto scheduler =
        std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
    auto repository = std::make_unique<BlockingCanonicalDetectionRepository>();
    auto *repository_view = repository.get();
    CanonicalDetectionBuffer buffer(scheduler, "blocking-close");
    std::string error;
    CHECK(buffer.open(std::move(repository), 2, 2, &error));
    const auto policy = canonicalDetectionProductionResidencyPolicy();
    CHECK(buffer.startUiResidency(policy, &error));
    CHECK(repository_view->waitForPreloadStart(std::chrono::seconds(1)));

    std::thread closer([&] { buffer.close(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    repository_view->releasePreload();
    closer.join();
    const auto cancelled = buffer.residencyMetrics();
    CHECK(cancelled.state == CanonicalDetectionResidencyState::Cancelled);
    CHECK(cancelled.elapsed_ms > 0.0);
    CHECK(cancelled.publications == 0);
    scheduler->shutdown();
  }
  return true;
}

} // namespace

int main() {
  if (!testZarrMetadataEquivalence() || !testRepositoryAndOverlay() ||
      !testRefinedRepositoryAndSelection() ||
      !testRefinedFailClosedValidation() || !testPageBuffer() ||
      !testDemandOvertakesResidencyAndCancellation()) {
    return 1;
  }
  std::cout << "canonical_detection_repository_tests: PASS\n";
  return 0;
}
