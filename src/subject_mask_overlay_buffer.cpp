#include "subject_mask_overlay_buffer.h"

#include <algorithm>
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

struct SubjectMaskOverlayBuffer::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> repository;
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  std::thread worker;
  bool stopping = false;
  size_t lookahead = 12;
  size_t capacity = 24;
  uint64_t generation = 1;
  int64_t last_request = -1;
  std::deque<PendingRequest> pending;
  std::unordered_set<int64_t> pending_frames;
  std::optional<PendingRequest> active_request;
  std::unordered_map<
      int64_t,
      std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>>
      cache;
  std::deque<int64_t> cache_order;
  SubjectMaskOverlayBufferMetrics metrics;

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
      std::shared_ptr<const crimson::zarr::SubjectMaskOverlayResolution>
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
          std::make_shared<const crimson::zarr::SubjectMaskOverlayResolution>(
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
        case crimson::zarr::SubjectMaskOverlayStatus::Mapped:
          ++metrics.resolved_frames;
          break;
        case crimson::zarr::SubjectMaskOverlayStatus::Missing:
        case crimson::zarr::SubjectMaskOverlayStatus::OutOfRange:
          ++metrics.missing_frames;
          break;
        case crimson::zarr::SubjectMaskOverlayStatus::InvalidDimensions:
        case crimson::zarr::SubjectMaskOverlayStatus::ReadFailed:
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

SubjectMaskOverlayBuffer::SubjectMaskOverlayBuffer()
    : impl_(std::make_unique<Impl>()) {}

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
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->descriptor = repository->descriptor();
    impl_->repository = std::move(repository);
    impl_->lookahead = std::min(lookahead_frames, cache_capacity - 1);
    impl_->capacity = cache_capacity;
    impl_->stopping = false;
    impl_->generation = 1;
    impl_->last_request = -1;
    impl_->metrics = {};
  }
  impl_->worker = std::thread([this] { impl_->run(); });
  return true;
}

void SubjectMaskOverlayBuffer::close() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    impl_->condition.notify_all();
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->repository.reset();
  impl_->descriptor = {};
  impl_->clearPendingLocked();
  impl_->clearCacheLocked();
  impl_->active_request.reset();
  impl_->stopping = false;
  impl_->last_request = -1;
}

bool SubjectMaskOverlayBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr && impl_->worker.joinable() &&
         !impl_->stopping;
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
  const bool jumped =
      impl_->last_request >= 0 &&
      (camera_frame < impl_->last_request ||
       (camera_frame >= impl_->last_request &&
        camera_frame - impl_->last_request >
            static_cast<int64_t>(impl_->lookahead) + 2));
  if (discontinuity || jumped) {
    ++impl_->generation;
    impl_->clearPendingLocked();
    impl_->clearCacheLocked();
  }
  impl_->last_request = camera_frame;

  const int64_t frame_count = impl_->descriptor.camera_frame_count >
                                      static_cast<size_t>(
                                          std::numeric_limits<int64_t>::max())
                                  ? std::numeric_limits<int64_t>::max()
                                  : static_cast<int64_t>(
                                        impl_->descriptor.camera_frame_count);
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
