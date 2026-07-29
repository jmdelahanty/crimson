#include "canonical_detection_buffer.h"
#include "endurance_workload.h"
#include "platform/macos/apple_analysis_repository_loader.h"
#include "process_memory.h"
#include "read_only_overlay_scene.h"
#include "session_readiness.h"
#include "zarr/affiliated_video_repository.h"
#include "zarr/canonical_detection_overlay_scene_adapter.h"

#include <nlohmann/json.hpp>
#include <tensorstore/internal/metrics/collect.h>
#include <tensorstore/internal/metrics/registry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
namespace ts = tensorstore;

constexpr size_t kExpectedFrames = 1188000;
constexpr size_t kExpectedRows = 1187087;
constexpr int kSourceWidth = 4512;
constexpr int kSourceHeight = 4512;
constexpr size_t kPageFrames = 70;
constexpr size_t kCachePages = 32;
constexpr size_t kTraversalFrames = 3500;
constexpr size_t kPlaybackFps = 700;
constexpr std::chrono::milliseconds kPageDeadline{100};
constexpr uint64_t kResidentBudgetBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kResidencyChunkBytes = 512ULL * 1024ULL;
constexpr int64_t kInterferenceProbeFrame = 271085;
constexpr uint64_t kMiB = 1024ULL * 1024ULL;

constexpr std::array<int64_t, 8> kSettleFrames = {
    271085, 85499, 397712, 1003450, 939795, 903492, 351953, 1141796};
constexpr std::array<int64_t, 8> kSeekBurstFrames = {
    560111, 1066017, 905397, 100063, 996466, 909512, 639969, 378251};

struct PhysicalMetrics {
  int64_t file_reads = 0;
  int64_t file_batch_reads = 0;
  int64_t file_bytes = 0;
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;
  int64_t cache_evictions = 0;
};

struct LoadedProducts {
  std::shared_ptr<crimson::zarr::ArchiveContext> archive;
  std::unique_ptr<crimson::zarr::KeypointOverlayRepository> keypoints;
  std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> masks;
  std::unique_ptr<crimson::zarr::SubjectShapeOverlayRepository> shape;
  std::unique_ptr<crimson::zarr::EyeGeometryOverlayRepository> eye_geometry;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository> motion;
  std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository> eye_angles;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
      tail_kinematics;
  std::unique_ptr<crimson::zarr::AnalysisCropGeometryRepository> crop_geometry;
  crimson::zarr::CanonicalDetectionRepositoryOpenMetrics detection_open;
  uint64_t preloaded_trace_bytes = 0;
};

struct LoadOutcome {
  LoadedProducts products;
  std::unique_ptr<CanonicalDetectionBuffer> detection_buffer;
  double archive_ready_ms = 0.0;
  double first_detection_overlay_ms = 0.0;
  double first_detection_request_ms = 0.0;
  double first_detection_publish_ms = 0.0;
  double first_detection_repository_ms = 0.0;
  uint64_t first_detection_resolved_pages = 0;
  size_t first_detection_pending_at_request = 0;
  size_t first_detection_active_at_request = 0;
  double required_products_ready_ms = 0.0;
  double required_products_load_ms = 0.0;
  double interference_request_ms = 0.0;
  double interference_publish_ms = 0.0;
  double interference_repository_ms = 0.0;
  size_t interference_pending_at_request = 0;
  size_t interference_active_at_request = 0;
  CanonicalDetectionResidencyMetrics residency;
};

struct CommandLine {
  std::filesystem::path archive_path;
  std::string run_name;
  std::filesystem::path video_path;
  std::string layout;
  int repetition = 0;
  std::string strategy = "paged";
  std::filesystem::path output_path;
  crimson::diagnostics::EnduranceWorkloadConfig endurance;
};

json memoryAttributionJson(
    const crimson::diagnostics::MemoryAttributionSnapshot &snapshot);
json schedulerJson(
    const crimson::data::DataAccessSchedulerMetrics &scheduler_metrics);

std::vector<crimson::diagnostics::RetainedMemoryOwner>
retainedMemoryOwners(const LoadOutcome &outcome) {
  using Owner = crimson::diagnostics::RetainedMemoryOwner;
  std::vector<Owner> owners;
  if (outcome.products.archive) {
    owners.push_back(
        {"tensorstore", "decoded_cache", 0, false,
         "Archive cachePoolBytes is a configured limit, not allocated bytes"});
  }
  if (outcome.detection_buffer) {
    const auto descriptor = outcome.detection_buffer->descriptor();
    const auto buffer = outcome.detection_buffer->metrics();
    const auto residency = outcome.detection_buffer->residencyMetrics();
    owners.push_back({"canonical_detection_offsets", "retained_index",
                      descriptor.retained_offset_bytes, true,
                      "Authoritative frame_row_offsets vector"});
    owners.push_back({"canonical_detection_pages", "presentation_cache",
                      buffer.cached_bytes, true,
                      "Current bounded detection page cache"});
    owners.push_back({"canonical_detection_residency", "resident_columns",
                      residency.retained_bytes, true,
                      "Atomically published UI-column snapshot"});
  }
  if (outcome.products.masks) {
    const auto metrics = outcome.products.masks->metrics();
    owners.push_back({"subject_masks_metadata", "retained_index",
                      metrics.metadata_retained_bytes, false,
                      "Reported metadata only; TensorStore handles excluded"});
    owners.push_back({"subject_masks_frame_index", "retained_index",
                      metrics.frame_index_retained_bytes, true,
                      "Retained camera-frame index"});
    owners.push_back({"subject_masks_mapping_pages", "decoded_cache",
                      metrics.cached_mapping_bytes, true,
                      "Current bounded mapping-page cache"});
    owners.push_back({"subject_masks_payload_chunks", "decoded_cache",
                      metrics.cached_payload_bytes, true,
                      "Current bounded sparse mask payload cache"});
  }
  if (outcome.products.motion) {
    const auto metrics = outcome.products.motion->metrics();
    owners.push_back({"motion_preload", "resident_trace",
                      metrics.preloaded_retained_bytes, true,
                      "Decoded default-source trace preload"});
    owners.push_back({"motion_frame_index", "retained_index",
                      metrics.cached_frame_index_bytes, true,
                      "Current frame-index cache"});
  }
  if (outcome.products.eye_angles) {
    const auto metrics = outcome.products.eye_angles->metrics();
    owners.push_back({"eye_angle_preload", "resident_trace",
                      metrics.preloaded_retained_bytes, true,
                      "Decoded frame-series preload"});
  }
  if (outcome.products.tail_kinematics) {
    const auto metrics = outcome.products.tail_kinematics->metrics();
    owners.push_back({"tail_kinematics_preload", "resident_trace",
                      metrics.preloaded_retained_bytes, true,
                      "Decoded default-source trace preload"});
    owners.push_back({"tail_kinematics_frame_index", "retained_index",
                      metrics.cached_frame_index_bytes, true,
                      "Current frame-index cache"});
  }
  if (outcome.products.keypoints) {
    const auto metrics = outcome.products.keypoints->memoryMetrics();
    owners.push_back({"keypoints", "repository_lower_bound",
                      metrics.reportedRetainedBytes(), metrics.complete,
                      "Offsets/containers only; TensorStore handles excluded"});
  }
  if (outcome.products.shape) {
    const auto metrics = outcome.products.shape->memoryMetrics();
    owners.push_back(
        {"subject_shape", "repository_lower_bound",
         metrics.reportedRetainedBytes(), metrics.complete,
         "Placement/index/chunk containers; allocator overhead excluded"});
  }
  if (outcome.products.eye_geometry) {
    const auto metrics = outcome.products.eye_geometry->memoryMetrics();
    owners.push_back(
        {"eye_geometry", "repository_lower_bound",
         metrics.reportedRetainedBytes(), metrics.complete,
         "Placement/index/chunk containers; allocator overhead excluded"});
  }
  if (outcome.products.crop_geometry) {
    const auto metrics = outcome.products.crop_geometry->memoryMetrics();
    owners.push_back(
        {"crop_geometry", "repository_lower_bound",
         metrics.reportedRetainedBytes(), metrics.complete,
         "Geometry rows/index containers; allocator overhead excluded"});
  }
  owners.push_back({"scheduler", "in_flight", 0, false,
                    "Queued callable and thread-stack bytes are not reported"});
  return owners;
}

void captureMemoryAttribution(const std::string &phase,
                              const LoadOutcome &outcome, json *evidence) {
  auto snapshot = crimson::diagnostics::attributeProcessMemory(
      crimson::diagnostics::sampleProcessMemory(),
      retainedMemoryOwners(outcome));
  auto value = memoryAttributionJson(snapshot);
  value["phase"] = phase;
  value["configured_limits"] = {
      {"tensorstore_cache_pool_bytes",
       outcome.products.archive ? outcome.products.archive->cachePoolBytes()
                                : 0},
      {"detection_page_capacity", kCachePages},
      {"detection_residency_budget_bytes", kResidentBudgetBytes},
  };
  if (outcome.detection_buffer) {
    const auto detection = outcome.detection_buffer->metrics();
    value["reported_peaks"]["canonical_detection_page_cache_bytes"] =
        detection.peak_cached_bytes;
  }
  if (outcome.products.masks) {
    const auto masks = outcome.products.masks->metrics();
    value["reported_peaks"]["subject_mask_mapping_cache_bytes"] =
        masks.peak_cached_mapping_bytes;
    value["reported_peaks"]["subject_mask_payload_cache_bytes"] =
        masks.peak_cached_payload_bytes;
  }
  auto repositoryMemory = [](const auto &repository) {
    const auto metrics = repository->memoryMetrics();
    return json{{"retained_metadata_bytes", metrics.retained_metadata_bytes},
                {"retained_index_bytes", metrics.retained_index_bytes},
                {"retained_payload_bytes", metrics.retained_payload_bytes},
                {"decoded_cache_bytes", metrics.decoded_cache_bytes},
                {"reported_retained_bytes", metrics.reportedRetainedBytes()},
                {"complete", metrics.complete}};
  };
  if (outcome.products.keypoints) {
    value["repository_breakdown"]["keypoints"] =
        repositoryMemory(outcome.products.keypoints);
  }
  if (outcome.products.shape) {
    value["repository_breakdown"]["subject_shape"] =
        repositoryMemory(outcome.products.shape);
  }
  if (outcome.products.eye_geometry) {
    value["repository_breakdown"]["eye_geometry"] =
        repositoryMemory(outcome.products.eye_geometry);
  }
  if (outcome.products.crop_geometry) {
    value["repository_breakdown"]["crop_geometry"] =
        repositoryMemory(outcome.products.crop_geometry);
  }
  (*evidence)["memory_attribution"]["phase_snapshots"].push_back(
      std::move(value));
}

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double elapsedMilliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
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

json residencyMetricsJson(const CanonicalDetectionResidencyMetrics &metrics) {
  return {
      {"state", canonicalDetectionResidencyStateName(metrics.state)},
      {"attempts", metrics.attempts},
      {"decoded_hot_bytes", metrics.decoded_hot_bytes},
      {"maximum_resident_bytes", metrics.maximum_resident_bytes},
      {"maximum_chunk_decoded_bytes", metrics.maximum_chunk_decoded_bytes},
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

uint64_t peakRssBytes() {
  return crimson::diagnostics::sampleProcessMemory().peak_rss_bytes;
}

json processMemoryJson(
    const crimson::diagnostics::ProcessMemorySnapshot &snapshot) {
  return {
      {"current_rss_bytes", snapshot.current_rss_bytes},
      {"peak_rss_bytes", snapshot.peak_rss_bytes},
      {"current_rss_supported", snapshot.current_rss_supported},
      {"peak_rss_supported", snapshot.peak_rss_supported},
  };
}

json memorySamplesJson(
    const std::vector<crimson::diagnostics::ProcessMemorySample> &samples) {
  json result = json::array();
  for (const auto &sample : samples) {
    result.push_back({
        {"elapsed_ms", sample.elapsed_ms},
        {"phase", sample.phase},
        {"event", sample.event},
        {"current_rss_bytes", sample.process.current_rss_bytes},
        {"peak_rss_bytes", sample.process.peak_rss_bytes},
        {"current_rss_supported", sample.process.current_rss_supported},
        {"peak_rss_supported", sample.process.peak_rss_supported},
    });
  }
  return result;
}

json memoryAttributionJson(
    const crimson::diagnostics::MemoryAttributionSnapshot &snapshot) {
  json owners = json::array();
  for (const auto &owner : snapshot.owners) {
    owners.push_back({
        {"owner", owner.owner},
        {"category", owner.category},
        {"retained_bytes", owner.retained_bytes},
        {"complete", owner.complete},
        {"note", owner.note},
    });
  }
  return {
      {"process", processMemoryJson(snapshot.process)},
      {"owners", std::move(owners)},
      {"reported_retained_bytes", snapshot.reported_retained_bytes},
      {"unattributed_rss_bytes", snapshot.unattributed_rss_bytes},
      {"reported_over_rss_bytes", snapshot.reported_over_rss_bytes},
      {"all_owners_complete", snapshot.all_owners_complete},
      {"interpretation",
       "reported_retained_bytes is an exact lower bound; unattributed RSS "
       "includes uninstrumented repositories, TensorStore/cache internals, "
       "temporary allocations, libraries, thread stacks, and allocator "
       "retention"},
  };
}

json memoryPlateauPolicyJson(
    const crimson::diagnostics::MemoryPlateauPolicy &policy) {
  return {
      {"warmup_samples", policy.warmup_samples},
      {"minimum_analysis_samples", policy.minimum_analysis_samples},
      {"endpoint_window_samples", policy.endpoint_window_samples},
      {"maximum_final_growth_bytes", policy.maximum_final_growth_bytes},
      {"maximum_peak_growth_bytes", policy.maximum_peak_growth_bytes},
      {"maximum_slope_bytes_per_minute", policy.maximum_slope_bytes_per_minute},
  };
}

json memoryPlateauJson(
    const crimson::diagnostics::MemoryPlateauResult &result) {
  return {
      {"enough_data", result.enough_data},
      {"pass", result.pass},
      {"total_samples", result.total_samples},
      {"analyzed_samples", result.analyzed_samples},
      {"analysis_first_elapsed_ms", result.analysis_first_elapsed_ms},
      {"analysis_last_elapsed_ms", result.analysis_last_elapsed_ms},
      {"initial_window_median_bytes", result.initial_window_median_bytes},
      {"final_window_median_bytes", result.final_window_median_bytes},
      {"peak_bytes", result.peak_bytes},
      {"final_growth_bytes", result.final_growth_bytes},
      {"peak_growth_bytes", result.peak_growth_bytes},
      {"slope_bytes_per_minute", result.slope_bytes_per_minute},
      {"reason", result.reason},
  };
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
    hashBytes(hash, detection.normalized_cxcywh.data(),
              detection.normalized_cxcywh.size() * sizeof(float));
    hashBytes(hash, &detection.score, sizeof(detection.score));
    hashBytes(hash, &detection.class_id, sizeof(detection.class_id));
  }
}

std::string hexDigest(uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string output(16, '0');
  for (size_t index = 0; index < output.size(); ++index) {
    output[output.size() - index - 1] = digits[value & 0xfU];
    value >>= 4U;
  }
  return output;
}

void recordTimings(AppleAnalysisRepositoryBundle *event, json *timings,
                   LoadedProducts *products) {
  for (const auto &timing : event->timings) {
    (*timings)[timing.product] = {
        {"available", timing.available},
        {"elapsed_ms", timing.elapsed_ms},
        {"error", timing.error},
    };
  }
  products->preloaded_trace_bytes += event->preloaded_trace_bytes;
}

void adoptProduct(
    AppleAnalysisRepositoryBundle event, Clock::time_point process_started,
    const std::filesystem::path &archive_path,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    LoadedProducts *products,
    std::unique_ptr<CanonicalDetectionBuffer> *detection_buffer,
    std::optional<double> *archive_ready_ms, json *timings) {
  recordTimings(&event, timings, products);
  if (event.archive) {
    products->archive = event.archive;
  }
  for (const auto &timing : event.timings) {
    if (timing.product == "archive" && timing.available &&
        !archive_ready_ms->has_value()) {
      *archive_ready_ms = elapsedMilliseconds(process_started);
    }
  }
  if (event.canonical_detection) {
    products->detection_open = event.canonical_detection_open_metrics;
    auto buffer = std::make_unique<CanonicalDetectionBuffer>(
        scheduler, archive_path.string());
    std::string error;
    require(buffer->open(std::move(event.canonical_detection), kPageFrames,
                         kCachePages, &error),
            "Could not open canonical detection buffer: " + error);
    require(buffer->requestFrame(0, true, &error),
            "Could not request first canonical detection frame: " + error);
    *detection_buffer = std::move(buffer);
  }
  if (event.keypoints) {
    products->keypoints = std::move(event.keypoints);
  }
  if (event.subject_masks) {
    products->masks = std::move(event.subject_masks);
  }
  if (event.subject_shape) {
    products->shape = std::move(event.subject_shape);
  }
  if (event.eye_geometry) {
    products->eye_geometry = std::move(event.eye_geometry);
  }
  if (event.motion) {
    products->motion = std::move(event.motion);
  }
  if (event.eye_angles) {
    products->eye_angles = std::move(event.eye_angles);
  }
  if (event.tail_kinematics) {
    products->tail_kinematics = std::move(event.tail_kinematics);
  }
  if (event.crop_geometry) {
    products->crop_geometry = std::move(event.crop_geometry);
  }
}

LoadOutcome loadRequiredProducts(
    const std::filesystem::path &archive_path, const std::string &run_name,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    Clock::time_point process_started, const std::string &strategy,
    crimson::diagnostics::ProcessMemorySampler *memory_sampler,
    json *evidence) {
  AppleAnalysisRepositoryLoadRequest request;
  request.archive_path = archive_path.string();
  request.detection_run = run_name;
  request.camera_frame_count = kExpectedFrames;
  request.swim_bout_timeline_enabled = false;
  request.stimulus_context_timeline_enabled = false;
  request.scheduler = scheduler;

  AppleAnalysisRepositoryLoader loader;
  std::string error;
  const auto phase_metrics_before = snapshotPhysicalMetrics();
  const auto started = Clock::now();
  require(loader.start(std::move(request), &error),
          "Could not start repository loader: " + error);

  LoadOutcome outcome;
  json timings = json::object();
  std::optional<double> archive_ready_ms;
  std::optional<double> first_detection_request_ms;
  std::optional<double> first_detection_ready_ms;
  std::optional<double> interference_request_ms;
  std::optional<double> interference_ready_ms;
  std::optional<double> residency_started_ms;
  std::optional<double> residency_ready_ms;
  std::optional<PhysicalMetrics> residency_physical_before;
  std::optional<PhysicalMetrics> residency_physical_after;
  std::optional<crimson::zarr::CanonicalDetectionRepositoryMetrics>
      interference_repository_before;
  const auto deadline = Clock::now() + std::chrono::seconds(240);
  auto startInterferenceProbe = [&] {
    if (interference_request_ms.has_value()) {
      return;
    }
    require(outcome.detection_buffer != nullptr,
            "Canonical detection buffer is unavailable for interference probe");
    if (strategy == "resident") {
      CanonicalDetectionResidencyPolicy policy;
      policy.maximum_resident_bytes = kResidentBudgetBytes;
      policy.maximum_chunk_decoded_bytes = kResidencyChunkBytes;
      residency_physical_before = snapshotPhysicalMetrics();
      residency_started_ms = elapsedMilliseconds(process_started);
      std::string residency_error;
      require(
          outcome.detection_buffer->startUiResidency(policy, &residency_error),
          "Could not start canonical detection residency: " + residency_error);
    }

    const auto scheduler_metrics = scheduler->metrics();
    outcome.interference_pending_at_request =
        scheduler_metrics.queue.pending_requests;
    outcome.interference_active_at_request =
        scheduler_metrics.queue.active_requests;
    interference_repository_before =
        outcome.detection_buffer->repositoryMetrics();
    interference_request_ms = elapsedMilliseconds(process_started);
    std::string probe_error;
    require(outcome.detection_buffer->requestFrame(kInterferenceProbeFrame,
                                                   true, &probe_error),
            "Could not request interference probe frame: " + probe_error);
  };
  auto drain = [&] {
    while (auto ready = loader.takeReady()) {
      const bool had_detection_buffer = outcome.detection_buffer != nullptr;
      std::vector<std::pair<std::string, bool>> completed_products;
      completed_products.reserve(ready->timings.size());
      for (const auto &timing : ready->timings) {
        completed_products.emplace_back(timing.product, timing.available);
      }
      adoptProduct(std::move(*ready), process_started, archive_path, scheduler,
                   &outcome.products, &outcome.detection_buffer,
                   &archive_ready_ms, &timings);
      if (memory_sampler) {
        for (const auto &[product, available] : completed_products) {
          memory_sampler->mark("product_ready:" + product + ":" +
                               (available ? "available" : "unavailable"));
        }
      }
      if (!had_detection_buffer && outcome.detection_buffer) {
        first_detection_request_ms = elapsedMilliseconds(process_started);
        const auto scheduler_metrics = scheduler->metrics();
        outcome.first_detection_pending_at_request =
            scheduler_metrics.queue.pending_requests;
        outcome.first_detection_active_at_request =
            scheduler_metrics.queue.active_requests;
      }
    }
    if (outcome.detection_buffer && !first_detection_ready_ms.has_value() &&
        outcome.detection_buffer->frame(0)) {
      first_detection_ready_ms = elapsedMilliseconds(process_started);
      const auto repository_metrics =
          outcome.detection_buffer->repositoryMetrics();
      outcome.first_detection_repository_ms =
          repository_metrics.maximum_range_read_ms;
      outcome.first_detection_resolved_pages =
          outcome.detection_buffer->metrics().resolved_pages;
      startInterferenceProbe();
    }
    if (interference_request_ms.has_value() &&
        !interference_ready_ms.has_value() &&
        outcome.detection_buffer->frame(kInterferenceProbeFrame)) {
      interference_ready_ms = elapsedMilliseconds(process_started);
      outcome.interference_repository_ms =
          outcome.detection_buffer->repositoryMetrics().maximum_range_read_ms;
    }
    if (strategy == "resident" && residency_started_ms.has_value() &&
        !residency_ready_ms.has_value() &&
        outcome.detection_buffer->residencyMetrics().state ==
            CanonicalDetectionResidencyState::Ready) {
      residency_ready_ms = elapsedMilliseconds(process_started);
      residency_physical_after = snapshotPhysicalMetrics();
    }
  };
  while (loader.loading() && Clock::now() < deadline) {
    drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  drain();
  require(!loader.loading(), "Repository loader exceeded 240 seconds");
  outcome.required_products_ready_ms = elapsedMilliseconds(process_started);
  outcome.required_products_load_ms = elapsedMilliseconds(started);
  const auto phase_metrics_at_ready = snapshotPhysicalMetrics();
  const auto peak_rss_at_ready = peakRssBytes();
  const auto progress = loader.progress();

  using Requirement = crimson::session::ProductAvailabilityRequirement;
  const std::vector<crimson::session::SessionReadinessProductRule> rules = {
      {"archive", Requirement::Required},
      {"canonical_detection", Requirement::Required},
      {"keypoints", Requirement::Required},
      {"subject_masks", Requirement::Required},
      {"subject_shape", Requirement::Required},
      {"eye_geometry", Requirement::Required},
      {"motion", Requirement::Required},
      {"eye_angles", Requirement::Required},
      {"tail_kinematics", Requirement::Required},
      {"crop_geometry", Requirement::Required},
  };
  const auto readiness =
      crimson::session::evaluateSessionReadiness(progress, rules);
  loader.close();

  if (!first_detection_ready_ms.has_value()) {
    require(outcome.detection_buffer->waitForFrame(0, std::chrono::seconds(30)),
            "First canonical detection overlay timed out");
    first_detection_ready_ms = elapsedMilliseconds(process_started);
    const auto repository_metrics =
        outcome.detection_buffer->repositoryMetrics();
    outcome.first_detection_repository_ms =
        repository_metrics.maximum_range_read_ms;
    outcome.first_detection_resolved_pages =
        outcome.detection_buffer->metrics().resolved_pages;
    startInterferenceProbe();
  }
  if (!interference_ready_ms.has_value()) {
    require(outcome.detection_buffer->waitForFrame(kInterferenceProbeFrame,
                                                   std::chrono::seconds(30)),
            "Canonical detection interference probe timed out");
    interference_ready_ms = elapsedMilliseconds(process_started);
    outcome.interference_repository_ms =
        outcome.detection_buffer->repositoryMetrics().maximum_range_read_ms;
  }
  if (strategy == "resident" && !residency_ready_ms.has_value()) {
    require(
        outcome.detection_buffer->waitForUiResidency(std::chrono::seconds(120)),
        "Canonical detection residency timed out");
    residency_ready_ms = elapsedMilliseconds(process_started);
    residency_physical_after = snapshotPhysicalMetrics();
  }

  require(readiness.ready(),
          "Required-product readiness failed: " + readiness.reason);
  require(outcome.products.archive != nullptr,
          "Archive product is unavailable");
  require(outcome.detection_buffer && outcome.detection_buffer->isOpen(),
          "Canonical detection product is unavailable");
  require(outcome.products.keypoints != nullptr,
          "Keypoint product is unavailable");
  require(outcome.products.masks != nullptr,
          "Subject-mask product is unavailable");
  require(outcome.products.shape != nullptr,
          "Subject-shape product is unavailable");
  require(outcome.products.eye_geometry != nullptr,
          "Eye-geometry product is unavailable");
  require(outcome.products.motion != nullptr, "Motion product is unavailable");
  require(outcome.products.eye_angles != nullptr,
          "Eye-angle product is unavailable");
  require(outcome.products.tail_kinematics != nullptr,
          "Tail-kinematics product is unavailable");
  require(outcome.products.crop_geometry != nullptr,
          "Crop-geometry product is unavailable");
  require(archive_ready_ms.has_value(), "Archive ready timestamp is missing");
  require(first_detection_request_ms.has_value(),
          "First canonical detection request timestamp is missing");
  const auto first_detection_repository =
      outcome.detection_buffer->repositoryMetrics();
  outcome.first_detection_repository_ms =
      first_detection_repository.maximum_range_read_ms;
  outcome.first_detection_resolved_pages =
      outcome.detection_buffer->metrics().resolved_pages;
  outcome.archive_ready_ms = *archive_ready_ms;
  outcome.first_detection_request_ms = *first_detection_request_ms;
  outcome.first_detection_publish_ms = *first_detection_ready_ms;
  outcome.first_detection_overlay_ms =
      *first_detection_ready_ms - *archive_ready_ms;
  outcome.interference_request_ms = *interference_request_ms;
  outcome.interference_publish_ms = *interference_ready_ms;
  outcome.residency = outcome.detection_buffer->residencyMetrics();

  if (strategy == "resident") {
    require(outcome.residency.state == CanonicalDetectionResidencyState::Ready,
            "Canonical detection residency did not become ready");
    require(outcome.residency.publications == 1,
            "Canonical detection residency did not publish exactly once");
    require(outcome.residency.stale_chunks == 0,
            "Canonical detection residency published stale chunks");
  } else {
    require(
        outcome.residency.state == CanonicalDetectionResidencyState::Disabled,
        "Paged strategy unexpectedly started canonical detection residency");
  }

  const auto interference_repository_after =
      outcome.detection_buffer->repositoryMetrics();
  const double interference_service_upper_bound_ms =
      std::max(0.0, interference_repository_after.maximum_range_read_ms -
                        interference_repository_before->maximum_range_read_ms);
  (*evidence)["interference_probe"] = {
      {"frame", kInterferenceProbeFrame},
      {"request_elapsed_ms", outcome.interference_request_ms},
      {"publish_elapsed_ms", outcome.interference_publish_ms},
      {"request_to_publish_ms",
       outcome.interference_publish_ms - outcome.interference_request_ms},
      {"pending_requests_at_request", outcome.interference_pending_at_request},
      {"active_requests_at_request", outcome.interference_active_at_request},
      {"repository_service_upper_bound_ms",
       interference_service_upper_bound_ms},
      {"inferred_queue_wait_lower_bound_ms",
       std::max(0.0, outcome.interference_publish_ms -
                         outcome.interference_request_ms -
                         interference_service_upper_bound_ms)},
      {"interpretation",
       "queue wait is inferred because scheduler work is non-preemptive"},
  };

  json residency_evidence = {
      {"strategy", strategy},
      {"started_after_first_page", strategy == "resident"},
      {"metrics", residencyMetricsJson(outcome.residency)},
  };
  if (residency_started_ms.has_value()) {
    residency_evidence["start_elapsed_ms"] = *residency_started_ms;
  }
  if (residency_ready_ms.has_value()) {
    residency_evidence["ready_elapsed_ms"] = *residency_ready_ms;
  }
  if (residency_physical_before.has_value() &&
      residency_physical_after.has_value()) {
    residency_evidence["physical"] = physicalMetricsJson(
        *residency_physical_after - *residency_physical_before);
  }
  (*evidence)["residency"] = std::move(residency_evidence);

  (*evidence)["loading"] = {
      {"elapsed_ms", outcome.required_products_load_ms},
      {"required_products_ready_elapsed_ms",
       outcome.required_products_ready_ms},
      {"state", crimson::loading::loadingStateName(progress.state)},
      {"completed_products", progress.completed_products},
      {"total_products", progress.total_products},
      {"readiness",
       crimson::session::sessionReadinessStateName(readiness.state)},
      {"readiness_reason", readiness.reason},
      {"products", std::move(timings)},
      {"preloaded_trace_bytes", outcome.products.preloaded_trace_bytes},
      {"physical",
       physicalMetricsJson(phase_metrics_at_ready - phase_metrics_before)},
      {"peak_rss_bytes", peak_rss_at_ready},
  };
  captureMemoryAttribution("required_products_ready", outcome, evidence);
  return outcome;
}

crimson::timeline::AnalysisSeriesTimelineWindow resolveSeriesWindow(
    crimson::timeline::AnalysisSeriesTimelineRepository *repository,
    int64_t frame) {
  const auto &descriptor = repository->descriptor();
  const int64_t first = std::max<int64_t>(0, frame - 2048);
  const int64_t last = std::min<int64_t>(
      static_cast<int64_t>(kExpectedFrames) - 1, frame + 2047);
  crimson::timeline::AnalysisSeriesTimelineRequest request;
  request.source_key = descriptor.default_source;
  request.first_frame = first;
  request.last_frame = last;
  request.anchor_frame = frame;
  request.fallback_frames_per_second = 30.0;
  return repository->resolveWindow(request);
}

crimson::timeline::EyeAngleTimelineWindow
resolveEyeAngleWindow(crimson::timeline::EyeAngleTimelineRepository *repository,
                      int64_t frame) {
  const auto &descriptor = repository->descriptor();
  const int64_t first = std::max<int64_t>(0, frame - 2048);
  const int64_t last = std::min<int64_t>(
      static_cast<int64_t>(kExpectedFrames) - 1, frame + 2047);
  crimson::timeline::EyeAngleTimelineRequest request;
  request.representation_key = descriptor.default_representation;
  request.first_frame = first;
  request.last_frame = last;
  request.anchor_frame = frame;
  request.fallback_frames_per_second = 30.0;
  return repository->resolveWindow(request);
}

json resolveSimultaneousFrame(LoadOutcome *outcome, int64_t frame,
                              bool discontinuity) {
  const auto metrics_before = snapshotPhysicalMetrics();
  const auto started = Clock::now();
  std::string error;
  require(outcome->detection_buffer->requestFrame(frame, discontinuity, &error),
          "Canonical frame request failed: " + error);

  auto keypoints = std::async(std::launch::async, [&] {
    return outcome->products.keypoints->resolveCameraFrame(frame, kSourceWidth,
                                                           kSourceHeight);
  });
  auto masks = std::async(std::launch::async, [&] {
    return outcome->products.masks->resolveCameraFrame(frame, kSourceWidth,
                                                       kSourceHeight);
  });
  auto shape = std::async(std::launch::async, [&] {
    return outcome->products.shape->resolveCameraFrame(frame, kSourceWidth,
                                                       kSourceHeight);
  });
  auto eyes = std::async(std::launch::async, [&] {
    return outcome->products.eye_geometry->resolveCameraFrame(
        frame, kSourceWidth, kSourceHeight);
  });
  auto crop = std::async(std::launch::async, [&] {
    return outcome->products.crop_geometry->resolveCameraFrame(
        frame, kSourceWidth, kSourceHeight);
  });
  auto motion = std::async(std::launch::async, [&] {
    return resolveSeriesWindow(outcome->products.motion.get(), frame);
  });
  auto tail = std::async(std::launch::async, [&] {
    return resolveSeriesWindow(outcome->products.tail_kinematics.get(), frame);
  });
  auto eye_angles = std::async(std::launch::async, [&] {
    return resolveEyeAngleWindow(outcome->products.eye_angles.get(), frame);
  });

  require(
      outcome->detection_buffer->waitForFrame(frame, std::chrono::seconds(60)),
      "Canonical frame timed out: " + std::to_string(frame));
  const auto detection = outcome->detection_buffer->frame(frame);
  require(detection != nullptr, "Canonical frame did not publish");
  const auto keypoint_result = keypoints.get();
  const auto mask_result = masks.get();
  const auto shape_result = shape.get();
  const auto eye_result = eyes.get();
  const auto crop_result = crop.get();
  const auto motion_result = motion.get();
  const auto tail_result = tail.get();
  const auto eye_angle_result = eye_angles.get();

  require(keypoint_result.status !=
                  crimson::zarr::KeypointOverlayStatus::InvalidDimensions &&
              keypoint_result.status !=
                  crimson::zarr::KeypointOverlayStatus::ReadFailed &&
              keypoint_result.status !=
                  crimson::zarr::KeypointOverlayStatus::OutOfRange,
          "Keypoint scrub resolution failed: " + keypoint_result.error);
  require(mask_result.status !=
                  crimson::zarr::SubjectMaskOverlayStatus::InvalidDimensions &&
              mask_result.status !=
                  crimson::zarr::SubjectMaskOverlayStatus::ReadFailed &&
              mask_result.status !=
                  crimson::zarr::SubjectMaskOverlayStatus::OutOfRange,
          "Mask scrub resolution failed: " + mask_result.error);
  require(shape_result.status !=
                  crimson::zarr::SubjectShapeOverlayStatus::InvalidDimensions &&
              shape_result.status !=
                  crimson::zarr::SubjectShapeOverlayStatus::ReadFailed &&
              shape_result.status !=
                  crimson::zarr::SubjectShapeOverlayStatus::OutOfRange,
          "Shape scrub resolution failed: " + shape_result.error);
  require(eye_result.status !=
                  crimson::zarr::EyeGeometryOverlayStatus::InvalidDimensions &&
              eye_result.status !=
                  crimson::zarr::EyeGeometryOverlayStatus::ReadFailed &&
              eye_result.status !=
                  crimson::zarr::EyeGeometryOverlayStatus::OutOfRange,
          "Eye-geometry scrub resolution failed: " + eye_result.error);
  require(crop_result.status !=
              crimson::zarr::AnalysisCropGeometryStatus::OutOfRange,
          "Crop scrub resolution was out of range");
  require(motion_result.ready(), "Motion scrub window is unavailable");
  require(tail_result.ready(), "Tail scrub window is unavailable");
  require(eye_angle_result.ready(), "Eye-angle scrub window is unavailable");

  return {
      {"frame", frame},
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"canonical_detections", detection->detections.size()},
      {"keypoint_status", static_cast<int>(keypoint_result.status)},
      {"keypoint_detections", keypoint_result.detections.size()},
      {"mask_status", static_cast<int>(mask_result.status)},
      {"mask_detections", mask_result.detections.size()},
      {"shape_status", static_cast<int>(shape_result.status)},
      {"shape_detections", shape_result.detections.size()},
      {"eye_status", static_cast<int>(eye_result.status)},
      {"eye_detections", eye_result.detections.size()},
      {"crop_status", static_cast<int>(crop_result.status)},
      {"crop_rows", crop_result.frame_row_count},
      {"motion_points", motion_result.published_point_count},
      {"tail_points", tail_result.published_point_count},
      {"eye_angle_points", eye_angle_result.published_point_count},
      {"physical",
       physicalMetricsJson(snapshotPhysicalMetrics() - metrics_before)},
  };
}

void validateFirstPresentations(LoadOutcome *outcome,
                                Clock::time_point process_started,
                                json *evidence) {
  const auto started = Clock::now();
  auto resolved = resolveSimultaneousFrame(outcome, 0, false);
  const auto detection = outcome->detection_buffer->frame(0);
  require(detection != nullptr, "First canonical frame is unavailable");
  const auto descriptor = outcome->detection_buffer->descriptor();
  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(
      crimson::zarr::makeCanonicalDetectionOverlaySceneInput(
          descriptor, *detection, 0, 0, 0, kSourceWidth, kSourceHeight));
  require(scene.ready(), "First canonical overlay scene is unavailable");
  const size_t boxes =
      scene.count(crimson::overlay::CameraOverlayLayer::BoundingBoxes);
  require(boxes == detection->detections.size(),
          "First canonical overlay box count is inconsistent");
  resolved["overlay_boxes"] = boxes;
  resolved["phase_elapsed_ms"] = elapsedMilliseconds(started);
  resolved["ready_elapsed_ms"] = elapsedMilliseconds(process_started);
  resolved["first_detection_overlay_after_archive_ms"] =
      outcome->first_detection_overlay_ms;
  const double request_to_publish_ms =
      outcome->first_detection_publish_ms - outcome->first_detection_request_ms;
  resolved["first_detection_scheduling"] = {
      {"archive_ready_to_request_ms",
       outcome->first_detection_request_ms - outcome->archive_ready_ms},
      {"request_to_publish_ms", request_to_publish_ms},
      {"repository_read_decode_max_ms", outcome->first_detection_repository_ms},
      {"inferred_queue_wait_ms",
       std::max(0.0, request_to_publish_ms -
                         outcome->first_detection_repository_ms)},
      {"pending_requests_at_request",
       outcome->first_detection_pending_at_request},
      {"active_requests_at_request",
       outcome->first_detection_active_at_request},
      {"resolved_pages_at_publish", outcome->first_detection_resolved_pages},
      {"interpretation",
       "queue wait is inferred because scheduler work is non-preemptive"},
  };
  (*evidence)["first_presentations"] = std::move(resolved);
}

void validateCanonicalOpen(const LoadOutcome &outcome,
                           const std::string &run_name, json *evidence) {
  const auto descriptor = outcome.detection_buffer->descriptor();
  require(descriptor.run_name == run_name,
          "Canonical detection selected the wrong run");
  require(descriptor.camera_frame_count == kExpectedFrames,
          "Canonical detection frame count is incorrect");
  require(descriptor.row_count == kExpectedRows,
          "Canonical detection row count is incorrect");
  const auto &open = outcome.products.detection_open;
  require(open.root_metadata_reads == 1,
          "Canonical adapter did not read root metadata exactly once");
  require(open.consolidated_array_declarations == 9,
          "Canonical adapter did not validate nine declarations");
  require(open.exact_handle_opens == 4,
          "Canonical adapter did not perform four exact opens");
  require(open.fallback_metadata_reads == 0 && open.fallback_dtype_opens == 0,
          "Canonical adapter used a forbidden fallback probe");
  require(open.offset_read_calls == 1,
          "Canonical adapter did not read offsets exactly once");
  (*evidence)["canonical_open"] = {
      {"run", descriptor.run_name},
      {"frames", descriptor.camera_frame_count},
      {"rows", descriptor.row_count},
      {"root_metadata_reads", open.root_metadata_reads},
      {"consolidated_declarations", open.consolidated_array_declarations},
      {"exact_handle_opens", open.exact_handle_opens},
      {"fallback_metadata_reads", open.fallback_metadata_reads},
      {"fallback_dtype_opens", open.fallback_dtype_opens},
      {"offset_reads", open.offset_read_calls},
      {"retained_offset_bytes", open.retained_offset_bytes},
      {"elapsed_ms", open.total_ms},
      {"offset_ms", open.offset_read_ms},
  };
}

void runSettleFrames(LoadOutcome *outcome, json *evidence) {
  json frames = json::array();
  const auto metrics_before = snapshotPhysicalMetrics();
  const auto started = Clock::now();
  for (const int64_t frame : kSettleFrames) {
    frames.push_back(resolveSimultaneousFrame(outcome, frame, true));
  }
  (*evidence)["random_settles"] = {
      {"frames", std::move(frames)},
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"physical",
       physicalMetricsJson(snapshotPhysicalMetrics() - metrics_before)},
      {"peak_rss_bytes", peakRssBytes()},
  };
}

void runSeekBurst(
    LoadOutcome *outcome,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    json *evidence) {
  const auto queue_before = scheduler->metrics();
  const auto metrics_before = snapshotPhysicalMetrics();
  const auto started = Clock::now();
  std::string error;
  for (const int64_t frame : kSeekBurstFrames) {
    require(outcome->detection_buffer->requestFrame(frame, true, &error),
            "Seek burst request failed: " + error);
  }
  require(outcome->detection_buffer->waitForFrame(kSeekBurstFrames.back(),
                                                  std::chrono::seconds(60)),
          "Final seek burst frame timed out");
  scheduler->waitUntilIdle();
  const double settle_ms = elapsedMilliseconds(started);
  require(outcome->detection_buffer->frame(kSeekBurstFrames.back()) != nullptr,
          "Final seek generation was not published");
  for (size_t index = 0; index + 1 < kSeekBurstFrames.size(); ++index) {
    require(outcome->detection_buffer->frame(kSeekBurstFrames[index]) ==
                nullptr,
            "A stale seek generation remained publishable");
  }
  const auto queue_after = scheduler->metrics();
  const auto physical = snapshotPhysicalMetrics() - metrics_before;
  const uint64_t cancelled = queue_after.queue.cancelled_requests -
                             queue_before.queue.cancelled_requests;
  require(cancelled > 0, "Seek burst cancelled no stale work");
  (*evidence)["seek_burst"] = {
      {"source_scope", "canonical_detection_only_after_scheduler_idle"},
      {"physical_metric_scope", "process-global TensorStore delta"},
      {"frames", kSeekBurstFrames},
      {"seek_count", kSeekBurstFrames.size()},
      {"settle_ms", settle_ms},
      {"cancelled_requests", cancelled},
      {"discarded_completions", queue_after.queue.discarded_completions -
                                    queue_before.queue.discarded_completions},
      {"stale_publications", 0},
      {"post_cancel_file_bytes_total",
       std::max<int64_t>(0, physical.file_bytes)},
      {"post_cancel_file_bytes_per_superseded_seek",
       static_cast<double>(std::max<int64_t>(0, physical.file_bytes)) /
           static_cast<double>(kSeekBurstFrames.size() - 1)},
      {"physical", physicalMetricsJson(physical)},
  };
}

json runTraversal(
    LoadOutcome *outcome,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    bool reverse, int64_t first_frame = 0,
    int64_t traversal_frames = static_cast<int64_t>(kTraversalFrames)) {
  require(first_frame >= 0 && traversal_frames > 0 &&
              first_frame + traversal_frames <=
                  static_cast<int64_t>(kExpectedFrames),
          "Traversal range is outside the camera frame domain");
  require(first_frame % static_cast<int64_t>(kPageFrames) == 0,
          "Traversal start is not page-aligned");
  const int64_t last_frame_exclusive = first_frame + traversal_frames;
  std::vector<int64_t> page_starts;
  for (int64_t frame = first_frame; frame < last_frame_exclusive;
       frame += static_cast<int64_t>(kPageFrames)) {
    page_starts.push_back(frame);
  }
  if (reverse) {
    std::reverse(page_starts.begin(), page_starts.end());
  }

  std::string error;
  const int64_t warm_frame = reverse ? last_frame_exclusive - 1 : first_frame;
  require(outcome->detection_buffer->requestFrame(warm_frame, true, &error),
          "Traversal warm request failed: " + error);
  require(outcome->detection_buffer->waitForFrame(warm_frame,
                                                  std::chrono::seconds(60)),
          "Traversal warm frame timed out");
  require(outcome->detection_buffer->requestFrame(page_starts.front(), false,
                                                  &error),
          "Traversal lead request failed: " + error);

  const auto metrics_before = snapshotPhysicalMetrics();
  const auto repository_before = outcome->detection_buffer->repositoryMetrics();
  const auto buffer_before = outcome->detection_buffer->metrics();
  const auto started = Clock::now();
  size_t deadline_misses = 0;
  size_t post_warmup_misses = 0;
  size_t detections = 0;
  uint64_t digest = 1469598103934665603ULL;

  for (size_t page_index = 0; page_index < page_starts.size(); ++page_index) {
    const auto deadline =
        started + kPageDeadline * static_cast<int64_t>(page_index);
    std::this_thread::sleep_until(deadline);
    const int64_t page_start = page_starts[page_index];
    auto first = outcome->detection_buffer->frame(page_start);
    if (!first) {
      ++deadline_misses;
      if (page_index > 0) {
        ++post_warmup_misses;
      }
      require(outcome->detection_buffer->waitForFrame(page_start,
                                                      std::chrono::seconds(60)),
              "Traversal demand page timed out at frame " +
                  std::to_string(page_start));
    }
    // The pre-clock request already established the traversal direction and
    // queued its one-page lead. Repeating the first reverse request would make
    // an equal frame look forward and cancel that reverse lead.
    if (page_index > 0) {
      require(
          outcome->detection_buffer->requestFrame(page_start, false, &error),
          "Traversal page request failed: " + error);
    }
    const int64_t page_stop = std::min<int64_t>(
        last_frame_exclusive, page_start + static_cast<int64_t>(kPageFrames));
    if (!reverse) {
      for (int64_t frame = page_start; frame < page_stop; ++frame) {
        const auto resolved = outcome->detection_buffer->frame(frame);
        require(resolved != nullptr, "Forward traversal cache missed frame " +
                                         std::to_string(frame));
        detections += resolved->detections.size();
        hashFrame(&digest, *resolved);
      }
    } else {
      for (int64_t frame = page_stop; frame-- > page_start;) {
        const auto resolved = outcome->detection_buffer->frame(frame);
        require(resolved != nullptr, "Reverse traversal cache missed frame " +
                                         std::to_string(frame));
        detections += resolved->detections.size();
        hashFrame(&digest, *resolved);
      }
    }
  }
  scheduler->waitUntilIdle();
  const auto physical = snapshotPhysicalMetrics() - metrics_before;
  const auto repository_after = outcome->detection_buffer->repositoryMetrics();
  const auto buffer_after = outcome->detection_buffer->metrics();
  const size_t post_warmup_pages = page_starts.size() - 1;
  return {
      {"direction", reverse ? "reverse" : "forward"},
      {"frame_range", {first_frame, last_frame_exclusive}},
      {"source_scope", "canonical_detection_only_after_scheduler_idle"},
      {"physical_metric_scope", "process-global TensorStore delta"},
      {"frames", traversal_frames},
      {"pages", page_starts.size()},
      {"source_fps", kPlaybackFps},
      {"page_deadline_ms", kPageDeadline.count()},
      {"wall_ms", elapsedMilliseconds(started)},
      {"deadline_misses", deadline_misses},
      {"post_warmup_pages", post_warmup_pages},
      {"post_warmup_misses", post_warmup_misses},
      {"post_warmup_deadline_miss_rate",
       post_warmup_pages == 0 ? 0.0
                              : static_cast<double>(post_warmup_misses) /
                                    static_cast<double>(post_warmup_pages)},
      {"detections", detections},
      {"logical_digest_fnv1a64", hexDigest(digest)},
      {"physical", physicalMetricsJson(physical)},
      {"range_reads",
       repository_after.range_reads - repository_before.range_reads},
      {"field_reads",
       repository_after.ui_field_reads - repository_before.ui_field_reads},
      {"peak_concurrent_fields",
       repository_after.peak_concurrent_ui_field_reads},
      {"resolved_pages",
       buffer_after.resolved_pages - buffer_before.resolved_pages},
      {"failed_pages", buffer_after.failed_pages - buffer_before.failed_pages},
      {"discarded_pages",
       buffer_after.discarded_pages - buffer_before.discarded_pages},
      {"peak_cached_bytes", buffer_after.peak_cached_bytes},
      {"maximum_resolve_ms", buffer_after.maximum_resolve_ms},
      {"peak_rss_bytes", peakRssBytes()},
  };
}

json runEnduranceSeekBurst(
    LoadOutcome *outcome,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    const std::vector<int64_t> &frames) {
  require(frames.size() >= 2,
          "Endurance seek burst requires at least two frames");
  scheduler->waitUntilIdle();
  const auto queue_before = scheduler->metrics();
  const auto metrics_before = snapshotPhysicalMetrics();
  const auto buffer_before = outcome->detection_buffer->metrics();
  const auto started = Clock::now();
  std::string error;
  for (const int64_t frame : frames) {
    require(outcome->detection_buffer->requestFrame(frame, true, &error),
            "Endurance seek request failed: " + error);
  }
  require(outcome->detection_buffer->waitForFrame(frames.back(),
                                                  std::chrono::seconds(60)),
          "Endurance final seek frame timed out");
  scheduler->waitUntilIdle();

  size_t stale_publications = 0;
  for (size_t index = 0; index + 1 < frames.size(); ++index) {
    if (outcome->detection_buffer->frame(frames[index])) {
      ++stale_publications;
    }
  }
  require(stale_publications == 0,
          "An endurance seek published a superseded frame");
  require(outcome->detection_buffer->frame(frames.back()) != nullptr,
          "Endurance final seek generation did not publish");

  const auto queue_after = scheduler->metrics();
  const auto buffer_after = outcome->detection_buffer->metrics();
  const auto physical = snapshotPhysicalMetrics() - metrics_before;
  return {
      {"frames", frames},
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"cancelled_requests", queue_after.queue.cancelled_requests -
                                 queue_before.queue.cancelled_requests},
      {"discarded_completions", queue_after.queue.discarded_completions -
                                    queue_before.queue.discarded_completions},
      {"discarded_pages",
       buffer_after.discarded_pages - buffer_before.discarded_pages},
      {"stale_publications", stale_publications},
      {"physical", physicalMetricsJson(physical)},
  };
}

json enduranceProductMetricsJson(const LoadOutcome &outcome) {
  const auto detections = outcome.detection_buffer->metrics();
  const auto detection_repository =
      outcome.detection_buffer->repositoryMetrics();
  const auto masks = outcome.products.masks->metrics();
  const auto motion = outcome.products.motion->metrics();
  const auto eye_angles = outcome.products.eye_angles->metrics();
  const auto tail = outcome.products.tail_kinematics->metrics();
  return {
      {"canonical_detection",
       {{"requests", detections.requests},
        {"cache_hits", detections.cache_hits},
        {"resolved_pages", detections.resolved_pages},
        {"failed_pages", detections.failed_pages},
        {"discarded_pages", detections.discarded_pages},
        {"evicted_pages", detections.evicted_pages},
        {"cached_bytes", detections.cached_bytes},
        {"peak_cached_bytes", detections.peak_cached_bytes},
        {"repository_range_reads", detection_repository.range_reads},
        {"repository_paged_reads", detection_repository.paged_range_reads},
        {"repository_resident_reads",
         detection_repository.resident_range_reads}}},
      {"subject_masks",
       {{"mapping_page_reads", masks.mapping_page_reads},
        {"mapping_page_cache_hits", masks.mapping_page_cache_hits},
        {"mapping_page_evictions", masks.mapping_page_evictions},
        {"cached_mapping_bytes", masks.cached_mapping_bytes},
        {"peak_cached_mapping_bytes", masks.peak_cached_mapping_bytes},
        {"demand_chunk_loads", masks.demand_chunk_loads},
        {"prefetched_chunk_loads", masks.prefetched_chunk_loads},
        {"chunk_cache_hits", masks.chunk_cache_hits},
        {"chunk_evictions", masks.chunk_evictions},
        {"cached_payload_bytes", masks.cached_payload_bytes},
        {"peak_cached_payload_bytes", masks.peak_cached_payload_bytes}}},
      {"motion",
       {{"preloaded", motion.default_source_preloaded},
        {"resident_resolves", motion.preloaded_window_resolves},
        {"paged_resolves", motion.paged_window_resolves},
        {"index_reads", motion.frame_index_block_reads},
        {"index_hits", motion.frame_index_cache_hits},
        {"index_evictions", motion.frame_index_cache_evictions},
        {"cached_index_bytes", motion.cached_frame_index_bytes}}},
      {"eye_angles",
       {{"preloaded", eye_angles.frame_series_preloaded},
        {"resident_resolves", eye_angles.preloaded_window_resolves},
        {"paged_resolves", eye_angles.paged_window_resolves}}},
      {"tail_kinematics",
       {{"preloaded", tail.default_source_preloaded},
        {"resident_resolves", tail.preloaded_window_resolves},
        {"paged_resolves", tail.paged_window_resolves},
        {"index_reads", tail.frame_index_block_reads},
        {"index_hits", tail.frame_index_cache_hits},
        {"index_evictions", tail.frame_index_cache_evictions},
        {"cached_index_bytes", tail.cached_frame_index_bytes}}},
  };
}

json runEndurance(
    LoadOutcome *outcome,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    const crimson::diagnostics::EnduranceWorkloadConfig &config,
    crimson::diagnostics::ProcessMemorySampler *memory_sampler,
    json *evidence) {
  std::string config_error;
  require(crimson::diagnostics::validateEnduranceWorkloadConfig(config,
                                                                &config_error),
          config_error);
  const auto plans = crimson::diagnostics::buildEnduranceWorkload(config);
  require(plans.size() == config.cycle_count,
          "Endurance workload planner returned an incomplete plan");

  const auto started = Clock::now();
  const auto physical_before = snapshotPhysicalMetrics();
  const auto scheduler_before = scheduler->metrics();
  const auto detection_open_before = outcome->products.detection_open;
  std::vector<crimson::diagnostics::MemoryPlateauSample> rss_samples;
  std::vector<crimson::diagnostics::MemoryPlateauSample> retained_samples;
  json cycles = json::array();
  size_t stale_publications = 0;
  size_t post_warmup_pages = 0;
  size_t post_warmup_misses = 0;

  for (const auto &plan : plans) {
    const auto cycle_started = Clock::now();
    const auto cycle_physical_before = snapshotPhysicalMetrics();
    memory_sampler->mark("endurance_cycle_begin:" +
                         std::to_string(plan.cycle_index));

    json probes = json::array();
    for (const int64_t frame : plan.simultaneous_probe_frames) {
      probes.push_back(resolveSimultaneousFrame(outcome, frame, true));
    }
    auto seek =
        runEnduranceSeekBurst(outcome, scheduler, plan.rapid_seek_frames);
    stale_publications += seek["stale_publications"].get<size_t>();
    auto traversal = runTraversal(
        outcome, scheduler, plan.reverse, plan.traversal_first_frame,
        plan.traversal_last_frame_exclusive - plan.traversal_first_frame);
    post_warmup_pages += traversal["post_warmup_pages"].get<size_t>();
    post_warmup_misses += traversal["post_warmup_misses"].get<size_t>();
    scheduler->waitUntilIdle();

    const auto memory = crimson::diagnostics::attributeProcessMemory(
        crimson::diagnostics::sampleProcessMemory(),
        retainedMemoryOwners(*outcome));
    const double endurance_elapsed_ms = elapsedMilliseconds(started);
    require(memory.process.current_rss_supported,
            "Current RSS is unavailable during endurance sampling");
    rss_samples.push_back(
        {endurance_elapsed_ms, memory.process.current_rss_bytes});
    retained_samples.push_back(
        {endurance_elapsed_ms, memory.reported_retained_bytes});
    captureMemoryAttribution("endurance_cycle_" +
                                 std::to_string(plan.cycle_index),
                             *outcome, evidence);
    memory_sampler->mark("endurance_cycle_end:" +
                         std::to_string(plan.cycle_index));

    cycles.push_back({
        {"cycle", plan.cycle_index},
        {"direction", plan.reverse ? "reverse" : "forward"},
        {"traversal_range",
         {plan.traversal_first_frame, plan.traversal_last_frame_exclusive}},
        {"simultaneous_probes", std::move(probes)},
        {"rapid_seek", std::move(seek)},
        {"traversal", std::move(traversal)},
        {"elapsed_ms", elapsedMilliseconds(cycle_started)},
        {"endurance_elapsed_ms", endurance_elapsed_ms},
        {"memory", memoryAttributionJson(memory)},
        {"physical", physicalMetricsJson(snapshotPhysicalMetrics() -
                                         cycle_physical_before)},
        {"product_metrics", enduranceProductMetricsJson(*outcome)},
    });
  }

  crimson::diagnostics::MemoryPlateauPolicy rss_policy;
  rss_policy.maximum_final_growth_bytes = 256 * kMiB;
  rss_policy.maximum_peak_growth_bytes = 512 * kMiB;
  rss_policy.maximum_slope_bytes_per_minute = 64.0 * kMiB;
  crimson::diagnostics::MemoryPlateauPolicy retained_policy = rss_policy;
  retained_policy.maximum_final_growth_bytes = 64 * kMiB;
  retained_policy.maximum_peak_growth_bytes = 128 * kMiB;
  retained_policy.maximum_slope_bytes_per_minute = 32.0 * kMiB;
  const auto rss_plateau =
      crimson::diagnostics::analyzeMemoryPlateau(rss_samples, rss_policy);
  const auto retained_plateau = crimson::diagnostics::analyzeMemoryPlateau(
      retained_samples, retained_policy);
  const auto scheduler_after = scheduler->metrics();
  const auto detection_open_after = outcome->products.detection_open;
  require(detection_open_after.offset_read_calls ==
              detection_open_before.offset_read_calls,
          "Endurance workload reread canonical frame offsets");
  require(stale_publications == 0,
          "Endurance workload published stale seek results");
  require(scheduler_after.queue.failed_completions ==
              scheduler_before.queue.failed_completions,
          "Endurance workload added a failed scheduler completion");
  require(scheduler_after.work_exceptions == scheduler_before.work_exceptions,
          "Endurance workload added a scheduler work exception");
  const bool rss_pass = rss_plateau.enough_data && rss_plateau.pass;
  const bool retained_pass =
      retained_plateau.enough_data && retained_plateau.pass;
  json failure_reasons = json::array();
  if (!rss_pass) {
    failure_reasons.push_back("RSS: " + rss_plateau.reason);
  }
  if (!retained_pass) {
    failure_reasons.push_back("reported retained memory: " +
                              retained_plateau.reason);
  }

  return {
      {"schema_id", "crimson.analysis_endurance"},
      {"schema_version", 1},
      {"classification", "full_archive_bounded_memory_checkpoint"},
      {"config",
       {{"frame_count", config.frame_count},
        {"traversal_span_frames", config.traversal_span_frames},
        {"page_frames", config.page_frames},
        {"cycle_count", config.cycle_count},
        {"simultaneous_probes_per_cycle", config.simultaneous_probes_per_cycle},
        {"seek_requests_per_cycle", config.seek_requests_per_cycle},
        {"seed", config.seed}}},
      {"cycles", std::move(cycles)},
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"stale_publications", stale_publications},
      {"post_warmup_pages", post_warmup_pages},
      {"post_warmup_misses", post_warmup_misses},
      {"post_warmup_deadline_miss_rate",
       post_warmup_pages == 0 ? 0.0
                              : static_cast<double>(post_warmup_misses) /
                                    static_cast<double>(post_warmup_pages)},
      {"offset_reads_during_endurance",
       detection_open_after.offset_read_calls -
           detection_open_before.offset_read_calls},
      {"rss_plateau",
       {{"policy", memoryPlateauPolicyJson(rss_policy)},
        {"result", memoryPlateauJson(rss_plateau)}}},
      {"reported_retained_plateau",
       {{"policy", memoryPlateauPolicyJson(retained_policy)},
        {"result", memoryPlateauJson(retained_plateau)}}},
      {"physical",
       physicalMetricsJson(snapshotPhysicalMetrics() - physical_before)},
      {"scheduler", schedulerJson(scheduler_after)},
      {"product_metrics", enduranceProductMetricsJson(*outcome)},
      {"failure_reasons", std::move(failure_reasons)},
      {"pass", rss_pass && retained_pass},
  };
}

void validateAffiliatedVideo(const LoadedProducts &products,
                             const std::filesystem::path &explicit_video_path,
                             json *evidence) {
  require(std::filesystem::is_regular_file(explicit_video_path),
          "Explicit affiliated video is unavailable: " +
              explicit_video_path.string());
  std::string discovery_error;
  const auto discovered = crimson::zarr::DiscoverAffiliatedVideo(
      products.archive, &discovery_error);
  if (discovered) {
    require(std::filesystem::equivalent(discovered->resolved_path,
                                        explicit_video_path),
            "Discovered and explicit affiliated videos do not match");
  }
  (*evidence)["affiliated_video"] = {
      {"explicit_path", explicit_video_path.string()},
      {"explicit_exists", true},
      {"discovery_succeeded", discovered.has_value()},
      {"discovery_error", discovery_error},
  };
}

json schedulerJson(
    const crimson::data::DataAccessSchedulerMetrics &scheduler_metrics) {
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
        timing_json(scheduler_metrics.timing_by_priority[index]);
  }
  json by_source = json::array();
  for (const auto &source : scheduler_metrics.timing_by_source) {
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
      {"workers", scheduler_metrics.worker_count},
      {"reserved_current_frame_workers",
       scheduler_metrics.reserved_current_frame_workers},
      {"peak_active", scheduler_metrics.queue.peak_active_requests},
      {"peak_active_non_current",
       scheduler_metrics.peak_active_non_current_requests},
      {"peak_pending", scheduler_metrics.queue.peak_pending_requests},
      {"submissions", scheduler_metrics.queue.submissions},
      {"accepted", scheduler_metrics.queue.accepted},
      {"cancelled", scheduler_metrics.queue.cancelled_requests},
      {"completed", scheduler_metrics.queue.completed_requests},
      {"discarded", scheduler_metrics.queue.discarded_completions},
      {"failed", scheduler_metrics.queue.failed_completions},
      {"work_exceptions", scheduler_metrics.work_exceptions},
      {"timing_by_priority", std::move(by_priority)},
      {"timing_by_source", std::move(by_source)},
  };
}

void writeEvidence(const std::filesystem::path &path, const json &evidence) {
  std::ofstream output(path);
  require(output.good(), "Could not open evidence output: " + path.string());
  output << evidence.dump(2) << '\n';
  require(output.good(), "Could not write evidence output: " + path.string());
}

void printUsage(const char *program) {
  std::cerr << "Usage: " << program
            << " ARCHIVE.zarr DETECTION_RUN VIDEO LAYOUT REPETITION "
               "[STRATEGY] OUTPUT.json [--endurance-cycles N] "
               "[--endurance-traversal-frames N] "
               "[--endurance-probes-per-cycle N] "
               "[--endurance-seeks-per-cycle N] [--endurance-seed N]\n";
}

size_t parseSizeArgument(const std::string &name, const char *value) {
  const std::string text = value;
  size_t consumed = 0;
  const unsigned long long parsed = std::stoull(text, &consumed);
  require(consumed == text.size(), name + " is not an unsigned integer");
  require(parsed <= std::numeric_limits<size_t>::max(), name + " is too large");
  return static_cast<size_t>(parsed);
}

CommandLine parseCommandLine(int argc, char **argv) {
  require(argc >= 7, "Missing required benchmark arguments");
  CommandLine result;
  result.archive_path = argv[1];
  result.run_name = argv[2];
  result.video_path = argv[3];
  result.layout = argv[4];
  result.repetition = std::stoi(argv[5]);
  int index = 6;
  if (std::string_view(argv[index]) == "paged" ||
      std::string_view(argv[index]) == "resident") {
    result.strategy = argv[index++];
  }
  require(index < argc, "Missing output JSON path");
  result.output_path = argv[index++];
  result.endurance.frame_count = kExpectedFrames;
  result.endurance.page_frames = kPageFrames;
  while (index < argc) {
    const std::string option = argv[index++];
    require(index < argc, "Missing value for " + option);
    const char *value = argv[index++];
    if (option == "--endurance-cycles") {
      result.endurance.cycle_count = parseSizeArgument(option, value);
    } else if (option == "--endurance-traversal-frames") {
      const size_t parsed = parseSizeArgument(option, value);
      require(parsed <=
                  static_cast<size_t>(std::numeric_limits<int64_t>::max()),
              option + " is too large");
      result.endurance.traversal_span_frames = static_cast<int64_t>(parsed);
    } else if (option == "--endurance-probes-per-cycle") {
      result.endurance.simultaneous_probes_per_cycle =
          parseSizeArgument(option, value);
    } else if (option == "--endurance-seeks-per-cycle") {
      result.endurance.seek_requests_per_cycle =
          parseSizeArgument(option, value);
    } else if (option == "--endurance-seed") {
      result.endurance.seed = parseSizeArgument(option, value);
    } else {
      throw std::runtime_error("Unknown benchmark option: " + option);
    }
  }
  if (result.endurance.cycle_count > 0) {
    require(result.endurance.cycle_count >= 10,
            "Endurance mode requires at least ten cycles for its plateau "
            "verdict");
    std::string error;
    require(crimson::diagnostics::validateEnduranceWorkloadConfig(
                result.endurance, &error),
            error);
  }
  return result;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 7) {
    printUsage(argv[0]);
    return 2;
  }
  CommandLine command;
  try {
    command = parseCommandLine(argc, argv);
  } catch (const std::exception &exception) {
    printUsage(argv[0]);
    std::cerr << "canonical_detection_full_archive_stage1_benchmark: "
              << exception.what() << '\n';
    return 2;
  }
  const auto &archive_path = command.archive_path;
  const auto &run_name = command.run_name;
  const auto &video_path = command.video_path;
  const auto &layout = command.layout;
  const int repetition = command.repetition;
  const auto &strategy = command.strategy;
  const auto &output_path = command.output_path;
  const auto process_started = Clock::now();
  const auto process_physical_before = snapshotPhysicalMetrics();
  crimson::diagnostics::ProcessMemorySampler memory_sampler(
      std::chrono::milliseconds(25));
  if (!memory_sampler.start("process_start")) {
    std::cerr << "Could not start process-memory sampler\n";
    return 1;
  }

  json evidence = {
      {"schema_id", "crimson.canonical_detection_full_archive_stage1"},
      {"schema_version", 1},
      {"classification", "full_duration_stage1"},
      {"layout", layout},
      {"strategy", strategy},
      {"condition", layout + "_" + strategy},
      {"repetition", repetition},
      {"archive", archive_path.string()},
      {"requested_run", run_name},
      {"crimson_commit", CRIMSON_GIT_COMMIT},
      {"crimson_worktree_dirty", CRIMSON_WORKTREE_DIRTY != 0},
      {"frame_range", {0, kExpectedFrames}},
      {"pass", false},
  };
  evidence["memory_attribution"] = {
      {"schema_id", "crimson.process_memory_attribution"},
      {"schema_version", 1},
      {"sample_interval_ms", 25},
      {"phase_snapshots", json::array()},
  };

  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(64, 4, 1, 1);
  std::unique_ptr<CanonicalDetectionBuffer> buffer_for_cleanup;
  try {
    require(layout == "regular" || layout == "hybrid",
            "Layout must be regular or hybrid");
    require(strategy == "paged" || strategy == "resident",
            "Strategy must be paged or resident");
    require(repetition >= 0 && repetition < 5,
            "Repetition must be 0 through 4");
    require(std::filesystem::exists(archive_path / "zarr.json"),
            "Archive root is unavailable");

    memory_sampler.setPhase("required_product_loading");
    auto outcome =
        loadRequiredProducts(archive_path, run_name, scheduler, process_started,
                             strategy, &memory_sampler, &evidence);
    evidence["archive_context"] = {
        {"cache_pool_bytes", outcome.products.archive->cachePoolBytes()},
    };
    memory_sampler.setPhase("first_presentations");
    validateAffiliatedVideo(outcome.products, video_path, &evidence);
    validateCanonicalOpen(outcome, run_name, &evidence);
    validateFirstPresentations(&outcome, process_started, &evidence);
    captureMemoryAttribution("first_presentations", outcome, &evidence);
    memory_sampler.setPhase("random_settles");
    runSettleFrames(&outcome, &evidence);
    captureMemoryAttribution("random_settles", outcome, &evidence);
    memory_sampler.setPhase("seek_burst");
    runSeekBurst(&outcome, scheduler, &evidence);
    captureMemoryAttribution("seek_burst", outcome, &evidence);
    memory_sampler.setPhase("forward_traversal");
    evidence["traversal"]["forward"] = runTraversal(&outcome, scheduler, false);
    captureMemoryAttribution("forward_traversal", outcome, &evidence);
    memory_sampler.setPhase("reverse_traversal");
    evidence["traversal"]["reverse"] = runTraversal(&outcome, scheduler, true);
    captureMemoryAttribution("reverse_traversal", outcome, &evidence);
    if (command.endurance.cycle_count > 0) {
      memory_sampler.setPhase("endurance");
      evidence["endurance"] = runEndurance(
          &outcome, scheduler, command.endurance, &memory_sampler, &evidence);
      captureMemoryAttribution("endurance_complete", outcome, &evidence);
      require(evidence["endurance"]["pass"].get<bool>(),
              "Endurance memory plateau gate failed: " +
                  evidence["endurance"]["failure_reasons"].dump());
    }

    scheduler->waitUntilIdle();
    const auto scheduler_metrics = scheduler->metrics();
    require(scheduler_metrics.queue.failed_completions == 0,
            "Shared scheduler reported failed work");
    require(scheduler_metrics.work_exceptions == 0,
            "Shared scheduler reported a work exception");
    evidence["scheduler"] = schedulerJson(scheduler_metrics);

    memory_sampler.setPhase("shutdown");
    captureMemoryAttribution("before_shutdown", outcome, &evidence);
    const auto shutdown_started = Clock::now();
    outcome.detection_buffer->close();
    scheduler->shutdown();
    evidence["shutdown_ms"] = elapsedMilliseconds(shutdown_started);
    memory_sampler.mark("scheduler_shutdown");
    outcome.detection_buffer.reset();
    outcome.products.keypoints.reset();
    outcome.products.masks.reset();
    outcome.products.shape.reset();
    outcome.products.eye_geometry.reset();
    outcome.products.motion.reset();
    outcome.products.eye_angles.reset();
    outcome.products.tail_kinematics.reset();
    outcome.products.crop_geometry.reset();
    outcome.products.archive.reset();
    memory_sampler.mark("repositories_released");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    memory_sampler.mark("release_settled");
    evidence["memory_attribution"]["after_release"] =
        processMemoryJson(crimson::diagnostics::sampleProcessMemory());
    memory_sampler.stop();
    evidence["memory_attribution"]["timeline"] =
        memorySamplesJson(memory_sampler.samples());
    evidence["memory_attribution"]["maximum_observed"] =
        processMemoryJson(memory_sampler.maximumObserved());
    evidence["peak_rss_bytes"] = peakRssBytes();
    evidence["total_elapsed_ms"] = elapsedMilliseconds(process_started);
    evidence["physical_total"] = physicalMetricsJson(snapshotPhysicalMetrics() -
                                                     process_physical_before);
    evidence["pass"] = true;
    writeEvidence(output_path, evidence);
    std::cout << "canonical_detection_full_archive_stage1_benchmark: PASS "
              << layout << " strategy=" << strategy
              << " repetition=" << repetition << '\n';
    return 0;
  } catch (const std::exception &exception) {
    memory_sampler.setPhase("failure");
    memory_sampler.mark("exception");
    memory_sampler.stop();
    evidence["error"] = exception.what();
    evidence["memory_attribution"]["timeline"] =
        memorySamplesJson(memory_sampler.samples());
    evidence["memory_attribution"]["maximum_observed"] =
        processMemoryJson(memory_sampler.maximumObserved());
    evidence["peak_rss_bytes"] = peakRssBytes();
    evidence["total_elapsed_ms"] = elapsedMilliseconds(process_started);
    evidence["physical_total"] = physicalMetricsJson(snapshotPhysicalMetrics() -
                                                     process_physical_before);
    try {
      writeEvidence(output_path, evidence);
    } catch (...) {
    }
    scheduler->shutdown();
    std::cerr << "canonical_detection_full_archive_stage1_benchmark: FAIL "
              << layout << " strategy=" << strategy
              << " repetition=" << repetition << ": " << exception.what()
              << '\n';
    return 1;
  }
}
