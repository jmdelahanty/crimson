#include "keypoint_overlay_buffer.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {

void assignError(std::string* destination, const std::string& value) {
  if (destination) *destination = value;
}

}  // namespace

struct KeypointOverlayBuffer::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::zarr::KeypointOverlayRepository> repository;
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::string archive_identity;
  crimson::data::SourceIdentity scheduler_source;
  bool stopping = false;
  size_t lookahead = 12;
  size_t capacity = 24;
  uint64_t generation = 1;
  int64_t last_request = -1;
  std::unordered_map<
      int64_t, std::shared_ptr<const crimson::zarr::KeypointOverlayResolution>>
      cache;
  std::deque<int64_t> cache_order;
  KeypointOverlayBufferMetrics metrics;

  void clearCacheLocked() {
    cache.clear();
    cache_order.clear();
  }

  void publishLocked(
      int64_t frame,
      std::shared_ptr<const crimson::zarr::KeypointOverlayResolution> value) {
    if (cache.find(frame) == cache.end()) cache_order.push_back(frame);
    cache[frame] = std::move(value);
    while (cache_order.size() > capacity) {
      cache.erase(cache_order.front());
      cache_order.pop_front();
    }
    metrics.peak_cached_frames =
        std::max(metrics.peak_cached_frames, cache.size());
  }
};

KeypointOverlayBuffer::KeypointOverlayBuffer(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    std::string archive_identity)
    : impl_(std::make_unique<Impl>()) {
  impl_->scheduler = scheduler
                         ? std::move(scheduler)
                         : std::make_shared<crimson::data::DataAccessScheduler>(
                               32, 1);
  impl_->archive_identity = std::move(archive_identity);
}

KeypointOverlayBuffer::~KeypointOverlayBuffer() { close(); }

bool KeypointOverlayBuffer::open(
    std::unique_ptr<crimson::zarr::KeypointOverlayRepository> repository,
    size_t lookahead_frames, size_t cache_capacity, std::string* error) {
  close();
  if (!repository) {
    assignError(error, "Keypoint repository is null");
    return false;
  }
  if (cache_capacity == 0) {
    assignError(error, "Keypoint cache capacity must be positive");
    return false;
  }
  if (!impl_->scheduler || !impl_->scheduler->running()) {
    assignError(error, "Keypoint data scheduler is unavailable");
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->descriptor = repository->descriptor();
  impl_->scheduler_source = {
      impl_->archive_identity.empty() ? "in_memory" : impl_->archive_identity,
      "keypoints",
      impl_->descriptor.source_group + "/" + impl_->descriptor.run_name};
  impl_->repository = std::move(repository);
  impl_->lookahead = std::min(lookahead_frames, cache_capacity - 1);
  impl_->capacity = cache_capacity;
  impl_->stopping = false;
  impl_->generation = 1;
  impl_->last_request = -1;
  impl_->metrics = {};
  return true;
}

void KeypointOverlayBuffer::close() {
  crimson::data::SourceIdentity source;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    impl_->condition.notify_all();
    source = impl_->scheduler_source;
  }
  if (source.valid()) {
    impl_->scheduler->cancelSource(source);
    impl_->scheduler->waitForSourceIdle(source);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->repository.reset();
  impl_->descriptor = {};
  impl_->scheduler_source = {};
  impl_->clearCacheLocked();
  impl_->stopping = false;
  impl_->last_request = -1;
}

bool KeypointOverlayBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository && !impl_->stopping && impl_->scheduler &&
         impl_->scheduler->running();
}

bool KeypointOverlayBuffer::requestFrame(int64_t camera_frame,
                                         int full_frame_width,
                                         int full_frame_height,
                                         bool discontinuity,
                                         std::string* error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Keypoint buffer is not open");
    return false;
  }
  if (camera_frame < 0 || full_frame_width <= 0 || full_frame_height <= 0) {
    assignError(error, "Keypoint frame request is invalid");
    return false;
  }
  ++impl_->metrics.requests;
  crimson::data::AccessPattern pattern =
      discontinuity ? crimson::data::AccessPattern::RandomSeek
                    : crimson::data::AccessPattern::Paused;
  if (!discontinuity && impl_->last_request >= 0) {
    pattern = camera_frame >= impl_->last_request
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
      impl_->last_request >= 0 &&
      request_distance > bounded_lookahead + 2;
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
      pattern == crimson::data::AccessPattern::Reverse ? -1 : 1;
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
    if (frame_count > 0) final_frame = std::min(final_frame, frame_count - 1);
  }
  impl_->scheduler->retainSourceRange(impl_->scheduler_source,
                                      impl_->generation,
                                      {std::min(camera_frame, final_frame),
                                       std::max(camera_frame, final_frame)});
  if (impl_->cache.find(camera_frame) != impl_->cache.end())
    ++impl_->metrics.cache_hits;

  for (int64_t frame = camera_frame;; frame += direction) {
    if (impl_->cache.find(frame) == impl_->cache.end()) {
      const uint64_t generation = impl_->generation;
      crimson::data::DataRangeRequest request{
          impl_->scheduler_source,
          {frame, frame},
          crimson::data::FieldSelection::All(),
          frame == camera_frame ? crimson::data::RequestPriority::CurrentFrame
                                : crimson::data::RequestPriority::Speculative,
          pattern,
          generation};
      const auto outcome = impl_->scheduler->submit(
          std::move(request),
          [this, frame, full_frame_width, full_frame_height, generation](
              const crimson::data::ScheduledDataRequest& scheduled) {
            if (scheduled.cancellation.cancelled())
              return crimson::data::DataResultStatus::Stale;
            const auto started = std::chrono::steady_clock::now();
            crimson::zarr::KeypointOverlayResolution resolved;
            try {
              resolved = impl_->repository->resolveCameraFrame(
                  frame, full_frame_width, full_frame_height);
            } catch (const std::exception& exception) {
              resolved.camera_frame = frame;
              resolved.status = crimson::zarr::KeypointOverlayStatus::ReadFailed;
              resolved.error = exception.what();
            } catch (...) {
              resolved.camera_frame = frame;
              resolved.status = crimson::zarr::KeypointOverlayStatus::ReadFailed;
              resolved.error = "Overlay reader threw an unknown exception";
            }
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            auto shared = std::make_shared<
                const crimson::zarr::KeypointOverlayResolution>(
                std::move(resolved));
            std::lock_guard<std::mutex> callback_lock(impl_->mutex);
            impl_->metrics.maximum_resolve_ms =
                std::max(impl_->metrics.maximum_resolve_ms, elapsed_ms);
            if (scheduled.cancellation.cancelled() || impl_->stopping ||
                generation != impl_->generation) {
              ++impl_->metrics.discarded_results;
              impl_->condition.notify_all();
              return crimson::data::DataResultStatus::Stale;
            }
            auto status = crimson::data::DataResultStatus::Failed;
            switch (shared->status) {
              case crimson::zarr::KeypointOverlayStatus::Mapped:
                ++impl_->metrics.resolved_frames;
                status = crimson::data::DataResultStatus::Ready;
                break;
              case crimson::zarr::KeypointOverlayStatus::Missing:
              case crimson::zarr::KeypointOverlayStatus::OutOfRange:
                ++impl_->metrics.missing_frames;
                status = crimson::data::DataResultStatus::Missing;
                break;
              case crimson::zarr::KeypointOverlayStatus::InvalidDimensions:
              case crimson::zarr::KeypointOverlayStatus::ReadFailed:
                ++impl_->metrics.failed_frames;
                impl_->metrics.last_error = shared->error;
                break;
            }
            impl_->publishLocked(frame, std::move(shared));
            impl_->condition.notify_all();
            return status;
          });
      if (frame == camera_frame && !outcome.accepted()) {
        assignError(error, "Keypoint scheduler rejected current frame");
        return false;
      }
    }
    if (frame == final_frame) break;
  }
  impl_->metrics.peak_pending_frames = std::max(
      impl_->metrics.peak_pending_frames,
      impl_->scheduler->metrics().queue.pending_requests);
  return true;
}

bool KeypointOverlayBuffer::waitForFrame(
    int64_t camera_frame, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->stopping || impl_->cache.find(camera_frame) != impl_->cache.end();
  }) && !impl_->stopping;
}

std::shared_ptr<const crimson::zarr::KeypointOverlayResolution>
KeypointOverlayBuffer::frame(int64_t camera_frame) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->cache.find(camera_frame);
  return found == impl_->cache.end() ? nullptr : found->second;
}

crimson::zarr::KeypointOverlayDescriptor KeypointOverlayBuffer::descriptor()
    const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

KeypointOverlayBufferMetrics KeypointOverlayBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}

std::unique_ptr<crimson::timeline::KeypointQualityTimelineRepository>
KeypointOverlayBuffer::createQualityTimelineRepository(std::string* error) {
  crimson::zarr::KeypointOverlayRepository* repository = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->repository || impl_->stopping) {
      assignError(error, "Keypoint buffer is not open");
      return nullptr;
    }
    repository = impl_->repository.get();
  }
  return repository->createQualityTimelineRepository(error);
}
