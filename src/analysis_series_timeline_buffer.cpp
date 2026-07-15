#include "analysis_series_timeline_buffer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
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

struct PageKey {
  std::string source;
  int64_t first = 0;
  int64_t last = -1;

  bool operator==(const PageKey &other) const {
    return source == other.source && first == other.first && last == other.last;
  }
};

struct PageKeyHash {
  size_t operator()(const PageKey &key) const {
    size_t hash = std::hash<std::string>{}(key.source);
    hash ^= std::hash<int64_t>{}(key.first) + 0x9e3779b9 + (hash << 6) +
            (hash >> 2);
    hash ^=
        std::hash<int64_t>{}(key.last) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct PendingRequest {
  PageKey key;
  crimson::timeline::AnalysisSeriesTimelineRequest request;
  uint64_t generation = 0;
};

} // namespace

struct AnalysisSeriesTimelineBuffer::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
      repository;
  crimson::timeline::AnalysisSeriesTimelineDescriptor descriptor;
  std::thread worker;
  bool stopping = false;
  size_t page_span = 4096;
  size_t page_step = 2048;
  size_t maximum_points = 1200;
  size_t capacity = 3;
  uint64_t generation = 1;
  std::optional<PageKey> last_key;
  std::deque<PendingRequest> pending;
  std::unordered_set<PageKey, PageKeyHash> pending_keys;
  std::optional<PendingRequest> active;
  std::unordered_map<
      PageKey,
      std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>,
      PageKeyHash>
      cache;
  std::deque<PageKey> cache_order;
  AnalysisSeriesTimelineBufferMetrics metrics;

  std::optional<PageKey> keyFor(int64_t frame,
                                const std::string &source) const {
    const auto bounds = crimson::timeline::analysisSeriesTimelinePageBounds(
        frame, descriptor.frame_count, page_span, page_step);
    if (!bounds.valid() || source.empty()) {
      return std::nullopt;
    }
    return PageKey{source, bounds.first_frame, bounds.last_frame};
  }

  void clearPendingLocked() {
    pending.clear();
    pending_keys.clear();
  }

  void publishLocked(
      const PageKey &key,
      std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
          value) {
    if (cache.find(key) == cache.end()) {
      cache_order.push_back(key);
    }
    cache[key] = std::move(value);
    while (cache_order.size() > capacity) {
      const PageKey evicted = cache_order.front();
      cache_order.pop_front();
      cache.erase(evicted);
    }
    metrics.peak_cached_windows =
        std::max(metrics.peak_cached_windows, cache.size());
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
        pending_keys.erase(request.key);
        active = request;
      }

      const auto start = std::chrono::steady_clock::now();
      auto resolved = repository->resolveWindow(request.request);
      const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
      auto shared = std::make_shared<
          const crimson::timeline::AnalysisSeriesTimelineWindow>(
          std::move(resolved));
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
        metrics.source_rows_read += shared->source_row_count;
        metrics.published_points += shared->published_point_count;
        switch (shared->status) {
        case crimson::timeline::AnalysisSeriesTimelineStatus::Mapped:
          ++metrics.resolved_windows;
          break;
        case crimson::timeline::AnalysisSeriesTimelineStatus::Missing:
        case crimson::timeline::AnalysisSeriesTimelineStatus::OutOfRange:
          ++metrics.missing_windows;
          break;
        case crimson::timeline::AnalysisSeriesTimelineStatus::InvalidRequest:
        case crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed:
          ++metrics.failed_windows;
          metrics.last_error = shared->error;
          break;
        }
        publishLocked(request.key, std::move(shared));
        condition.notify_all();
      }
    }
  }
};

AnalysisSeriesTimelineBuffer::AnalysisSeriesTimelineBuffer()
    : impl_(std::make_unique<Impl>()) {}

AnalysisSeriesTimelineBuffer::~AnalysisSeriesTimelineBuffer() { close(); }

bool AnalysisSeriesTimelineBuffer::open(
    std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
        repository,
    size_t page_span_frames, size_t page_step_frames,
    size_t max_points_per_trace, size_t cache_capacity, std::string *error) {
  close();
  if (!repository) {
    assignError(error, "Analysis-series timeline repository is null");
    return false;
  }
  if (page_span_frames < 3 || page_step_frames == 0 ||
      page_step_frames > page_span_frames || max_points_per_trace < 3 ||
      cache_capacity == 0) {
    assignError(error,
                "Analysis-series timeline buffer configuration is invalid");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->descriptor = repository->descriptor();
    impl_->repository = std::move(repository);
    impl_->page_span = page_span_frames;
    impl_->page_step = page_step_frames;
    impl_->maximum_points = max_points_per_trace;
    impl_->capacity = cache_capacity;
    impl_->stopping = false;
    impl_->generation = 1;
    impl_->last_key.reset();
    impl_->metrics = {};
  }
  impl_->worker = std::thread([this] { impl_->run(); });
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void AnalysisSeriesTimelineBuffer::close() {
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
  impl_->active.reset();
  impl_->cache.clear();
  impl_->cache_order.clear();
  impl_->stopping = false;
  impl_->last_key.reset();
}

bool AnalysisSeriesTimelineBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr && impl_->worker.joinable() &&
         !impl_->stopping;
}

bool AnalysisSeriesTimelineBuffer::requestFrame(
    int64_t frame, const std::string &source_key,
    double fallback_frames_per_second, bool discontinuity, std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Analysis-series timeline buffer is not open");
    return false;
  }
  const std::string source =
      source_key.empty()
          ? crimson::timeline::defaultAnalysisSeriesSource(impl_->descriptor)
          : source_key;
  const auto key = impl_->keyFor(frame, source);
  if (!key || !std::isfinite(fallback_frames_per_second) ||
      fallback_frames_per_second <= 0.0) {
    assignError(error, "Analysis-series timeline frame request is invalid");
    return false;
  }
  ++impl_->metrics.requests;
  const bool changed_page = impl_->last_key && !(*impl_->last_key == *key);
  if ((discontinuity || changed_page) &&
      (impl_->active.has_value() || !impl_->pending.empty())) {
    ++impl_->generation;
    impl_->clearPendingLocked();
  }
  impl_->last_key = key;
  if (impl_->cache.find(*key) != impl_->cache.end()) {
    ++impl_->metrics.cache_hits;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  if (impl_->pending_keys.find(*key) == impl_->pending_keys.end() &&
      (!impl_->active || !(impl_->active->key == *key))) {
    crimson::timeline::AnalysisSeriesTimelineRequest request;
    request.source_key = source;
    request.first_frame = key->first;
    request.last_frame = key->last;
    request.anchor_frame = frame;
    request.max_points_per_trace = impl_->maximum_points;
    request.fallback_frames_per_second = fallback_frames_per_second;
    impl_->pending.push_back({*key, std::move(request), impl_->generation});
    impl_->pending_keys.insert(*key);
    impl_->metrics.peak_pending_windows =
        std::max(impl_->metrics.peak_pending_windows, impl_->pending.size());
    impl_->condition.notify_all();
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool AnalysisSeriesTimelineBuffer::waitForFrame(
    int64_t frame, const std::string &source_key,
    double fallback_frames_per_second,
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const std::string source =
      source_key.empty()
          ? crimson::timeline::defaultAnalysisSeriesSource(impl_->descriptor)
          : source_key;
  const auto key = impl_->keyFor(frame, source);
  if (!key || fallback_frames_per_second <= 0.0) {
    return false;
  }
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->stopping || impl_->cache.find(*key) != impl_->cache.end();
  }) && !impl_->stopping;
}

std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
AnalysisSeriesTimelineBuffer::window(int64_t frame,
                                     const std::string &source_key) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::string source =
      source_key.empty()
          ? crimson::timeline::defaultAnalysisSeriesSource(impl_->descriptor)
          : source_key;
  const auto key = impl_->keyFor(frame, source);
  if (!key) {
    return nullptr;
  }
  const auto found = impl_->cache.find(*key);
  return found == impl_->cache.end() ? nullptr : found->second;
}

crimson::timeline::AnalysisSeriesTimelineDescriptor
AnalysisSeriesTimelineBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

AnalysisSeriesTimelineBufferMetrics
AnalysisSeriesTimelineBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}
