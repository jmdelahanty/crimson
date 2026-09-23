#include "canonical_detection_buffer.h"
#include "read_only_overlay_scene.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_detection_overlay_scene_adapter.h"
#include "zarr/detection_repository_selection.h"
#include "zarr/refined_detection_contract.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif

#ifndef CRIMSON_WORKTREE_DIRTY
#define CRIMSON_WORKTREE_DIRTY 1
#endif

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr size_t kPageFrames = 70;
constexpr size_t kCachePages = 8;
constexpr uint64_t kResidentBudgetBytes = 64ULL * 1024 * 1024;
constexpr uint64_t kResidencyChunkBytes = 256ULL * 1024;

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double elapsedMilliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

json readJson(const std::filesystem::path &path) {
  std::ifstream input(path);
  require(input.good(), "Could not open JSON document: " + path.string());
  try {
    return json::parse(input);
  } catch (const json::exception &exception) {
    throw std::runtime_error("Could not parse " + path.string() + ": " +
                             exception.what());
  }
}

void writeJson(const std::filesystem::path &path, const json &value) {
  std::ofstream output(path);
  require(output.good(), "Could not open evidence output: " + path.string());
  output << value.dump(2) << '\n';
  require(output.good(), "Could not write evidence output: " + path.string());
}

void hashBytes(uint64_t *hash, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < size; ++index) {
    *hash ^= bytes[index];
    *hash *= 1099511628211ULL;
  }
}

void hashDetection(uint64_t *hash,
                   const crimson::zarr::CanonicalDetection &detection) {
  hashBytes(hash, &detection.row_index, sizeof(detection.row_index));
  hashBytes(hash, &detection.instance_key, sizeof(detection.instance_key));
  hashBytes(hash, &detection.refined_row_id, sizeof(detection.refined_row_id));
  hashBytes(hash, &detection.source_detect_row_index,
            sizeof(detection.source_detect_row_index));
  hashBytes(hash, &detection.source_kind_code,
            sizeof(detection.source_kind_code));
  hashBytes(hash, &detection.score_valid, sizeof(detection.score_valid));
  hashBytes(hash, &detection.manual_edit, sizeof(detection.manual_edit));
  hashBytes(hash, detection.normalized_cxcywh.data(),
            detection.normalized_cxcywh.size() * sizeof(float));
  hashBytes(hash, &detection.score, sizeof(detection.score));
  hashBytes(hash, &detection.class_id, sizeof(detection.class_id));
}

std::string hexDigest(uint64_t value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
}

json openMetricsJson(
    const crimson::zarr::RefinedDetectionRepositoryOpenMetrics &metrics) {
  return {
      {"total_ms", metrics.total_ms},
      {"root_metadata_ms", metrics.root_metadata_ms},
      {"manifest_validation_ms", metrics.manifest_validation_ms},
      {"exact_handle_open_ms", metrics.exact_handle_open_ms},
      {"offset_read_ms", metrics.offset_read_ms},
      {"root_metadata_reads", metrics.root_metadata_reads},
      {"direct_run_metadata_reads", metrics.direct_run_metadata_reads},
      {"direct_group_metadata_reads", metrics.direct_group_metadata_reads},
      {"consolidated_array_declarations",
       metrics.consolidated_array_declarations},
      {"exact_handle_opens", metrics.exact_handle_opens},
      {"source_audit_handle_opens", metrics.source_audit_handle_opens},
      {"offset_read_calls", metrics.offset_read_calls},
      {"retained_offset_bytes", metrics.retained_offset_bytes},
  };
}

json repositoryMetricsJson(
    const crimson::zarr::CanonicalDetectionRepositoryMetrics &metrics) {
  return {
      {"range_reads", metrics.range_reads},
      {"paged_range_reads", metrics.paged_range_reads},
      {"resident_range_reads", metrics.resident_range_reads},
      {"resolved_frames", metrics.resolved_frames},
      {"resolved_rows", metrics.resolved_rows},
      {"failed_reads", metrics.failed_reads},
      {"ui_field_reads", metrics.ui_field_reads},
      {"residency_chunk_reads", metrics.residency_chunk_reads},
      {"residency_rows_read", metrics.residency_rows_read},
      {"residency_decoded_bytes", metrics.residency_decoded_bytes},
      {"resident_publications", metrics.resident_publications},
      {"resident_retained_bytes", metrics.resident_retained_bytes},
      {"peak_concurrent_ui_field_reads",
       metrics.peak_concurrent_ui_field_reads},
      {"maximum_range_read_ms", metrics.maximum_range_read_ms},
      {"maximum_residency_chunk_read_ms",
       metrics.maximum_residency_chunk_read_ms},
      {"last_error", metrics.last_error},
  };
}

json bufferMetricsJson(const CanonicalDetectionBufferMetrics &metrics) {
  return {
      {"requests", metrics.requests},
      {"cache_hits", metrics.cache_hits},
      {"demand_pages", metrics.demand_pages},
      {"lead_pages", metrics.lead_pages},
      {"resolved_pages", metrics.resolved_pages},
      {"failed_pages", metrics.failed_pages},
      {"discarded_pages", metrics.discarded_pages},
      {"evicted_pages", metrics.evicted_pages},
      {"peak_cached_pages", metrics.peak_cached_pages},
      {"peak_pending_pages", metrics.peak_pending_pages},
      {"peak_cached_bytes", metrics.peak_cached_bytes},
      {"maximum_resolve_ms", metrics.maximum_resolve_ms},
      {"last_error", metrics.last_error},
  };
}

json residencyMetricsJson(const CanonicalDetectionResidencyMetrics &metrics) {
  return {
      {"state", canonicalDetectionResidencyStateName(metrics.state)},
      {"attempts", metrics.attempts},
      {"decoded_hot_bytes", metrics.decoded_hot_bytes},
      {"planned_chunks", metrics.planned_chunks},
      {"completed_chunks", metrics.completed_chunks},
      {"decoded_source_bytes", metrics.decoded_source_bytes},
      {"retained_bytes", metrics.retained_bytes},
      {"stale_chunks", metrics.stale_chunks},
      {"failed_chunks", metrics.failed_chunks},
      {"publications", metrics.publications},
      {"elapsed_ms", metrics.elapsed_ms},
      {"maximum_chunk_ms", metrics.maximum_chunk_ms},
      {"last_error", metrics.last_error},
  };
}

struct Handoff {
  std::string schema_id;
  std::filesystem::path refined_archive;
  std::filesystem::path canonical_archive;
  std::string refined_run;
  std::string canonical_run;
  std::string refined_manifest_digest;
  std::string canonical_manifest_digest;
  std::string lineage_profile;
  size_t frame_count = 0;
  size_t instance_count = 0;
  size_t source_detection_count = 0;
};

Handoff validateHandoff(const json &document,
                        const std::string &expected_payload_digest) {
  const std::string schema_id = document.value("schema_id", "");
  const bool legacy_handoff =
      schema_id == "palette.refined_detection.crimson_shadow_handoff";
  const bool storage_candidate_handoff =
      schema_id == "palette.crimson.storage_candidate_handoff";
  require((legacy_handoff || storage_candidate_handoff) &&
              document.value("schema_version", 0) == 1,
          "Palette handoff schema is incompatible");
  const char *digest_algorithm_field =
      legacy_handoff ? "payload_digest_algorithm" : "digest_algorithm";
  require(document.value(digest_algorithm_field, "") ==
                  "sha256_canonical_json_v1" &&
              document.value("payload_digest", "") == expected_payload_digest,
          "Palette handoff digest declaration disagrees with the request");
  require(crimson::zarr::CanonicalJsonSha256(document.at("payload")) ==
              expected_payload_digest,
          "Palette handoff canonical payload digest is invalid");

  const auto &payload = document.at("payload");
  require(payload.value("status", "") == "complete" &&
              payload.value("benchmark_only", false) &&
              !payload.value("selector_eligible", true) &&
              !payload.value("registry_registered", true),
          "Palette handoff publication state is invalid");
  if (legacy_handoff) {
    const auto &validation = payload.at("validation");
    require(validation.value("fresh_process", false) &&
                validation.value("canonical_refined_source_equality", false) &&
                validation.value("direct_consolidated_metadata_equivalence",
                                 false) &&
                validation.value("selector_attributes_absent", false) &&
                validation.at("production_state_changes").empty(),
            "Palette handoff validation receipt is incomplete");
  } else {
    require(payload.value("classification", "") == "full_duration_fixture" &&
                payload.value("promotion_semantics", "") ==
                    "full_duration_candidate_requires_crimson_gate" &&
                payload.at("production_state_changes").empty(),
            "Palette storage-candidate state is incompatible");
  }

  const auto &artifacts = payload.at("artifacts");
  const auto &refined =
      artifacts.at(legacy_handoff ? "refined" : "refined_detection");
  const auto &canonical =
      artifacts.at(legacy_handoff ? "canonical" : "canonical_detection");
  if (storage_candidate_handoff) {
    require(refined.value("stage", "") == "refined_detection" &&
                canonical.value("stage", "") == "canonical_detection" &&
                refined.value("direct_consolidated_manifest_equal", false) &&
                canonical.value("direct_consolidated_manifest_equal", false),
            "Palette detection artifacts lack exact metadata evidence");
  }
  const auto &dimensions = refined.at("dimensions");
  Handoff result;
  result.schema_id = schema_id;
  result.refined_archive = refined.at("macos_path").get<std::string>();
  result.canonical_archive = canonical.at("macos_path").get<std::string>();
  result.refined_run = refined.at("run_id").get<std::string>();
  result.canonical_run = canonical.at("run_id").get<std::string>();
  const char *manifest_digest_field =
      legacy_handoff ? "manifest_digest" : "manifest_payload_digest";
  result.refined_manifest_digest =
      refined.at(manifest_digest_field).get<std::string>();
  result.canonical_manifest_digest =
      canonical.at(manifest_digest_field).get<std::string>();
  result.lineage_profile =
      dimensions.value("lineage_profile", "full_acquisition");
  result.frame_count = dimensions.at("n_frames").get<size_t>();
  result.instance_count = dimensions.at("n_instances").get<size_t>();
  result.source_detection_count =
      dimensions.at("n_source_detections").get<size_t>();
  require(dimensions.at("n_frame_boundaries").get<size_t>() ==
                  result.frame_count + 1 &&
              canonical.at("dimensions").at("n_frames").get<size_t>() ==
                  result.frame_count &&
              canonical.at("dimensions").at("n_instances").get<size_t>() ==
                  result.source_detection_count,
          "Palette handoff dimensions are inconsistent");
  return result;
}

void validateFailClosedSelection(
    const std::shared_ptr<crimson::zarr::ArchiveContext> &archive,
    const Handoff &handoff, json *evidence) {
  std::string ordinary_error;
  crimson::zarr::DetectionRepositorySelectionRequest ordinary;
  ordinary.explicit_refined_run = handoff.refined_run;
  auto ordinary_repository = crimson::zarr::OpenSelectedDetectionRepository(
      archive, ordinary, &ordinary_error);
  require(!ordinary_repository,
          "Selector-ineligible run opened without benchmark opt-in");
  require(ordinary_error.find("selector-ineligible") != std::string::npos,
          "Ordinary refined selection failed for an unexpected reason: " +
              ordinary_error);

  std::string implicit_error;
  crimson::zarr::DetectionRepositorySelectionRequest implicit;
  auto implicit_repository = crimson::zarr::OpenSelectedDetectionRepository(
      archive, implicit, &implicit_error);
  require(!implicit_repository,
          "Selector-ineligible run was discovered implicitly");
  (*evidence)["selection_fail_closed"] = {
      {"ordinary_explicit_error", ordinary_error},
      {"implicit_error", implicit_error},
      {"selector_ineligible_opened_without_opt_in", false},
      {"implicit_selection_succeeded", false},
  };
}

std::unique_ptr<crimson::zarr::CanonicalDetectionRepository>
openBenchmarkRefined(
    const std::shared_ptr<crimson::zarr::ArchiveContext> &archive,
    const Handoff &handoff,
    crimson::zarr::DetectionRepositorySelectionMetrics *metrics) {
  crimson::zarr::DetectionRepositorySelectionRequest request;
  request.explicit_refined_run = handoff.refined_run;
  request.allow_selector_ineligible_benchmark = true;
  std::string error;
  auto repository = crimson::zarr::OpenSelectedDetectionRepository(
      archive, request, &error, metrics);
  require(repository != nullptr,
          "Benchmark refined selection failed: " + error);
  return repository;
}

bool validateCanonicalCompanion(const Handoff &handoff, json *evidence) {
  std::string error;
  auto archive =
      crimson::zarr::ArchiveContext::Open(handoff.canonical_archive, &error);
  if (!archive) {
    (*evidence)["canonical_companion"] = {
        {"compatible", false},
        {"archive", handoff.canonical_archive.string()},
        {"run", handoff.canonical_run},
        {"error", "Canonical companion open failed: " + error},
    };
    return false;
  }
  crimson::zarr::DetectionRepositorySelectionRequest request;
  request.canonical_raw_run = handoff.canonical_run;
  request.raw_fallback_policy = crimson::zarr::DetectionRawFallbackPolicy::
      AllowOnlyWhenNoRefinedAuthority;
  crimson::zarr::DetectionRepositorySelectionMetrics metrics;
  auto repository = crimson::zarr::OpenSelectedDetectionRepository(
      archive, request, &error, &metrics);
  if (!repository) {
    (*evidence)["canonical_companion"] = {
        {"compatible", false},
        {"archive", handoff.canonical_archive.string()},
        {"run", handoff.canonical_run},
        {"error", "Canonical companion selection failed: " + error},
    };
    return false;
  }
  const auto descriptor = repository->descriptor();
  require(descriptor.camera_frame_count == handoff.frame_count &&
              descriptor.row_count == handoff.source_detection_count &&
              descriptor.offset_read_calls == 1,
          "Canonical companion descriptor disagrees with the handoff");
  const auto first = repository->resolveCameraFrameRange(0, 0);
  const auto last = repository->resolveCameraFrameRange(
      static_cast<int64_t>(handoff.frame_count - 1),
      static_cast<int64_t>(handoff.frame_count - 1));
  require(first.status == crimson::zarr::CanonicalDetectionPageStatus::Ready &&
              last.status == crimson::zarr::CanonicalDetectionPageStatus::Ready,
          "Canonical companion sample reads failed");
  (*evidence)["canonical_companion"] = {
      {"compatible", true},
      {"archive", handoff.canonical_archive.string()},
      {"run", descriptor.run_name},
      {"frames", descriptor.camera_frame_count},
      {"rows", descriptor.row_count},
      {"offset_read_calls", descriptor.offset_read_calls},
      {"direct_group_metadata_reads",
       metrics.canonical_open.direct_group_metadata_reads},
      {"fallback_dtype_opens", metrics.canonical_open.fallback_dtype_opens},
      {"fallback_metadata_reads",
       metrics.canonical_open.fallback_metadata_reads},
      {"first_frame_rows", first.frames.front().detections.size()},
      {"last_frame_rows", last.frames.front().detections.size()},
  };
  return true;
}

void validateRefinedTraversal(
    std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> repository,
    const Handoff &handoff,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    json *evidence) {
  const auto descriptor = repository->descriptor();
  require(
      descriptor.surface_kind ==
              crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1 &&
          descriptor.run_name == handoff.refined_run &&
          descriptor.run_manifest_digest == handoff.refined_manifest_digest &&
          descriptor.camera_frame_count == handoff.frame_count &&
          descriptor.row_count == handoff.instance_count &&
          descriptor.stable_identity && descriptor.source_audit_lazy &&
          !descriptor.authority_approved && descriptor.offset_read_calls == 1,
      "Refined descriptor disagrees with the handoff");

  CanonicalDetectionBuffer buffer(scheduler, handoff.refined_archive.string());
  std::string error;
  require(buffer.open(std::move(repository), kPageFrames, kCachePages, &error),
          "Could not open refined detection buffer: " + error);

  const std::vector<int64_t> seek_anchors = {
      0,
      static_cast<int64_t>(handoff.frame_count - 1),
      static_cast<int64_t>(handoff.frame_count / 4),
      static_cast<int64_t>((handoff.frame_count * 3) / 4),
  };
  const auto scheduler_before_seek = scheduler->metrics();
  const auto seek_started = Clock::now();
  for (size_t index = 0; index < 96; ++index) {
    const int64_t frame = seek_anchors[index % seek_anchors.size()];
    require(buffer.requestFrame(frame, true, &error),
            "Rapid seek request failed: " + error);
  }
  const int64_t final_seek = static_cast<int64_t>(handoff.frame_count / 2);
  require(buffer.requestFrame(final_seek, true, &error),
          "Final seek request failed: " + error);
  require(buffer.waitForFrame(final_seek, std::chrono::seconds(60)),
          "Final seek did not resolve");
  scheduler->waitUntilIdle();
  const auto scheduler_after_seek = scheduler->metrics();
  require(buffer.frame(final_seek) != nullptr,
          "Final seek generation was not published");
  require(buffer.frame(0) == nullptr &&
              buffer.frame(static_cast<int64_t>(handoff.frame_count - 1)) ==
                  nullptr,
          "A superseded seek generation remained publishable");
  const uint64_t cancelled = scheduler_after_seek.queue.cancelled_requests -
                             scheduler_before_seek.queue.cancelled_requests;
  const uint64_t discarded = scheduler_after_seek.queue.discarded_completions -
                             scheduler_before_seek.queue.discarded_completions;
  require(cancelled + discarded > 0,
          "Rapid seek burst did not exercise cancellation");
  (*evidence)["rapid_seek"] = {
      {"requests", 97},
      {"final_frame", final_seek},
      {"elapsed_ms", elapsedMilliseconds(seek_started)},
      {"cancelled_requests", cancelled},
      {"discarded_completions", discarded},
      {"stale_publications", 0},
  };

  std::unordered_set<uint64_t> instance_keys;
  std::unordered_set<int64_t> refined_row_ids;
  std::unordered_set<int64_t> raw_source_rows;
  uint64_t digest = 1469598103934665603ULL;
  size_t resolved_rows = 0;
  size_t empty_frames = 0;
  size_t multi_instance_frames = 0;
  size_t raw_rows = 0;
  size_t manual_rows = 0;
  size_t overlay_boxes = 0;
  const auto traversal_started = Clock::now();
  require(buffer.requestFrame(0, true, &error),
          "Traversal reset failed: " + error);
  for (int64_t page_start = 0;
       page_start < static_cast<int64_t>(handoff.frame_count);
       page_start += static_cast<int64_t>(kPageFrames)) {
    require(buffer.requestFrame(page_start, false, &error),
            "Traversal request failed: " + error);
    require(buffer.waitForFrame(page_start, std::chrono::seconds(60)),
            "Traversal page timed out at frame " + std::to_string(page_start));
    const int64_t page_end =
        std::min(static_cast<int64_t>(handoff.frame_count),
                 page_start + static_cast<int64_t>(kPageFrames));
    for (int64_t camera_frame = page_start; camera_frame < page_end;
         ++camera_frame) {
      const auto frame = buffer.frame(camera_frame);
      require(frame != nullptr && frame->camera_frame == camera_frame,
              "Traversal cache missed frame " + std::to_string(camera_frame));
      if (frame->detections.empty()) {
        ++empty_frames;
      }
      if (frame->detections.size() > 1) {
        ++multi_instance_frames;
      }
      resolved_rows += frame->detections.size();
      const auto scene = crimson::overlay::buildReadOnlyOverlayScene(
          crimson::zarr::makeCanonicalDetectionOverlaySceneInput(
              descriptor, *frame, 0, camera_frame, 0,
              static_cast<int>(descriptor.source_width),
              static_cast<int>(descriptor.source_height)));
      require(scene.ready(), "Refined overlay scene was not ready");
      require(
          scene.count(crimson::overlay::CameraOverlayLayer::BoundingBoxes) ==
              frame->detections.size(),
          "Refined overlay scene dropped a detection row");
      overlay_boxes += frame->detections.size();
      for (const auto &detection : frame->detections) {
        require(instance_keys.insert(detection.instance_key).second,
                "Duplicate refined instance_key reached the consumer");
        require(refined_row_ids.insert(detection.refined_row_id).second,
                "Duplicate refined_row_id reached the consumer");
        if (detection.source_kind_code == 1) {
          ++raw_rows;
          require(detection.source_detect_row_index >= 0 &&
                      static_cast<size_t>(detection.source_detect_row_index) <
                          handoff.source_detection_count &&
                      raw_source_rows.insert(detection.source_detect_row_index)
                          .second,
                  "Raw-backed source row was invalid or reused");
        } else if (detection.source_kind_code == 3) {
          ++manual_rows;
          require(detection.source_detect_row_index == -1,
                  "Manual refined row had a source detection row");
        } else {
          require(false, "Unsupported refined source-kind code");
        }
        hashDetection(&digest, detection);
      }
    }
  }
  require(resolved_rows == handoff.instance_count &&
              instance_keys.size() == handoff.instance_count &&
              refined_row_ids.size() == handoff.instance_count &&
              overlay_boxes == handoff.instance_count,
          "Full refined traversal did not preserve every instance");
  require(raw_rows + manual_rows == handoff.instance_count &&
              raw_source_rows.size() == raw_rows,
          "Refined source-kind accounting is inconsistent");
  (*evidence)["traversal"] = {
      {"elapsed_ms", elapsedMilliseconds(traversal_started)},
      {"frames", handoff.frame_count},
      {"rows", resolved_rows},
      {"empty_frames", empty_frames},
      {"multi_instance_frames", multi_instance_frames},
      {"raw_rows", raw_rows},
      {"manual_rows", manual_rows},
      {"unique_instance_keys", instance_keys.size()},
      {"unique_refined_row_ids", refined_row_ids.size()},
      {"unique_raw_source_rows", raw_source_rows.size()},
      {"overlay_boxes", overlay_boxes},
      {"logical_digest_fnv1a64", hexDigest(digest)},
  };

  CanonicalDetectionResidencyPolicy residency_policy;
  residency_policy.maximum_resident_bytes = kResidentBudgetBytes;
  residency_policy.maximum_chunk_decoded_bytes = kResidencyChunkBytes;
  require(buffer.startUiResidency(residency_policy, &error),
          "Could not start refined UI residency: " + error);
  require(buffer.waitForUiResidency(std::chrono::seconds(120)),
          "Refined UI residency timed out");
  const auto residency = buffer.residencyMetrics();
  require(residency.state == CanonicalDetectionResidencyState::Ready &&
              residency.publications == 1 && residency.failed_chunks == 0 &&
              residency.stale_chunks == 0 &&
              residency.retained_bytes == residency.decoded_hot_bytes,
          "Refined UI residency did not publish exactly one complete snapshot");

  const auto repository_before_resident_probe = buffer.repositoryMetrics();
  require(buffer.requestFrame(static_cast<int64_t>(handoff.frame_count - 1),
                              true, &error),
          "Resident probe request failed: " + error);
  require(buffer.waitForFrame(static_cast<int64_t>(handoff.frame_count - 1),
                              std::chrono::seconds(30)),
          "Resident probe timed out");
  scheduler->waitUntilIdle();
  const auto repository_after_resident_probe = buffer.repositoryMetrics();
  require(repository_after_resident_probe.ui_field_reads ==
                  repository_before_resident_probe.ui_field_reads &&
              repository_after_resident_probe.resident_range_reads >
                  repository_before_resident_probe.resident_range_reads,
          "Resident probe returned to TensorStore UI field reads");
  (*evidence)["residency"] = residencyMetricsJson(residency);
  (*evidence)["resident_probe"] = {
      {"frame", handoff.frame_count - 1},
      {"additional_ui_field_reads", 0},
      {"additional_resident_range_reads",
       repository_after_resident_probe.resident_range_reads -
           repository_before_resident_probe.resident_range_reads},
  };

  const auto buffer_metrics = buffer.metrics();
  const auto repository_metrics = buffer.repositoryMetrics();
  require(buffer_metrics.failed_pages == 0 &&
              repository_metrics.failed_reads == 0,
          "Refined buffer or repository reported a failed read");
  (*evidence)["buffer"] = bufferMetricsJson(buffer_metrics);
  (*evidence)["repository"] = repositoryMetricsJson(repository_metrics);
  buffer.close();
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0]
              << " HANDOFF_MANIFEST.json EXPECTED_PAYLOAD_DIGEST OUTPUT.json\n";
    return 2;
  }

  const std::filesystem::path handoff_path = argv[1];
  const std::string expected_digest = argv[2];
  const std::filesystem::path output_path = argv[3];
  json evidence = {
      {"schema_id", "crimson.refined_detection_shadow_gate"},
      {"schema_version", 2},
      {"classification", "headless_real_shadow_integration"},
      {"handoff_manifest", handoff_path.string()},
      {"expected_handoff_payload_digest", expected_digest},
      {"crimson_base_commit", CRIMSON_GIT_COMMIT},
      {"crimson_worktree_dirty", CRIMSON_WORKTREE_DIRTY != 0},
      {"immutable_crimson_revision_bound", CRIMSON_WORKTREE_DIRTY == 0},
      {"pass", false},
  };

  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(256, 4, 2);
  try {
    const auto handoff_document = readJson(handoff_path);
    const auto handoff = validateHandoff(handoff_document, expected_digest);
    evidence["handoff"] = {
        {"schema_id", handoff.schema_id},
        {"payload_digest", expected_digest},
        {"refined_archive", handoff.refined_archive.string()},
        {"refined_run", handoff.refined_run},
        {"refined_manifest_digest", handoff.refined_manifest_digest},
        {"lineage_profile", handoff.lineage_profile},
        {"canonical_archive", handoff.canonical_archive.string()},
        {"canonical_run", handoff.canonical_run},
        {"canonical_manifest_digest", handoff.canonical_manifest_digest},
    };
    require(
        std::filesystem::exists(handoff.refined_archive / "zarr.json") &&
            std::filesystem::exists(handoff.canonical_archive / "zarr.json"),
        "A handoff archive is unavailable");

    std::string error;
    const auto archive_started = Clock::now();
    auto archive =
        crimson::zarr::ArchiveContext::Open(handoff.refined_archive, &error);
    require(archive != nullptr, "Refined archive open failed: " + error);
    evidence["archive_context"] = {
        {"elapsed_ms", elapsedMilliseconds(archive_started)},
        {"cache_pool_bytes", archive->cachePoolBytes()},
    };
    validateFailClosedSelection(archive, handoff, &evidence);

    crimson::zarr::DetectionRepositorySelectionMetrics selection_metrics;
    auto repository =
        openBenchmarkRefined(archive, handoff, &selection_metrics);
    require(
        selection_metrics.kind ==
                crimson::zarr::DetectionRepositorySelectionKind::
                    ExplicitRefinedV1 &&
            selection_metrics.refined_open.root_metadata_reads == 1 &&
            selection_metrics.refined_open.direct_run_metadata_reads == 1 &&
            selection_metrics.refined_open.direct_group_metadata_reads == 3 &&
            selection_metrics.refined_open.consolidated_array_declarations ==
                (handoff.lineage_profile == "clipped_recording_snapshot"
                     ? 38
                     : 28) &&
            selection_metrics.refined_open.exact_handle_opens == 11 &&
            selection_metrics.refined_open.source_audit_handle_opens == 0 &&
            selection_metrics.refined_open.offset_read_calls == 1,
        "Refined repository open did not follow the exact lazy-audit contract");
    evidence["refined_open"] = openMetricsJson(selection_metrics.refined_open);

    validateRefinedTraversal(std::move(repository), handoff, scheduler,
                             &evidence);
    const bool canonical_companion_compatible =
        validateCanonicalCompanion(handoff, &evidence);
    scheduler->waitUntilIdle();
    const auto scheduler_metrics = scheduler->metrics();
    require(scheduler_metrics.queue.failed_completions == 0 &&
                scheduler_metrics.work_exceptions == 0,
            "Shared scheduler reported failed work");
    evidence["scheduler"] = {
        {"workers", scheduler_metrics.worker_count},
        {"submissions", scheduler_metrics.queue.submissions},
        {"completed", scheduler_metrics.queue.completed_requests},
        {"cancelled", scheduler_metrics.queue.cancelled_requests},
        {"discarded", scheduler_metrics.queue.discarded_completions},
        {"failed", scheduler_metrics.queue.failed_completions},
        {"work_exceptions", scheduler_metrics.work_exceptions},
        {"peak_active", scheduler_metrics.queue.peak_active_requests},
    };
    if (!canonical_companion_compatible) {
      require(
          false,
          evidence.at("canonical_companion").at("error").get<std::string>());
    }
    evidence["pass"] = true;
    writeJson(output_path, evidence);
    scheduler->shutdown();
    std::cout << "refined_detection_shadow_gate: PASS\n";
    return 0;
  } catch (const std::exception &exception) {
    evidence["error"] = exception.what();
    try {
      writeJson(output_path, evidence);
    } catch (...) {
    }
    scheduler->shutdown();
    std::cerr << "refined_detection_shadow_gate: FAIL: " << exception.what()
              << '\n';
    return 1;
  }
}
