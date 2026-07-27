#pragma once

#include "data_access.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace crimson::data {

class DataCancellationToken {
public:
  DataCancellationToken() = default;

  bool cancelled() const;
  explicit operator bool() const;

private:
  struct State {
    std::atomic<bool> cancelled{false};
  };

  explicit DataCancellationToken(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
  friend class DataAccessQueue;
};

struct ScheduledDataRequest {
  DataRangeRequest request;
  uint64_t sequence = 0;
  DataCancellationToken cancellation;
};

enum class DataRequestSubmitStatus : uint8_t {
  Accepted,
  Duplicate,
  Promoted,
  RejectedInvalid,
  RejectedStale,
  RejectedCapacity,
};

struct DataRequestSubmitOutcome {
  DataRequestSubmitStatus status = DataRequestSubmitStatus::RejectedInvalid;
  uint64_t sequence = 0;
  size_t cancelled_requests = 0;
  size_t capacity_evictions = 0;

  bool accepted() const;
};

struct DataAccessQueueMetrics {
  uint64_t submissions = 0;
  uint64_t accepted = 0;
  uint64_t duplicates = 0;
  uint64_t promotions = 0;
  uint64_t rejected_invalid = 0;
  uint64_t rejected_stale = 0;
  uint64_t rejected_capacity = 0;
  uint64_t cancelled_requests = 0;
  uint64_t capacity_evictions = 0;
  uint64_t completed_requests = 0;
  uint64_t discarded_completions = 0;
  uint64_t failed_completions = 0;
  size_t pending_requests = 0;
  size_t peak_pending_requests = 0;
  size_t active_requests = 0;
  size_t peak_active_requests = 0;
};

class DataAccessQueue {
public:
  explicit DataAccessQueue(size_t capacity);
  ~DataAccessQueue();

  DataAccessQueue(const DataAccessQueue &) = delete;
  DataAccessQueue &operator=(const DataAccessQueue &) = delete;

  DataRequestSubmitOutcome submit(DataRangeRequest request);
  std::optional<ScheduledDataRequest> popNext();
  std::optional<ScheduledDataRequest>
  popNextRunnable(RequestPriority maximum_priority);
  bool complete(uint64_t sequence, DataResultStatus status);

  size_t advanceGeneration(const SourceIdentity &source, uint64_t generation);
  size_t retainSourceRange(const SourceIdentity &source, uint64_t generation,
                           const FrameRange &frames);
  size_t cancelSource(const SourceIdentity &source);
  void clear();

  bool contains(uint64_t sequence) const;
  bool containsSource(const SourceIdentity &source) const;
  bool hasRunnable(RequestPriority maximum_priority) const;
  size_t size() const;
  size_t capacity() const;
  DataAccessQueueMetrics metrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char *dataRequestSubmitStatusName(DataRequestSubmitStatus status);

using DataAccessWork =
    std::function<DataResultStatus(const ScheduledDataRequest &request)>;

constexpr size_t kDataRequestPriorityCount =
    static_cast<size_t>(RequestPriority::Speculative) + 1;

struct DataAccessTimingMetrics {
  uint64_t started = 0;
  uint64_t completed = 0;
  double total_queue_wait_ms = 0.0;
  double maximum_queue_wait_ms = 0.0;
  double total_service_ms = 0.0;
  double maximum_service_ms = 0.0;
  uint64_t queue_wait_over_100_ms = 0;
  uint64_t queue_wait_over_1000_ms = 0;
  uint64_t queue_wait_over_5000_ms = 0;
  uint64_t service_over_100_ms = 0;
  uint64_t service_over_1000_ms = 0;
  uint64_t service_over_5000_ms = 0;

  double averageQueueWaitMs() const {
    return started == 0 ? 0.0 : total_queue_wait_ms / started;
  }
  double averageServiceMs() const {
    return completed == 0 ? 0.0 : total_service_ms / completed;
  }
};

struct DataAccessSourceTimingMetrics {
  SourceIdentity source;
  std::array<DataAccessTimingMetrics, kDataRequestPriorityCount> by_priority;
};

struct DataAccessSchedulerMetrics {
  DataAccessQueueMetrics queue;
  uint64_t work_started = 0;
  uint64_t work_completed = 0;
  uint64_t work_exceptions = 0;
  size_t worker_count = 0;
  size_t peak_active_speculative_requests = 0;
  std::array<DataAccessTimingMetrics, kDataRequestPriorityCount>
      timing_by_priority;
  std::vector<DataAccessSourceTimingMetrics> timing_by_source;
};

class DataAccessScheduler {
public:
  DataAccessScheduler(size_t pending_capacity, size_t worker_count,
                      size_t maximum_speculative_workers = 1);
  ~DataAccessScheduler();

  DataAccessScheduler(const DataAccessScheduler &) = delete;
  DataAccessScheduler &operator=(const DataAccessScheduler &) = delete;

  DataRequestSubmitOutcome submit(DataRangeRequest request,
                                  DataAccessWork work);
  size_t advanceGeneration(const SourceIdentity &source, uint64_t generation);
  size_t retainSourceRange(const SourceIdentity &source, uint64_t generation,
                           const FrameRange &frames);
  size_t cancelSource(const SourceIdentity &source);
  void waitForSourceIdle(const SourceIdentity &source);
  void waitUntilIdle();
  void shutdown();

  bool running() const;
  DataAccessSchedulerMetrics metrics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace crimson::data
