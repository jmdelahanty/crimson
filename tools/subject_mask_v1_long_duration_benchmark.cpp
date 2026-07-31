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
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data_access_scheduler.h"
#include "subject_mask_overlay_buffer.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_json.h"
#include "zarr/subject_mask_v1_contract.h"
#include "zarr/tensorstore_subject_mask_overlay_repository.h"

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
    "crimson.subject_mask_v1.long_duration_benchmark";
constexpr int kResultSchemaVersion = 1;

struct Options {
  bool self_test = false;
  std::filesystem::path store;
  std::string run;
  std::string manifest_payload_digest;
  std::filesystem::path workload_path;
  std::filesystem::path output_path;
  size_t repetition = 0;
  int frame_width = 0;
  int frame_height = 0;
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

struct FrameMeasurement {
  double elapsed_ms = 0.0;
  size_t observations = 0;
  uint64_t foreground_pixels = 0;
};

class Digest {
public:
  template <typename T> void scalar(const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    write(std::string_view(reinterpret_cast<const char *>(&value),
                           sizeof(value)));
  }

  void label(std::string_view value) {
    scalar(value.size());
    write(value);
  }

  void bytes(const std::vector<uint8_t> &values) {
    scalar(values.size());
    if (values.empty()) {
      return;
    }
    write(std::string_view(reinterpret_cast<const char *>(values.data()),
                           values.size()));
  }

  std::string finish() const {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint64_t value : states_) {
      output << std::setw(16) << value;
    }
    return output.str();
  }

private:
  void write(std::string_view values) {
    constexpr std::array<uint64_t, 4> primes = {
        1099511628211ULL, 1099511627791ULL, 1099511627689ULL, 1099511627563ULL};
    for (char byte : values) {
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

double elapsedMs(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<size_t>(
      std::max(0.0, std::ceil(fraction * values.size()) - 1.0));
  return values[std::min(rank, values.size() - 1)];
}

uint64_t splitMix64(uint64_t *state) {
  uint64_t value = (*state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

std::vector<int64_t> randomFrames(size_t frame_count, size_t count,
                                  uint64_t seed) {
  require(frame_count > 0, "The selected mask run has no frames");
  count = std::min(count, frame_count);
  std::unordered_set<int64_t> seen;
  std::vector<int64_t> result;
  result.reserve(count);
  while (result.size() < count) {
    const int64_t frame = static_cast<int64_t>(splitMix64(&seed) % frame_count);
    if (seen.insert(frame).second) {
      result.push_back(frame);
    }
  }
  return result;
}

std::vector<int64_t> forwardFrames(size_t frame_count, size_t count,
                                   size_t repetition) {
  count = std::min(count, frame_count);
  const size_t maximum_start = frame_count - count;
  const size_t start =
      maximum_start == 0 ? 0 : (repetition * count) % (maximum_start + 1);
  std::vector<int64_t> result;
  result.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    result.push_back(static_cast<int64_t>(start + index));
  }
  return result;
}

std::vector<int64_t> reverseFrames(size_t frame_count, size_t count,
                                   size_t repetition) {
  count = std::min(count, frame_count);
  const size_t shift = std::min(repetition * count, frame_count - count);
  const size_t high = frame_count - 1 - shift;
  std::vector<int64_t> result;
  result.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    result.push_back(static_cast<int64_t>(high - index));
  }
  return result;
}

int64_t counterValue(std::string_view name) {
  const auto metric =
      tensorstore::internal_metrics::GetMetricRegistry().Collect(name);
  if (!metric || metric->values.empty()) {
    return 0;
  }
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

TensorStoreMetrics operator-(const TensorStoreMetrics &after,
                             const TensorStoreMetrics &before) {
  return {after.file_reads - before.file_reads,
          after.file_batch_reads - before.file_batch_reads,
          after.file_bytes - before.file_bytes,
          after.cache_hits - before.cache_hits,
          after.cache_misses - before.cache_misses,
          after.cache_evictions - before.cache_evictions};
}

json physicalJson(const TensorStoreMetrics &metrics) {
  return {{"file_reads", metrics.file_reads},
          {"file_batch_reads", metrics.file_batch_reads},
          {"file_bytes", metrics.file_bytes},
          {"cache_hits", metrics.cache_hits},
          {"cache_misses", metrics.cache_misses},
          {"cache_evictions", metrics.cache_evictions}};
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

std::optional<size_t> parseSize(std::string_view value) {
  try {
    size_t consumed = 0;
    const auto parsed = std::stoull(std::string(value), &consumed);
    if (consumed != value.size() ||
        parsed > std::numeric_limits<size_t>::max()) {
      return std::nullopt;
    }
    return static_cast<size_t>(parsed);
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

bool parseFrameSize(std::string_view value, int *width, int *height) {
  const size_t separator = value.find('x');
  if (separator == std::string_view::npos) {
    return false;
  }
  const auto parsed_width = parseSize(value.substr(0, separator));
  const auto parsed_height = parseSize(value.substr(separator + 1));
  if (!parsed_width || !parsed_height || *parsed_width == 0 ||
      *parsed_height == 0 ||
      *parsed_width > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      *parsed_height > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  *width = static_cast<int>(*parsed_width);
  *height = static_cast<int>(*parsed_height);
  return true;
}

std::optional<Options> parseOptions(int argc, char **argv, std::string *error) {
  Options options;
  if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
    options.self_test = true;
    return options;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (index + 1 >= argc) {
      *error = "Missing value for " + argument;
      return std::nullopt;
    }
    const std::string value = argv[++index];
    if (argument == "--store") {
      options.store = value;
    } else if (argument == "--run") {
      options.run = value;
    } else if (argument == "--manifest-payload-digest") {
      options.manifest_payload_digest = value;
    } else if (argument == "--workload") {
      options.workload_path = value;
    } else if (argument == "--output") {
      options.output_path = value;
    } else if (argument == "--repetition") {
      const auto parsed = parseSize(value);
      if (!parsed) {
        *error = "Invalid repetition";
        return std::nullopt;
      }
      options.repetition = *parsed;
    } else if (argument == "--frame-size") {
      if (!parseFrameSize(value, &options.frame_width, &options.frame_height)) {
        *error = "Invalid frame size; expected WIDTHxHEIGHT";
        return std::nullopt;
      }
    } else {
      *error = "Unknown argument: " + argument;
      return std::nullopt;
    }
  }
  if (options.store.empty() || options.run.empty() ||
      options.manifest_payload_digest.empty() ||
      options.workload_path.empty() || options.output_path.empty() ||
      options.frame_width <= 0 || options.frame_height <= 0) {
    *error = "Explicit store, run, digest, frame size, workload, and output "
             "are required";
    return std::nullopt;
  }
  return options;
}

Workload loadWorkload(const std::filesystem::path &path) {
  std::ifstream input(path);
  require(input.good(), "Could not open workload: " + path.string());
  Workload result;
  input >> result.document;
  require(result.document.value("schema_id", "") ==
                  "crimson.subject_mask_v1.long_duration_workload" &&
              result.document.value("schema_version", 0) == 1,
          "Unsupported subject-mask workload schema");
  result.digest = crimson::zarr::CanonicalJsonSha256(result.document);
  result.repetitions = result.document.at("repetition_count");
  const auto &random = result.document.at("random_frames");
  result.random_count = random.at("count");
  result.random_seed = random.at("seed");
  const auto &seeks = result.document.at("rapid_seeks");
  result.rapid_seek_count = seeks.at("count");
  result.rapid_seek_seed = seeks.at("seed");
  const auto &traversal = result.document.at("traversal");
  result.traversal_frames = traversal.at("frames");
  result.page_frames = traversal.at("page_frames");
  result.source_fps = traversal.at("source_fps");
  result.warmup_pages = traversal.at("warmup_pages");
  const auto &scheduler = result.document.at("scheduler");
  result.pending_capacity = scheduler.at("pending_capacity");
  result.workers = scheduler.at("workers");
  result.speculative_workers = scheduler.at("maximum_speculative_workers");
  result.reserved_current_workers =
      scheduler.at("reserved_current_frame_workers");
  const auto &cache = result.document.at("presentation_cache");
  result.lookahead_frames = cache.at("lookahead_frames");
  result.cache_frames = cache.at("capacity_frames");
  const auto &timeouts = result.document.at("timeouts_ms");
  result.frame_timeout_ms = timeouts.at("frame");
  result.close_timeout_ms = timeouts.at("close");
  const auto &gates = result.document.at("gates");
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
          "Subject-mask workload contains inconsistent values");
  return result;
}

json timingJson(const crimson::data::DataAccessTimingMetrics &value) {
  return {{"started", value.started},
          {"completed", value.completed},
          {"queue_average_ms", value.averageQueueWaitMs()},
          {"queue_maximum_ms", value.maximum_queue_wait_ms},
          {"service_average_ms", value.averageServiceMs()},
          {"service_maximum_ms", value.maximum_service_ms},
          {"queue_wait_over_100_ms", value.queue_wait_over_100_ms},
          {"queue_wait_over_1000_ms", value.queue_wait_over_1000_ms},
          {"service_over_100_ms", value.service_over_100_ms},
          {"service_over_1000_ms", value.service_over_1000_ms}};
}

json schedulerMetricsJson(
    const crimson::data::DataAccessSchedulerMetrics &value) {
  json priorities = json::object();
  for (size_t index = 0; index < crimson::data::kDataRequestPriorityCount;
       ++index) {
    const auto priority = static_cast<crimson::data::RequestPriority>(index);
    priorities[crimson::data::requestPriorityName(priority)] =
        timingJson(value.timing_by_priority[index]);
  }
  json sources = json::array();
  for (const auto &source : value.timing_by_source) {
    json by_priority = json::object();
    for (size_t index = 0; index < crimson::data::kDataRequestPriorityCount;
         ++index) {
      if (source.by_priority[index].started == 0) {
        continue;
      }
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

json bufferMetricsJson(const SubjectMaskOverlayBufferMetrics &value) {
  return {
      {"requests", value.requests},
      {"cache_hits", value.cache_hits},
      {"resolved_frames", value.resolved_frames},
      {"missing_frames", value.missing_frames},
      {"failed_frames", value.failed_frames},
      {"discarded_results", value.discarded_results},
      {"cached_payload_bytes", value.cached_payload_bytes},
      {"peak_cached_payload_bytes", value.peak_cached_payload_bytes},
      {"released_payload_bytes", value.released_payload_bytes},
      {"peak_cached_frames", value.peak_cached_frames},
      {"peak_pending_frames", value.peak_pending_frames},
      {"maximum_resolve_ms", value.maximum_resolve_ms},
      {"scheduler_submissions", value.scheduler_submissions},
      {"scheduler_duplicates", value.scheduler_duplicates},
      {"scheduler_promotions", value.scheduler_promotions},
      {"scheduler_capacity_rejections", value.scheduler_capacity_rejections},
      {"last_error", value.last_error}};
}

json repositoryMetricsJson(
    const crimson::zarr::SubjectMaskOverlayRepositoryMetrics &value) {
  return {
      {"open_total_ms", value.open_total_ms},
      {"catalog_ms", value.catalog_ms},
      {"mapping_read_ms", value.mapping_read_ms},
      {"storage_open_ms", value.storage_open_ms},
      {"frame_offset_reads", value.frame_offset_reads},
      {"frame_index_source_bytes", value.frame_index_source_bytes},
      {"frame_index_retained_bytes", value.frame_index_retained_bytes},
      {"metadata_decoded_bytes", value.metadata_decoded_bytes},
      {"metadata_retained_bytes", value.metadata_retained_bytes},
      {"derived_metric_payload_reads", value.derived_metric_payload_reads},
      {"roi_image_open_attempts", value.roi_image_open_attempts},
      {"demand_chunk_loads", value.demand_chunk_loads},
      {"prefetched_chunk_loads", value.prefetched_chunk_loads},
      {"chunk_cache_hits", value.chunk_cache_hits},
      {"prefetch_requests", value.prefetch_requests},
      {"chunk_evictions", value.chunk_evictions},
      {"chunk_load_failures", value.chunk_load_failures},
      {"logical_chunk_source_bytes", value.chunk_source_bytes_read},
      {"sparse_retained_bytes_produced", value.chunk_retained_bytes_produced},
      {"cached_sparse_bytes", value.cached_payload_bytes},
      {"peak_cached_sparse_bytes", value.peak_cached_payload_bytes},
      {"evicted_sparse_bytes", value.evicted_payload_bytes},
      {"cached_chunks", value.cached_chunks},
      {"peak_cached_chunks", value.peak_cached_chunks},
      {"chunk_read_ms", value.chunk_read_ms},
      {"chunk_convert_ms", value.chunk_convert_ms},
      {"maximum_chunk_read_ms", value.maximum_chunk_read_ms},
      {"maximum_chunk_convert_ms", value.maximum_chunk_convert_ms}};
}

void digestResolution(const crimson::zarr::SubjectMaskOverlayResolution &value,
                      Digest *digest, uint64_t *foreground_pixels) {
  digest->scalar(value.camera_frame);
  digest->scalar(static_cast<uint8_t>(value.status));
  digest->scalar(value.detections.size());
  for (const auto &detection : value.detections) {
    digest->scalar(detection.instance_key);
    digest->scalar(detection.source_crop_row_id);
    digest->scalar(detection.roi_x);
    digest->scalar(detection.roi_y);
    digest->scalar(detection.roi_width);
    digest->scalar(detection.roi_height);
    digest->scalar(detection.components.size());
    for (const auto &component : detection.components) {
      digest->label(component.label);
      digest->scalar(component.channel_index);
      digest->scalar(component.present);
      digest->scalar(component.mask_width);
      digest->scalar(component.mask_height);
      const bool has_mask = component.mask != nullptr;
      digest->scalar(has_mask);
      if (has_mask) {
        digest->bytes(*component.mask);
        *foreground_pixels += static_cast<uint64_t>(
            std::count_if(component.mask->begin(), component.mask->end(),
                          [](uint8_t pixel) { return pixel != 0; }));
      }
    }
  }
}

FrameMeasurement readFrame(SubjectMaskOverlayBuffer *buffer, int64_t frame,
                           int width, int height, bool discontinuity,
                           std::chrono::milliseconds timeout, Digest *digest) {
  const auto started = Clock::now();
  std::string error;
  require(buffer->requestFrame(frame, width, height, discontinuity, &error),
          "Frame request failed: " + error);
  require(buffer->waitForFrame(frame, timeout),
          "Timed out waiting for subject-mask frame " + std::to_string(frame));
  const auto value = buffer->frame(frame);
  require(value != nullptr, "Requested subject-mask frame was not published");
  require(value->status == crimson::zarr::SubjectMaskOverlayStatus::Mapped ||
              value->status == crimson::zarr::SubjectMaskOverlayStatus::Missing,
          "Subject-mask frame resolution failed: " + value->error);
  std::unordered_set<uint64_t> keys;
  for (const auto &detection : value->detections) {
    require(keys.insert(detection.instance_key).second,
            "Subject-mask frame contains duplicate instance keys");
    require(std::isfinite(detection.roi_x) && std::isfinite(detection.roi_y) &&
                std::isfinite(detection.roi_width) &&
                std::isfinite(detection.roi_height) &&
                detection.roi_width > 0.0 && detection.roi_height > 0.0,
            "Subject-mask frame contains invalid crop placement");
  }
  uint64_t foreground_pixels = 0;
  digestResolution(*value, digest, &foreground_pixels);
  return {elapsedMs(started), value->detections.size(), foreground_pixels};
}

json latencySummary(const std::vector<double> &latencies, size_t observations,
                    uint64_t foreground_pixels, const std::string &digest,
                    const TensorStoreMetrics &physical) {
  double total = 0.0;
  for (double value : latencies) {
    total += value;
  }
  return {{"requests", latencies.size()},
          {"observations", observations},
          {"foreground_pixels", foreground_pixels},
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

json runRandomPass(SubjectMaskOverlayBuffer *buffer,
                   const std::vector<int64_t> &frames, int width, int height,
                   std::chrono::milliseconds timeout) {
  const auto physical_before = snapshotMetrics();
  std::vector<double> latencies;
  size_t observations = 0;
  uint64_t foreground_pixels = 0;
  Digest digest;
  for (int64_t frame : frames) {
    const auto measured =
        readFrame(buffer, frame, width, height, true, timeout, &digest);
    latencies.push_back(measured.elapsed_ms);
    observations += measured.observations;
    foreground_pixels += measured.foreground_pixels;
  }
  return latencySummary(latencies, observations, foreground_pixels,
                        digest.finish(), snapshotMetrics() - physical_before);
}

json runTraversal(SubjectMaskOverlayBuffer *buffer,
                  const std::vector<int64_t> &frames, size_t page_frames,
                  size_t warmup_pages, double source_fps, int width, int height,
                  std::chrono::milliseconds timeout) {
  const auto physical_before = snapshotMetrics();
  std::vector<double> frame_latencies;
  std::vector<double> page_latencies;
  size_t observations = 0;
  uint64_t foreground_pixels = 0;
  Digest digest;
  auto page_started = Clock::now();
  for (size_t index = 0; index < frames.size(); ++index) {
    const auto measured = readFrame(buffer, frames[index], width, height,
                                    index == 0, timeout, &digest);
    frame_latencies.push_back(measured.elapsed_ms);
    observations += measured.observations;
    foreground_pixels += measured.foreground_pixels;
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
    if (page_latencies[index] > deadline_ms) {
      ++missed_pages;
    }
  }
  json result =
      latencySummary(frame_latencies, observations, foreground_pixels,
                     digest.finish(), snapshotMetrics() - physical_before);
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
    SubjectMaskOverlayBuffer *buffer, const std::vector<int64_t> &frames,
    size_t lookahead, int width, int height, std::chrono::milliseconds timeout,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler) {
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
    if (buffer->frame(frame)) {
      ++stale_visible;
    }
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

double currentFrameQueueMaximum(
    const crimson::data::DataAccessSchedulerMetrics &metrics) {
  return metrics
      .timing_by_priority[static_cast<size_t>(
          crimson::data::RequestPriority::CurrentFrame)]
      .maximum_queue_wait_ms;
}

json execute(const Options &options, const Workload &workload) {
  require(options.repetition < workload.repetitions,
          "Repetition is outside the frozen workload");
  const auto process_started = Clock::now();
  const auto physical_process_before = snapshotMetrics();

  const auto archive_started = Clock::now();
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(options.store, &error);
  require(archive != nullptr,
          "Could not open archive " + options.store.string() + ": " + error);
  const double archive_open_ms = elapsedMs(archive_started);

  const auto repository_started = Clock::now();
  auto repository = crimson::zarr::OpenSubjectMaskOverlayRepository(
      archive,
      crimson::zarr::SubjectMaskOverlayOpenOptions{
          options.run, options.manifest_payload_digest, true, true},
      &error);
  require(repository != nullptr,
          "Subject-mask repository open failed: " + error);
  const double repository_open_ms = elapsedMs(repository_started);
  const auto descriptor = repository->descriptor();
  const auto open_metrics = repository->metrics();
  require(descriptor.strict_v1 &&
              descriptor.run_manifest_payload_digest ==
                  options.manifest_payload_digest &&
              descriptor.camera_frame_count > 0 && descriptor.row_count > 0 &&
              descriptor.mask_width > 0 && descriptor.mask_height > 0 &&
              open_metrics.frame_offset_reads == 1 &&
              open_metrics.fallback_frame_index_builds == 0 &&
              open_metrics.derived_metric_payload_reads == 0 &&
              open_metrics.roi_image_open_attempts == 0,
          "Subject-mask exact-schema readiness contract was not established");

  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(
      workload.pending_capacity, workload.workers, workload.speculative_workers,
      workload.reserved_current_workers);
  SubjectMaskOverlayBuffer buffer(scheduler, options.store.string());
  require(buffer.open(std::move(repository), workload.lookahead_frames,
                      workload.cache_frames, &error),
          "Could not open subject-mask presentation buffer: " + error);
  const auto timeout = std::chrono::milliseconds(workload.frame_timeout_ms);

  const auto random = randomFrames(
      descriptor.camera_frame_count, workload.random_count,
      workload.random_seed + options.repetition * 0x100000001b3ULL);
  const auto rapid = randomFrames(
      descriptor.camera_frame_count, workload.rapid_seek_count,
      workload.rapid_seek_seed + options.repetition * 0x9e3779b9ULL);
  const auto forward =
      forwardFrames(descriptor.camera_frame_count, workload.traversal_frames,
                    options.repetition);
  const auto reverse =
      reverseFrames(descriptor.camera_frame_count, workload.traversal_frames,
                    options.repetition);

  const auto first_physical_before = snapshotMetrics();
  Digest first_digest;
  const auto first =
      readFrame(&buffer, random.front(), options.frame_width,
                options.frame_height, true, timeout, &first_digest);
  const json first_json = {
      {"frame", random.front()},
      {"elapsed_ms", first.elapsed_ms},
      {"observations", first.observations},
      {"foreground_pixels", first.foreground_pixels},
      {"logical_digest", first_digest.finish()},
      {"physical", physicalJson(snapshotMetrics() - first_physical_before)}};
  const double readiness_ms = elapsedMs(process_started);

  const json random_first = runRandomPass(&buffer, random, options.frame_width,
                                          options.frame_height, timeout);
  const json random_warm = runRandomPass(&buffer, random, options.frame_width,
                                         options.frame_height, timeout);
  const json forward_json = runTraversal(
      &buffer, forward, workload.page_frames, workload.warmup_pages,
      workload.source_fps, options.frame_width, options.frame_height, timeout);
  const json reverse_json = runTraversal(
      &buffer, reverse, workload.page_frames, workload.warmup_pages,
      workload.source_fps, options.frame_width, options.frame_height, timeout);
  const json rapid_json = runRapidSeeks(
      &buffer, rapid, workload.lookahead_frames, options.frame_width,
      options.frame_height, timeout, scheduler);

  const auto buffer_metrics = buffer.metrics();
  const auto close_started = Clock::now();
  buffer.close();
  const double close_ms = elapsedMs(close_started);
  const auto repository_metrics = buffer.repositoryMetrics();
  const auto scheduler_metrics = scheduler->metrics();
  scheduler->shutdown();
  const uint64_t peak_rss = peakRssBytes();

  const double deadline_miss_ratio = std::max(
      forward_json.at("post_warmup_deadline_miss_ratio").get<double>(),
      reverse_json.at("post_warmup_deadline_miss_ratio").get<double>());
  std::vector<std::string> failures;
  const auto gate = [&](bool passed, const std::string &failure) {
    if (!passed) {
      failures.push_back(failure);
    }
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
  gate(repository_metrics.frame_offset_reads == 1,
       "exactly_one_frame_offset_read");
  gate(repository_metrics.derived_metric_payload_reads == 0,
       "derived_metric_payload_reads");
  gate(repository_metrics.roi_image_open_attempts == 0,
       "roi_image_open_attempts");
  gate(repository_metrics.chunk_load_failures == 0 &&
           buffer_metrics.failed_frames == 0,
       "read_failures");

  return {{"schema_id", kResultSchema},
          {"schema_version", kResultSchemaVersion},
          {"status", failures.empty() ? "pass" : "fail"},
          {"classification", "integration_read_harness_process_trial"},
          {"profile_promotion_verdict", "not_evaluated"},
          {"crimson_commit", CRIMSON_GIT_COMMIT},
          {"worktree_dirty", CRIMSON_WORKTREE_DIRTY != 0},
          {"repetition", options.repetition},
          {"workload_path", options.workload_path.string()},
          {"workload_sha256", workload.digest},
          {"store", options.store.string()},
          {"selected_run", descriptor.run_name},
          {"manifest_payload_digest", descriptor.run_manifest_payload_digest},
          {"frame_size", {options.frame_width, options.frame_height}},
          {"frame_count", descriptor.camera_frame_count},
          {"row_count", descriptor.row_count},
          {"component_count", descriptor.component_labels.size()},
          {"mask_size", {descriptor.mask_width, descriptor.mask_height}},
          {"storage_chunk_rows", descriptor.storage_chunk_rows},
          {"cache_pool_bytes", archive->cachePoolBytes()},
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
          {"repository_metrics", repositoryMetricsJson(repository_metrics)},
          {"buffer_metrics", bufferMetricsJson(buffer_metrics)},
          {"scheduler_metrics", schedulerMetricsJson(scheduler_metrics)},
          {"process_physical",
           physicalJson(snapshotMetrics() - physical_process_before)},
          {"effective_deadline_miss_ratio", deadline_miss_ratio},
          {"gate_failures", failures}};
}

void selfTest() {
  const std::vector<int64_t> offsets = {0, 2, 2, 3, 6};
  const std::vector<int64_t> frames = {0, 0, 2, 3, 3, 3};
  const std::vector<uint64_t> keys = {11, 12, 13, 14, 15, 16};
  std::string error;
  require(crimson::zarr::ValidateSubjectMaskV1FrameIndex(offsets, frames, 4, 6,
                                                         &error),
          "Valid [2,0,1,3] frame index was rejected: " + error);
  require(crimson::zarr::ValidateSubjectMaskV1InstanceKeys(keys, 6, &error),
          "Valid multi-row keys were rejected: " + error);
  auto duplicate = keys;
  duplicate.back() = duplicate.front();
  require(
      !crimson::zarr::ValidateSubjectMaskV1InstanceKeys(duplicate, 6, &error),
      "Duplicate subject-mask keys were accepted");
  require(forwardFrames(20, 5, 1) == std::vector<int64_t>({5, 6, 7, 8, 9}),
          "Forward traversal generation changed");
  require(reverseFrames(20, 5, 1) == std::vector<int64_t>({14, 13, 12, 11, 10}),
          "Reverse traversal generation changed");
  require(percentile({1.0, 2.0, 3.0, 100.0}, 0.95) == 100.0,
          "Percentile reducer changed");
  int width = 0;
  int height = 0;
  require(parseFrameSize("4512x4512", &width, &height) && width == 4512 &&
              height == 4512,
          "Frame-size parser changed");
}

void usage(const char *program) {
  std::cerr << "Usage: " << program
            << " --store PATH --run RUN --manifest-payload-digest SHA256 "
               "--frame-size WIDTHxHEIGHT --workload JSON --repetition N "
               "--output JSON\n"
            << "       " << program << " --self-test\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    std::string error;
    const auto options = parseOptions(argc, argv, &error);
    if (!options) {
      usage(argv[0]);
      throw std::runtime_error(error);
    }
    if (options->self_test) {
      selfTest();
      std::cout << "subject_mask_v1_long_duration_benchmark self-test: PASS\n";
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
    std::cout << "subject_mask_v1_long_duration_benchmark: "
              << result.at("status").get<std::string>()
              << " output=" << options->output_path << '\n';
    return result.at("status") == "pass" ? 0 : 2;
  } catch (const std::exception &exception) {
    std::cerr << "[SubjectMaskV1LongDuration] FAIL " << exception.what()
              << '\n';
    return 1;
  }
}
