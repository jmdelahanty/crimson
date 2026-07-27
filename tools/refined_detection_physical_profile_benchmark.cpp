#include "canonical_detection_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/detection_repository_selection.h"
#include "zarr/refined_detection_contract.h"

#include <nlohmann/json.hpp>
#include <tensorstore/internal/metrics/registry.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/resource.h>

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif

#ifndef CRIMSON_WORKTREE_DIRTY
#define CRIMSON_WORKTREE_DIRTY 1
#endif

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace ts = tensorstore;

constexpr size_t kExpectedFrames = 1188000;
constexpr size_t kExpectedRows = 1187087;
constexpr size_t kExpectedOffsets = 1188001;
constexpr size_t kPageFrames = 70;
constexpr size_t kCachePages = 32;
constexpr size_t kTraversalPages = 50;
constexpr size_t kWarmupPages = 10;
constexpr std::chrono::milliseconds kPageDeadline{100};
constexpr uint64_t kCachePoolBytes = 64ULL * 1024ULL * 1024ULL;

constexpr std::array<int64_t, 8> kCurrentFrames = {
    271085, 85499, 397712, 1003450, 939795, 903492, 351953, 1141796};
constexpr std::array<int64_t, 8> kSeekFrames = {
    560111, 1066017, 905397, 100063, 996466, 909512, 639969, 378251};
constexpr std::array<int64_t, 5> kForwardTraversalStarts = {
    300020, 399980, 500010, 700000, 800030};
constexpr std::array<int64_t, 5> kReverseTraversalStarts = {
    600040, 899990, 1050000, 1000020, 1099980};

struct PhysicalMetrics {
  int64_t file_reads = 0;
  int64_t file_batch_reads = 0;
  int64_t file_bytes = 0;
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;
  int64_t cache_evictions = 0;
};

struct CanaryInput {
  std::filesystem::path archive;
  std::string run;
  std::string manifest_digest;
  std::string storage_profile_id;
  size_t payload_objects = 0;
  size_t frames = 0;
  size_t rows = 0;
  size_t offsets = 0;
  std::string palette_commit;
};

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
  require(input.good(), "Could not open JSON input: " + path.string());
  json value;
  input >> value;
  require(input.good() || input.eof(),
          "Could not parse JSON input: " + path.string());
  return value;
}

void writeJson(const std::filesystem::path &path, const json &value) {
  std::ofstream output(path);
  require(output.good(), "Could not open JSON output: " + path.string());
  output << value.dump(2) << '\n';
  require(output.good(), "Could not write JSON output: " + path.string());
}

int64_t counterValue(std::string_view name) {
  const auto metric = ts::internal_metrics::GetMetricRegistry().Collect(name);
  if (!metric || metric->values.empty()) {
    return 0;
  }
  return std::get<int64_t>(metric->values.front().value);
}

PhysicalMetrics snapshotPhysicalMetrics() {
  return {
      counterValue("/tensorstore/kvstore/file/read"),
      counterValue("/tensorstore/kvstore/file/batch_read"),
      counterValue("/tensorstore/kvstore/file/bytes_read"),
      counterValue("/tensorstore/cache/hit_count"),
      counterValue("/tensorstore/cache/miss_count"),
      counterValue("/tensorstore/cache/evict_count"),
  };
}

PhysicalMetrics operator-(const PhysicalMetrics &after,
                          const PhysicalMetrics &before) {
  return {
      after.file_reads - before.file_reads,
      after.file_batch_reads - before.file_batch_reads,
      after.file_bytes - before.file_bytes,
      after.cache_hits - before.cache_hits,
      after.cache_misses - before.cache_misses,
      after.cache_evictions - before.cache_evictions,
  };
}

json physicalMetricsJson(const PhysicalMetrics &metrics) {
  return {
      {"file_reads", metrics.file_reads},
      {"file_batch_reads", metrics.file_batch_reads},
      {"file_bytes", metrics.file_bytes},
      {"cache_hits", metrics.cache_hits},
      {"cache_misses", metrics.cache_misses},
      {"cache_evictions", metrics.cache_evictions},
  };
}

uint64_t peakRssBytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

void hashBytes(uint64_t *hash, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < size; ++index) {
    *hash ^= bytes[index];
    *hash *= 1099511628211ULL;
  }
}

void hashFrame(uint64_t *hash,
               const crimson::zarr::CanonicalDetectionFrame &frame) {
  hashBytes(hash, &frame.camera_frame, sizeof(frame.camera_frame));
  const uint64_t count = frame.detections.size();
  hashBytes(hash, &count, sizeof(count));
  for (const auto &detection : frame.detections) {
    hashBytes(hash, &detection.row_index, sizeof(detection.row_index));
    hashBytes(hash, &detection.instance_key, sizeof(detection.instance_key));
    hashBytes(hash, &detection.refined_row_id,
              sizeof(detection.refined_row_id));
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
}

std::string hexDigest(uint64_t value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
}

double nearestRankPercentile(std::vector<double> values, double fraction) {
  require(!values.empty(), "Cannot reduce an empty latency sample");
  std::sort(values.begin(), values.end());
  const size_t index =
      std::max<size_t>(
          1, static_cast<size_t>(std::ceil(fraction * values.size()))) -
      1;
  return values[std::min(index, values.size() - 1)];
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
      {"peak_concurrent_ui_field_reads",
       metrics.peak_concurrent_ui_field_reads},
      {"maximum_range_read_ms", metrics.maximum_range_read_ms},
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

json schedulerMetricsJson(const crimson::data::DataAccessSchedulerMetrics &m) {
  auto timing_json = [](const crimson::data::DataAccessTimingMetrics &timing) {
    return json{
        {"started", timing.started},
        {"completed", timing.completed},
        {"queue_average_ms", timing.averageQueueWaitMs()},
        {"queue_maximum_ms", timing.maximum_queue_wait_ms},
        {"service_average_ms", timing.averageServiceMs()},
        {"service_maximum_ms", timing.maximum_service_ms},
        {"queue_wait_over_100_ms", timing.queue_wait_over_100_ms},
        {"queue_wait_over_1000_ms", timing.queue_wait_over_1000_ms},
        {"queue_wait_over_5000_ms", timing.queue_wait_over_5000_ms},
        {"service_over_100_ms", timing.service_over_100_ms},
        {"service_over_1000_ms", timing.service_over_1000_ms},
        {"service_over_5000_ms", timing.service_over_5000_ms},
    };
  };
  json by_priority = json::object();
  for (size_t index = 0; index < crimson::data::kDataRequestPriorityCount;
       ++index) {
    const auto priority = static_cast<crimson::data::RequestPriority>(index);
    by_priority[crimson::data::requestPriorityName(priority)] =
        timing_json(m.timing_by_priority[index]);
  }
  json by_source = json::array();
  for (const auto &source : m.timing_by_source) {
    json source_priorities = json::object();
    for (size_t index = 0; index < crimson::data::kDataRequestPriorityCount;
         ++index) {
      const auto priority = static_cast<crimson::data::RequestPriority>(index);
      if (source.by_priority[index].started > 0) {
        source_priorities[crimson::data::requestPriorityName(priority)] =
            timing_json(source.by_priority[index]);
      }
    }
    by_source.push_back({
        {"archive", source.source.archive},
        {"product", source.source.product},
        {"run", source.source.run},
        {"by_priority", std::move(source_priorities)},
    });
  }
  return {
      {"workers", m.worker_count},
      {"reserved_current_frame_workers", m.reserved_current_frame_workers},
      {"submissions", m.queue.submissions},
      {"completed", m.queue.completed_requests},
      {"cancelled", m.queue.cancelled_requests},
      {"discarded", m.queue.discarded_completions},
      {"failed", m.queue.failed_completions},
      {"work_exceptions", m.work_exceptions},
      {"peak_active", m.queue.peak_active_requests},
      {"peak_active_non_current", m.peak_active_non_current_requests},
      {"peak_pending", m.queue.peak_pending_requests},
      {"timing_by_priority", std::move(by_priority)},
      {"timing_by_source", std::move(by_source)},
  };
}

CanaryInput validateCanary(const json &document,
                           const std::string &expected_digest,
                           const std::string &layout) {
  require(document.value("schema_id", "") ==
                  "palette.refined_detection.physical_profile_canary" &&
              document.value("schema_version", 0) == 1,
          "Palette canary schema is incompatible");
  require(document.value("payload_digest_algorithm", "") ==
                  "sha256_canonical_json_v1" &&
              document.value("payload_digest", "") == expected_digest &&
              crimson::zarr::CanonicalJsonSha256(document.at("payload")) ==
                  expected_digest,
          "Palette canary payload digest is invalid");
  const auto &payload = document.at("payload");
  require(payload.value("status", "") == "complete" &&
              payload.value("purpose", "") ==
                  "paired_refined_detection_physical_profile_gate" &&
              payload.value("benchmark_only", false) &&
              !payload.value("selector_eligible", true) &&
              !payload.value("registry_registered", true) &&
              !payload.value("profile_promoted", true),
          "Palette canary publication state is invalid");
  const auto &validation = payload.at("pair_validation");
  require(
      validation.value("canonical_source_audit_equality", false) &&
          validation.value("codec_and_crc_validation", false) &&
          validation.value("direct_consolidated_metadata_equivalence", false) &&
          validation.value("exact_decoded_logical_hash_equality", false) &&
          validation.value("offsets_begin_at_zero_and_end_at_row_count",
                           false) &&
          validation.value("payload_object_gate_passed", false) &&
          validation.value("selector_attributes_absent", false) &&
          validation.at("production_state_changes").empty(),
      "Palette canary validation receipt is incomplete");
  require(layout == "regular" || layout == "access_aware",
          "Layout must be regular or access_aware");
  const auto &artifact = payload.at("artifacts").at(layout);
  const auto &dimensions = payload.at("logical_snapshot").at("dimensions");
  CanaryInput input;
  input.archive = artifact.at("macos_path").get<std::string>();
  input.run = artifact.at("run_id").get<std::string>();
  input.manifest_digest = artifact.at("manifest_digest").get<std::string>();
  input.storage_profile_id =
      artifact.at("storage_profile_id").get<std::string>();
  input.payload_objects = artifact.at("payload_object_count").get<size_t>();
  input.frames = dimensions.at("n_frames").get<size_t>();
  input.rows = dimensions.at("n_instances").get<size_t>();
  input.offsets = dimensions.at("n_frame_boundaries").get<size_t>();
  input.palette_commit = payload.at("palette").at("commit").get<std::string>();
  require(input.frames == kExpectedFrames && input.rows == kExpectedRows &&
              input.offsets == kExpectedOffsets &&
              std::filesystem::exists(input.archive / "zarr.json"),
          "Palette canary dimensions or mounted archive are invalid");
  return input;
}

json runCurrentFrames(CanonicalDetectionBuffer *buffer) {
  const auto physical_before = snapshotPhysicalMetrics();
  std::vector<double> latencies;
  std::vector<size_t> row_counts;
  std::string error;
  for (const int64_t frame : kCurrentFrames) {
    const auto started = Clock::now();
    require(buffer->requestFrame(frame, true, &error),
            "Current-frame request failed: " + error);
    require(buffer->waitForFrame(frame, std::chrono::seconds(60)),
            "Current-frame request timed out at " + std::to_string(frame));
    const auto result = buffer->frame(frame);
    require(result && result->camera_frame == frame,
            "Current-frame result was not published");
    latencies.push_back(elapsedMilliseconds(started));
    row_counts.push_back(result->detections.size());
  }
  return {
      {"frames", kCurrentFrames},
      {"row_counts", row_counts},
      {"latencies_ms", latencies},
      {"p50_ms", nearestRankPercentile(latencies, 0.50)},
      {"p95_ms", nearestRankPercentile(latencies, 0.95)},
      {"maximum_ms", nearestRankPercentile(latencies, 1.0)},
      {"physical",
       physicalMetricsJson(snapshotPhysicalMetrics() - physical_before)},
  };
}

json runSeekBurst(
    CanonicalDetectionBuffer *buffer,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler) {
  const auto scheduler_before = scheduler->metrics();
  const auto physical_before = snapshotPhysicalMetrics();
  const auto started = Clock::now();
  std::string error;
  for (const int64_t frame : kSeekFrames) {
    require(buffer->requestFrame(frame, true, &error),
            "Seek-burst request failed: " + error);
  }
  const int64_t final_frame = kSeekFrames.back();
  require(buffer->waitForFrame(final_frame, std::chrono::seconds(60)),
          "Seek-burst final generation timed out");
  scheduler->waitUntilIdle();
  require(buffer->frame(final_frame) != nullptr,
          "Seek-burst final generation was not published");
  for (size_t index = 0; index + 1 < kSeekFrames.size(); ++index) {
    require(buffer->frame(kSeekFrames[index]) == nullptr,
            "A superseded seek remained publishable");
  }
  const auto scheduler_after = scheduler->metrics();
  return {
      {"frames", kSeekFrames},
      {"settle_ms", elapsedMilliseconds(started)},
      {"cancelled_requests", scheduler_after.queue.cancelled_requests -
                                 scheduler_before.queue.cancelled_requests},
      {"discarded_completions",
       scheduler_after.queue.discarded_completions -
           scheduler_before.queue.discarded_completions},
      {"stale_publications", 0},
      {"physical",
       physicalMetricsJson(snapshotPhysicalMetrics() - physical_before)},
  };
}

json runTraversal(
    CanonicalDetectionBuffer *buffer,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    int64_t low_frame, bool reverse) {
  require(low_frame % static_cast<int64_t>(kPageFrames) == 0,
          "Traversal must begin on a production page boundary");
  const auto physical_before = snapshotPhysicalMetrics();
  const int64_t initial =
      reverse ? low_frame +
                    static_cast<int64_t>((kTraversalPages - 1) * kPageFrames)
              : low_frame;
  std::string error;
  require(buffer->requestFrame(initial, true, &error),
          "Traversal initial request failed: " + error);
  require(buffer->waitForFrame(initial, std::chrono::seconds(60)),
          "Traversal initial page timed out");
  require(buffer->requestFrame(initial, false, &error),
          "Traversal lead request failed: " + error);

  const auto started = Clock::now();
  uint64_t digest = 1469598103934665603ULL;
  size_t deadline_misses = 0;
  size_t post_warmup_misses = 0;
  size_t rows = 0;
  std::vector<double> ready_lateness_ms;
  for (size_t index = 0; index < kTraversalPages; ++index) {
    const auto deadline = started + kPageDeadline * index;
    if (index > 0) {
      std::this_thread::sleep_until(deadline);
    }
    const int64_t page_start =
        reverse ? initial - static_cast<int64_t>(index * kPageFrames)
                : initial + static_cast<int64_t>(index * kPageFrames);
    const bool ready_at_deadline = buffer->frame(page_start) != nullptr;
    if (!ready_at_deadline) {
      ++deadline_misses;
      if (index >= kWarmupPages) {
        ++post_warmup_misses;
      }
    }
    require(buffer->requestFrame(page_start, false, &error),
            "Traversal page request failed: " + error);
    if (!ready_at_deadline) {
      require(buffer->waitForFrame(page_start, std::chrono::seconds(60)),
              "Traversal page timed out at " + std::to_string(page_start));
    }
    ready_lateness_ms.push_back(std::max(
        0.0, std::chrono::duration<double, std::milli>(Clock::now() - deadline)
                 .count()));
    if (reverse) {
      for (int64_t frame = page_start + static_cast<int64_t>(kPageFrames) - 1;
           frame >= page_start; --frame) {
        const auto resolved = buffer->frame(frame);
        require(resolved && resolved->camera_frame == frame,
                "Reverse traversal cache missed a frame");
        rows += resolved->detections.size();
        hashFrame(&digest, *resolved);
      }
    } else {
      for (int64_t frame = page_start;
           frame < page_start + static_cast<int64_t>(kPageFrames); ++frame) {
        const auto resolved = buffer->frame(frame);
        require(resolved && resolved->camera_frame == frame,
                "Forward traversal cache missed a frame");
        rows += resolved->detections.size();
        hashFrame(&digest, *resolved);
      }
    }
  }
  scheduler->waitUntilIdle();
  return {
      {"direction", reverse ? "reverse" : "forward"},
      {"low_frame", low_frame},
      {"frames", kTraversalPages * kPageFrames},
      {"pages", kTraversalPages},
      {"rows", rows},
      {"deadline_ms", kPageDeadline.count()},
      {"warmup_pages", kWarmupPages},
      {"deadline_misses", deadline_misses},
      {"post_warmup_pages", kTraversalPages - kWarmupPages},
      {"post_warmup_misses", post_warmup_misses},
      {"maximum_ready_lateness_ms",
       nearestRankPercentile(ready_lateness_ms, 1.0)},
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"logical_digest_fnv1a64", hexDigest(digest)},
      {"physical",
       physicalMetricsJson(snapshotPhysicalMetrics() - physical_before)},
  };
}

int run(int argc, char **argv) {
  if (argc != 6) {
    std::cerr << "Usage: " << argv[0]
              << " CANARY_MANIFEST.json EXPECTED_PAYLOAD_DIGEST"
                 " regular|access_aware REPETITION OUTPUT.json\n";
    return 2;
  }
  const std::filesystem::path manifest_path = argv[1];
  const std::string expected_digest = argv[2];
  const std::string layout = argv[3];
  const size_t repetition = std::stoull(argv[4]);
  const std::filesystem::path output_path = argv[5];
  require(repetition < 5, "Repetition must be in [0, 5)");

  json evidence = {
      {"schema_id", "crimson.refined_detection_physical_profile_trial"},
      {"schema_version", 1},
      {"classification", "mounted_macos_detection_isolated"},
      {"canary_manifest", manifest_path.string()},
      {"expected_canary_payload_digest", expected_digest},
      {"layout", layout},
      {"repetition", repetition},
      {"cache_pool_bytes", kCachePoolBytes},
      {"crimson_commit", CRIMSON_GIT_COMMIT},
      {"crimson_worktree_dirty", CRIMSON_WORKTREE_DIRTY != 0},
      {"immutable_crimson_revision_bound", CRIMSON_WORKTREE_DIRTY == 0},
      {"pass", false},
  };

  const auto process_started = Clock::now();
  const auto total_physical_before = snapshotPhysicalMetrics();
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(256, 4, 2);
  try {
    const auto canary =
        validateCanary(readJson(manifest_path), expected_digest, layout);
    evidence["input"] = {
        {"archive", canary.archive.string()},
        {"run", canary.run},
        {"manifest_digest", canary.manifest_digest},
        {"storage_profile_id", canary.storage_profile_id},
        {"payload_objects", canary.payload_objects},
        {"frames", canary.frames},
        {"rows", canary.rows},
        {"offsets", canary.offsets},
        {"palette_commit", canary.palette_commit},
    };

    std::string error;
    const auto archive_started = Clock::now();
    auto archive = crimson::zarr::ArchiveContext::Open(canary.archive, &error);
    require(archive != nullptr, "Archive context open failed: " + error);
    require(archive->cachePoolBytes() == kCachePoolBytes,
            "Archive context did not use the production 64 MiB cache");
    evidence["archive_context"] = {
        {"elapsed_ms", elapsedMilliseconds(archive_started)},
        {"cache_pool_bytes", archive->cachePoolBytes()},
    };

    crimson::zarr::DetectionRepositorySelectionRequest request;
    request.explicit_refined_run = canary.run;
    request.allow_selector_ineligible_benchmark = true;
    crimson::zarr::DetectionRepositorySelectionMetrics selection_metrics;
    const auto repository_started = Clock::now();
    auto repository = crimson::zarr::OpenSelectedDetectionRepository(
        archive, request, &error, &selection_metrics);
    require(repository != nullptr, "Refined repository open failed: " + error);
    const auto &open = selection_metrics.refined_open;
    const auto descriptor = repository->descriptor();
    require(
        selection_metrics.kind ==
                crimson::zarr::DetectionRepositorySelectionKind::
                    ExplicitRefinedV1 &&
            descriptor.run_name == canary.run &&
            descriptor.run_manifest_digest == canary.manifest_digest &&
            descriptor.camera_frame_count == canary.frames &&
            descriptor.row_count == canary.rows && descriptor.stable_identity &&
            descriptor.source_audit_lazy && open.root_metadata_reads == 1 &&
            open.direct_run_metadata_reads == 1 &&
            open.direct_group_metadata_reads == 3 &&
            open.consolidated_array_declarations == 28 &&
            open.exact_handle_opens == 11 &&
            open.source_audit_handle_opens == 0 &&
            open.offset_read_calls == 1 &&
            open.retained_offset_bytes == kExpectedOffsets * sizeof(int64_t),
        "Refined repository did not follow the exact v1 open contract");
    evidence["refined_open"] = openMetricsJson(open);
    evidence["refined_open"]["selection_elapsed_ms"] =
        elapsedMilliseconds(repository_started);

    CanonicalDetectionBuffer buffer(scheduler, canary.archive.string());
    require(
        buffer.open(std::move(repository), kPageFrames, kCachePages, &error),
        "Refined buffer open failed: " + error);
    const auto first_physical_before = snapshotPhysicalMetrics();
    const auto first_started = Clock::now();
    require(buffer.requestFrame(0, true, &error),
            "First-page request failed: " + error);
    require(buffer.waitForFrame(0, std::chrono::seconds(60)),
            "First-page request timed out");
    const auto first = buffer.frame(0);
    require(first && first->camera_frame == 0,
            "First refined frame was not published");
    evidence["readiness"] = {
        {"total_ms", elapsedMilliseconds(process_started)},
        {"first_page_request_to_publish_ms",
         elapsedMilliseconds(first_started)},
        {"first_frame_rows", first->detections.size()},
        {"physical", physicalMetricsJson(snapshotPhysicalMetrics() -
                                         first_physical_before)},
    };

    const int64_t forward_start = kForwardTraversalStarts[repetition];
    const int64_t reverse_start = kReverseTraversalStarts[repetition];
    require(reverse_start +
                    static_cast<int64_t>(kTraversalPages * kPageFrames) <=
                static_cast<int64_t>(canary.frames),
            "Traversal region is outside the canary");
    evidence["traversal"] = {
        {"forward", runTraversal(&buffer, scheduler, forward_start, false)},
        {"reverse", runTraversal(&buffer, scheduler, reverse_start, true)},
    };

    evidence["current_frames"] = runCurrentFrames(&buffer);
    evidence["seek_burst"] = runSeekBurst(&buffer, scheduler);

    const auto buffer_metrics = buffer.metrics();
    const auto repository_metrics = buffer.repositoryMetrics();
    const auto scheduler_metrics = scheduler->metrics();
    require(buffer_metrics.failed_pages == 0 &&
                repository_metrics.failed_reads == 0 &&
                repository_metrics.peak_concurrent_ui_field_reads >= 2 &&
                scheduler_metrics.queue.failed_completions == 0 &&
                scheduler_metrics.work_exceptions == 0 &&
                open.offset_read_calls == 1,
            "Refined paging or scheduler reported a contract failure");
    evidence["buffer"] = bufferMetricsJson(buffer_metrics);
    evidence["repository"] = repositoryMetricsJson(repository_metrics);
    evidence["scheduler"] = schedulerMetricsJson(scheduler_metrics);

    const auto close_started = Clock::now();
    buffer.close();
    scheduler->waitUntilIdle();
    evidence["shutdown_ms"] = elapsedMilliseconds(close_started);
    evidence["physical_total"] =
        physicalMetricsJson(snapshotPhysicalMetrics() - total_physical_before);
    evidence["peak_rss_bytes"] = peakRssBytes();
    evidence["process_elapsed_ms"] = elapsedMilliseconds(process_started);
    evidence["pass"] = true;
    writeJson(output_path, evidence);
    scheduler->shutdown();
    std::cout << "refined_detection_physical_profile_benchmark: PASS\n";
    return 0;
  } catch (const std::exception &exception) {
    evidence["error"] = exception.what();
    evidence["physical_total"] =
        physicalMetricsJson(snapshotPhysicalMetrics() - total_physical_before);
    evidence["peak_rss_bytes"] = peakRssBytes();
    evidence["process_elapsed_ms"] = elapsedMilliseconds(process_started);
    try {
      writeJson(output_path, evidence);
    } catch (...) {
    }
    scheduler->shutdown();
    std::cerr << "refined_detection_physical_profile_benchmark: FAIL: "
              << exception.what() << '\n';
    return 1;
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &exception) {
    std::cerr << "refined_detection_physical_profile_benchmark: FAIL: "
              << exception.what() << '\n';
    return 2;
  }
}
