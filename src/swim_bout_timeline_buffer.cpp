#include "swim_bout_timeline_buffer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
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
  std::string candidate;
  int64_t first = 0;
  int64_t last = -1;
  bool detector = false;

  bool operator==(const PageKey &other) const {
    return candidate == other.candidate && first == other.first &&
           last == other.last && detector == other.detector;
  }
};

struct PageKeyHash {
  size_t operator()(const PageKey &key) const {
    size_t hash = std::hash<std::string>{}(key.candidate);
    hash ^= std::hash<int64_t>{}(key.first) + 0x9e3779b9 + (hash << 6) +
            (hash >> 2);
    hash ^=
        std::hash<int64_t>{}(key.last) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<bool>{}(key.detector) + 0x9e3779b9 + (hash << 6) +
            (hash >> 2);
    return hash;
  }
};

} // namespace

struct SwimBoutTimelineBuffer::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository>
      repository;
  crimson::timeline::SwimBoutTimelineDescriptor descriptor;
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
      std::shared_ptr<const crimson::timeline::SwimBoutTimelineWindow>,
      PageKeyHash>
      cache;
  std::deque<PageKey> cache_order;
  SwimBoutTimelineBufferMetrics metrics;

  crimson::data::SourceIdentity
  schedulerSource(const std::string &candidate_key) const {
    const auto *candidate =
        crimson::timeline::findSwimBoutCandidate(descriptor, candidate_key);
    const std::string run = candidate
        ? candidate->source_group + "/" + candidate->run_name + "/" + candidate_key
        : candidate_key;
    return {archive_identity.empty() ? "in_memory" : archive_identity,
            "swim_bout_timeline", run};
  }

  std::optional<PageKey> keyFor(int64_t frame,
                                const std::string &candidate, bool detector) const {
    const auto bounds = crimson::timeline::swimBoutTimelinePageBounds(
        frame, descriptor.frame_count, page_span, page_step);
    if (!bounds.valid() || candidate.empty()) {
      return std::nullopt;
    }
    return PageKey{candidate, bounds.first_frame, bounds.last_frame, detector};
  }

  void publishLocked(
      const PageKey &key,
      std::shared_ptr<const crimson::timeline::SwimBoutTimelineWindow>
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

SwimBoutTimelineBuffer::SwimBoutTimelineBuffer(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    std::string archive_identity)
    : impl_(std::make_unique<Impl>()) {
  impl_->scheduler = scheduler ? std::move(scheduler)
                               : std::make_shared<
                                     crimson::data::DataAccessScheduler>(16, 1);
  impl_->archive_identity = std::move(archive_identity);
}

SwimBoutTimelineBuffer::~SwimBoutTimelineBuffer() { close(); }

bool SwimBoutTimelineBuffer::open(
    std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository>
        repository,
    size_t page_span_frames, size_t page_step_frames,
    size_t max_detector_points, size_t cache_capacity, std::string *error) {
  close();
  if (!repository) {
    assignError(error, "Swim-bout timeline repository is null");
    return false;
  }
  if (page_span_frames < 3 || page_step_frames == 0 ||
      page_step_frames > page_span_frames || max_detector_points < 3 ||
      cache_capacity == 0) {
    assignError(error,
                "Swim-bout timeline buffer configuration is invalid");
    return false;
  }
  if (!impl_->scheduler || !impl_->scheduler->running()) {
    assignError(error, "Swim-bout data scheduler is unavailable");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->descriptor = repository->descriptor();
    impl_->repository = std::move(repository);
    impl_->page_span = page_span_frames;
    impl_->page_step = page_step_frames;
    impl_->maximum_points = max_detector_points;
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

void SwimBoutTimelineBuffer::close() {
  std::vector<crimson::data::SourceIdentity> sources;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    impl_->condition.notify_all();
    sources.assign(impl_->scheduler_sources.begin(),
                   impl_->scheduler_sources.end());
  }
  for (const auto &candidate : sources) {
    impl_->scheduler->cancelSource(candidate);
  }
  for (const auto &candidate : sources) {
    impl_->scheduler->waitForSourceIdle(candidate);
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

bool SwimBoutTimelineBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository != nullptr && impl_->scheduler &&
         impl_->scheduler->running() && !impl_->stopping;
}

bool SwimBoutTimelineBuffer::requestFrame(
    int64_t frame, const std::string &candidate_key,
    double fallback_frames_per_second, bool include_detector_trace,
    bool discontinuity, std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Swim-bout timeline buffer is not open");
    return false;
  }
  const std::string candidate =
      candidate_key.empty()
          ? impl_->descriptor.default_candidate
          : candidate_key;
  const auto key = impl_->keyFor(frame, candidate, include_detector_trace);
  if (!key || !std::isfinite(fallback_frames_per_second) ||
      fallback_frames_per_second <= 0.0) {
    assignError(error, "Swim-bout timeline frame request is invalid");
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
          impl_->schedulerSource(impl_->last_key->candidate));
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
  crimson::timeline::SwimBoutTimelineRequest request;
  request.candidate_key = candidate;
  request.first_frame = key->first;
  request.last_frame = key->last;
  request.anchor_frame = frame;
  request.max_detector_points = impl_->maximum_points;
  request.fallback_frames_per_second = fallback_frames_per_second;
  request.include_detector_trace = include_detector_trace;
  const uint64_t request_generation = impl_->generation;
  const auto scheduler_source = impl_->schedulerSource(candidate);
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
        crimson::timeline::SwimBoutTimelineWindow resolved;
        try {
          resolved = impl_->repository->resolveWindow(request);
        } catch (const std::exception &exception) {
          resolved.request = request;
          resolved.status = crimson::timeline::SwimBoutTimelineStatus::ReadFailed;
          resolved.error = exception.what();
        } catch (...) {
          resolved.request = request;
          resolved.status = crimson::timeline::SwimBoutTimelineStatus::ReadFailed;
          resolved.error = "Swim-bout timeline reader threw an unknown exception";
        }
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
        auto shared = std::make_shared<
            const crimson::timeline::SwimBoutTimelineWindow>(
            std::move(resolved));
        std::lock_guard<std::mutex> callback_lock(impl_->mutex);
        impl_->metrics.maximum_resolve_ms =
            std::max(impl_->metrics.maximum_resolve_ms, elapsed_ms);
        if (scheduled.cancellation.cancelled() || impl_->stopping ||
            request_generation != impl_->generation) {
          ++impl_->metrics.discarded_results;
          impl_->condition.notify_all();
          return crimson::data::DataResultStatus::Stale;
        }
        impl_->metrics.candidate_intervals_scanned += shared->candidate_interval_count;
        impl_->metrics.source_detector_rows_read += shared->source_detector_row_count;
        impl_->metrics.published_intervals += shared->intervals.size();
        impl_->metrics.published_detector_points += shared->detector_values.size();
        crimson::data::DataResultStatus status =
            crimson::data::DataResultStatus::Failed;
        switch (shared->status) {
        case crimson::timeline::SwimBoutTimelineStatus::Mapped:
          ++impl_->metrics.resolved_windows;
          status = shared->intervals.empty() && shared->detector_values.empty()
                       ? crimson::data::DataResultStatus::ValidEmpty
                       : crimson::data::DataResultStatus::Ready;
          break;
        case crimson::timeline::SwimBoutTimelineStatus::Missing:
        case crimson::timeline::SwimBoutTimelineStatus::OutOfRange:
          ++impl_->metrics.missing_windows;
          status = crimson::data::DataResultStatus::Missing;
          break;
        case crimson::timeline::SwimBoutTimelineStatus::InvalidRequest:
        case crimson::timeline::SwimBoutTimelineStatus::ReadFailed:
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
    assignError(error, "Swim-bout timeline scheduler rejected request");
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool SwimBoutTimelineBuffer::waitForFrame(
    int64_t frame, const std::string &candidate_key,
    double fallback_frames_per_second, bool include_detector_trace,
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const std::string candidate =
      candidate_key.empty()
          ? impl_->descriptor.default_candidate
          : candidate_key;
  const auto key = impl_->keyFor(frame, candidate, include_detector_trace);
  if (!key || !std::isfinite(fallback_frames_per_second) ||
      fallback_frames_per_second <= 0.0) {
    return false;
  }
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->stopping || impl_->cache.find(*key) != impl_->cache.end();
  }) && !impl_->stopping;
}

std::shared_ptr<const crimson::timeline::SwimBoutTimelineWindow>
SwimBoutTimelineBuffer::window(int64_t frame,
                                     const std::string &candidate_key,
                                     bool include_detector_trace) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping) {
    return nullptr;
  }
  const std::string candidate =
      candidate_key.empty()
          ? impl_->descriptor.default_candidate
          : candidate_key;
  const auto key = impl_->keyFor(frame, candidate, include_detector_trace);
  if (!key) {
    return nullptr;
  }
  const auto found = impl_->cache.find(*key);
  return found == impl_->cache.end() ? nullptr : found->second;
}

crimson::timeline::SwimBoutTimelineDescriptor
SwimBoutTimelineBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

SwimBoutTimelineBufferMetrics
SwimBoutTimelineBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}
