#include "data_access_scheduler.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crimson::data {
namespace {

bool scheduledBefore(const ScheduledDataRequest &left,
                     const ScheduledDataRequest &right) {
  if (left.request.priority != right.request.priority) {
    return static_cast<uint8_t>(left.request.priority) <
           static_cast<uint8_t>(right.request.priority);
  }
  return left.sequence < right.sequence;
}

} // namespace

DataCancellationToken::DataCancellationToken(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

bool DataCancellationToken::cancelled() const {
  return !state_ || state_->cancelled.load(std::memory_order_acquire);
}

DataCancellationToken::operator bool() const { return state_ != nullptr; }

bool DataRequestSubmitOutcome::accepted() const {
  return status == DataRequestSubmitStatus::Accepted ||
         status == DataRequestSubmitStatus::Duplicate ||
         status == DataRequestSubmitStatus::Promoted;
}

struct DataAccessQueue::Impl {
  explicit Impl(size_t maximum_pending) : capacity(maximum_pending) {}

  mutable std::mutex mutex;
  size_t capacity = 0;
  uint64_t next_sequence = 1;
  std::vector<ScheduledDataRequest> pending;
  std::unordered_map<uint64_t, ScheduledDataRequest> active;
  std::unordered_map<SourceIdentity, uint64_t, SourceIdentityHash> generations;
  DataAccessQueueMetrics metrics;

  bool cancel(const ScheduledDataRequest &request) {
    if (!request.cancellation.state_) {
      return false;
    }
    return !request.cancellation.state_->cancelled.exchange(
        true, std::memory_order_acq_rel);
  }

  size_t advanceGenerationLocked(const SourceIdentity &source,
                                 uint64_t generation) {
    if (!source.valid() || generation == 0) {
      return 0;
    }
    const auto found = generations.find(source);
    if (found != generations.end() && generation <= found->second) {
      return 0;
    }
    generations[source] = generation;
    size_t cancelled = 0;
    pending.erase(
        std::remove_if(
            pending.begin(), pending.end(), [&](const ScheduledDataRequest &item) {
              if (item.request.source != source ||
                  item.request.generation >= generation) {
                return false;
              }
              if (cancel(item)) {
                ++cancelled;
              }
              return true;
            }),
        pending.end());
    for (const auto &item : active) {
      if (item.second.request.source == source &&
          item.second.request.generation < generation &&
          cancel(item.second)) {
        ++cancelled;
      }
    }
    metrics.cancelled_requests += cancelled;
    metrics.pending_requests = pending.size();
    return cancelled;
  }
};

DataAccessQueue::DataAccessQueue(size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}

DataAccessQueue::~DataAccessQueue() { clear(); }

DataRequestSubmitOutcome DataAccessQueue::submit(DataRangeRequest request) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ++impl_->metrics.submissions;
  request = request.normalized();
  if (impl_->capacity == 0 || !request.valid()) {
    ++impl_->metrics.rejected_invalid;
    return {DataRequestSubmitStatus::RejectedInvalid};
  }

  DataRequestSubmitOutcome outcome;
  const auto generation = impl_->generations.find(request.source);
  if (generation != impl_->generations.end() &&
      request.generation < generation->second) {
    ++impl_->metrics.rejected_stale;
    outcome.status = DataRequestSubmitStatus::RejectedStale;
    return outcome;
  }
  if (generation == impl_->generations.end() ||
      request.generation > generation->second) {
    outcome.cancelled_requests =
        impl_->advanceGenerationLocked(request.source, request.generation);
  }

  const auto duplicate = std::find_if(
      impl_->pending.begin(), impl_->pending.end(),
      [&](const ScheduledDataRequest &item) {
        return equivalentDataWork(item.request, request);
      });
  if (duplicate != impl_->pending.end()) {
    outcome.sequence = duplicate->sequence;
    duplicate->request.access_pattern = request.access_pattern;
    if (static_cast<uint8_t>(request.priority) <
        static_cast<uint8_t>(duplicate->request.priority)) {
      duplicate->request.priority = request.priority;
      ++impl_->metrics.promotions;
      outcome.status = DataRequestSubmitStatus::Promoted;
    } else {
      ++impl_->metrics.duplicates;
      outcome.status = DataRequestSubmitStatus::Duplicate;
    }
    return outcome;
  }
  const auto active_duplicate = std::find_if(
      impl_->active.begin(), impl_->active.end(),
      [&](const auto &item) {
        return !item.second.cancellation.cancelled() &&
               equivalentDataWork(item.second.request, request);
      });
  if (active_duplicate != impl_->active.end()) {
    ++impl_->metrics.duplicates;
    outcome.status = DataRequestSubmitStatus::Duplicate;
    outcome.sequence = active_duplicate->second.sequence;
    return outcome;
  }

  auto state = std::make_shared<DataCancellationToken::State>();
  ScheduledDataRequest scheduled{
      std::move(request), impl_->next_sequence++, DataCancellationToken(state)};

  if (impl_->pending.size() >= impl_->capacity) {
    const auto worst = std::max_element(impl_->pending.begin(),
                                        impl_->pending.end(), scheduledBefore);
    if (worst == impl_->pending.end() || !scheduledBefore(scheduled, *worst)) {
      ++impl_->metrics.rejected_capacity;
      outcome.status = DataRequestSubmitStatus::RejectedCapacity;
      return outcome;
    }
    impl_->cancel(*worst);
    impl_->pending.erase(worst);
    ++impl_->metrics.cancelled_requests;
    ++impl_->metrics.capacity_evictions;
    outcome.cancelled_requests = 1;
    outcome.capacity_evictions = 1;
  }

  outcome.status = DataRequestSubmitStatus::Accepted;
  outcome.sequence = scheduled.sequence;
  impl_->pending.push_back(std::move(scheduled));
  ++impl_->metrics.accepted;
  impl_->metrics.pending_requests = impl_->pending.size();
  impl_->metrics.peak_pending_requests =
      std::max(impl_->metrics.peak_pending_requests, impl_->pending.size());
  return outcome;
}

std::optional<ScheduledDataRequest> DataAccessQueue::popNext() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->pending.empty()) {
    return std::nullopt;
  }
  const auto best = std::min_element(impl_->pending.begin(),
                                     impl_->pending.end(), scheduledBefore);
  ScheduledDataRequest result = std::move(*best);
  impl_->pending.erase(best);
  impl_->active.emplace(result.sequence, result);
  impl_->metrics.pending_requests = impl_->pending.size();
  impl_->metrics.active_requests = impl_->active.size();
  impl_->metrics.peak_active_requests =
      std::max(impl_->metrics.peak_active_requests, impl_->active.size());
  return result;
}

std::optional<ScheduledDataRequest>
DataAccessQueue::popNextRunnable(RequestPriority maximum_priority) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto runnable = [&](const ScheduledDataRequest &candidate) {
    if (static_cast<uint8_t>(candidate.request.priority) >
        static_cast<uint8_t>(maximum_priority)) {
      return false;
    }
    return std::none_of(
        impl_->active.begin(), impl_->active.end(), [&](const auto &active) {
          return !active.second.cancellation.cancelled() &&
                 active.second.request.source == candidate.request.source;
        });
  };
  auto best = impl_->pending.end();
  for (auto candidate = impl_->pending.begin();
       candidate != impl_->pending.end(); ++candidate) {
    if (runnable(*candidate) &&
        (best == impl_->pending.end() || scheduledBefore(*candidate, *best))) {
      best = candidate;
    }
  }
  if (best == impl_->pending.end()) {
    return std::nullopt;
  }
  ScheduledDataRequest result = std::move(*best);
  impl_->pending.erase(best);
  impl_->active.emplace(result.sequence, result);
  impl_->metrics.pending_requests = impl_->pending.size();
  impl_->metrics.active_requests = impl_->active.size();
  impl_->metrics.peak_active_requests =
      std::max(impl_->metrics.peak_active_requests, impl_->active.size());
  return result;
}

bool DataAccessQueue::complete(uint64_t sequence, DataResultStatus status) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->active.find(sequence);
  if (found == impl_->active.end() || status == DataResultStatus::Pending) {
    return false;
  }
  ++impl_->metrics.completed_requests;
  if (status == DataResultStatus::Stale ||
      status == DataResultStatus::Discarded) {
    ++impl_->metrics.discarded_completions;
  } else if (status == DataResultStatus::Failed) {
    ++impl_->metrics.failed_completions;
  }
  impl_->active.erase(found);
  impl_->metrics.active_requests = impl_->active.size();
  return true;
}

size_t DataAccessQueue::advanceGeneration(const SourceIdentity &source,
                                          uint64_t generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->advanceGenerationLocked(source, generation);
}

size_t DataAccessQueue::retainSourceRange(const SourceIdentity &source,
                                          uint64_t generation,
                                          const FrameRange &frames) {
  if (!source.valid() || generation == 0 || !frames.valid()) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  size_t cancelled = 0;
  impl_->pending.erase(
      std::remove_if(
          impl_->pending.begin(), impl_->pending.end(),
          [&](const ScheduledDataRequest &item) {
            if (item.request.source != source ||
                item.request.generation != generation ||
                item.request.frames.intersects(frames)) {
              return false;
            }
            if (impl_->cancel(item)) {
              ++cancelled;
            }
            return true;
          }),
      impl_->pending.end());
  impl_->metrics.cancelled_requests += cancelled;
  impl_->metrics.pending_requests = impl_->pending.size();
  return cancelled;
}

size_t DataAccessQueue::cancelSource(const SourceIdentity &source) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  size_t cancelled = 0;
  impl_->pending.erase(
      std::remove_if(
          impl_->pending.begin(), impl_->pending.end(),
          [&](const ScheduledDataRequest &item) {
            if (item.request.source != source) {
              return false;
            }
            if (impl_->cancel(item)) {
              ++cancelled;
            }
            return true;
          }),
      impl_->pending.end());
  for (const auto &item : impl_->active) {
    if (item.second.request.source == source && impl_->cancel(item.second)) {
      ++cancelled;
    }
  }
  impl_->generations.erase(source);
  impl_->metrics.cancelled_requests += cancelled;
  impl_->metrics.pending_requests = impl_->pending.size();
  return cancelled;
}

void DataAccessQueue::clear() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  size_t cancelled = 0;
  for (const auto &request : impl_->pending) {
    if (impl_->cancel(request)) {
      ++cancelled;
    }
  }
  for (const auto &request : impl_->active) {
    if (impl_->cancel(request.second)) {
      ++cancelled;
    }
  }
  impl_->metrics.cancelled_requests += cancelled;
  impl_->pending.clear();
  impl_->active.clear();
  impl_->generations.clear();
  impl_->metrics.pending_requests = 0;
  impl_->metrics.active_requests = 0;
}

bool DataAccessQueue::contains(uint64_t sequence) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->active.find(sequence) != impl_->active.end() ||
         std::any_of(impl_->pending.begin(), impl_->pending.end(),
                     [&](const ScheduledDataRequest &request) {
                       return request.sequence == sequence;
                     });
}

bool DataAccessQueue::containsSource(const SourceIdentity &source) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return std::any_of(impl_->pending.begin(), impl_->pending.end(),
                     [&](const ScheduledDataRequest &request) {
                       return request.request.source == source;
                     }) ||
         std::any_of(impl_->active.begin(), impl_->active.end(),
                     [&](const auto &request) {
                       return request.second.request.source == source;
                     });
}

bool DataAccessQueue::hasRunnable(RequestPriority maximum_priority) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return std::any_of(
      impl_->pending.begin(), impl_->pending.end(),
      [&](const ScheduledDataRequest &candidate) {
        if (static_cast<uint8_t>(candidate.request.priority) >
            static_cast<uint8_t>(maximum_priority)) {
          return false;
        }
        return std::none_of(
            impl_->active.begin(), impl_->active.end(),
            [&](const auto &active) {
              return !active.second.cancellation.cancelled() &&
                     active.second.request.source ==
                         candidate.request.source;
            });
      });
}

size_t DataAccessQueue::size() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->pending.size();
}

size_t DataAccessQueue::capacity() const { return impl_->capacity; }

DataAccessQueueMetrics DataAccessQueue::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}

const char *dataRequestSubmitStatusName(DataRequestSubmitStatus status) {
  switch (status) {
  case DataRequestSubmitStatus::Accepted:
    return "accepted";
  case DataRequestSubmitStatus::Duplicate:
    return "duplicate";
  case DataRequestSubmitStatus::Promoted:
    return "promoted";
  case DataRequestSubmitStatus::RejectedInvalid:
    return "rejected_invalid";
  case DataRequestSubmitStatus::RejectedStale:
    return "rejected_stale";
  case DataRequestSubmitStatus::RejectedCapacity:
    return "rejected_capacity";
  }
  return "unknown";
}

struct DataAccessScheduler::Impl {
  Impl(size_t pending_capacity, size_t requested_workers,
       size_t requested_speculative_workers)
      : queue(pending_capacity), worker_count(requested_workers),
        maximum_speculative_workers(
            std::min(requested_workers, requested_speculative_workers)) {}

  mutable std::mutex mutex;
  std::condition_variable condition;
  DataAccessQueue queue;
  size_t worker_count = 0;
  size_t maximum_speculative_workers = 0;
  size_t active_speculative_requests = 0;
  size_t peak_active_speculative_requests = 0;
  bool stopping = false;
  std::unordered_map<uint64_t, DataAccessWork> tasks;
  std::vector<std::thread> workers;
  uint64_t work_started = 0;
  uint64_t work_completed = 0;
  uint64_t work_exceptions = 0;

  bool workCanStartLocked() const {
    if (queue.hasRunnable(RequestPriority::VisibleWindow)) {
      return true;
    }
    return active_speculative_requests < maximum_speculative_workers &&
           queue.hasRunnable(RequestPriority::Speculative);
  }

  void removeDetachedTasksLocked() {
    for (auto task = tasks.begin(); task != tasks.end();) {
      if (!queue.contains(task->first)) {
        task = tasks.erase(task);
      } else {
        ++task;
      }
    }
  }

  void run() {
    while (true) {
      ScheduledDataRequest request;
      DataAccessWork work;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return stopping || workCanStartLocked(); });
        if (stopping) {
          return;
        }
        const RequestPriority maximum_priority =
            active_speculative_requests < maximum_speculative_workers
                ? RequestPriority::Speculative
                : RequestPriority::VisibleWindow;
        auto scheduled = queue.popNextRunnable(maximum_priority);
        if (!scheduled) {
          continue;
        }
        request = std::move(*scheduled);
        if (request.request.priority == RequestPriority::Speculative) {
          ++active_speculative_requests;
          peak_active_speculative_requests =
              std::max(peak_active_speculative_requests,
                       active_speculative_requests);
        }
        const auto task = tasks.find(request.sequence);
        if (task == tasks.end()) {
          if (request.request.priority == RequestPriority::Speculative) {
            --active_speculative_requests;
          }
          queue.complete(request.sequence, DataResultStatus::Discarded);
          condition.notify_all();
          continue;
        }
        work = task->second;
        ++work_started;
      }

      DataResultStatus status = DataResultStatus::Failed;
      try {
        status = work(request);
        if (!DataRangeResult{request.request, status}.terminal()) {
          status = DataResultStatus::Failed;
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        ++work_exceptions;
        status = DataResultStatus::Failed;
      }
      if (request.cancellation.cancelled() &&
          status != DataResultStatus::Stale &&
          status != DataResultStatus::Discarded) {
        status = DataResultStatus::Discarded;
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        if (request.request.priority == RequestPriority::Speculative) {
          --active_speculative_requests;
        }
        queue.complete(request.sequence, status);
        tasks.erase(request.sequence);
        ++work_completed;
        condition.notify_all();
      }
    }
  }
};

DataAccessScheduler::DataAccessScheduler(size_t pending_capacity,
                                         size_t worker_count,
                                         size_t maximum_speculative_workers)
    : impl_(std::make_unique<Impl>(pending_capacity, worker_count,
                                   maximum_speculative_workers)) {
  if (pending_capacity == 0 || worker_count == 0) {
    return;
  }
  impl_->workers.reserve(worker_count);
  for (size_t index = 0; index < worker_count; ++index) {
    impl_->workers.emplace_back([this] { impl_->run(); });
  }
}

DataAccessScheduler::~DataAccessScheduler() { shutdown(); }

DataRequestSubmitOutcome DataAccessScheduler::submit(DataRangeRequest request,
                                                     DataAccessWork work) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping || impl_->workers.empty() || !work) {
    return {DataRequestSubmitStatus::RejectedInvalid};
  }
  auto outcome = impl_->queue.submit(std::move(request));
  if (outcome.status == DataRequestSubmitStatus::Accepted) {
    impl_->tasks.emplace(outcome.sequence, std::move(work));
  }
  impl_->removeDetachedTasksLocked();
  if (outcome.accepted()) {
    impl_->condition.notify_all();
  }
  return outcome;
}

size_t DataAccessScheduler::advanceGeneration(const SourceIdentity &source,
                                              uint64_t generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const size_t cancelled = impl_->queue.advanceGeneration(source, generation);
  impl_->removeDetachedTasksLocked();
  impl_->condition.notify_all();
  return cancelled;
}

size_t DataAccessScheduler::retainSourceRange(const SourceIdentity &source,
                                              uint64_t generation,
                                              const FrameRange &frames) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const size_t cancelled =
      impl_->queue.retainSourceRange(source, generation, frames);
  impl_->removeDetachedTasksLocked();
  impl_->condition.notify_all();
  return cancelled;
}

size_t DataAccessScheduler::cancelSource(const SourceIdentity &source) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const size_t cancelled = impl_->queue.cancelSource(source);
  impl_->removeDetachedTasksLocked();
  impl_->condition.notify_all();
  return cancelled;
}

void DataAccessScheduler::waitForSourceIdle(const SourceIdentity &source) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->condition.wait(
      lock, [&] { return !impl_->queue.containsSource(source); });
}

void DataAccessScheduler::waitUntilIdle() {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->condition.wait(lock, [&] {
    const auto metrics = impl_->queue.metrics();
    return metrics.pending_requests == 0 && metrics.active_requests == 0;
  });
}

void DataAccessScheduler::shutdown() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping && impl_->workers.empty()) {
      return;
    }
    impl_->stopping = true;
    impl_->queue.clear();
    impl_->tasks.clear();
    impl_->condition.notify_all();
  }
  for (auto &worker : impl_->workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->workers.clear();
  impl_->condition.notify_all();
}

bool DataAccessScheduler::running() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return !impl_->stopping && !impl_->workers.empty();
}

DataAccessSchedulerMetrics DataAccessScheduler::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return {impl_->queue.metrics(), impl_->work_started, impl_->work_completed,
          impl_->work_exceptions, impl_->worker_count,
          impl_->peak_active_speculative_requests};
}

} // namespace crimson::data
