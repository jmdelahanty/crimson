#include "subject_mask_overlay_buffer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

void assignError(std::string *destination, const std::string &value) {
  if (destination != nullptr) {
    *destination = value;
  }
}

void addPayloadBytes(uint64_t *total, size_t count, size_t element_size) {
  if (total == nullptr || count == 0 || element_size == 0) {
    return;
  }
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  if (count > maximum / element_size ||
      *total > maximum - static_cast<uint64_t>(count) * element_size) {
    *total = maximum;
    return;
  }
  *total += static_cast<uint64_t>(count) * element_size;
}

uint64_t resolutionPayloadBytes(
    const crimson::zarr::SubjectMaskOverlayResolution &resolution) {
  uint64_t total = sizeof(resolution);
  addPayloadBytes(&total, resolution.error.capacity(), sizeof(char));
  addPayloadBytes(&total, resolution.detections.capacity(),
                  sizeof(crimson::zarr::SubjectMaskOverlayDetection));
  for (const auto &detection : resolution.detections) {
    addPayloadBytes(&total, detection.components.capacity(),
                    sizeof(crimson::zarr::SubjectMaskOverlayComponent));
    for (const auto &component : detection.components) {
      addPayloadBytes(&total, component.label.capacity(), sizeof(char));
      if (component.mask) {
        total = total > std::numeric_limits<uint64_t>::max() -
                            sizeof(std::vector<uint8_t>)
                    ? std::numeric_limits<uint64_t>::max()
                    : total + sizeof(std::vector<uint8_t>);
        addPayloadBytes(&total, component.mask->capacity(), sizeof(uint8_t));
      }
      addPayloadBytes(&total, component.contour.capacity(),
                      sizeof(crimson::zarr::SubjectMaskOverlayPoint));
    }
  }
  return total;
}

} // namespace

struct SubjectMaskOverlayBuffer::Impl {
  struct CacheEntry {
    std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
        resolution;
    uint64_t payload_bytes = 0;
  };
  struct PendingEntry {
    uint64_t generation = 0;
    bool current_priority = false;
  };

  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> repository;
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::string archive_identity;
  crimson::data::SourceIdentity scheduler_source;
  bool stopping = false;
  SubjectMaskOverlayBufferPolicy policy;
  uint64_t generation = 1;
  int64_t last_request = -1;
  int64_t current_frame = -1;
  int playback_direction = 1;
  int64_t retained_first = 0;
  int64_t retained_last = -1;
  size_t effective_lookahead = 0;
  double resolve_latency_ewma_ms = 0.0;
  double resolved_payload_ewma_bytes = 0.0;
  uint64_t resolve_samples = 0;
  double total_resolve_ms = 0.0;
  std::unordered_map<int64_t, CacheEntry> cache;
  std::unordered_map<int64_t, PendingEntry> pending;
  // A far speculative frame evicted by a byte/item bound is not requested
  // again on every render of the same camera frame. It becomes eligible when
  // playback advances and capacity is released behind the new current frame.
  std::unordered_set<int64_t> suppressed_until_advance;
  SubjectMaskOverlayBufferMetrics metrics;
  crimson::zarr::SubjectMaskOverlayRepositoryMetrics repository_metrics;

  void eraseCacheLocked(int64_t frame, bool budget_eviction) {
    const auto found = cache.find(frame);
    if (found == cache.end()) return;
    metrics.cached_payload_bytes -=
        std::min(metrics.cached_payload_bytes, found->second.payload_bytes);
    metrics.released_payload_bytes += found->second.payload_bytes;
    ++metrics.cache_evictions;
    if (budget_eviction) {
      ++metrics.byte_budget_evictions;
      if (frame != current_frame && frame >= retained_first &&
          frame <= retained_last) {
        suppressed_until_advance.insert(frame);
      }
    }
    cache.erase(found);
  }

  void clearCacheLocked() {
    metrics.released_payload_bytes += metrics.cached_payload_bytes;
    metrics.cached_payload_bytes = 0;
    cache.clear();
    suppressed_until_advance.clear();
  }

  bool retainedLocked(int64_t frame) const {
    return retained_last >= retained_first && frame >= retained_first &&
           frame <= retained_last;
  }

  void pruneOutsideRetainedLocked() {
    for (auto item = cache.begin(); item != cache.end();) {
      if (retainedLocked(item->first) || item->first == current_frame) {
        ++item;
        continue;
      }
      const int64_t frame = item->first;
      ++item;
      eraseCacheLocked(frame, false);
    }
    for (auto item = pending.begin(); item != pending.end();) {
      if (retainedLocked(item->first)) {
        ++item;
      } else {
        item = pending.erase(item);
      }
    }
  }

  int64_t evictionCandidateLocked() const {
    int64_t candidate = -1;
    bool candidate_outside = false;
    uint64_t candidate_distance = 0;
    for (const auto &item : cache) {
      if (item.first == current_frame) continue;
      const bool outside = !retainedLocked(item.first);
      const uint64_t distance = item.first >= current_frame
                                    ? static_cast<uint64_t>(item.first - current_frame)
                                    : static_cast<uint64_t>(current_frame - item.first);
      if (candidate < 0 || (outside && !candidate_outside) ||
          (outside == candidate_outside && distance > candidate_distance)) {
        candidate = item.first;
        candidate_outside = outside;
        candidate_distance = distance;
      }
    }
    return candidate;
  }

  std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
  boundedResolutionLocked(
      int64_t frame,
      std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
          resolution) {
    const uint64_t payload_bytes = resolutionPayloadBytes(*resolution);
    if (policy.maximum_cached_payload_bytes == 0 ||
        payload_bytes <= policy.maximum_cached_payload_bytes) {
      return resolution;
    }
    auto failed = std::make_shared<crimson::zarr::SubjectMaskOverlayResolution>();
    failed->camera_frame = frame;
    failed->status = crimson::zarr::SubjectMaskOverlayStatus::ReadFailed;
    failed->error = "Decoded mask frame exceeds the configured retained-byte budget";
    ++metrics.oversized_result_rejections;
    metrics.last_error = failed->error;
    return failed;
  }

  std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
  publishLocked(
      int64_t frame,
      std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
          resolution) {
    resolution = boundedResolutionLocked(frame, std::move(resolution));
    const uint64_t payload_bytes = resolutionPayloadBytes(*resolution);
    const auto existing = cache.find(frame);
    if (existing != cache.end()) {
      const uint64_t previous_bytes = existing->second.payload_bytes;
      metrics.cached_payload_bytes -=
          std::min(metrics.cached_payload_bytes, previous_bytes);
      metrics.released_payload_bytes += previous_bytes;
    }
    cache[frame] = {resolution, payload_bytes};
    metrics.cached_payload_bytes += payload_bytes;
    while (cache.size() > policy.maximum_cached_frames ||
           (policy.maximum_cached_payload_bytes > 0 &&
            metrics.cached_payload_bytes >
                policy.maximum_cached_payload_bytes)) {
      const int64_t evicted = evictionCandidateLocked();
      if (evicted < 0) {
        // Even the compact admission-failure resolution may not fit a
        // deliberately tiny budget. The byte bound remains hard; telemetry
        // still exposes the rejection even though no snapshot can be retained.
        eraseCacheLocked(frame, true);
        break;
      }
      eraseCacheLocked(evicted, true);
    }
    metrics.peak_cached_payload_bytes =
        std::max(metrics.peak_cached_payload_bytes,
                 metrics.cached_payload_bytes);
    metrics.peak_cached_frames =
        std::max(metrics.peak_cached_frames, cache.size());
    return resolution;
  }

  size_t chunkFrameSpanLocked() const {
    if (descriptor.storage_chunk_rows == 0 || descriptor.row_count == 0 ||
        descriptor.camera_frame_count == 0) {
      return 1;
    }
    // storage_chunk_rows counts observations, not video frames. Convert using
    // this recording's mean observation density; the hard frame/byte caps are
    // still authoritative for bursts up to the per-frame observation limit.
    const long double span =
        static_cast<long double>(descriptor.storage_chunk_rows) *
        static_cast<long double>(descriptor.camera_frame_count) /
        static_cast<long double>(descriptor.row_count);
    if (!std::isfinite(static_cast<double>(span))) return 1;
    const long double bounded = std::min<long double>(
        std::max<long double>(1.0L, std::ceil(span)),
        static_cast<long double>(policy.maximum_lookahead_frames));
    return static_cast<size_t>(bounded);
  }

  uint64_t estimatedResolutionBytesLocked() const {
    if (resolved_payload_ewma_bytes > 0.0) {
      if (!std::isfinite(resolved_payload_ewma_bytes) ||
          resolved_payload_ewma_bytes >=
              static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
      }
      return std::max<uint64_t>(
          1, static_cast<uint64_t>(std::ceil(resolved_payload_ewma_bytes)));
    }
    const long double observations =
        descriptor.camera_frame_count > 0
            ? std::max<long double>(
                  1.0L, static_cast<long double>(descriptor.row_count) /
                            descriptor.camera_frame_count)
            : 1.0L;
    const long double components =
        std::max<size_t>(1, descriptor.component_labels.size());
    const long double pixels =
        static_cast<long double>(descriptor.mask_width) * descriptor.mask_height;
    const long double estimate = sizeof(crimson::zarr::SubjectMaskOverlayResolution) +
                                 observations * components *
                                     (pixels + sizeof(crimson::zarr::SubjectMaskOverlayComponent));
    if (estimate <= 1.0L) return 1;
    if (estimate >= std::numeric_limits<uint64_t>::max())
      return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(std::ceil(estimate));
  }

  size_t lookaheadForLocked(const SubjectMaskPlaybackDemand &playback) const {
    if (!playback.read_ahead || policy.maximum_lookahead_frames == 0 ||
        policy.maximum_cached_frames <= 1) {
      return 0;
    }
    const size_t minimum = std::min(
        policy.maximum_lookahead_frames,
        std::max(policy.minimum_lookahead_frames, chunkFrameSpanLocked()));
    size_t requested = minimum;
    if (playback.direction != SubjectMaskPlaybackDirection::Paused &&
        std::isfinite(playback.source_frames_per_second) &&
        playback.source_frames_per_second > 0.0) {
      const double rate = std::isfinite(playback.playback_rate)
                              ? std::abs(playback.playback_rate)
                              : 1.0;
      const double latency_seconds =
          resolve_latency_ewma_ms * policy.resolve_latency_multiplier / 1000.0;
      const double lead_seconds =
          std::max(policy.minimum_lead_seconds, latency_seconds);
      const double requested_frames =
          playback.source_frames_per_second * rate * lead_seconds;
      requested = !std::isfinite(requested_frames) ||
                          requested_frames >=
                              static_cast<double>(policy.maximum_lookahead_frames)
                      ? policy.maximum_lookahead_frames
                      : std::max<size_t>(
                            minimum, static_cast<size_t>(
                                         std::ceil(requested_frames)));
    }
    requested = std::min(requested, policy.maximum_lookahead_frames);
    requested = std::min(requested, policy.maximum_cached_frames - 1);
    if (policy.maximum_cached_payload_bytes > 0) {
      const uint64_t estimate = estimatedResolutionBytesLocked();
      const uint64_t admitted = estimate == 0
                                    ? 0
                                    : policy.maximum_cached_payload_bytes / estimate;
      requested = admitted > 0
                      ? std::min<uint64_t>(requested, admitted - 1)
                      : 0;
    }
    return requested;
  }
};

SubjectMaskOverlayBuffer::SubjectMaskOverlayBuffer(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    std::string archive_identity)
    : impl_(std::make_unique<Impl>()) {
  impl_->scheduler = scheduler ? std::move(scheduler)
                               : std::make_shared<
                                     crimson::data::DataAccessScheduler>(32, 1);
  impl_->archive_identity = std::move(archive_identity);
}

SubjectMaskOverlayBuffer::~SubjectMaskOverlayBuffer() { close(); }

bool SubjectMaskOverlayBuffer::open(
    std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> repository,
    size_t lookahead_frames, size_t cache_capacity, std::string *error) {
  SubjectMaskOverlayBufferPolicy policy;
  policy.minimum_lookahead_frames = lookahead_frames;
  policy.maximum_lookahead_frames = lookahead_frames;
  policy.maximum_cached_frames = cache_capacity;
  // Preserve the legacy overload's item-bounded behavior. Canonical playback
  // uses the policy overload below with an explicit decoded-byte bound.
  policy.maximum_cached_payload_bytes = 0;
  return open(std::move(repository), policy, error);
}

bool SubjectMaskOverlayBuffer::open(
    std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> repository,
    const SubjectMaskOverlayBufferPolicy &policy, std::string *error) {
  close();
  if (!repository) {
    assignError(error, "Subject-mask repository is null");
    return false;
  }
  if (policy.maximum_cached_frames == 0) {
    assignError(error, "Subject-mask cache capacity must be positive");
    return false;
  }
  crimson::zarr::SubjectMaskOverlayResolution minimum_failure;
  minimum_failure.error =
      "Decoded mask frame exceeds the configured retained-byte budget";
  const uint64_t minimum_failure_bytes =
      resolutionPayloadBytes(minimum_failure);
  if (policy.maximum_cached_payload_bytes > 0 &&
      policy.maximum_cached_payload_bytes < minimum_failure_bytes) {
    assignError(error,
                "Subject-mask retained-byte budget cannot hold an error result");
    return false;
  }
  if (!std::isfinite(policy.minimum_lead_seconds) ||
      policy.minimum_lead_seconds < 0.0 ||
      !std::isfinite(policy.resolve_latency_multiplier) ||
      policy.resolve_latency_multiplier < 0.0 ||
      policy.minimum_lookahead_frames > policy.maximum_lookahead_frames) {
    assignError(error, "Subject-mask read-ahead policy is invalid");
    return false;
  }
  if (!impl_->scheduler || !impl_->scheduler->running()) {
    assignError(error, "Subject-mask data scheduler is unavailable");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->descriptor = repository->descriptor();
    impl_->scheduler_source = {
        impl_->archive_identity.empty() ? "in_memory" : impl_->archive_identity,
        "subject_masks",
        impl_->descriptor.source_group + "/" + impl_->descriptor.run_name};
    impl_->repository = std::move(repository);
    impl_->policy = policy;
    impl_->policy.maximum_lookahead_frames =
        std::min(impl_->policy.maximum_lookahead_frames,
                 impl_->policy.maximum_cached_frames - 1);
    impl_->policy.minimum_lookahead_frames =
        std::min(impl_->policy.minimum_lookahead_frames,
                 impl_->policy.maximum_lookahead_frames);
    impl_->stopping = false;
    impl_->generation = 1;
    impl_->last_request = -1;
    impl_->current_frame = -1;
    impl_->playback_direction = 1;
    impl_->retained_first = 0;
    impl_->retained_last = -1;
    impl_->effective_lookahead = 0;
    impl_->resolve_latency_ewma_ms = 0.0;
    impl_->resolved_payload_ewma_bytes = 0.0;
    impl_->resolve_samples = 0;
    impl_->total_resolve_ms = 0.0;
    impl_->pending.clear();
    impl_->suppressed_until_advance.clear();
    impl_->metrics = {};
    impl_->metrics.maximum_cached_payload_bytes =
        impl_->policy.maximum_cached_payload_bytes;
    impl_->repository_metrics = {};
  }
  return true;
}

void SubjectMaskOverlayBuffer::close() {
  crimson::data::SourceIdentity scheduler_source;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    impl_->condition.notify_all();
    scheduler_source = impl_->scheduler_source;
  }
  if (scheduler_source.valid()) {
    impl_->scheduler->cancelSource(scheduler_source);
    impl_->scheduler->waitForSourceIdle(scheduler_source);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->repository) {
    impl_->repository_metrics = impl_->repository->metrics();
  }
  impl_->repository.reset();
  impl_->descriptor = {};
  impl_->clearCacheLocked();
  impl_->pending.clear();
  impl_->scheduler_source = {};
  impl_->stopping = false;
  impl_->last_request = -1;
  impl_->current_frame = -1;
  impl_->retained_first = 0;
  impl_->retained_last = -1;
  impl_->effective_lookahead = 0;
}

bool SubjectMaskOverlayBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr && impl_->scheduler &&
         impl_->scheduler->running() && !impl_->stopping;
}

bool SubjectMaskOverlayBuffer::requestFrame(int64_t camera_frame,
                                            int full_frame_width,
                                            int full_frame_height,
                                            bool discontinuity,
                                            std::string *error) {
  SubjectMaskPlaybackDemand playback;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    playback.read_ahead = impl_->policy.maximum_lookahead_frames > 0;
    if (impl_->last_request >= 0 && camera_frame < impl_->last_request) {
      playback.direction = SubjectMaskPlaybackDirection::Reverse;
    } else {
      playback.direction = SubjectMaskPlaybackDirection::Forward;
    }
    if (!discontinuity && impl_->last_request >= 0) {
      const uint64_t distance = camera_frame >= impl_->last_request
                                    ? static_cast<uint64_t>(camera_frame - impl_->last_request)
                                    : static_cast<uint64_t>(impl_->last_request - camera_frame);
      const uint64_t jump_threshold =
          impl_->policy.maximum_lookahead_frames >
                  std::numeric_limits<uint64_t>::max() - 2
              ? std::numeric_limits<uint64_t>::max()
              : static_cast<uint64_t>(
                    impl_->policy.maximum_lookahead_frames) + 2;
      if (distance > jump_threshold) {
        discontinuity = true;
      }
    }
  }
  return requestFrame(camera_frame, full_frame_width, full_frame_height,
                      playback, discontinuity, error);
}

bool SubjectMaskOverlayBuffer::requestFrame(
    int64_t camera_frame, int full_frame_width, int full_frame_height,
    const SubjectMaskPlaybackDemand &playback, bool discontinuity,
    std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Subject-mask buffer is not open");
    return false;
  }
  if (camera_frame < 0 || full_frame_width <= 0 || full_frame_height <= 0) {
    assignError(error, "Subject-mask frame request is invalid");
    return false;
  }
  ++impl_->metrics.requests;
  int requested_direction = impl_->playback_direction;
  if (playback.direction == SubjectMaskPlaybackDirection::Forward) {
    requested_direction = 1;
  } else if (playback.direction == SubjectMaskPlaybackDirection::Reverse) {
    requested_direction = -1;
  }
  if (discontinuity) {
    ++impl_->generation;
    impl_->scheduler->cancelSource(impl_->scheduler_source);
    impl_->pending.clear();
    impl_->clearCacheLocked();
  }
  if (playback.direction != SubjectMaskPlaybackDirection::Paused) {
    impl_->playback_direction = requested_direction;
  }
  const bool advanced = impl_->last_request != camera_frame;
  if (advanced) impl_->suppressed_until_advance.clear();
  impl_->last_request = camera_frame;
  impl_->current_frame = camera_frame;
  impl_->effective_lookahead = impl_->lookaheadForLocked(playback);

  const int64_t frame_count =
      impl_->descriptor.camera_frame_count >
              static_cast<size_t>(std::numeric_limits<int64_t>::max())
          ? std::numeric_limits<int64_t>::max()
          : static_cast<int64_t>(impl_->descriptor.camera_frame_count);
  const int64_t direction = impl_->playback_direction;
  const int64_t bounded_lookahead = static_cast<int64_t>(std::min<size_t>(
      impl_->effective_lookahead,
      static_cast<size_t>(std::numeric_limits<int64_t>::max() - 2)));
  const int64_t lookahead = direction < 0
                                ? bounded_lookahead
                                : std::min(bounded_lookahead,
                                           std::numeric_limits<int64_t>::max() -
                                               camera_frame);
  int64_t final_frame = camera_frame;
  if (direction < 0) {
    final_frame = std::max<int64_t>(0, camera_frame - lookahead);
  } else if (frame_count == 0 || camera_frame < frame_count) {
    final_frame = camera_frame + lookahead;
    if (frame_count > 0) {
      final_frame = std::min(final_frame, frame_count - 1);
    }
  }
  impl_->retained_first = std::min(camera_frame, final_frame);
  impl_->retained_last = std::max(camera_frame, final_frame);
  impl_->scheduler->retainSourceRange(impl_->scheduler_source,
                                      impl_->generation,
                                      {impl_->retained_first,
                                       impl_->retained_last});
  impl_->pruneOutsideRetainedLocked();
  if (impl_->cache.find(camera_frame) != impl_->cache.end()) {
    ++impl_->metrics.cache_hits;
  }
  for (int64_t frame = camera_frame;; frame += direction) {
    if (impl_->cache.find(frame) != impl_->cache.end()) {
      if (frame == final_frame) {
        break;
      }
      continue;
    }
    const bool current_priority = frame == camera_frame;
    if (!current_priority &&
        impl_->suppressed_until_advance.find(frame) !=
            impl_->suppressed_until_advance.end()) {
      if (frame == final_frame) break;
      continue;
    }
    // The shared scheduler is authoritative: it may evict a queued
    // speculative request to admit higher-priority work, and an evicted work
    // callback does not run. Re-submit unresolved demand so the scheduler can
    // deduplicate/promote live work or recover an evicted request.
    const uint64_t request_generation = impl_->generation;
    crimson::data::AccessPattern access_pattern =
        discontinuity ? crimson::data::AccessPattern::RandomSeek
                      : playback.direction == SubjectMaskPlaybackDirection::Paused
                            ? crimson::data::AccessPattern::Paused
                            : direction < 0
                                  ? crimson::data::AccessPattern::Reverse
                                  : crimson::data::AccessPattern::Forward;
    crimson::data::DataRangeRequest data_request{
        impl_->scheduler_source,
        {frame, frame},
        crimson::data::FieldSelection::All(),
        current_priority ? crimson::data::RequestPriority::CurrentFrame
                         : crimson::data::RequestPriority::Speculative,
        access_pattern,
        request_generation};
    impl_->pending[frame] = {request_generation, current_priority};
    const auto outcome = impl_->scheduler->submit(
        std::move(data_request),
        [this, frame, full_frame_width, full_frame_height, request_generation](
            const crimson::data::ScheduledDataRequest &scheduled) {
          if (scheduled.cancellation.cancelled()) {
            return crimson::data::DataResultStatus::Stale;
          }
          const auto start = std::chrono::steady_clock::now();
          crimson::zarr::SubjectMaskOverlayResolution resolution;
          try {
            resolution = impl_->repository->resolveCameraFrame(frame, full_frame_width, full_frame_height);
          } catch (const std::exception& exception) {
            resolution.camera_frame = frame;
            resolution.status = crimson::zarr::SubjectMaskOverlayStatus::ReadFailed;
            resolution.error = exception.what();
          } catch (...) {
            resolution.camera_frame = frame;
            resolution.status = crimson::zarr::SubjectMaskOverlayStatus::ReadFailed;
            resolution.error = "Mask reader threw an unknown exception";
          }
          if (resolution.camera_frame != frame) {
            resolution = {};
            resolution.camera_frame = frame;
            resolution.status =
                crimson::zarr::SubjectMaskOverlayStatus::ReadFailed;
            resolution.error =
                "Mask reader returned a frame different from its request";
          }
          const double elapsed_ms =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - start)
                  .count();
          auto shared = std::make_shared<
              const crimson::zarr::SubjectMaskOverlayResolution>(
              std::move(resolution));
          std::lock_guard<std::mutex> callback_lock(impl_->mutex);
          const auto pending = impl_->pending.find(frame);
          if (pending != impl_->pending.end() &&
              pending->second.generation == request_generation) {
            impl_->pending.erase(pending);
          }
          ++impl_->resolve_samples;
          impl_->total_resolve_ms += elapsed_ms;
          impl_->resolve_latency_ewma_ms =
              impl_->resolve_samples == 1
                  ? elapsed_ms
                  : impl_->resolve_latency_ewma_ms * 0.8 + elapsed_ms * 0.2;
          impl_->metrics.maximum_resolve_ms =
              std::max(impl_->metrics.maximum_resolve_ms, elapsed_ms);
          if (scheduled.cancellation.cancelled() || impl_->stopping ||
              request_generation != impl_->generation ||
              !impl_->retainedLocked(frame)) {
            ++impl_->metrics.discarded_results;
            impl_->condition.notify_all();
            return crimson::data::DataResultStatus::Stale;
          }
          const uint64_t decoded_payload_bytes = resolutionPayloadBytes(*shared);
          impl_->resolved_payload_ewma_bytes =
              impl_->resolve_samples == 1
                  ? static_cast<double>(decoded_payload_bytes)
                  : impl_->resolved_payload_ewma_bytes * 0.8 +
                        static_cast<double>(decoded_payload_bytes) * 0.2;
          shared = impl_->publishLocked(frame, std::move(shared));
          crimson::data::DataResultStatus status =
              crimson::data::DataResultStatus::Failed;
          switch (shared->status) {
          case crimson::zarr::SubjectMaskOverlayStatus::Mapped:
            ++impl_->metrics.resolved_frames;
            status = crimson::data::DataResultStatus::Ready;
            break;
          case crimson::zarr::SubjectMaskOverlayStatus::Missing:
          case crimson::zarr::SubjectMaskOverlayStatus::OutOfRange:
            ++impl_->metrics.missing_frames;
            status = crimson::data::DataResultStatus::Missing;
            break;
          case crimson::zarr::SubjectMaskOverlayStatus::InvalidDimensions:
          case crimson::zarr::SubjectMaskOverlayStatus::ReadFailed:
            ++impl_->metrics.failed_frames;
            impl_->metrics.last_error = shared->error;
            break;
          }
          impl_->condition.notify_all();
          return status;
        });
    ++impl_->metrics.scheduler_submissions;
    switch (outcome.status) {
    case crimson::data::DataRequestSubmitStatus::Duplicate:
      ++impl_->metrics.scheduler_duplicates;
      break;
    case crimson::data::DataRequestSubmitStatus::Promoted:
      ++impl_->metrics.scheduler_promotions;
      break;
    case crimson::data::DataRequestSubmitStatus::RejectedCapacity:
      ++impl_->metrics.scheduler_capacity_rejections;
      break;
    default:
      break;
    }
    if (!outcome.accepted()) {
      const auto pending = impl_->pending.find(frame);
      if (pending != impl_->pending.end() &&
          pending->second.generation == request_generation) {
        impl_->pending.erase(pending);
      }
    } else if (current_priority) {
      const auto pending = impl_->pending.find(frame);
      if (pending != impl_->pending.end()) {
        pending->second.current_priority = true;
      }
    }
    if (current_priority && !outcome.accepted() &&
        outcome.status !=
            crimson::data::DataRequestSubmitStatus::RejectedCapacity) {
      assignError(error, "Subject-mask scheduler rejected current frame");
      return false;
    }
    if (frame == final_frame) {
      break;
    }
  }
  impl_->metrics.peak_pending_frames =
      std::max(impl_->metrics.peak_pending_frames,
               impl_->pending.size());
  return true;
}

bool SubjectMaskOverlayBuffer::waitForFrame(
    int64_t camera_frame, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->stopping ||
           impl_->cache.find(camera_frame) != impl_->cache.end();
  }) && !impl_->stopping;
}

std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
SubjectMaskOverlayBuffer::frame(int64_t camera_frame) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->cache.find(camera_frame);
  return found == impl_->cache.end() ? nullptr : found->second.resolution;
}

crimson::zarr::SubjectMaskOverlayDescriptor
SubjectMaskOverlayBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

SubjectMaskOverlayBufferMetrics SubjectMaskOverlayBuffer::metrics() const {
  SubjectMaskOverlayBufferMetrics result;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  crimson::data::SourceIdentity source;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    result = impl_->metrics;
    result.cached_frames = impl_->cache.size();
    result.pending_frames = impl_->pending.size();
    result.effective_lookahead_frames = impl_->effective_lookahead;
    result.maximum_cached_payload_bytes =
        impl_->policy.maximum_cached_payload_bytes;
    result.current_frame = impl_->current_frame;
    result.current_frame_ready =
        impl_->cache.find(impl_->current_frame) != impl_->cache.end();
    result.average_resolve_ms =
        impl_->resolve_samples == 0
            ? 0.0
            : impl_->total_resolve_ms / impl_->resolve_samples;
    result.contiguous_ready_ahead = 0;
    if (result.current_frame_ready) {
      int64_t candidate = impl_->current_frame;
      while (result.contiguous_ready_ahead < impl_->effective_lookahead) {
        if ((impl_->playback_direction > 0 &&
             candidate == std::numeric_limits<int64_t>::max()) ||
            (impl_->playback_direction < 0 && candidate == 0)) {
          break;
        }
        candidate += impl_->playback_direction;
        if (impl_->cache.find(candidate) == impl_->cache.end()) break;
        ++result.contiguous_ready_ahead;
      }
    }
    scheduler = impl_->scheduler;
    source = impl_->scheduler_source;
  }
  if (scheduler && source.valid()) {
    const auto scheduler_metrics = scheduler->metrics();
    const auto found = std::find_if(
        scheduler_metrics.timing_by_source.begin(),
        scheduler_metrics.timing_by_source.end(),
        [&](const crimson::data::DataAccessSourceTimingMetrics &item) {
          return item.source == source;
        });
    if (found != scheduler_metrics.timing_by_source.end()) {
      const auto current_index = static_cast<size_t>(
          crimson::data::RequestPriority::CurrentFrame);
      const auto speculative_index = static_cast<size_t>(
          crimson::data::RequestPriority::Speculative);
      const auto &current = found->by_priority[current_index];
      const auto &speculative = found->by_priority[speculative_index];
      result.current_average_queue_wait_ms = current.averageQueueWaitMs();
      result.current_maximum_queue_wait_ms = current.maximum_queue_wait_ms;
      result.current_average_service_ms = current.averageServiceMs();
      result.current_maximum_service_ms = current.maximum_service_ms;
      result.speculative_average_queue_wait_ms =
          speculative.averageQueueWaitMs();
      result.speculative_maximum_queue_wait_ms =
          speculative.maximum_queue_wait_ms;
      result.speculative_average_service_ms =
          speculative.averageServiceMs();
      result.speculative_maximum_service_ms =
          speculative.maximum_service_ms;
    }
  }
  return result;
}

crimson::zarr::SubjectMaskOverlayRepositoryMetrics
SubjectMaskOverlayBuffer::repositoryMetrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository ? impl_->repository->metrics()
                           : impl_->repository_metrics;
}
