#include "keypoint_quality_timeline_buffer.h"

#include "analysis_series_timeline.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace {

void assignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

struct PageKey {
  int64_t first = 0;
  int64_t last = -1;
  bool operator==(const PageKey &other) const {
    return first == other.first && last == other.last;
  }
};

struct PageKeyHash {
  size_t operator()(const PageKey &key) const {
    size_t hash = std::hash<int64_t>{}(key.first);
    hash ^=
        std::hash<int64_t>{}(key.last) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

} // namespace

struct KeypointQualityTimelineBuffer::Impl {
  mutable std::mutex mutex;
  std::unique_ptr<crimson::timeline::KeypointQualityTimelineRepository>
      repository;
  crimson::timeline::KeypointQualityTimelineDescriptor descriptor;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::string archive_identity;
  crimson::data::SourceIdentity scheduler_source;
  crimson::data::SourceIdentity overview_scheduler_source;
  bool stopping = false;
  size_t page_span = 4096;
  size_t page_step = 2048;
  size_t capacity = 3;
  uint64_t generation = 1;
  std::optional<PageKey> last_key;
  std::unordered_map<
      PageKey,
      std::shared_ptr<const crimson::timeline::KeypointQualityTimelineWindow>,
      PageKeyHash>
      cache;
  std::deque<PageKey> cache_order;
  uint64_t overview_generation = 1;
  size_t overview_maximum_points = 0;
  size_t overview_maximum_decoded_bytes = 0;
  bool overview_pending = false;
  std::shared_ptr<const crimson::timeline::KeypointQualityTimelineOverview>
      overview;
  KeypointQualityTimelineBufferMetrics metrics;

  std::optional<PageKey> keyFor(int64_t frame) const {
    if (frame < 0 || static_cast<size_t>(frame) >= descriptor.frame_count) {
      return std::nullopt;
    }
    const int64_t step = static_cast<int64_t>(page_step);
    const int64_t first = (frame / step) * step;
    const int64_t last =
        std::min<int64_t>(static_cast<int64_t>(descriptor.frame_count) - 1,
                          first + static_cast<int64_t>(page_span) - 1);
    return PageKey{first, last};
  }

  void publishLocked(
      const PageKey &key,
      std::shared_ptr<const crimson::timeline::KeypointQualityTimelineWindow>
          value) {
    if (cache.find(key) == cache.end()) {
      cache_order.push_back(key);
    }
    cache[key] = std::move(value);
    while (cache_order.size() > capacity) {
      cache.erase(cache_order.front());
      cache_order.pop_front();
    }
    metrics.peak_cached_windows =
        std::max(metrics.peak_cached_windows, cache.size());
  }
};

KeypointQualityTimelineBuffer::KeypointQualityTimelineBuffer(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    std::string archive_identity)
    : impl_(std::make_unique<Impl>()) {
  impl_->scheduler =
      scheduler ? std::move(scheduler)
                : std::make_shared<crimson::data::DataAccessScheduler>(16, 1);
  impl_->archive_identity = std::move(archive_identity);
}

KeypointQualityTimelineBuffer::~KeypointQualityTimelineBuffer() { close(); }

bool KeypointQualityTimelineBuffer::open(
    std::unique_ptr<crimson::timeline::KeypointQualityTimelineRepository>
        repository,
    size_t page_span_frames, size_t page_step_frames, size_t cache_capacity,
    std::string *error) {
  close();
  if (!repository || !repository->descriptor().ready() ||
      page_span_frames < 3 || page_step_frames == 0 ||
      page_step_frames > page_span_frames || cache_capacity == 0 ||
      !impl_->scheduler || !impl_->scheduler->running()) {
    assignError(error,
                "Keypoint-quality timeline buffer configuration is invalid");
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->descriptor = repository->descriptor();
  impl_->scheduler_source = {
      impl_->archive_identity.empty() ? "in_memory" : impl_->archive_identity,
      "keypoint_quality_timeline", impl_->descriptor.run_name};
  impl_->overview_scheduler_source = {
      impl_->archive_identity.empty() ? "in_memory" : impl_->archive_identity,
      "keypoint_quality_overview", impl_->descriptor.run_name};
  impl_->repository = std::move(repository);
  impl_->page_span = page_span_frames;
  impl_->page_step = page_step_frames;
  impl_->capacity = cache_capacity;
  impl_->stopping = false;
  impl_->generation = 1;
  impl_->last_key.reset();
  impl_->cache.clear();
  impl_->cache_order.clear();
  impl_->overview_generation = 1;
  impl_->overview_maximum_points = 0;
  impl_->overview_maximum_decoded_bytes = 0;
  impl_->overview_pending = false;
  impl_->overview.reset();
  impl_->metrics = {};
  if (error) {
    error->clear();
  }
  return true;
}

void KeypointQualityTimelineBuffer::close() {
  crimson::data::SourceIdentity source;
  crimson::data::SourceIdentity overview_source;
  bool active = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    active = impl_->repository != nullptr;
    source = impl_->scheduler_source;
    overview_source = impl_->overview_scheduler_source;
  }
  if (active && impl_->scheduler) {
    impl_->scheduler->cancelSource(source);
    impl_->scheduler->cancelSource(overview_source);
    impl_->scheduler->waitForSourceIdle(source);
    impl_->scheduler->waitForSourceIdle(overview_source);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->repository.reset();
  impl_->descriptor = {};
  impl_->scheduler_source = {};
  impl_->overview_scheduler_source = {};
  impl_->cache.clear();
  impl_->cache_order.clear();
  impl_->last_key.reset();
  impl_->overview.reset();
  impl_->overview_pending = false;
  impl_->stopping = false;
}

bool KeypointQualityTimelineBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr && !impl_->stopping;
}

bool KeypointQualityTimelineBuffer::requestFrame(int64_t frame,
                                                 bool discontinuity,
                                                 std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Keypoint-quality timeline buffer is not open");
    return false;
  }
  const auto key = impl_->keyFor(frame);
  if (!key) {
    assignError(error, "Keypoint-quality timeline frame is invalid");
    return false;
  }
  ++impl_->metrics.requests;
  const bool changed = impl_->last_key && !(*impl_->last_key == *key);
  crimson::data::AccessPattern pattern = crimson::data::AccessPattern::Paused;
  if (discontinuity) {
    pattern = crimson::data::AccessPattern::RandomSeek;
  } else if (impl_->last_key) {
    pattern = frame >= impl_->last_key->first
                  ? crimson::data::AccessPattern::Forward
                  : crimson::data::AccessPattern::Reverse;
  }
  if (discontinuity || changed) {
    ++impl_->generation;
    impl_->scheduler->cancelSource(impl_->scheduler_source);
  }
  impl_->last_key = key;
  if (impl_->cache.find(*key) != impl_->cache.end()) {
    ++impl_->metrics.cache_hits;
    if (error)
      error->clear();
    return true;
  }
  const uint64_t generation = impl_->generation;
  crimson::data::DataRangeRequest request{
      impl_->scheduler_source,
      {key->first, key->last},
      crimson::data::FieldSelection::All(),
      crimson::data::RequestPriority::VisibleWindow,
      pattern,
      generation};
  const auto outcome = impl_->scheduler->submit(
      std::move(request),
      [this, key = *key,
       generation](const crimson::data::ScheduledDataRequest &scheduled) {
        if (scheduled.cancellation.cancelled()) {
          return crimson::data::DataResultStatus::Stale;
        }
        const auto started = std::chrono::steady_clock::now();
        auto resolved = impl_->repository->resolveWindow(key.first, key.last);
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
        auto shared = std::make_shared<
            const crimson::timeline::KeypointQualityTimelineWindow>(
            std::move(resolved));
        std::lock_guard<std::mutex> callback_lock(impl_->mutex);
        impl_->metrics.maximum_resolve_ms =
            std::max(impl_->metrics.maximum_resolve_ms, elapsed);
        if (scheduled.cancellation.cancelled() || impl_->stopping ||
            generation != impl_->generation) {
          ++impl_->metrics.discarded_results;
          return crimson::data::DataResultStatus::Stale;
        }
        crimson::data::DataResultStatus status =
            crimson::data::DataResultStatus::Failed;
        if (shared->ready()) {
          ++impl_->metrics.resolved_windows;
          status = crimson::data::DataResultStatus::Ready;
        } else if (shared->status ==
                   crimson::timeline::KeypointQualityTimelineStatus::
                       OutOfRange) {
          status = crimson::data::DataResultStatus::Missing;
        } else {
          ++impl_->metrics.failed_windows;
          impl_->metrics.last_error = shared->error;
        }
        impl_->publishLocked(key, std::move(shared));
        return status;
      });
  impl_->metrics.peak_pending_windows =
      std::max(impl_->metrics.peak_pending_windows,
               impl_->scheduler->metrics().queue.pending_requests);
  if (!outcome.accepted()) {
    assignError(error, "Keypoint-quality scheduler rejected the request");
    return false;
  }
  if (error)
    error->clear();
  return true;
}

std::shared_ptr<const crimson::timeline::KeypointQualityTimelineWindow>
KeypointQualityTimelineBuffer::window(int64_t frame) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto key = impl_->keyFor(frame);
  if (!key)
    return nullptr;
  const auto found = impl_->cache.find(*key);
  return found == impl_->cache.end() ? nullptr : found->second;
}

bool KeypointQualityTimelineBuffer::requestOverview(
    size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
    std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping || maximum_points_per_trace < 2 ||
      maximum_decoded_bytes < (impl_->descriptor.keypoint_count + 1) *
                                  (sizeof(float) + sizeof(uint8_t))) {
    assignError(error, "Keypoint-quality overview request is invalid");
    return false;
  }
  ++impl_->metrics.overview_requests;
  const bool same_request =
      impl_->overview_maximum_points == maximum_points_per_trace &&
      impl_->overview_maximum_decoded_bytes == maximum_decoded_bytes;
  if (same_request && impl_->overview && impl_->overview->ready()) {
    ++impl_->metrics.overview_cache_hits;
    if (error) {
      error->clear();
    }
    return true;
  }
  if (same_request && impl_->overview_pending) {
    if (error) {
      error->clear();
    }
    return true;
  }
  ++impl_->overview_generation;
  impl_->scheduler->cancelSource(impl_->overview_scheduler_source);
  impl_->overview_maximum_points = maximum_points_per_trace;
  impl_->overview_maximum_decoded_bytes = maximum_decoded_bytes;
  impl_->overview_pending = true;
  impl_->overview.reset();
  const uint64_t generation = impl_->overview_generation;
  crimson::data::DataRangeRequest request{
      impl_->overview_scheduler_source,
      {0, static_cast<int64_t>(impl_->descriptor.frame_count) - 1},
      crimson::data::FieldSelection::All(),
      crimson::data::RequestPriority::VisibleWindow,
      crimson::data::AccessPattern::Paused,
      generation};
  const auto outcome = impl_->scheduler->submit(
      std::move(request),
      [this, generation, maximum_points_per_trace, maximum_decoded_bytes](
          const crimson::data::ScheduledDataRequest &scheduled) {
        if (scheduled.cancellation.cancelled()) {
          return crimson::data::DataResultStatus::Stale;
        }
        const auto started = std::chrono::steady_clock::now();
        auto resolved = impl_->repository->resolveOverview(
            maximum_points_per_trace, maximum_decoded_bytes,
            [&scheduled] { return scheduled.cancellation.cancelled(); });
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
        auto shared = std::make_shared<
            const crimson::timeline::KeypointQualityTimelineOverview>(
            std::move(resolved));
        std::lock_guard<std::mutex> callback_lock(impl_->mutex);
        impl_->metrics.maximum_overview_resolve_ms =
            std::max(impl_->metrics.maximum_overview_resolve_ms, elapsed);
        if (scheduled.cancellation.cancelled() || impl_->stopping ||
            generation != impl_->overview_generation) {
          ++impl_->metrics.discarded_overviews;
          return crimson::data::DataResultStatus::Stale;
        }
        impl_->overview_pending = false;
        impl_->overview = std::move(shared);
        if (impl_->overview->ready()) {
          ++impl_->metrics.resolved_overviews;
          return crimson::data::DataResultStatus::Ready;
        }
        ++impl_->metrics.failed_overviews;
        impl_->metrics.last_error = impl_->overview->error;
        return crimson::data::DataResultStatus::Failed;
      });
  if (outcome.status ==
      crimson::data::DataRequestSubmitStatus::RejectedCapacity) {
    impl_->overview_pending = false;
    if (error) {
      error->clear();
    }
    return true;
  }
  if (!outcome.accepted() &&
      outcome.status !=
          crimson::data::DataRequestSubmitStatus::RejectedCapacity) {
    impl_->overview_pending = false;
    assignError(error, "Keypoint-quality scheduler rejected the overview");
    return false;
  }
  if (error) {
    error->clear();
  }
  return true;
}

std::shared_ptr<const crimson::timeline::KeypointQualityTimelineOverview>
KeypointQualityTimelineBuffer::overview() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->overview;
}

crimson::timeline::KeypointQualityTimelineDescriptor
KeypointQualityTimelineBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

KeypointQualityTimelineBufferMetrics
KeypointQualityTimelineBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}

crimson::timeline::KeypointQualityTimelineRepositoryMetrics
KeypointQualityTimelineBuffer::repositoryMetrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository
             ? impl_->repository->metrics()
             : crimson::timeline::KeypointQualityTimelineRepositoryMetrics{};
}
