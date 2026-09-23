#include "eye_geometry_overlay_buffer.h"

#include <algorithm>
#include <exception>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

void assignError(std::string *destination, const std::string &value) {
  if (destination != nullptr) {
    *destination = value;
  }
}

struct PendingRequest {
  int64_t frame = -1;
  int width = 0;
  int height = 0;
  uint64_t generation = 0;
};

} // namespace

struct EyeGeometryOverlayBuffer::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::zarr::EyeGeometryOverlayRepository> repository;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::string archive_identity;
  crimson::data::SourceIdentity scheduler_source;
  crimson::zarr::EyeGeometryOverlayDescriptor descriptor;
  std::thread worker;
  bool stopping = false;
  size_t lookahead = 6;
  size_t capacity = 16;
  uint64_t generation = 1;
  bool demanded = false;
  int64_t last_request = -1;
  std::deque<PendingRequest> pending;
  std::unordered_set<int64_t> pending_frames;
  std::optional<PendingRequest> active_request;
  std::unordered_map<
      int64_t,
      std::shared_ptr<const crimson::zarr::EyeGeometryOverlayResolution>>
      cache;
  std::deque<int64_t> cache_order;
  EyeGeometryOverlayBufferMetrics metrics;

  void clearPendingLocked() {
    pending.clear();
    pending_frames.clear();
  }

  void retainPendingWindowLocked(int64_t first_frame, int64_t final_frame) {
    std::deque<PendingRequest> retained;
    pending_frames.clear();
    for (auto &request : pending) {
      if (request.generation != generation || request.frame < first_frame ||
          request.frame > final_frame) {
        continue;
      }
      pending_frames.insert(request.frame);
      retained.push_back(std::move(request));
    }
    pending.swap(retained);
  }

  void clearCacheLocked() {
    cache.clear();
    cache_order.clear();
  }

  void publishLocked(
      int64_t frame,
      std::shared_ptr<const crimson::zarr::EyeGeometryOverlayResolution>
          resolution) {
    if (cache.find(frame) == cache.end()) {
      cache_order.push_back(frame);
    }
    cache[frame] = std::move(resolution);
    while (cache_order.size() > capacity) {
      const int64_t evicted = cache_order.front();
      cache_order.pop_front();
      cache.erase(evicted);
    }
    metrics.peak_cached_frames =
        std::max(metrics.peak_cached_frames, cache.size());
  }

  void run() {
    while (true) {
      PendingRequest request;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return stopping || !pending.empty(); });
        if (stopping) {
          return;
        }
        request = pending.front();
        pending.pop_front();
        pending_frames.erase(request.frame);
        active_request = request;
      }

      const auto start = std::chrono::steady_clock::now();
      auto resolution = repository->resolveCameraFrame(
          request.frame, request.width, request.height);
      const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
      auto shared =
          std::make_shared<const crimson::zarr::EyeGeometryOverlayResolution>(
              std::move(resolution));
      {
        std::lock_guard<std::mutex> lock(mutex);
        active_request.reset();
        metrics.maximum_resolve_ms =
            std::max(metrics.maximum_resolve_ms, elapsed_ms);
        if (request.generation != generation) {
          ++metrics.discarded_results;
          condition.notify_all();
          continue;
        }
        switch (shared->status) {
        case crimson::zarr::EyeGeometryOverlayStatus::Mapped:
          ++metrics.resolved_frames;
          break;
        case crimson::zarr::EyeGeometryOverlayStatus::Missing:
        case crimson::zarr::EyeGeometryOverlayStatus::OutOfRange:
          ++metrics.missing_frames;
          break;
        case crimson::zarr::EyeGeometryOverlayStatus::InvalidDimensions:
        case crimson::zarr::EyeGeometryOverlayStatus::ReadFailed:
          ++metrics.failed_frames;
          metrics.last_error = shared->error;
          break;
        }
        publishLocked(request.frame, std::move(shared));
        condition.notify_all();
      }
    }
  }
};

EyeGeometryOverlayBuffer::EyeGeometryOverlayBuffer()
    : impl_(std::make_unique<Impl>()) {}

EyeGeometryOverlayBuffer::EyeGeometryOverlayBuffer(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    std::string archive_identity)
    : impl_(std::make_unique<Impl>()) {
  impl_->scheduler = std::move(scheduler);
  impl_->archive_identity = std::move(archive_identity);
}

EyeGeometryOverlayBuffer::~EyeGeometryOverlayBuffer() { close(); }

bool EyeGeometryOverlayBuffer::open(
    std::unique_ptr<crimson::zarr::EyeGeometryOverlayRepository> repository,
    size_t lookahead_frames, size_t cache_capacity, std::string *error) {
  close();
  if (!repository) {
    assignError(error, "Eye-geometry repository is null");
    return false;
  }
  if (cache_capacity == 0) {
    assignError(error, "Eye-geometry cache capacity must be positive");
    return false;
  }
  if (impl_->scheduler && !impl_->scheduler->running()) {
    assignError(error, "Eye-geometry data scheduler is unavailable");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->descriptor = repository->descriptor();
    if (impl_->scheduler) {
      impl_->scheduler_source = {
          impl_->archive_identity.empty() ? "in_memory" : impl_->archive_identity,
          "eye_geometry",
          impl_->descriptor.source_group + "/" + impl_->descriptor.run_name};
    }
    impl_->repository = std::move(repository);
    impl_->lookahead = std::min(lookahead_frames, cache_capacity - 1);
    impl_->capacity = cache_capacity;
    impl_->stopping = false;
    impl_->generation = 1;
    impl_->last_request = -1;
    impl_->demanded = false;
    impl_->metrics = {};
  }
  if (!impl_->scheduler)
    impl_->worker = std::thread([this] { impl_->run(); });
  return true;
}

void EyeGeometryOverlayBuffer::close() {
  crimson::data::SourceIdentity source;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    source = impl_->scheduler_source;
    impl_->condition.notify_all();
  }
  if (source.valid() && impl_->scheduler) {
    impl_->scheduler->cancelSource(source);
    impl_->scheduler->waitForSourceIdle(source);
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->repository.reset();
  impl_->descriptor = {};
  impl_->scheduler_source = {};
  impl_->clearPendingLocked();
  impl_->clearCacheLocked();
  impl_->active_request.reset();
  impl_->stopping = false;
  impl_->last_request = -1;
  impl_->demanded = false;
}

void EyeGeometryOverlayBuffer::suspend() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->scheduler || !impl_->demanded || impl_->stopping) return;
  impl_->demanded = false;
  ++impl_->generation;
  impl_->scheduler->cancelSource(impl_->scheduler_source);
  impl_->clearCacheLocked();
  impl_->last_request = -1;
  impl_->condition.notify_all();
}

bool EyeGeometryOverlayBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr &&
         (impl_->worker.joinable() || impl_->scheduler != nullptr) &&
         !impl_->stopping;
}

bool EyeGeometryOverlayBuffer::requestFrame(int64_t camera_frame,
                                            int full_frame_width,
                                            int full_frame_height,
                                            bool discontinuity,
                                            std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Eye-geometry buffer is not open");
    return false;
  }
  if (camera_frame < 0 || full_frame_width <= 0 || full_frame_height <= 0) {
    assignError(error, "Eye-geometry frame request is invalid");
    return false;
  }
  ++impl_->metrics.requests;
  impl_->demanded = true;
  const bool jumped = impl_->last_request >= 0 &&
                      (camera_frame < impl_->last_request ||
                       camera_frame - impl_->last_request >
                           static_cast<int64_t>(impl_->lookahead) + 2);
  if (discontinuity || jumped) {
    ++impl_->generation;
    if (impl_->scheduler)
      impl_->scheduler->cancelSource(impl_->scheduler_source);
    impl_->clearPendingLocked();
    impl_->clearCacheLocked();
  }
  impl_->last_request = camera_frame;

  const int64_t frame_count =
      impl_->descriptor.camera_frame_count >
              static_cast<size_t>(std::numeric_limits<int64_t>::max())
          ? std::numeric_limits<int64_t>::max()
          : static_cast<int64_t>(impl_->descriptor.camera_frame_count);
  const int64_t maximum_lookahead =
      std::numeric_limits<int64_t>::max() - camera_frame;
  int64_t final_frame =
      camera_frame + std::min<int64_t>(static_cast<int64_t>(impl_->lookahead),
                                       maximum_lookahead);
  if (frame_count > 0) {
    final_frame = camera_frame < frame_count
                      ? std::min<int64_t>(frame_count - 1, final_frame)
                      : camera_frame;
  }
  impl_->retainPendingWindowLocked(camera_frame, final_frame);
  if (impl_->cache.find(camera_frame) != impl_->cache.end()) {
    ++impl_->metrics.cache_hits;
  }
  if (impl_->scheduler) {
    impl_->scheduler->retainSourceRange(
        impl_->scheduler_source, impl_->generation,
        {camera_frame, final_frame});
    for (int64_t frame = camera_frame; frame <= final_frame; ++frame) {
      if (impl_->cache.find(frame) != impl_->cache.end()) continue;
      const uint64_t generation = impl_->generation;
      crimson::data::DataRangeRequest request{
          impl_->scheduler_source, {frame, frame},
          crimson::data::FieldSelection::All(),
          frame == camera_frame ? crimson::data::RequestPriority::CurrentFrame
                                : crimson::data::RequestPriority::Speculative,
          discontinuity ? crimson::data::AccessPattern::RandomSeek
                        : crimson::data::AccessPattern::Forward,
          generation};
      const auto outcome = impl_->scheduler->submit(
          std::move(request),
          [this, frame, full_frame_width, full_frame_height, generation](
              const crimson::data::ScheduledDataRequest& scheduled) {
            if (scheduled.cancellation.cancelled())
              return crimson::data::DataResultStatus::Stale;
            const auto start = std::chrono::steady_clock::now();
            crimson::zarr::EyeGeometryOverlayResolution resolved;
            try {
              resolved = impl_->repository->resolveCameraFrame(
                  frame, full_frame_width, full_frame_height);
            } catch (const std::exception& exception) {
              resolved.camera_frame = frame;
              resolved.status = crimson::zarr::EyeGeometryOverlayStatus::ReadFailed;
              resolved.error = exception.what();
            } catch (...) {
              resolved.camera_frame = frame;
              resolved.status = crimson::zarr::EyeGeometryOverlayStatus::ReadFailed;
              resolved.error = "Eye reader threw an unknown exception";
            }
            const double elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            auto shared = std::make_shared<
                const crimson::zarr::EyeGeometryOverlayResolution>(
                std::move(resolved));
            std::lock_guard<std::mutex> callback_lock(impl_->mutex);
            impl_->metrics.maximum_resolve_ms =
                std::max(impl_->metrics.maximum_resolve_ms, elapsed_ms);
            if (scheduled.cancellation.cancelled() || impl_->stopping ||
                !impl_->demanded || generation != impl_->generation) {
              ++impl_->metrics.discarded_results;
              impl_->condition.notify_all();
              return crimson::data::DataResultStatus::Stale;
            }
            auto status = crimson::data::DataResultStatus::Failed;
            switch (shared->status) {
              case crimson::zarr::EyeGeometryOverlayStatus::Mapped:
                ++impl_->metrics.resolved_frames;
                status = crimson::data::DataResultStatus::Ready;
                break;
              case crimson::zarr::EyeGeometryOverlayStatus::Missing:
              case crimson::zarr::EyeGeometryOverlayStatus::OutOfRange:
                ++impl_->metrics.missing_frames;
                status = crimson::data::DataResultStatus::Missing;
                break;
              case crimson::zarr::EyeGeometryOverlayStatus::InvalidDimensions:
              case crimson::zarr::EyeGeometryOverlayStatus::ReadFailed:
                ++impl_->metrics.failed_frames;
                impl_->metrics.last_error = shared->error;
                break;
            }
            impl_->publishLocked(frame, std::move(shared));
            impl_->condition.notify_all();
            return status;
          });
      if (frame == camera_frame && !outcome.accepted()) {
        assignError(error, "Eye scheduler rejected current frame");
        return false;
      }
    }
    impl_->metrics.peak_pending_frames = std::max(
        impl_->metrics.peak_pending_frames,
        impl_->scheduler->metrics().queue.pending_requests);
    return true;
  }
  for (int64_t frame = camera_frame; frame <= final_frame; ++frame) {
    if (impl_->cache.find(frame) != impl_->cache.end() ||
        impl_->pending_frames.find(frame) != impl_->pending_frames.end() ||
        (impl_->active_request &&
         impl_->active_request->generation == impl_->generation &&
         impl_->active_request->frame == frame)) {
      continue;
    }
    impl_->pending.push_back(
        {frame, full_frame_width, full_frame_height, impl_->generation});
    impl_->pending_frames.insert(frame);
  }
  impl_->metrics.peak_pending_frames =
      std::max(impl_->metrics.peak_pending_frames, impl_->pending.size());
  impl_->condition.notify_all();
  return true;
}

bool EyeGeometryOverlayBuffer::waitForFrame(
    int64_t camera_frame, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->stopping ||
           impl_->cache.find(camera_frame) != impl_->cache.end();
  }) && !impl_->stopping;
}

std::shared_ptr<const crimson::zarr::EyeGeometryOverlayResolution>
EyeGeometryOverlayBuffer::frame(int64_t camera_frame) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->cache.find(camera_frame);
  return found == impl_->cache.end() ? nullptr : found->second;
}

crimson::zarr::EyeGeometryOverlayDescriptor
EyeGeometryOverlayBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

EyeGeometryOverlayBufferMetrics EyeGeometryOverlayBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}

crimson::zarr::EyeGeometryOverlayRepository::AccessMetrics
EyeGeometryOverlayBuffer::repositoryMetrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository ? impl_->repository->accessMetrics()
                           : crimson::zarr::EyeGeometryOverlayRepository::AccessMetrics{};
}
