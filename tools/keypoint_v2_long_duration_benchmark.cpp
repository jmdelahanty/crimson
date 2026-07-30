#include <sys/resource.h>
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
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data_access_scheduler.h"
#include "keypoint_overlay_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_json.h"
#include "zarr/keypoint_v2_contract.h"
#include "zarr/tensorstore_keypoint_v2_repository.h"

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif
#ifndef CRIMSON_WORKTREE_DIRTY
#define CRIMSON_WORKTREE_DIRTY 1
#endif

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

constexpr std::string_view kResultSchema =
    "crimson.keypoint_v2.long_duration_benchmark";
constexpr int kResultSchemaVersion = 1;

struct Options {
  bool self_test = false;
  bool deep_validate_identity = false;
  std::string mode;
  std::filesystem::path raw_store;
  std::string raw_run;
  std::string raw_digest;
  std::filesystem::path quality_store;
  std::string quality_run;
  std::string quality_digest;
  std::filesystem::path refined_store;
  std::string refined_run;
  std::string refined_digest;
  std::filesystem::path body_store;
  std::string body_run;
  std::string body_digest;
  std::filesystem::path workload_path;
  std::filesystem::path output_path;
  size_t repetition = 0;
};

struct Workload {
  json document;
  std::string digest;
  size_t repetitions = 0;
  size_t random_count = 0;
  uint64_t random_seed = 0;
  size_t rapid_seek_count = 0;
  uint64_t rapid_seek_seed = 0;
  size_t traversal_frames = 0;
  size_t page_frames = 0;
  double source_fps = 0.0;
  size_t warmup_pages = 0;
  size_t pending_capacity = 0;
  size_t workers = 0;
  size_t speculative_workers = 0;
  size_t reserved_current_workers = 0;
  size_t lookahead_frames = 0;
  size_t cache_frames = 0;
  size_t frame_timeout_ms = 0;
  size_t close_timeout_ms = 0;
  double readiness_gate_ms = 0.0;
  double random_p95_gate_ms = 0.0;
  double current_queue_gate_ms = 0.0;
  double deadline_miss_gate = 0.0;
  double rapid_seek_gate_ms = 0.0;
  uint64_t peak_rss_gate_bytes = 0;
};

struct TensorStoreMetrics {
  int64_t file_reads = 0;
  int64_t file_batch_reads = 0;
  int64_t file_bytes = 0;
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;
  int64_t cache_evictions = 0;
};

class Digest {
 public:
  template <typename T>
  void scalar(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    write(
        std::string_view(reinterpret_cast<const char*>(&value), sizeof(value)));
  }

  void label(std::string_view value) {
    scalar(value.size());
    write(value);
  }

  std::string finish() const {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint64_t value : states_) output << std::setw(16) << value;
    return output.str();
  }

 private:
  void write(std::string_view bytes) {
    constexpr std::array<uint64_t, 4> primes = {
        1099511628211ULL, 1099511627791ULL, 1099511627689ULL, 1099511627563ULL};
    for (char byte : bytes) {
      const auto value = static_cast<uint8_t>(byte);
      for (size_t lane = 0; lane < states_.size(); ++lane) {
        states_[lane] ^= static_cast<uint64_t>(value + lane * 37U);
        states_[lane] *= primes[lane];
      }
    }
  }

  std::array<uint64_t, 4> states_ = {1469598103934665603ULL, 1099511628211ULL,
                                     7809847782465536322ULL,
                                     9650029242287828579ULL};
};

struct FrameMeasurement {
  double elapsed_ms = 0.0;
  size_t observations = 0;
};

double elapsedMs(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<size_t>(
      std::max(0.0, std::ceil(fraction * values.size()) - 1.0));
  return values[std::min(rank, values.size() - 1)];
}

uint64_t splitMix64(uint64_t* state) {
  uint64_t value = (*state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

std::vector<int64_t> randomFrames(size_t frame_count, size_t count,
                                  uint64_t seed) {
  require(frame_count > 0, "The selected keypoint run has no frames");
  count = std::min(count, frame_count);
  std::unordered_set<int64_t> seen;
  std::vector<int64_t> result;
  result.reserve(count);
  while (result.size() < count) {
    const auto frame = static_cast<int64_t>(splitMix64(&seed) % frame_count);
    if (seen.insert(frame).second) result.push_back(frame);
  }
  return result;
}

int64_t counterValue(std::string_view name) {
  const auto metric =
      tensorstore::internal_metrics::GetMetricRegistry().Collect(name);
  if (!metric || metric->values.empty()) return 0;
  return std::get<int64_t>(metric->values.front().value);
}

TensorStoreMetrics snapshotMetrics() {
  return {counterValue("/tensorstore/kvstore/file/read"),
          counterValue("/tensorstore/kvstore/file/batch_read"),
          counterValue("/tensorstore/kvstore/file/bytes_read"),
          counterValue("/tensorstore/cache/hit_count"),
          counterValue("/tensorstore/cache/miss_count"),
          counterValue("/tensorstore/cache/evict_count")};
}

TensorStoreMetrics operator-(const TensorStoreMetrics& after,
                             const TensorStoreMetrics& before) {
  return {after.file_reads - before.file_reads,
          after.file_batch_reads - before.file_batch_reads,
          after.file_bytes - before.file_bytes,
          after.cache_hits - before.cache_hits,
          after.cache_misses - before.cache_misses,
          after.cache_evictions - before.cache_evictions};
}

json physicalJson(const TensorStoreMetrics& metrics) {
  return {{"file_reads", metrics.file_reads},
          {"file_batch_reads", metrics.file_batch_reads},
          {"file_bytes", metrics.file_bytes},
          {"cache_hits", metrics.cache_hits},
          {"cache_misses", metrics.cache_misses},
          {"cache_evictions", metrics.cache_evictions}};
}

uint64_t peakRssBytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) return 0;
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

std::optional<size_t> parseSize(std::string_view value, std::string_view name) {
  try {
    size_t consumed = 0;
    const auto parsed = std::stoull(std::string(value), &consumed);
    require(consumed == value.size() &&
                parsed <= std::numeric_limits<size_t>::max(),
            "Invalid " + std::string(name));
    return static_cast<size_t>(parsed);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<Options> parseOptions(int argc, char** argv, std::string* error) {
  Options options;
  if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
    options.self_test = true;
    return options;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto next = [&]() -> std::optional<std::string> {
      if (index + 1 >= argc) return std::nullopt;
      return std::string(argv[++index]);
    };
    if (argument == "--deep-validate-identity") {
      options.deep_validate_identity = true;
      continue;
    }
    const auto value = next();
    if (!value) {
      *error = "Missing value for " + argument;
      return std::nullopt;
    }
    if (argument == "--mode")
      options.mode = *value;
    else if (argument == "--raw-store")
      options.raw_store = *value;
    else if (argument == "--raw-run")
      options.raw_run = *value;
    else if (argument == "--raw-digest")
      options.raw_digest = *value;
    else if (argument == "--quality-store")
      options.quality_store = *value;
    else if (argument == "--quality-run")
      options.quality_run = *value;
    else if (argument == "--quality-digest")
      options.quality_digest = *value;
    else if (argument == "--refined-store")
      options.refined_store = *value;
    else if (argument == "--refined-run")
      options.refined_run = *value;
    else if (argument == "--refined-digest")
      options.refined_digest = *value;
    else if (argument == "--body-store")
      options.body_store = *value;
    else if (argument == "--body-run")
      options.body_run = *value;
    else if (argument == "--body-digest")
      options.body_digest = *value;
    else if (argument == "--workload")
      options.workload_path = *value;
    else if (argument == "--output")
      options.output_path = *value;
    else if (argument == "--repetition") {
      const auto parsed = parseSize(*value, argument);
      if (!parsed) {
        *error = "Invalid repetition";
        return std::nullopt;
      }
      options.repetition = *parsed;
    } else {
      *error = "Unknown argument: " + argument;
      return std::nullopt;
    }
  }
  const bool refined = options.mode == "refined";
  const bool valid =
      (options.mode == "raw" || refined) && !options.raw_store.empty() &&
      !options.raw_run.empty() && !options.raw_digest.empty() &&
      !options.quality_store.empty() && !options.quality_run.empty() &&
      !options.quality_digest.empty() && !options.body_store.empty() &&
      !options.body_run.empty() && !options.body_digest.empty() &&
      !options.workload_path.empty() && !options.output_path.empty() &&
      (!refined ||
       (!options.refined_store.empty() && !options.refined_run.empty() &&
        !options.refined_digest.empty()));
  if (!valid) {
    *error =
        "Mode and explicit raw, quality, body, workload, output, and "
        "refined (in refined mode) arguments are required";
    return std::nullopt;
  }
  return options;
}

Workload loadWorkload(const std::filesystem::path& path) {
  std::ifstream input(path);
  require(input.good(), "Could not open workload: " + path.string());
  Workload result;
  input >> result.document;
  require(result.document.value("schema_id", "") ==
                  "crimson.keypoint_v2.long_duration_workload" &&
              result.document.value("schema_version", 0) == 1,
          "Unsupported keypoint workload schema");
  result.digest = crimson::zarr::CanonicalJsonSha256(result.document);
  result.repetitions = result.document.at("repetition_count");
  const auto& random = result.document.at("random_frames");
  result.random_count = random.at("count");
  result.random_seed = random.at("seed");
  const auto& seeks = result.document.at("rapid_seeks");
  result.rapid_seek_count = seeks.at("count");
  result.rapid_seek_seed = seeks.at("seed");
  const auto& traversal = result.document.at("traversal");
  result.traversal_frames = traversal.at("frames");
  result.page_frames = traversal.at("page_frames");
  result.source_fps = traversal.at("source_fps");
  result.warmup_pages = traversal.at("warmup_pages");
  const auto& scheduler = result.document.at("scheduler");
  result.pending_capacity = scheduler.at("pending_capacity");
  result.workers = scheduler.at("workers");
  result.speculative_workers = scheduler.at("maximum_speculative_workers");
  result.reserved_current_workers =
      scheduler.at("reserved_current_frame_workers");
  const auto& cache = result.document.at("presentation_cache");
  result.lookahead_frames = cache.at("lookahead_frames");
  result.cache_frames = cache.at("capacity_frames");
  const auto& timeouts = result.document.at("timeouts_ms");
  result.frame_timeout_ms = timeouts.at("frame");
  result.close_timeout_ms = timeouts.at("close");
  const auto& gates = result.document.at("gates");
  result.readiness_gate_ms = gates.at("first_presentation_readiness_ms");
  result.random_p95_gate_ms = gates.at("warm_random_frame_p95_ms");
  result.current_queue_gate_ms = gates.at("current_frame_queue_max_ms");
  result.deadline_miss_gate = gates.at("post_warmup_page_deadline_miss_ratio");
  result.rapid_seek_gate_ms = gates.at("rapid_seek_final_readiness_ms");
  result.peak_rss_gate_bytes = gates.at("peak_rss_bytes");
  require(result.repetitions > 0 && result.random_count > 0 &&
              result.rapid_seek_count > 0 && result.traversal_frames > 0 &&
              result.page_frames > 0 && result.source_fps > 0.0 &&
              result.workers > 0 && result.cache_frames > 0 &&
              result.lookahead_frames < result.cache_frames &&
              result.frame_timeout_ms > 0,
          "Keypoint workload contains invalid zero or inconsistent values");
  return result;
}

json openMetricsJson(
    const crimson::zarr::KeypointV2RepositoryOpenMetrics& value) {
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

json accessMetricsJson(
    const crimson::zarr::KeypointV2RepositoryAccessMetrics& value) {
  return {{"frame_requests", value.frame_requests},
          {"rows_resolved", value.rows_resolved},
          {"payload_read_calls", value.payload_read_calls},
          {"payload_read_batches", value.payload_read_batches},
          {"maximum_columns_per_batch", value.maximum_columns_per_batch},
          {"quality_payload_read_calls", value.quality_payload_read_calls},
          {"read_failures", value.read_failures}};
}

json bufferMetricsJson(const KeypointOverlayBufferMetrics& value) {
  return {{"requests", value.requests},
          {"cache_hits", value.cache_hits},
          {"resolved_frames", value.resolved_frames},
          {"missing_frames", value.missing_frames},
          {"failed_frames", value.failed_frames},
          {"discarded_results", value.discarded_results},
          {"peak_cached_frames", value.peak_cached_frames},
          {"peak_pending_frames", value.peak_pending_frames},
          {"maximum_resolve_ms", value.maximum_resolve_ms},
          {"last_error", value.last_error}};
}

json timingJson(const crimson::data::DataAccessTimingMetrics& value) {
  return {{"started", value.started},
          {"completed", value.completed},
          {"queue_average_ms", value.averageQueueWaitMs()},
          {"queue_maximum_ms", value.maximum_queue_wait_ms},
          {"service_average_ms", value.averageServiceMs()},
          {"service_maximum_ms", value.maximum_service_ms},
          {"queue_wait_over_100_ms", value.queue_wait_over_100_ms},
          {"queue_wait_over_1000_ms", value.queue_wait_over_1000_ms},
          {"queue_wait_over_5000_ms", value.queue_wait_over_5000_ms},
          {"service_over_100_ms", value.service_over_100_ms},
          {"service_over_1000_ms", value.service_over_1000_ms},
          {"service_over_5000_ms", value.service_over_5000_ms}};
}

json schedulerMetricsJson(
    const crimson::data::DataAccessSchedulerMetrics& value) {
  json priorities = json::object();
  for (size_t index = 0; index < crimson::data::kDataRequestPriorityCount;
       ++index) {
    const auto priority = static_cast<crimson::data::RequestPriority>(index);
    priorities[crimson::data::requestPriorityName(priority)] =
        timingJson(value.timing_by_priority[index]);
  }
  json sources = json::array();
  for (const auto& source : value.timing_by_source) {
    json by_priority = json::object();
    for (size_t index = 0; index < crimson::data::kDataRequestPriorityCount;
         ++index) {
      if (source.by_priority[index].started == 0) continue;
      const auto priority = static_cast<crimson::data::RequestPriority>(index);
      by_priority[crimson::data::requestPriorityName(priority)] =
          timingJson(source.by_priority[index]);
    }
    sources.push_back({{"archive", source.source.archive},
                       {"product", source.source.product},
                       {"run", source.source.run},
                       {"by_priority", std::move(by_priority)}});
  }
  return {
      {"workers", value.worker_count},
      {"reserved_current_frame_workers", value.reserved_current_frame_workers},
      {"submissions", value.queue.submissions},
      {"accepted", value.queue.accepted},
      {"duplicates", value.queue.duplicates},
      {"promotions", value.queue.promotions},
      {"cancelled", value.queue.cancelled_requests},
      {"completed", value.queue.completed_requests},
      {"discarded", value.queue.discarded_completions},
      {"failed", value.queue.failed_completions},
      {"peak_pending", value.queue.peak_pending_requests},
      {"peak_active", value.queue.peak_active_requests},
      {"peak_active_non_current", value.peak_active_non_current_requests},
      {"peak_active_speculative", value.peak_active_speculative_requests},
      {"work_exceptions", value.work_exceptions},
      {"timing_by_priority", std::move(priorities)},
      {"timing_by_source", std::move(sources)}};
}

void digestResolution(const crimson::zarr::KeypointOverlayResolution& value,
                      Digest* digest) {
  digest->scalar(value.camera_frame);
  digest->scalar(static_cast<uint8_t>(value.status));
  digest->scalar(value.detections.size());
  for (const auto& detection : value.detections) {
    digest->scalar(detection.instance_key);
    digest->scalar(detection.source_crop_row_id);
    digest->scalar(detection.source_success);
    digest->scalar(detection.refined_success);
    digest->scalar(detection.confidence_valid);
    digest->scalar(detection.geometry_valid);
    digest->scalar(detection.review_state_code);
    digest->scalar(detection.reason_code);
    digest->scalar(detection.heading_valid);
    digest->scalar(detection.heading_from_body_frame);
    digest->scalar(detection.keypoints.size());
    for (const auto& point : detection.keypoints) {
      digest->scalar(point.x);
      digest->scalar(point.y);
    }
    digest->scalar(detection.keypoint_edit_flags.size());
    for (uint8_t flag : detection.keypoint_edit_flags) digest->scalar(flag);
    const bool has_heading = detection.heading_degrees.has_value();
    digest->scalar(has_heading);
    if (has_heading) digest->scalar(*detection.heading_degrees);
  }
}

FrameMeasurement readFrame(KeypointOverlayBuffer* buffer, int64_t frame,
                           int width, int height, bool discontinuity,
                           std::chrono::milliseconds timeout, Digest* digest) {
  const auto started = Clock::now();
  std::string error;
  require(buffer->requestFrame(frame, width, height, discontinuity, &error),
          "Frame request failed: " + error);
  require(buffer->waitForFrame(frame, timeout),
          "Timed out waiting for keypoint frame " + std::to_string(frame));
  auto value = buffer->frame(frame);
  require(value != nullptr, "Requested keypoint frame was not published: " +
                                std::to_string(frame));
  require(value->status == crimson::zarr::KeypointOverlayStatus::Mapped ||
              value->status == crimson::zarr::KeypointOverlayStatus::Missing,
          "Keypoint frame resolution failed: " + value->error);
  std::unordered_set<uint64_t> keys;
  for (const auto& detection : value->detections) {
    require(detection.instance_key != 0 &&
                keys.insert(detection.instance_key).second,
            "Keypoint frame contains a missing or duplicate instance key");
    require(detection.heading_from_body_frame,
            "Keypoint presentation did not use body-frame heading");
  }
  digestResolution(*value, digest);
  return {elapsedMs(started), value->detections.size()};
}

json latencySummary(const std::vector<double>& latencies, size_t observations,
                    const std::string& digest,
                    const TensorStoreMetrics& physical) {
  double total = 0.0;
  for (double value : latencies) total += value;
  return {{"requests", latencies.size()},
          {"observations", observations},
          {"elapsed_ms", total},
          {"average_ms", latencies.empty() ? 0.0 : total / latencies.size()},
          {"p50_ms", percentile(latencies, 0.50)},
          {"p95_ms", percentile(latencies, 0.95)},
          {"maximum_ms",
           latencies.empty()
               ? 0.0
               : *std::max_element(latencies.begin(), latencies.end())},
          {"logical_digest", digest},
          {"physical", physicalJson(physical)}};
}

json runRandomPass(KeypointOverlayBuffer* buffer,
                   const std::vector<int64_t>& frames, int width, int height,
                   std::chrono::milliseconds timeout) {
  const auto physical_before = snapshotMetrics();
  std::vector<double> latencies;
  size_t observations = 0;
  Digest digest;
  for (int64_t frame : frames) {
    const auto measured =
        readFrame(buffer, frame, width, height, true, timeout, &digest);
    latencies.push_back(measured.elapsed_ms);
    observations += measured.observations;
  }
  return latencySummary(latencies, observations, digest.finish(),
                        snapshotMetrics() - physical_before);
}

json runTraversal(KeypointOverlayBuffer* buffer,
                  const std::vector<int64_t>& frames, size_t page_frames,
                  size_t warmup_pages, double source_fps, int width, int height,
                  std::chrono::milliseconds timeout) {
  const auto physical_before = snapshotMetrics();
  std::vector<double> frame_latencies;
  std::vector<double> page_latencies;
  size_t observations = 0;
  Digest digest;
  auto page_started = Clock::now();
  for (size_t index = 0; index < frames.size(); ++index) {
    const auto measured = readFrame(buffer, frames[index], width, height,
                                    index == 0, timeout, &digest);
    frame_latencies.push_back(measured.elapsed_ms);
    observations += measured.observations;
    if ((index + 1) % page_frames == 0 || index + 1 == frames.size()) {
      page_latencies.push_back(elapsedMs(page_started));
      page_started = Clock::now();
    }
  }
  const double deadline_ms =
      static_cast<double>(page_frames) * 1000.0 / source_fps;
  size_t measured_pages = 0;
  size_t missed_pages = 0;
  for (size_t index = std::min(warmup_pages, page_latencies.size());
       index < page_latencies.size(); ++index) {
    ++measured_pages;
    if (page_latencies[index] > deadline_ms) ++missed_pages;
  }
  json result = latencySummary(frame_latencies, observations, digest.finish(),
                               snapshotMetrics() - physical_before);
  result["page_frames"] = page_frames;
  result["page_count"] = page_latencies.size();
  result["warmup_pages"] = warmup_pages;
  result["deadline_ms"] = deadline_ms;
  result["page_p50_ms"] = percentile(page_latencies, 0.50);
  result["page_p95_ms"] = percentile(page_latencies, 0.95);
  result["post_warmup_measured_pages"] = measured_pages;
  result["post_warmup_deadline_misses"] = missed_pages;
  result["post_warmup_deadline_miss_ratio"] =
      measured_pages == 0 ? 0.0
                          : static_cast<double>(missed_pages) / measured_pages;
  return result;
}

json runRapidSeeks(
    KeypointOverlayBuffer* buffer, const std::vector<int64_t>& frames,
    size_t lookahead, int width, int height, std::chrono::milliseconds timeout,
    const std::shared_ptr<crimson::data::DataAccessScheduler>& scheduler) {
  require(!frames.empty(), "Rapid-seek workload is empty");
  const auto physical_before = snapshotMetrics();
  const auto scheduler_before = scheduler->metrics();
  std::string error;
  const auto started = Clock::now();
  for (int64_t frame : frames) {
    require(buffer->requestFrame(frame, width, height, true, &error),
            "Rapid seek was rejected: " + error);
  }
  const int64_t final_frame = frames.back();
  require(buffer->waitForFrame(final_frame, timeout),
          "Rapid-seek final frame did not become ready");
  const double final_readiness_ms = elapsedMs(started);
  scheduler->waitUntilIdle();
  size_t stale_visible = 0;
  for (size_t index = 0; index + 1 < frames.size(); ++index) {
    const int64_t frame = frames[index];
    if (frame >= final_frame &&
        frame <= final_frame + static_cast<int64_t>(lookahead)) {
      continue;
    }
    if (buffer->frame(frame)) ++stale_visible;
  }
  const auto scheduler_after = scheduler->metrics();
  return {{"requests", frames.size()},
          {"final_frame", final_frame},
          {"final_readiness_ms", final_readiness_ms},
          {"stale_visible_frames", stale_visible},
          {"cancelled_requests", scheduler_after.queue.cancelled_requests -
                                     scheduler_before.queue.cancelled_requests},
          {"discarded_completions",
           scheduler_after.queue.discarded_completions -
               scheduler_before.queue.discarded_completions},
          {"physical_transfer_upper_bound",
           physicalJson(snapshotMetrics() - physical_before)}};
}

std::vector<int64_t> forwardFrames(size_t frame_count, size_t count,
                                   size_t repetition) {
  count = std::min(count, frame_count);
  const size_t maximum_start = frame_count - count;
  const size_t start =
      maximum_start == 0 ? 0 : (repetition * count) % (maximum_start + 1);
  std::vector<int64_t> result;
  result.reserve(count);
  for (size_t index = 0; index < count; ++index)
    result.push_back(static_cast<int64_t>(start + index));
  return result;
}

std::vector<int64_t> reverseFrames(size_t frame_count, size_t count,
                                   size_t repetition) {
  count = std::min(count, frame_count);
  const size_t shift = std::min(repetition * count, frame_count - count);
  const size_t high = frame_count - 1 - shift;
  std::vector<int64_t> result;
  result.reserve(count);
  for (size_t index = 0; index < count; ++index)
    result.push_back(static_cast<int64_t>(high - index));
  return result;
}

std::shared_ptr<crimson::zarr::ArchiveContext> openArchive(
    const std::filesystem::path& path,
    std::map<std::string, std::shared_ptr<crimson::zarr::ArchiveContext>>*
        archives) {
  std::error_code error_code;
  const auto key =
      std::filesystem::absolute(path, error_code).lexically_normal().string();
  require(!error_code, "Could not normalize archive path: " + path.string());
  const auto found = archives->find(key);
  if (found != archives->end()) return found->second;
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(path, &error);
  require(archive != nullptr,
          "Could not open archive " + path.string() + ": " + error);
  archives->emplace(key, archive);
  return archive;
}

double currentFrameQueueMaximum(
    const crimson::data::DataAccessSchedulerMetrics& metrics) {
  return metrics
      .timing_by_priority[static_cast<size_t>(
          crimson::data::RequestPriority::CurrentFrame)]
      .maximum_queue_wait_ms;
}

json execute(const Options& options, const Workload& workload) {
  require(options.repetition < workload.repetitions,
          "Repetition is outside the frozen workload");
  const auto process_started = Clock::now();
  const auto physical_process_before = snapshotMetrics();
  std::map<std::string, std::shared_ptr<crimson::zarr::ArchiveContext>>
      archives;
  const auto archive_started = Clock::now();
  crimson::zarr::KeypointV2RepositoryOpenRequest request;
  request.raw_archive = openArchive(options.raw_store, &archives);
  request.raw_run = options.raw_run;
  request.quality_archive = openArchive(options.quality_store, &archives);
  request.quality_run = options.quality_run;
  request.body_frame_archive = openArchive(options.body_store, &archives);
  request.body_frame_run = options.body_run;
  request.expected_raw_manifest_digest = options.raw_digest;
  request.expected_quality_manifest_digest = options.quality_digest;
  request.expected_body_frame_manifest_digest = options.body_digest;
  if (options.mode == "refined") {
    request.refined_archive = openArchive(options.refined_store, &archives);
    request.refined_run = options.refined_run;
    request.expected_refined_manifest_digest = options.refined_digest;
  }
  request.allow_selector_ineligible = true;
  request.deep_validate_identity = options.deep_validate_identity;
  const double archive_open_ms = elapsedMs(archive_started);
  const auto repository_started = Clock::now();
  std::string error;
  crimson::zarr::KeypointV2RepositoryOpenMetrics open_metrics;
  auto repository =
      crimson::zarr::OpenKeypointV2Repository(request, &error, &open_metrics);
  require(repository != nullptr, "Keypoint repository open failed: " + error);
  const double repository_open_ms = elapsedMs(repository_started);
  auto* repository_metrics = repository.get();
  const auto descriptor = repository->v2Descriptor();
  require(descriptor.refined == (options.mode == "refined") &&
              descriptor.consolidated_metadata && descriptor.stable_identity &&
              descriptor.page_identity_validation &&
              descriptor.quality_payload_lazy,
          "Keypoint repository readiness contract was not established");
  require(open_metrics.fallback_metadata_reads == 0 &&
              open_metrics.fallback_dtype_opens == 0 &&
              open_metrics.quality_payload_reads == 0 &&
              descriptor.raw_offset_read_calls == 1 &&
              descriptor.selected_offset_read_calls ==
                  (descriptor.refined ? 1U : 0U) &&
              descriptor.quality_offset_read_calls == 0 &&
              descriptor.body_frame_offset_read_calls == 1,
          "Keypoint repository violated exact-schema, offset, or quality-lazy "
          "policy");

  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(
      workload.pending_capacity, workload.workers, workload.speculative_workers,
      workload.reserved_current_workers);
  KeypointOverlayBuffer buffer(scheduler, options.raw_store.string());
  require(buffer.open(std::move(repository), workload.lookahead_frames,
                      workload.cache_frames, &error),
          "Could not open keypoint presentation buffer: " + error);
  const int width = static_cast<int>(descriptor.selected.source_width);
  const int height = static_cast<int>(descriptor.selected.source_height);
  require(width > 0 && height > 0, "Keypoint source dimensions are invalid");
  const auto timeout = std::chrono::milliseconds(workload.frame_timeout_ms);

  const auto random = randomFrames(
      descriptor.selected.frame_count, workload.random_count,
      workload.random_seed + options.repetition * 0x100000001b3ULL);
  const auto rapid = randomFrames(
      descriptor.selected.frame_count, workload.rapid_seek_count,
      workload.rapid_seek_seed + options.repetition * 0x9e3779b9ULL);
  const auto forward =
      forwardFrames(descriptor.selected.frame_count, workload.traversal_frames,
                    options.repetition);
  const auto reverse =
      reverseFrames(descriptor.selected.frame_count, workload.traversal_frames,
                    options.repetition);

  const auto first_physical_before = snapshotMetrics();
  Digest first_digest;
  const auto first = readFrame(&buffer, random.front(), width, height, true,
                               timeout, &first_digest);
  const json first_json = {
      {"frame", random.front()},
      {"elapsed_ms", first.elapsed_ms},
      {"observations", first.observations},
      {"logical_digest", first_digest.finish()},
      {"physical", physicalJson(snapshotMetrics() - first_physical_before)}};
  const double readiness_ms = elapsedMs(process_started);

  const json random_first =
      runRandomPass(&buffer, random, width, height, timeout);
  const json random_warm =
      runRandomPass(&buffer, random, width, height, timeout);
  const json forward_json = runTraversal(
      &buffer, forward, workload.page_frames, workload.warmup_pages,
      workload.source_fps, width, height, timeout);
  const json reverse_json = runTraversal(
      &buffer, reverse, workload.page_frames, workload.warmup_pages,
      workload.source_fps, width, height, timeout);
  const json rapid_json =
      runRapidSeeks(&buffer, rapid, workload.lookahead_frames, width, height,
                    timeout, scheduler);

  const auto access_metrics = repository_metrics->accessMetrics();
  require(access_metrics.quality_payload_read_calls == 0,
          "Ordinary benchmark playback read quality payload arrays");
  const auto buffer_metrics = buffer.metrics();
  const auto close_started = Clock::now();
  buffer.close();
  const double close_ms = elapsedMs(close_started);
  const auto scheduler_metrics = scheduler->metrics();
  scheduler->shutdown();
  const uint64_t peak_rss = peakRssBytes();

  const double deadline_miss_ratio = std::max(
      forward_json.at("post_warmup_deadline_miss_ratio").get<double>(),
      reverse_json.at("post_warmup_deadline_miss_ratio").get<double>());
  std::vector<std::string> failures;
  auto gate = [&](bool passed, const std::string& failure) {
    if (!passed) failures.push_back(failure);
  };
  gate(readiness_ms <= workload.readiness_gate_ms,
       "first_presentation_readiness_ms");
  gate(random_warm.at("p95_ms").get<double>() <= workload.random_p95_gate_ms,
       "warm_random_frame_p95_ms");
  gate(currentFrameQueueMaximum(scheduler_metrics) <=
           workload.current_queue_gate_ms,
       "current_frame_queue_max_ms");
  gate(deadline_miss_ratio <= workload.deadline_miss_gate,
       "post_warmup_page_deadline_miss_ratio");
  gate(rapid_json.at("final_readiness_ms").get<double>() <=
           workload.rapid_seek_gate_ms,
       "rapid_seek_final_readiness_ms");
  gate(rapid_json.at("stale_visible_frames").get<size_t>() == 0,
       "zero_stale_publications");
  gate(close_ms <= static_cast<double>(workload.close_timeout_ms), "close_ms");
  gate(peak_rss <= workload.peak_rss_gate_bytes, "peak_rss_bytes");
  gate(access_metrics.quality_payload_read_calls == 0, "quality_payload_reads");
  gate(access_metrics.read_failures == 0 && buffer_metrics.failed_frames == 0,
       "read_failures");

  size_t configured_cache_bytes = 0;
  json archive_json = json::array();
  for (const auto& entry : archives) {
    configured_cache_bytes += entry.second->cachePoolBytes();
    archive_json.push_back(
        {{"path", entry.first},
         {"cache_pool_bytes", entry.second->cachePoolBytes()}});
  }
  return {{"schema_id", kResultSchema},
          {"schema_version", kResultSchemaVersion},
          {"status", failures.empty() ? "pass" : "fail"},
          {"classification", "full_duration_acceptance_process_trial"},
          {"profile_promotion_verdict", "pending_paired_repetitions"},
          {"crimson_commit", CRIMSON_GIT_COMMIT},
          {"worktree_dirty", CRIMSON_WORKTREE_DIRTY != 0},
          {"mode", options.mode},
          {"repetition", options.repetition},
          {"workload_path", options.workload_path.string()},
          {"workload_sha256", workload.digest},
          {"deep_validate_identity", options.deep_validate_identity},
          {"selected_run", descriptor.selected.run_id},
          {"selected_manifest_digest", descriptor.selected.manifest_digest},
          {"frame_count", descriptor.selected.frame_count},
          {"row_count", descriptor.selected.row_count},
          {"keypoint_count", descriptor.selected.keypoint_count},
          {"archives", std::move(archive_json)},
          {"configured_cache_bytes", configured_cache_bytes},
          {"archive_open_ms", archive_open_ms},
          {"repository_open_ms", repository_open_ms},
          {"first_presentation_readiness_ms", readiness_ms},
          {"first_presentation", first_json},
          {"random_process_first", random_first},
          {"random_warm", random_warm},
          {"forward_traversal", forward_json},
          {"reverse_traversal", reverse_json},
          {"rapid_seeks", rapid_json},
          {"close_ms", close_ms},
          {"peak_rss_bytes", peak_rss},
          {"open_metrics", openMetricsJson(open_metrics)},
          {"access_metrics", accessMetricsJson(access_metrics)},
          {"buffer_metrics", bufferMetricsJson(buffer_metrics)},
          {"scheduler_metrics", schedulerMetricsJson(scheduler_metrics)},
          {"process_physical",
           physicalJson(snapshotMetrics() - physical_process_before)},
          {"effective_deadline_miss_ratio", deadline_miss_ratio},
          {"gate_failures", failures}};
}

void selfTest() {
  const std::vector<int64_t> offsets{0, 2, 2, 3, 6};
  const std::vector<int64_t> frames{0, 0, 2, 3, 3, 3};
  const std::vector<uint64_t> keys{11, 12, 13, 14, 15, 16};
  std::string error;
  require(crimson::zarr::ValidateKeypointV2Offsets(offsets, 4, 6, &error),
          "Valid [2,0,1,3] offsets were rejected: " + error);
  require(crimson::zarr::ValidateKeypointV2FrameIndex(offsets, frames, 4, 6,
                                                      &error),
          "Valid [2,0,1,3] frame index was rejected: " + error);
  require(crimson::zarr::ValidateKeypointV2InstanceKeys(keys, 6, &error),
          "Valid multi-row instance keys were rejected: " + error);
  auto malformed = offsets;
  malformed[2] = 3;
  malformed[3] = 2;
  require(!crimson::zarr::ValidateKeypointV2Offsets(malformed, 4, 6, &error),
          "Nonmonotonic offsets were accepted");
  auto duplicate = keys;
  duplicate.back() = duplicate.front();
  require(!crimson::zarr::ValidateKeypointV2InstanceKeys(duplicate, 6, &error),
          "Duplicate instance keys were accepted");
  const auto forward = forwardFrames(20, 5, 1);
  const auto reverse = reverseFrames(20, 5, 1);
  require(forward == std::vector<int64_t>({5, 6, 7, 8, 9}),
          "Forward traversal generation changed");
  require(reverse == std::vector<int64_t>({14, 13, 12, 11, 10}),
          "Reverse traversal generation changed");
  require(percentile({1.0, 2.0, 3.0, 100.0}, 0.95) == 100.0,
          "Percentile reducer changed");
}

void usage(const char* program) {
  std::cerr
      << "Usage: " << program
      << " --mode raw|refined --raw-store PATH --raw-run RUN --raw-digest "
         "SHA256 --quality-store PATH --quality-run RUN --quality-digest "
         "SHA256 [--refined-store PATH --refined-run RUN --refined-digest "
         "SHA256] --body-store PATH --body-run RUN --body-digest SHA256 "
         "--workload JSON --repetition N --output JSON "
         "[--deep-validate-identity]\n"
      << "       " << program << " --self-test\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string error;
    const auto options = parseOptions(argc, argv, &error);
    if (!options) {
      usage(argv[0]);
      throw std::runtime_error(error);
    }
    if (options->self_test) {
      selfTest();
      std::cout << "keypoint_v2_long_duration_benchmark self-test: PASS\n";
      return 0;
    }
    const auto workload = loadWorkload(options->workload_path);
    const json result = execute(*options, workload);
    std::ofstream output(options->output_path);
    require(output.good(),
            "Could not open output: " + options->output_path.string());
    output << result.dump(2) << '\n';
    require(output.good(),
            "Could not write output: " + options->output_path.string());
    std::cout << "keypoint_v2_long_duration_benchmark: "
              << result.at("status").get<std::string>()
              << " output=" << options->output_path << '\n';
    return result.at("status") == "pass" ? 0 : 2;
  } catch (const std::exception& exception) {
    std::cerr << "[KeypointV2LongDuration] FAIL " << exception.what() << '\n';
    return 1;
  }
}
