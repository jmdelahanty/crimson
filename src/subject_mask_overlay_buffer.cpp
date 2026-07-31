#include "subject_mask_overlay_buffer.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
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
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> repository;
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::string archive_identity;
  crimson::data::SourceIdentity scheduler_source;
  bool stopping = false;
  size_t lookahead = 12;
  size_t capacity = 24;
  uint64_t generation = 1;
  int64_t last_request = -1;
  std::unordered_map<
      int64_t,
      std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>>
      cache;
  std::deque<int64_t> cache_order;
  SubjectMaskOverlayBufferMetrics metrics;
  crimson::zarr::SubjectMaskOverlayRepositoryMetrics repository_metrics;

  void clearCacheLocked() {
    metrics.released_payload_bytes += metrics.cached_payload_bytes;
    metrics.cached_payload_bytes = 0;
    cache.clear();
    cache_order.clear();
  }

  void publishLocked(
      int64_t frame,
      std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
          resolution) {
    const uint64_t payload_bytes = resolutionPayloadBytes(*resolution);
    const auto existing = cache.find(frame);
    if (existing == cache.end()) {
      cache_order.push_back(frame);
    } else {
      const uint64_t previous_bytes = resolutionPayloadBytes(*existing->second);
      metrics.cached_payload_bytes -=
          std::min(metrics.cached_payload_bytes, previous_bytes);
      metrics.released_payload_bytes += previous_bytes;
    }
    cache[frame] = std::move(resolution);
    metrics.cached_payload_bytes += payload_bytes;
    while (cache_order.size() > capacity) {
      const int64_t evicted = cache_order.front();
      cache_order.pop_front();
      const auto found = cache.find(evicted);
      if (found != cache.end()) {
        const uint64_t evicted_bytes = resolutionPayloadBytes(*found->second);
        metrics.cached_payload_bytes -=
            std::min(metrics.cached_payload_bytes, evicted_bytes);
        metrics.released_payload_bytes += evicted_bytes;
      }
      cache.erase(evicted);
    }
    metrics.peak_cached_payload_bytes =
        std::max(metrics.peak_cached_payload_bytes,
                 metrics.cached_payload_bytes);
    metrics.peak_cached_frames =
        std::max(metrics.peak_cached_frames, cache.size());
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
  close();
  if (!repository) {
    assignError(error, "Subject-mask repository is null");
    return false;
  }
  if (cache_capacity == 0) {
    assignError(error, "Subject-mask cache capacity must be positive");
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
    impl_->lookahead = std::min(lookahead_frames, cache_capacity - 1);
    impl_->capacity = cache_capacity;
    impl_->stopping = false;
    impl_->generation = 1;
    impl_->last_request = -1;
    impl_->metrics = {};
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
  impl_->scheduler_source = {};
  impl_->stopping = false;
  impl_->last_request = -1;
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
  crimson::data::AccessPattern access_pattern =
      discontinuity ? crimson::data::AccessPattern::RandomSeek
                    : crimson::data::AccessPattern::Paused;
  if (!discontinuity && impl_->last_request >= 0) {
    access_pattern = camera_frame >= impl_->last_request
                         ? crimson::data::AccessPattern::Forward
                         : crimson::data::AccessPattern::Reverse;
  }
  int64_t request_distance = 0;
  if (impl_->last_request >= 0) {
    request_distance = camera_frame >= impl_->last_request
                           ? camera_frame - impl_->last_request
                           : impl_->last_request - camera_frame;
  }
  const int64_t bounded_lookahead = static_cast<int64_t>(
      std::min(impl_->lookahead,
               static_cast<size_t>(std::numeric_limits<int64_t>::max() - 2)));
  const bool jumped =
      impl_->last_request >= 0 && request_distance > bounded_lookahead + 2;
  if (discontinuity || jumped) {
    ++impl_->generation;
    impl_->scheduler->cancelSource(impl_->scheduler_source);
    impl_->clearCacheLocked();
  }
  impl_->last_request = camera_frame;

  const int64_t frame_count =
      impl_->descriptor.camera_frame_count >
              static_cast<size_t>(std::numeric_limits<int64_t>::max())
          ? std::numeric_limits<int64_t>::max()
          : static_cast<int64_t>(impl_->descriptor.camera_frame_count);
  const int64_t direction =
      access_pattern == crimson::data::AccessPattern::Reverse ? -1 : 1;
  const int64_t lookahead =
      direction < 0
          ? bounded_lookahead
          : std::min(bounded_lookahead,
                     std::numeric_limits<int64_t>::max() - camera_frame);
  int64_t final_frame = camera_frame;
  if (direction < 0) {
    final_frame = std::max<int64_t>(0, camera_frame - lookahead);
  } else if (frame_count == 0 || camera_frame < frame_count) {
    final_frame = camera_frame + lookahead;
    if (frame_count > 0) {
      final_frame = std::min(final_frame, frame_count - 1);
    }
  }
  impl_->scheduler->retainSourceRange(impl_->scheduler_source,
                                      impl_->generation,
                                      {std::min(camera_frame, final_frame),
                                       std::max(camera_frame, final_frame)});
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
    const uint64_t request_generation = impl_->generation;
    crimson::data::DataRangeRequest data_request{
        impl_->scheduler_source,
        {frame, frame},
        crimson::data::FieldSelection::All(),
        frame == camera_frame ? crimson::data::RequestPriority::CurrentFrame
                              : crimson::data::RequestPriority::Speculative,
        access_pattern,
        request_generation};
    const auto outcome = impl_->scheduler->submit(
        std::move(data_request),
        [this, frame, full_frame_width, full_frame_height, request_generation](
            const crimson::data::ScheduledDataRequest &scheduled) {
          if (scheduled.cancellation.cancelled()) {
            return crimson::data::DataResultStatus::Stale;
          }
          const auto start = std::chrono::steady_clock::now();
          auto resolution = impl_->repository->resolveCameraFrame(
              frame, full_frame_width, full_frame_height);
          const double elapsed_ms =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - start)
                  .count();
          auto shared = std::make_shared<
              const crimson::zarr::SubjectMaskOverlayResolution>(
              std::move(resolution));
          std::lock_guard<std::mutex> callback_lock(impl_->mutex);
          impl_->metrics.maximum_resolve_ms =
              std::max(impl_->metrics.maximum_resolve_ms, elapsed_ms);
          if (scheduled.cancellation.cancelled() || impl_->stopping ||
              request_generation != impl_->generation) {
            ++impl_->metrics.discarded_results;
            impl_->condition.notify_all();
            return crimson::data::DataResultStatus::Stale;
          }
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
          impl_->publishLocked(frame, std::move(shared));
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
    if (frame == camera_frame && !outcome.accepted() &&
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
               impl_->scheduler->metrics().queue.pending_requests);
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
  return found == impl_->cache.end() ? nullptr : found->second;
}

crimson::zarr::SubjectMaskOverlayDescriptor
SubjectMaskOverlayBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

SubjectMaskOverlayBufferMetrics SubjectMaskOverlayBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}

crimson::zarr::SubjectMaskOverlayRepositoryMetrics
SubjectMaskOverlayBuffer::repositoryMetrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository ? impl_->repository->metrics()
                           : impl_->repository_metrics;
}
