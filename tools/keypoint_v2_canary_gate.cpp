#include "zarr/archive_context.h"
#include "zarr/tensorstore_keypoint_v2_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace ts = tensorstore;

constexpr char kRawRun[] = "raw_keypoints_crop_v2_yolo_v2";
constexpr char kQualityRun[] = "keypoint_quality_crop_v2_v1";
constexpr char kRawBodyRun[] = "body_frame_crop_v2_keypoints_v1";
constexpr char kRefinedRun[] = "refined_keypoints_crop_v2_synthetic_canary_v2";
constexpr char kRefinedBodyRun[] = "body_frame_refined_keypoints_canary_v1";
constexpr char kRawDigest[] =
    "227f0c80065a38d77604b0638bb16a22cd513b383609d364b4481a4fb0cf8db6";
constexpr char kQualityDigest[] =
    "3d0af6dab6ca0ddc478c80755c040c2af2381e00166ee8a4cab7f8d9cb920e81";
constexpr char kRawBodyDigest[] =
    "a8b12539669174bf20ebaf181b0c341148903588fc5ae27af46f94e24a2ab1af";
constexpr char kRefinedDigest[] =
    "2f594135b69987c62591d9caeb2d12430a9bc5b2f639cbb42388d2a746728dc0";
constexpr char kRefinedBodyDigest[] =
    "857f7c6bd912f19a3b11bdfe8e3fb9bb68ba563aa9f0da6635947b9f324ff16b";

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double elapsedMilliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

std::shared_ptr<crimson::zarr::ArchiveContext>
openArchive(const std::filesystem::path &path) {
  std::string error;
  auto result = crimson::zarr::ArchiveContext::Open(path, &error);
  require(result != nullptr,
          "Archive open failed for " + path.string() + ": " + error);
  return result;
}

json metricsJson(const crimson::zarr::KeypointV2RepositoryOpenMetrics &value) {
  return {{"total_ms", value.total_ms},
          {"metadata_ms", value.metadata_ms},
          {"exact_handle_open_ms", value.exact_handle_open_ms},
          {"identity_validation_ms", value.identity_validation_ms},
          {"root_metadata_reads", value.root_metadata_reads},
          {"direct_metadata_reads", value.direct_metadata_reads},
          {"consolidated_array_declarations",
           value.consolidated_array_declarations},
          {"exact_handle_opens", value.exact_handle_opens},
          {"fallback_metadata_reads", value.fallback_metadata_reads},
          {"fallback_dtype_opens", value.fallback_dtype_opens},
          {"raw_offset_read_calls", value.raw_offset_read_calls},
          {"selected_offset_read_calls", value.selected_offset_read_calls},
          {"quality_offset_read_calls", value.quality_offset_read_calls},
          {"body_frame_offset_read_calls", value.body_frame_offset_read_calls},
          {"quality_payload_reads", value.quality_payload_reads},
          {"retained_offset_bytes", value.retained_offset_bytes}};
}

json accessJson(const crimson::zarr::KeypointV2RepositoryAccessMetrics &value) {
  return {{"frame_requests", value.frame_requests},
          {"rows_resolved", value.rows_resolved},
          {"payload_read_calls", value.payload_read_calls},
          {"payload_read_batches", value.payload_read_batches},
          {"maximum_columns_per_batch", value.maximum_columns_per_batch},
          {"quality_payload_read_calls", value.quality_payload_read_calls},
          {"read_failures", value.read_failures}};
}

template <typename T>
std::vector<T> readRankOne(const std::filesystem::path &archive,
                           const std::string &path) {
  const json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", archive.string() + "/"}}},
      {"path", path}};
  auto store = ts::Open<T, 1>(spec, ts::OpenMode::open, ts::ReadWriteMode::read)
                   .result();
  require(store.ok(), "Could not open audit array " + path + ": " +
                          store.status().ToString());
  auto read = ts::Read(*store).result();
  require(read.ok(), "Could not read audit array " + path + ": " +
                         read.status().ToString());
  std::vector<T> result(static_cast<size_t>(read->shape()[0]));
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t index = 0; index < result.size(); ++index) {
    result[index] = *reinterpret_cast<const T *>(
        origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
  }
  return result;
}

json validateRefinedCases(const std::filesystem::path &refined_root,
                          crimson::zarr::KeypointV2Repository &repository) {
  const std::string base = std::string("refined_keypoints_runs/") + kRefinedRun;
  const auto keys = readRankOne<uint64_t>(refined_root / "refined.zarr",
                                          base + "/instance_key");
  const auto frames = readRankOne<int64_t>(refined_root / "refined.zarr",
                                           base + "/frame_indices");
  require(keys.size() == frames.size(), "Refined audit arrays disagree");
  struct Expected {
    uint64_t key;
    uint8_t review;
    uint16_t reason;
    bool source_success;
    bool refined_success;
    size_t edited_points;
  };
  constexpr Expected expected[] = {
      {5247891125059518930ULL, 1, 1, true, true, 1},
      {11017509320008049993ULL, 2, 2, true, false, 3},
      {14806374596985206755ULL, 1, 3, false, true, 3}};
  json results = json::array();
  for (const auto &item : expected) {
    const auto found = std::find(keys.begin(), keys.end(), item.key);
    require(found != keys.end(), "Refined decision key is absent");
    const size_t row = static_cast<size_t>(found - keys.begin());
    const auto resolved =
        repository.resolveCameraFrame(frames[row], 4512, 4512);
    require(resolved.status == crimson::zarr::KeypointOverlayStatus::Mapped,
            "Refined decision frame did not resolve");
    const auto detection =
        std::find_if(resolved.detections.begin(), resolved.detections.end(),
                     [&](const auto &candidate) {
                       return candidate.instance_key == item.key;
                     });
    require(detection != resolved.detections.end(),
            "Refined decision observation was dropped");
    const size_t edited = static_cast<size_t>(
        std::count(detection->keypoint_edit_flags.begin(),
                   detection->keypoint_edit_flags.end(), uint8_t{1}));
    const bool matches = detection->review_state_code == item.review &&
                         detection->reason_code == item.reason &&
                         detection->source_success == item.source_success &&
                         detection->refined_success == item.refined_success &&
                         edited == item.edited_points;
    require(matches, "Refined decision state changed during presentation key=" +
                         std::to_string(item.key) + " observed=" +
                         std::to_string(detection->review_state_code) + "/" +
                         std::to_string(detection->reason_code) + "/" +
                         std::to_string(detection->source_success) + "/" +
                         std::to_string(detection->refined_success) + "/" +
                         std::to_string(edited) +
                         " expected=" + std::to_string(item.review) + "/" +
                         std::to_string(item.reason) + "/" +
                         std::to_string(item.source_success) + "/" +
                         std::to_string(item.refined_success) + "/" +
                         std::to_string(item.edited_points));
    results.push_back({{"instance_key", item.key},
                       {"frame", frames[row]},
                       {"review_state_code", detection->review_state_code},
                       {"reason_code", detection->reason_code},
                       {"source_success", detection->source_success},
                       {"refined_success", detection->refined_success},
                       {"edited_points", edited}});
  }
  return results;
}

json exercise(const std::string &label,
              crimson::zarr::KeypointV2Repository &repository,
              const crimson::zarr::KeypointV2RepositoryOpenMetrics &open) {
  const auto &descriptor = repository.v2Descriptor();
  require(descriptor.consolidated_metadata && descriptor.stable_identity &&
              descriptor.quality_payload_lazy,
          label + " repository did not establish strict readiness");
  require(descriptor.raw_offset_read_calls == 1 &&
              descriptor.selected_offset_read_calls ==
                  (descriptor.refined ? 1U : 0U) &&
              descriptor.quality_offset_read_calls == 0 &&
              descriptor.body_frame_offset_read_calls == 1,
          label + " repository offset-read policy is invalid");
  require(open.fallback_metadata_reads == 0 && open.fallback_dtype_opens == 0 &&
              open.quality_payload_reads == 0,
          label + " repository used a compatibility fallback");

  const auto started = Clock::now();
  size_t mapped = 0;
  size_t missing = 0;
  size_t observations = 0;
  const size_t frames = descriptor.selected.frame_count;
  for (size_t index = 0; index < 128; ++index) {
    const int64_t frame = static_cast<int64_t>(
        (index * 104729ULL + index * index * 31ULL) % frames);
    const auto resolved = repository.resolveCameraFrame(
        frame, static_cast<int>(descriptor.selected.source_width),
        static_cast<int>(descriptor.selected.source_height));
    if (resolved.status == crimson::zarr::KeypointOverlayStatus::Mapped) {
      ++mapped;
      observations += resolved.detections.size();
      for (const auto &detection : resolved.detections) {
        require(detection.instance_key != 0,
                label + " dropped stable observation identity");
        require(detection.heading_from_body_frame,
                label + " synthesized an embedded keypoint heading");
      }
    } else {
      require(resolved.status == crimson::zarr::KeypointOverlayStatus::Missing,
              label + " representative frame read failed: " + resolved.error);
      ++missing;
    }
  }
  const double frame_reads_ms = elapsedMilliseconds(started);
  const auto before_quality = repository.accessMetrics();
  require(before_quality.quality_payload_read_calls == 0,
          label + " read quality payloads during ordinary presentation");
  std::string error;
  const auto quality_started = Clock::now();
  require(repository.validateQualityPayloadBindings(&error),
          label + " quality audit failed: " + error);
  const double quality_validation_ms = elapsedMilliseconds(quality_started);
  const auto after_quality = repository.accessMetrics();
  require(after_quality.quality_payload_read_calls == 5 &&
              repository.v2Descriptor().quality_offset_read_calls == 1,
          label + " quality audit read an unexpected payload set");
  return {{"label", label},
          {"refined", descriptor.refined},
          {"frames", frames},
          {"rows", descriptor.selected.row_count},
          {"keypoints", descriptor.selected.keypoint_count},
          {"selected_run", descriptor.selected.run_id},
          {"selected_manifest_digest", descriptor.selected.manifest_digest},
          {"mapped_samples", mapped},
          {"missing_samples", missing},
          {"observations", observations},
          {"frame_reads_ms", frame_reads_ms},
          {"quality_validation_ms", quality_validation_ms},
          {"open", metricsJson(open)},
          {"ordinary_access", accessJson(before_quality)},
          {"post_quality_audit_access", accessJson(after_quality)}};
}

} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 3 || argc == 4,
            "Usage: keypoint_v2_canary_gate RAW_PACKAGE REFINED_PACKAGE "
            "[OUTPUT_JSON]");
    const std::filesystem::path raw_root = argv[1];
    const std::filesystem::path refined_root = argv[2];
    auto raw = openArchive(raw_root / "raw.zarr");
    auto quality = openArchive(raw_root / "quality.zarr");

    crimson::zarr::KeypointV2RepositoryOpenRequest raw_request;
    raw_request.raw_archive = raw;
    raw_request.raw_run = kRawRun;
    raw_request.quality_archive = quality;
    raw_request.quality_run = kQualityRun;
    raw_request.body_frame_archive = openArchive(raw_root / "body_frame.zarr");
    raw_request.body_frame_run = kRawBodyRun;
    raw_request.expected_raw_manifest_digest = kRawDigest;
    raw_request.expected_quality_manifest_digest = kQualityDigest;
    raw_request.expected_body_frame_manifest_digest = kRawBodyDigest;
    raw_request.allow_selector_ineligible = true;
    raw_request.deep_validate_identity = true;
    std::string error;
    crimson::zarr::KeypointV2RepositoryOpenMetrics raw_metrics;
    auto raw_repository = crimson::zarr::OpenKeypointV2Repository(
        raw_request, &error, &raw_metrics);
    require(raw_repository != nullptr, "Raw repository open failed: " + error);

    crimson::zarr::KeypointV2RepositoryOpenRequest refined_request =
        raw_request;
    refined_request.refined_archive =
        openArchive(refined_root / "refined.zarr");
    refined_request.refined_run = kRefinedRun;
    refined_request.body_frame_archive =
        openArchive(refined_root / "body_frame.zarr");
    refined_request.body_frame_run = kRefinedBodyRun;
    refined_request.expected_refined_manifest_digest = kRefinedDigest;
    refined_request.expected_body_frame_manifest_digest = kRefinedBodyDigest;
    crimson::zarr::KeypointV2RepositoryOpenMetrics refined_metrics;
    error.clear();
    auto refined_repository = crimson::zarr::OpenKeypointV2Repository(
        refined_request, &error, &refined_metrics);
    require(refined_repository != nullptr,
            "Refined repository open failed: " + error);

    const json refined_cases =
        validateRefinedCases(refined_root, *refined_repository);
    json evidence = {
        {"schema_id", "crimson.keypoint_v2.canary_gate"},
        {"schema_version", 1},
        {"status", "pass"},
        {"raw_handoff_sha256",
         "cd33ac60e2f72f614a0ea5f2583d08229b9dee22d2ddb2692a56a284f4f2d8c2"},
        {"refined_handoff_sha256",
         "d1c0e27303b715c95c645f077406906f691be2f9d86a74307425bb55465606b1"},
        {"raw", exercise("raw", *raw_repository, raw_metrics)},
        {"refined", exercise("refined", *refined_repository, refined_metrics)},
        {"refined_cases", refined_cases}};
    if (argc == 4) {
      std::ofstream output(argv[3]);
      require(output.good(), "Could not open evidence output");
      output << evidence.dump(2) << '\n';
      require(output.good(), "Could not write evidence output");
    }
    std::cout << evidence.dump(2) << '\n';
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "[KeypointV2Canary] FAIL " << exception.what() << '\n';
    return 1;
  }
}
