#include "chaser_distance_polar_buffer.h"

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

namespace crimson::polar {
namespace {

void assignError(std::string* destination, const std::string& value) {
  if (destination != nullptr) {
    *destination = value;
  }
}

struct PendingRequest {
  int64_t frame = -1;
  uint64_t generation = 0;
};

}  // namespace

struct ChaserDistancePolarBuffer::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<ChaserDistancePolarRepository> repository;
  ChaserDistancePolarDescriptor descriptor;
  std::thread worker;
  bool stopping = false;
  size_t lookahead = 8;
  size_t capacity = 16;
  uint64_t generation = 1;
  int64_t last_request = -1;
  std::deque<PendingRequest> pending;
  std::unordered_set<int64_t> pending_frames;
  std::optional<PendingRequest> active;
  std::unordered_map<
      int64_t, std::shared_ptr<const ChaserDistancePolarFrameSample>> cache;
  std::deque<int64_t> cache_order;
  ChaserDistancePolarBufferMetrics metrics;

  void clearPendingLocked() {
    pending.clear();
    pending_frames.clear();
  }

  void clearCacheLocked() {
    cache.clear();
    cache_order.clear();
  }

  void retainPendingWindowLocked(int64_t first, int64_t last) {
    std::deque<PendingRequest> retained;
    pending_frames.clear();
    for (auto& request : pending) {
      if (request.generation == generation && request.frame >= first &&
          request.frame <= last) {
        pending_frames.insert(request.frame);
        retained.push_back(std::move(request));
      }
    }
    pending.swap(retained);
  }

  void publishLocked(
      int64_t frame,
      std::shared_ptr<const ChaserDistancePolarFrameSample> sample) {
    if (cache.find(frame) == cache.end()) {
      cache_order.push_back(frame);
    }
    cache[frame] = std::move(sample);
    while (cache_order.size() > capacity) {
      const int64_t evicted = cache_order.front();
      cache_order.pop_front();
      cache.erase(evicted);
    }
    metrics.peak_cached_frames =
        std::max(metrics.peak_cached_frames, cache.size());
  }

  void recordSampleLocked(const ChaserDistancePolarFrameSample& sample) {
    metrics.source_points += sample.source_point_count;
    metrics.published_points += sample.points.size();
    switch (sample.availability) {
      case ChaserDistancePolarAvailability::Ready:
        ++metrics.ready_frames;
        break;
      case ChaserDistancePolarAvailability::ValidFrameEmpty:
        ++metrics.empty_frames;
        break;
      case ChaserDistancePolarAvailability::ExactFrameMissing:
        ++metrics.missing_frames;
        break;
      case ChaserDistancePolarAvailability::DatasetUnavailable:
        ++metrics.unavailable_frames;
        break;
      case ChaserDistancePolarAvailability::UnsupportedMetadata:
        ++metrics.unsupported_frames;
        metrics.last_error = sample.error;
        break;
      case ChaserDistancePolarAvailability::ReadFailed:
        ++metrics.failed_frames;
        metrics.last_error = sample.error;
        break;
    }
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
        active = request;
      }

      const auto start = std::chrono::steady_clock::now();
      auto sample = repository->resolveCameraFrame(request.frame);
      const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
      auto shared = std::make_shared<const ChaserDistancePolarFrameSample>(
          std::move(sample));
      {
        std::lock_guard<std::mutex> lock(mutex);
        active.reset();
        metrics.maximum_resolve_ms =
            std::max(metrics.maximum_resolve_ms, elapsed_ms);
        if (request.generation != generation) {
          ++metrics.discarded_results;
          condition.notify_all();
          continue;
        }
        recordSampleLocked(*shared);
        publishLocked(request.frame, std::move(shared));
        condition.notify_all();
      }
    }
  }
};

ChaserDistancePolarBuffer::ChaserDistancePolarBuffer()
    : impl_(std::make_unique<Impl>()) {}

ChaserDistancePolarBuffer::~ChaserDistancePolarBuffer() {
  close();
}

bool ChaserDistancePolarBuffer::open(
    std::unique_ptr<ChaserDistancePolarRepository> repository,
    size_t lookahead_frames,
    size_t cache_capacity,
    std::string* error) {
  close();
  if (!repository) {
    assignError(error, "Chaser-distance polar repository is null");
    return false;
  }
  if (cache_capacity == 0) {
    assignError(error, "Chaser-distance polar cache capacity must be positive");
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
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void ChaserDistancePolarBuffer::close() {
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
  impl_->active.reset();
  impl_->stopping = false;
  impl_->last_request = -1;
}

bool ChaserDistancePolarBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr && impl_->worker.joinable() &&
         !impl_->stopping;
}

bool ChaserDistancePolarBuffer::requestFrame(int64_t camera_frame,
                                             bool discontinuity,
                                             std::string* error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Chaser-distance polar buffer is not open");
    return false;
  }
  if (camera_frame < 0) {
    assignError(error, "Chaser-distance polar frame request is invalid");
    return false;
  }
  ++impl_->metrics.requests;
  const bool jumped =
      impl_->last_request >= 0 &&
      (camera_frame < impl_->last_request ||
       camera_frame - impl_->last_request >
           static_cast<int64_t>(impl_->lookahead) + 2);
  if (discontinuity || jumped) {
    ++impl_->generation;
    impl_->clearPendingLocked();
    impl_->clearCacheLocked();
  }
  impl_->last_request = camera_frame;

  const int64_t maximum_lookahead =
      std::numeric_limits<int64_t>::max() - camera_frame;
  const int64_t final_frame =
      camera_frame +
      std::min<int64_t>(static_cast<int64_t>(impl_->lookahead),
                        maximum_lookahead);
  impl_->retainPendingWindowLocked(camera_frame, final_frame);
  if (impl_->cache.find(camera_frame) != impl_->cache.end()) {
    ++impl_->metrics.cache_hits;
  }
  for (int64_t frame = camera_frame;; ++frame) {
    if (impl_->cache.find(frame) == impl_->cache.end() &&
        impl_->pending_frames.find(frame) == impl_->pending_frames.end() &&
        (!impl_->active || impl_->active->generation != impl_->generation ||
         impl_->active->frame != frame)) {
      impl_->pending.push_back({frame, impl_->generation});
      impl_->pending_frames.insert(frame);
    }
    if (frame == final_frame) {
      break;
    }
  }
  impl_->metrics.peak_pending_frames =
      std::max(impl_->metrics.peak_pending_frames, impl_->pending.size());
  impl_->condition.notify_all();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ChaserDistancePolarBuffer::waitForFrame(
    int64_t camera_frame,
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->stopping ||
           impl_->cache.find(camera_frame) != impl_->cache.end();
  }) && !impl_->stopping;
}

std::shared_ptr<const ChaserDistancePolarFrameSample>
ChaserDistancePolarBuffer::frame(int64_t camera_frame) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->cache.find(camera_frame);
  return found == impl_->cache.end() ? nullptr : found->second;
}

ChaserDistancePolarDescriptor ChaserDistancePolarBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

ChaserDistancePolarBufferMetrics ChaserDistancePolarBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}

}  // namespace crimson::polar
