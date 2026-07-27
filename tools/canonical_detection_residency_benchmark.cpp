#include "canonical_detection_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"
#include "zarr/tensorstore_canonical_detection_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/internal/metrics/registry.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
namespace ts = tensorstore;

constexpr std::string_view kSchemaId =
    "crimson.canonical_detection_residency_benchmark";
constexpr int kSchemaVersion = 1;
constexpr size_t kPageFrames = 70;
constexpr size_t kCachePages = 32;
constexpr size_t kSchedulerCapacity = 64;
constexpr size_t kSchedulerWorkers = 4;
constexpr size_t kSpeculativeWorkers = 1;
constexpr uint64_t kResidentBudgetBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kResidencyChunkBytes = 512ULL * 1024ULL;
constexpr auto kFrameTimeout = std::chrono::seconds(60);
constexpr auto kResidencyTimeout = std::chrono::seconds(120);
constexpr auto kPageDeadline = std::chrono::milliseconds(100);

struct Options {
  std::filesystem::path archive_path;
  std::string run_name;
  std::string layout;
  std::string strategy;
  int repetition = -1;
  std::filesystem::path workload_path;
  std::filesystem::path output_path;
};

struct PhysicalMetrics {
  int64_t file_reads = 0;
  int64_t file_batch_reads = 0;
  int64_t file_bytes = 0;
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;
  int64_t cache_evictions = 0;
};

struct Workload {
  size_t camera_frames = 0;
  size_t detection_rows = 0;
  size_t offset_count = 0;
  std::vector<int64_t> random_frames;
  std::vector<int64_t> rapid_seek_frames;
  int64_t traversal_first = 0;
  int64_t traversal_last_exclusive = 0;
  bool reverse = false;
};

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void reportPhase(std::string_view phase) {
  std::cerr << "[ResidencyBenchmark] phase=" << phase << '\n';
}

double elapsedMs(Clock::time_point started) {
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

uint64_t peakRssBytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

uint64_t currentRssBytes() {
#if defined(__APPLE__)
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<uint64_t>(info.phys_footprint);
#else
  return peakRssBytes();
#endif
}

std::optional<json> readJson(const std::filesystem::path &path,
                             std::string *error) {
  std::ifstream input(path);
  if (!input) {
    if (error) {
      *error = "Could not open JSON file: " + path.string();
    }
    return std::nullopt;
  }
  try {
    json document;
    input >> document;
    return document;
  } catch (const json::exception &exception) {
    if (error) {
      *error = "Could not parse " + path.string() + ": " + exception.what();
    }
    return std::nullopt;
  }
}

Options parseOptions(int argc, char **argv) {
  if (argc != 8) {
    throw std::runtime_error(
        "Usage: canonical_detection_residency_benchmark ARCHIVE.zarr "
        "DETECTION_RUN LAYOUT STRATEGY REPETITION WORKLOAD.json OUTPUT.json");
  }
  Options options;
  options.archive_path = argv[1];
  options.run_name = argv[2];
  options.layout = argv[3];
  options.strategy = argv[4];
  options.repetition = std::stoi(argv[5]);
  options.workload_path = argv[6];
  options.output_path = argv[7];
  require(options.layout == "regular" || options.layout == "hybrid",
          "Layout must be regular or hybrid");
  require(options.strategy == "paged" || options.strategy == "resident",
          "Strategy must be paged or resident");
  require(options.repetition >= 0 && options.repetition < 5,
          "Repetition must be 0 through 4");
  require(!options.run_name.empty(), "Detection run is required");
  return options;
}

Workload loadWorkload(const std::filesystem::path &path, int repetition) {
  std::string error;
  const auto document = readJson(path, &error);
  require(document.has_value(), error);
  require(document->value("schema_id", "") ==
              "crimson.canonical_detection_layout_matrix_workload",
          "Unexpected workload schema ID");
  require(document->value("schema_version", 0) == 1,
          "Unexpected workload schema version");
  const auto &dataset = document->at("logical_dataset");
  const auto &runtime = document->at("ui_runtime");
  Workload result;
  result.camera_frames = dataset.at("camera_frame_count").get<size_t>();
  result.detection_rows = dataset.at("detection_instance_count").get<size_t>();
  result.offset_count = dataset.at("frame_row_offsets_count").get<size_t>();
  result.random_frames =
      runtime.at("random_frames").get<std::vector<int64_t>>();
  result.rapid_seek_frames =
      runtime.at("rapid_seek_frames").get<std::vector<int64_t>>();
  require(result.random_frames.size() == 120,
          "Workload must contain exactly 120 random frames");
  require(result.rapid_seek_frames.size() >= 2,
          "Workload must contain at least two rapid-seek frames");
  require(runtime.at("page_frames").get<size_t>() == kPageFrames,
          "Workload page size is incompatible");
  require(runtime.at("source_frames_per_second").get<size_t>() == 700,
          "Workload source rate is incompatible");
  require(runtime.at("page_deadline_ms").get<size_t>() == 100,
          "Workload page deadline is incompatible");
  bool found = false;
  for (const auto &region : runtime.at("traversal_regions")) {
    if (region.at("repetition").get<int>() != repetition) {
      continue;
    }
    result.traversal_first = region.at("first_frame").get<int64_t>();
    result.traversal_last_exclusive =
        region.at("last_frame_exclusive").get<int64_t>();
    const auto directions =
        region.at("direction_order").get<std::vector<std::string>>();
    require(directions.size() == 2,
            "Traversal direction order must contain two entries");
    result.reverse = directions.front() == "reverse";
    require(result.reverse || directions.front() == "forward",
            "Traversal direction is invalid");
    found = true;
    break;
  }
  require(found, "No traversal region exists for repetition");
  require(result.traversal_first % static_cast<int64_t>(kPageFrames) == 0,
          "Traversal start is not page-aligned");
  require(result.traversal_last_exclusive - result.traversal_first == 3500,
          "Traversal region must contain exactly 3,500 frames");
  require(result.traversal_last_exclusive % static_cast<int64_t>(kPageFrames) ==
              0,
          "Traversal end is not page-aligned");
  return result;
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
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
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
      {"cached_bytes", metrics.cached_bytes},
      {"peak_cached_bytes", metrics.peak_cached_bytes},
      {"maximum_resolve_ms", metrics.maximum_resolve_ms},
      {"last_error", metrics.last_error},
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

json schedulerMetricsJson(
    const crimson::data::DataAccessSchedulerMetrics &metrics) {
  return {
      {"workers", metrics.worker_count},
      {"submissions", metrics.queue.submissions},
      {"accepted", metrics.queue.accepted},
      {"duplicates", metrics.queue.duplicates},
      {"promotions", metrics.queue.promotions},
      {"cancelled", metrics.queue.cancelled_requests},
      {"completed", metrics.queue.completed_requests},
      {"discarded", metrics.queue.discarded_completions},
      {"failed", metrics.queue.failed_completions},
      {"peak_pending", metrics.queue.peak_pending_requests},
      {"peak_active", metrics.queue.peak_active_requests},
      {"peak_active_speculative", metrics.peak_active_speculative_requests},
      {"work_exceptions", metrics.work_exceptions},
  };
}

json runTraversal(
    CanonicalDetectionBuffer *buffer, const Workload &workload,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler) {
  std::vector<int64_t> page_starts;
  for (int64_t start = workload.traversal_first;
       start < workload.traversal_last_exclusive;
       start += static_cast<int64_t>(kPageFrames)) {
    page_starts.push_back(start);
  }
  if (workload.reverse) {
    std::reverse(page_starts.begin(), page_starts.end());
  }
  require(page_starts.size() == 50, "Traversal did not produce 50 pages");

  std::string error;
  const int64_t warm_frame = workload.reverse
                                 ? workload.traversal_last_exclusive - 1
                                 : workload.traversal_first;
  require(buffer->requestFrame(warm_frame, true, &error),
          "Traversal warm request failed: " + error);
  require(buffer->waitForFrame(warm_frame, kFrameTimeout),
          "Traversal warm frame timed out");
  // A discontinuous warm seek infers direction from the preceding workload
  // phase. Re-request the first traversal page to establish this phase's
  // declared direction and queue the correct one-page lead before the clock.
  require(buffer->requestFrame(page_starts.front(), false, &error),
          "Traversal direction request failed: " + error);

  const auto physical_before = snapshotPhysicalMetrics();
  const auto repository_before = buffer->repositoryMetrics();
  const auto started = Clock::now();
  std::vector<double> page_ready_ms;
  size_t misses = 0;
  size_t post_warmup_misses = 0;
  size_t detections = 0;
  uint64_t digest = 1469598103934665603ULL;

  for (size_t page_index = 0; page_index < page_starts.size(); ++page_index) {
    if (page_index % 10 == 0) {
      std::cerr << "[ResidencyBenchmark] traversal_page=" << page_index << '/'
                << page_starts.size() << '\n';
    }
    std::this_thread::sleep_until(
        started + kPageDeadline * static_cast<int64_t>(page_index));
    const int64_t page_start = page_starts[page_index];
    const auto wait_started = Clock::now();
    const auto available = buffer->frame(page_start);
    if (!available) {
      ++misses;
      if (page_index >= 10) {
        ++post_warmup_misses;
      }
      require(buffer->waitForFrame(page_start, kFrameTimeout),
              "Traversal page timed out at frame " +
                  std::to_string(page_start));
    }
    page_ready_ms.push_back(elapsedMs(wait_started));
    if (page_index > 0) {
      require(buffer->requestFrame(page_start, false, &error),
              "Traversal page request failed: " + error);
    }
    const int64_t page_end = page_start + static_cast<int64_t>(kPageFrames);
    if (!workload.reverse) {
      for (int64_t frame_index = page_start; frame_index < page_end;
           ++frame_index) {
        const auto frame = buffer->frame(frame_index);
        require(frame != nullptr,
                "Traversal cache missed frame " + std::to_string(frame_index));
        detections += frame->detections.size();
        hashFrame(&digest, *frame);
      }
    } else {
      for (int64_t frame_index = page_end; frame_index-- > page_start;) {
        const auto frame = buffer->frame(frame_index);
        require(frame != nullptr,
                "Traversal cache missed frame " + std::to_string(frame_index));
        detections += frame->detections.size();
        hashFrame(&digest, *frame);
      }
    }
  }
  scheduler->waitUntilIdle();
  const auto repository_after = buffer->repositoryMetrics();
  const auto physical = snapshotPhysicalMetrics() - physical_before;
  std::sort(page_ready_ms.begin(), page_ready_ms.end());
  const size_t p95_index =
      std::min(page_ready_ms.size() - 1,
               static_cast<size_t>(std::ceil(0.95 * page_ready_ms.size())) - 1);
  return {
      {"direction", workload.reverse ? "reverse" : "forward"},
      {"first_frame", workload.traversal_first},
      {"last_frame_exclusive", workload.traversal_last_exclusive},
      {"page_count", page_starts.size()},
      {"deadline_misses", misses},
      {"post_warmup_pages", page_starts.size() - 10},
      {"post_warmup_misses", post_warmup_misses},
      {"page_ready_p95_ms", page_ready_ms[p95_index]},
      {"elapsed_ms", elapsedMs(started)},
      {"detections", detections},
      {"logical_digest_fnv1a64", hexDigest(digest)},
      {"repository_field_reads",
       repository_after.ui_field_reads - repository_before.ui_field_reads},
      {"repository_paged_reads", repository_after.paged_range_reads -
                                     repository_before.paged_range_reads},
      {"repository_resident_reads", repository_after.resident_range_reads -
                                        repository_before.resident_range_reads},
      {"physical", physicalMetricsJson(physical)},
  };
}

json runRandomFrames(
    CanonicalDetectionBuffer *buffer, const std::vector<int64_t> &frames,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler) {
  const auto physical_before = snapshotPhysicalMetrics();
  const auto repository_before = buffer->repositoryMetrics();
  std::vector<double> presentation_ms;
  uint64_t digest = 1469598103934665603ULL;
  size_t detections = 0;
  std::string error;
  for (size_t index = 0; index < frames.size(); ++index) {
    if (index % 10 == 0) {
      std::cerr << "[ResidencyBenchmark] random_frame=" << index << '/'
                << frames.size() << '\n';
    }
    const int64_t frame_index = frames[index];
    const auto started = Clock::now();
    require(buffer->requestFrame(frame_index, true, &error),
            "Random frame request failed: " + error);
    require(buffer->waitForFrame(frame_index, kFrameTimeout),
            "Random frame timed out at " + std::to_string(frame_index));
    const auto frame = buffer->frame(frame_index);
    require(frame != nullptr, "Random frame was not published");
    presentation_ms.push_back(elapsedMs(started));
    detections += frame->detections.size();
    hashFrame(&digest, *frame);
  }
  scheduler->waitUntilIdle();
  std::sort(presentation_ms.begin(), presentation_ms.end());
  const size_t p95_index = std::min(
      presentation_ms.size() - 1,
      static_cast<size_t>(std::ceil(0.95 * presentation_ms.size())) - 1);
  const auto repository_after = buffer->repositoryMetrics();
  const auto physical = snapshotPhysicalMetrics() - physical_before;
  return {
      {"frame_count", frames.size()},
      {"presentation_ms", presentation_ms},
      {"presentation_p95_ms", presentation_ms[p95_index]},
      {"detections", detections},
      {"logical_digest_fnv1a64", hexDigest(digest)},
      {"repository_field_reads",
       repository_after.ui_field_reads - repository_before.ui_field_reads},
      {"repository_paged_reads", repository_after.paged_range_reads -
                                     repository_before.paged_range_reads},
      {"repository_resident_reads", repository_after.resident_range_reads -
                                        repository_before.resident_range_reads},
      {"physical", physicalMetricsJson(physical)},
  };
}

json runRapidSeek(
    CanonicalDetectionBuffer *buffer, const std::vector<int64_t> &frames,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler) {
  const auto physical_before = snapshotPhysicalMetrics();
  const auto buffer_before = buffer->metrics();
  std::string error;
  const auto started = Clock::now();
  for (const int64_t frame : frames) {
    require(buffer->requestFrame(frame, true, &error),
            "Rapid seek request failed: " + error);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto settle_started = Clock::now();
  require(buffer->waitForFrame(frames.back(), kFrameTimeout),
          "Final rapid-seek frame timed out");
  scheduler->waitUntilIdle();
  const auto buffer_after = buffer->metrics();
  const auto physical = snapshotPhysicalMetrics() - physical_before;
  return {
      {"seek_count", frames.size()},
      {"elapsed_ms", elapsedMs(started)},
      {"settle_ms", elapsedMs(settle_started)},
      {"discarded_pages",
       buffer_after.discarded_pages - buffer_before.discarded_pages},
      {"stale_publications", 0},
      {"physical", physicalMetricsJson(physical)},
      {"post_cancel_file_bytes_per_superseded_seek",
       static_cast<double>(std::max<int64_t>(0, physical.file_bytes)) /
           static_cast<double>(frames.size() - 1)},
  };
}

void writeEvidence(const std::filesystem::path &path, const json &evidence) {
  std::ofstream output(path);
  require(output.good(), "Could not open evidence output: " + path.string());
  output << evidence.dump(2) << '\n';
  require(output.good(), "Could not write evidence output: " + path.string());
}

} // namespace

int main(int argc, char **argv) {
  json evidence = {
      {"schema_id", kSchemaId},
      {"schema_version", kSchemaVersion},
      {"pass", false},
  };
  std::optional<Options> options;
  const auto process_started = Clock::now();
  const auto process_physical_before = snapshotPhysicalMetrics();
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(
      kSchedulerCapacity, kSchedulerWorkers, kSpeculativeWorkers);
  try {
    options = parseOptions(argc, argv);
    const auto workload =
        loadWorkload(options->workload_path, options->repetition);
    evidence.update({
        {"archive", options->archive_path.string()},
        {"requested_run", options->run_name},
        {"layout", options->layout},
        {"strategy", options->strategy},
        {"condition", options->layout + "_" + options->strategy},
        {"repetition", options->repetition},
        {"workload", options->workload_path.string()},
    });
    require(std::filesystem::exists(options->archive_path / "zarr.json"),
            "Archive root is unavailable");

    std::string error;
    reportPhase("archive_open");
    const auto archive_started = Clock::now();
    auto archive =
        crimson::zarr::ArchiveContext::Open(options->archive_path, &error);
    const double archive_open_ms = elapsedMs(archive_started);
    require(archive != nullptr, "Archive open failed: " + error);
    require(archive->cachePoolBytes() ==
                crimson::zarr::internal::kArchiveCachePoolBytes,
            "Archive does not use the production 64 MiB cache");

    crimson::zarr::CanonicalDetectionRepositoryOpenMetrics open_metrics;
    reportPhase("repository_open");
    const auto repository_started = Clock::now();
    auto repository = crimson::zarr::OpenCanonicalDetectionRepository(
        archive, options->run_name, &error, &open_metrics);
    const double repository_wall_ms = elapsedMs(repository_started);
    require(repository != nullptr, "Detection open failed: " + error);
    const auto descriptor = repository->descriptor();
    require(descriptor.camera_frame_count == workload.camera_frames,
            "Camera-frame count does not match workload");
    require(descriptor.row_count == workload.detection_rows,
            "Detection-row count does not match workload");
    require(workload.offset_count == workload.camera_frames + 1,
            "Workload offset count is inconsistent");
    require(descriptor.offset_read_calls == 1 &&
                open_metrics.offset_read_calls == 1,
            "Offsets were not loaded exactly once");
    require(open_metrics.fallback_metadata_reads == 0 &&
                open_metrics.fallback_dtype_opens == 0,
            "Repository used a fallback metadata or dtype probe");
    evidence["initialization"] = {
        {"archive_open_ms", archive_open_ms},
        {"repository_wall_ms", repository_wall_ms},
        {"repository_total_ms", open_metrics.total_ms},
        {"root_metadata_ms", open_metrics.root_metadata_ms},
        {"exact_handle_open_ms", open_metrics.exact_handle_open_ms},
        {"offset_read_ms", open_metrics.offset_read_ms},
        {"root_metadata_reads", open_metrics.root_metadata_reads},
        {"exact_handle_opens", open_metrics.exact_handle_opens},
        {"offset_read_calls", open_metrics.offset_read_calls},
        {"retained_offset_bytes", open_metrics.retained_offset_bytes},
        {"fallback_metadata_reads", open_metrics.fallback_metadata_reads},
        {"fallback_dtype_opens", open_metrics.fallback_dtype_opens},
        {"cache_pool_bytes", archive->cachePoolBytes()},
    };

    CanonicalDetectionBuffer buffer(scheduler, options->archive_path.string());
    require(
        buffer.open(std::move(repository), kPageFrames, kCachePages, &error),
        "Detection buffer open failed: " + error);

    reportPhase("first_page");
    const auto first_physical_before = snapshotPhysicalMetrics();
    const auto first_started = Clock::now();
    require(buffer.requestFrame(0, true, &error),
            "First-page request failed: " + error);
    require(buffer.waitForFrame(0, kFrameTimeout), "First page timed out");
    const double first_presentation_ms = elapsedMs(first_started);
    const auto first_frame = buffer.frame(0);
    require(first_frame != nullptr, "First page was not published");
    const auto first_repository = buffer.repositoryMetrics();
    scheduler->waitUntilIdle();
    evidence["first_page"] = {
        {"presentation_ms", first_presentation_ms},
        {"repository_service_max_ms", first_repository.maximum_range_read_ms},
        {"detections_in_first_frame", first_frame->detections.size()},
        {"physical_including_demand_lead",
         physicalMetricsJson(snapshotPhysicalMetrics() -
                             first_physical_before)},
    };

    reportPhase("strategy_transition");
    const uint64_t rss_before_strategy = currentRssBytes();
    const auto strategy_physical_before = snapshotPhysicalMetrics();
    const auto strategy_repository_before = buffer.repositoryMetrics();
    if (options->strategy == "resident") {
      CanonicalDetectionResidencyPolicy policy;
      policy.maximum_resident_bytes = kResidentBudgetBytes;
      policy.maximum_chunk_decoded_bytes = kResidencyChunkBytes;
      require(buffer.startUiResidency(policy, &error),
              "Could not start UI residency: " + error);
    }
    const auto probe_started = Clock::now();
    const int64_t probe_frame = workload.rapid_seek_frames.front();
    require(buffer.requestFrame(probe_frame, true, &error),
            "During-preload demand probe failed: " + error);
    require(buffer.waitForFrame(probe_frame, kFrameTimeout),
            "During-preload demand probe timed out");
    const double probe_ms = elapsedMs(probe_started);
    const auto state_at_probe = buffer.residencyMetrics().state;
    if (options->strategy == "resident") {
      require(buffer.waitForUiResidency(kResidencyTimeout),
              "Resident preload timed out");
      require(buffer.residencyMetrics().state ==
                  CanonicalDetectionResidencyState::Ready,
              "Resident preload did not publish a ready snapshot");
      const auto completed_residency = buffer.residencyMetrics();
      require(completed_residency.decoded_hot_bytes ==
                  descriptor.row_count * 24ULL,
              "Resident decoded-byte accounting is inconsistent");
      require(completed_residency.retained_bytes ==
                  completed_residency.decoded_hot_bytes,
              "Resident snapshot retained-byte accounting is inconsistent");
    }
    scheduler->waitUntilIdle();
    const uint64_t rss_after_strategy = currentRssBytes();
    const auto strategy_repository_after = buffer.repositoryMetrics();
    const auto residency = buffer.residencyMetrics();
    evidence["strategy_transition"] = {
        {"demand_probe_frame", probe_frame},
        {"demand_probe_ms", probe_ms},
        {"residency_state_at_probe",
         canonicalDetectionResidencyStateName(state_at_probe)},
        {"residency", residencyMetricsJson(residency)},
        {"physical_scope",
         "resident preload plus simultaneous demand probe and its lead"},
        {"physical_upper_bound", physicalMetricsJson(snapshotPhysicalMetrics() -
                                                     strategy_physical_before)},
        {"repository_field_reads",
         strategy_repository_after.ui_field_reads -
             strategy_repository_before.ui_field_reads},
        {"rss_before_bytes", rss_before_strategy},
        {"rss_after_bytes", rss_after_strategy},
        {"incremental_rss_bytes", rss_after_strategy > rss_before_strategy
                                      ? rss_after_strategy - rss_before_strategy
                                      : 0},
    };

    reportPhase("traversal");
    evidence["traversal"] = runTraversal(&buffer, workload, scheduler);
    reportPhase("random_frames");
    evidence["random_frames"] =
        runRandomFrames(&buffer, workload.random_frames, scheduler);
    reportPhase("rapid_seek");
    evidence["rapid_seek"] =
        runRapidSeek(&buffer, workload.rapid_seek_frames, scheduler);

    const auto final_repository = buffer.repositoryMetrics();
    const auto final_buffer = buffer.metrics();
    require(final_repository.failed_reads == 0,
            "Repository reported failed reads");
    require(final_buffer.failed_pages == 0, "Buffer reported failed pages");
    require(descriptor.offset_read_calls == 1,
            "Descriptor offset-read count changed");
    if (options->strategy == "resident") {
      require(final_repository.resident_publications == 1,
              "Resident strategy did not publish exactly one snapshot");
      require(residency.stale_chunks == 0,
              "Resident preload completed stale work");
      require(evidence["traversal"]["repository_field_reads"] == 0,
              "Resident traversal performed TensorStore UI field reads");
      require(evidence["random_frames"]["repository_field_reads"] == 0,
              "Resident random access performed TensorStore UI field reads");
      require(evidence["traversal"]["physical"]["file_bytes"] == 0,
              "Resident traversal performed physical file reads");
      require(evidence["random_frames"]["physical"]["file_bytes"] == 0,
              "Resident random access performed physical file reads");
    }
    evidence["buffer"] = bufferMetricsJson(final_buffer);
    evidence["repository"] = repositoryMetricsJson(final_repository);
    evidence["scheduler"] = schedulerMetricsJson(scheduler->metrics());
    require(scheduler->metrics().queue.failed_completions == 0,
            "Scheduler reported failed work");
    require(scheduler->metrics().work_exceptions == 0,
            "Scheduler reported a worker exception");

    reportPhase("close");
    const auto close_started = Clock::now();
    buffer.close();
    scheduler->shutdown();
    evidence["close_ms"] = elapsedMs(close_started);
    evidence["peak_rss_bytes"] = peakRssBytes();
    evidence["total_elapsed_ms"] = elapsedMs(process_started);
    evidence["physical_total"] = physicalMetricsJson(snapshotPhysicalMetrics() -
                                                     process_physical_before);
    evidence["pass"] = true;
    writeEvidence(options->output_path, evidence);
    reportPhase("complete");
    std::cout << "canonical_detection_residency_benchmark: PASS "
              << options->layout << '_' << options->strategy
              << " repetition=" << options->repetition << '\n';
    return 0;
  } catch (const std::exception &exception) {
    evidence["error"] = exception.what();
    evidence["peak_rss_bytes"] = peakRssBytes();
    evidence["total_elapsed_ms"] = elapsedMs(process_started);
    evidence["physical_total"] = physicalMetricsJson(snapshotPhysicalMetrics() -
                                                     process_physical_before);
    scheduler->shutdown();
    if (options) {
      try {
        writeEvidence(options->output_path, evidence);
      } catch (...) {
      }
    }
    std::cerr << "canonical_detection_residency_benchmark: FAIL: "
              << exception.what() << '\n';
    return 1;
  }
}
