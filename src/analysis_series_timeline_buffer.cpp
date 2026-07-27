#include "analysis_series_timeline_buffer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
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

} // namespace

struct AnalysisSeriesTimelineBuffer::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
      repository;
  crimson::timeline::AnalysisSeriesTimelineDescriptor descriptor;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::string archive_identity;
  std::unordered_set<crimson::data::SourceIdentity,
                     crimson::data::SourceIdentityHash>
      scheduler_sources;
  bool stopping = false;
  size_t page_span = 4096;
  size_t page_step = 2048;
  size_t maximum_points = 1200;
  size_t capacity = 3;
  uint64_t generation = 1;
  std::optional<PageKey> last_key;
  std::unordered_map<
      PageKey,
      std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>,
      PageKeyHash>
      cache;
  std::deque<PageKey> cache_order;
  AnalysisSeriesTimelineBufferMetrics metrics;

  crimson::data::SourceIdentity
  schedulerSource(const std::string &source_key) const {
    const auto *source =
        crimson::timeline::findAnalysisSeriesSource(descriptor, source_key);
    const std::string product =
        descriptor.kind == crimson::timeline::AnalysisSeriesKind::Motion
            ? "motion_timeline"
            : "tail_kinematics_timeline";
    std::string run = source_key;
    if (source) {
      run = source->source_group + "/" + source->run_name + "/" + source_key;
    }
    return {archive_identity.empty() ? "in_memory" : archive_identity,
            product, std::move(run)};
  }

  std::optional<PageKey> keyFor(int64_t frame,
                                const std::string &source) const {
    const auto bounds = crimson::timeline::analysisSeriesTimelinePageBounds(
        frame, descriptor.frame_count, page_span, page_step);
    if (!bounds.valid() || source.empty()) {
      return std::nullopt;
    }
    return PageKey{source, bounds.first_frame, bounds.last_frame};
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
};

AnalysisSeriesTimelineBuffer::AnalysisSeriesTimelineBuffer(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    std::string archive_identity)
    : impl_(std::make_unique<Impl>()) {
  impl_->scheduler = scheduler ? std::move(scheduler)
                               : std::make_shared<
                                     crimson::data::DataAccessScheduler>(16, 1);
  impl_->archive_identity = std::move(archive_identity);
}

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
  if (!impl_->scheduler || !impl_->scheduler->running()) {
    assignError(error, "Analysis-series data scheduler is unavailable");
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
    impl_->scheduler_sources.clear();
    impl_->metrics = {};
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void AnalysisSeriesTimelineBuffer::close() {
  std::vector<crimson::data::SourceIdentity> sources;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    impl_->condition.notify_all();
    sources.assign(impl_->scheduler_sources.begin(),
                   impl_->scheduler_sources.end());
  }
  for (const auto &source : sources) {
    impl_->scheduler->cancelSource(source);
  }
  for (const auto &source : sources) {
    impl_->scheduler->waitForSourceIdle(source);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->repository.reset();
  impl_->descriptor = {};
  impl_->scheduler_sources.clear();
  impl_->cache.clear();
  impl_->cache_order.clear();
  impl_->stopping = false;
  impl_->last_key.reset();
}

bool AnalysisSeriesTimelineBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr && impl_->scheduler &&
         impl_->scheduler->running() && !impl_->stopping;
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
  crimson::data::AccessPattern access_pattern =
      crimson::data::AccessPattern::Paused;
  if (discontinuity) {
    access_pattern = crimson::data::AccessPattern::RandomSeek;
  } else if (impl_->last_key) {
    access_pattern = frame >= impl_->last_key->first
                         ? crimson::data::AccessPattern::Forward
                         : crimson::data::AccessPattern::Reverse;
  }
  if (discontinuity || changed_page) {
    ++impl_->generation;
    if (impl_->last_key) {
      impl_->scheduler->cancelSource(
          impl_->schedulerSource(impl_->last_key->source));
    }
  }
  impl_->last_key = key;
  if (impl_->cache.find(*key) != impl_->cache.end()) {
    ++impl_->metrics.cache_hits;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  crimson::timeline::AnalysisSeriesTimelineRequest request;
  request.source_key = source;
  request.first_frame = key->first;
  request.last_frame = key->last;
  request.anchor_frame = frame;
  request.max_points_per_trace = impl_->maximum_points;
  request.fallback_frames_per_second = fallback_frames_per_second;
  const uint64_t request_generation = impl_->generation;
  const auto scheduler_source = impl_->schedulerSource(source);
  impl_->scheduler_sources.insert(scheduler_source);
  crimson::data::DataRangeRequest data_request{
      scheduler_source,
      {key->first, key->last},
      crimson::data::FieldSelection::All(),
      crimson::data::RequestPriority::VisibleWindow,
      access_pattern,
      request_generation};
  const auto outcome = impl_->scheduler->submit(
      std::move(data_request),
      [this, key = *key, request = std::move(request), request_generation](
          const crimson::data::ScheduledDataRequest &scheduled) {
        if (scheduled.cancellation.cancelled()) {
          return crimson::data::DataResultStatus::Stale;
        }
        const auto start = std::chrono::steady_clock::now();
        auto resolved = impl_->repository->resolveWindow(request);
        const auto repository_metrics = impl_->repository->metrics();
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
        auto shared = std::make_shared<
            const crimson::timeline::AnalysisSeriesTimelineWindow>(
            std::move(resolved));
        std::lock_guard<std::mutex> callback_lock(impl_->mutex);
        impl_->metrics.maximum_resolve_ms =
            std::max(impl_->metrics.maximum_resolve_ms, elapsed_ms);
        impl_->metrics.frame_index_block_reads =
            repository_metrics.frame_index_block_reads;
        impl_->metrics.frame_index_cache_hits =
            repository_metrics.frame_index_cache_hits;
        impl_->metrics.frame_index_cache_evictions =
            repository_metrics.frame_index_cache_evictions;
        impl_->metrics.frame_index_source_bytes =
            repository_metrics.frame_index_source_bytes;
        impl_->metrics.cached_frame_index_bytes =
            repository_metrics.cached_frame_index_bytes;
        impl_->metrics.peak_cached_frame_index_bytes =
            repository_metrics.peak_cached_frame_index_bytes;
        impl_->metrics.maximum_frame_index_read_ms =
            repository_metrics.maximum_frame_index_read_ms;
        if (scheduled.cancellation.cancelled() || impl_->stopping ||
            request_generation != impl_->generation) {
          ++impl_->metrics.discarded_results;
          impl_->condition.notify_all();
          return crimson::data::DataResultStatus::Stale;
        }
        impl_->metrics.source_rows_read += shared->source_row_count;
        impl_->metrics.published_points += shared->published_point_count;
        crimson::data::DataResultStatus status =
            crimson::data::DataResultStatus::Failed;
        switch (shared->status) {
        case crimson::timeline::AnalysisSeriesTimelineStatus::Mapped:
          ++impl_->metrics.resolved_windows;
          status = crimson::data::DataResultStatus::Ready;
          break;
        case crimson::timeline::AnalysisSeriesTimelineStatus::Missing:
        case crimson::timeline::AnalysisSeriesTimelineStatus::OutOfRange:
          ++impl_->metrics.missing_windows;
          status = crimson::data::DataResultStatus::Missing;
          break;
        case crimson::timeline::AnalysisSeriesTimelineStatus::InvalidRequest:
        case crimson::timeline::AnalysisSeriesTimelineStatus::ReadFailed:
          ++impl_->metrics.failed_windows;
          impl_->metrics.last_error = shared->error;
          break;
        }
        impl_->publishLocked(key, std::move(shared));
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
  impl_->metrics.peak_pending_windows = std::max(
      impl_->metrics.peak_pending_windows,
      impl_->scheduler->metrics().queue.pending_requests);
  if (!outcome.accepted() &&
      outcome.status !=
          crimson::data::DataRequestSubmitStatus::RejectedCapacity) {
    assignError(error, "Analysis-series timeline scheduler rejected request");
    return false;
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
