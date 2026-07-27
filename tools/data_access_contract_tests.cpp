#include "data_access.h"
#include "data_access_cache.h"
#include "data_access_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

crimson::data::SourceIdentity source(std::string run = "run-a") {
  return {"fixture.zarr", "subject_masks", std::move(run)};
}

crimson::data::DataRangeRequest
request(int64_t first, int64_t last, crimson::data::RequestPriority priority,
        uint64_t generation = 1,
        crimson::data::AccessPattern pattern =
            crimson::data::AccessPattern::Forward) {
  return {source(),
          {first, last},
          crimson::data::FieldSelection::Named({"contours", "mask"}),
          priority,
          pattern,
          generation};
}

bool testRequestAndResultContract() {
  using namespace crimson::data;
  CHECK(source().valid());
  CHECK(!SourceIdentity{}.valid());
  CHECK((FrameRange{4, 9}.valid()));
  CHECK((FrameRange{4, 9}.size() == 6));
  CHECK((FrameRange{4, 9}.contains(7)));
  CHECK(!(FrameRange{4, 9}.contains(10)));
  CHECK((FrameRange{4, 9}.intersects({9, 12})));
  CHECK(!(FrameRange{4, 9}.intersects({10, 12})));

  const auto named = FieldSelection::Named({"mask", "contours", "mask", ""});
  CHECK(named.valid());
  CHECK(named.names == std::vector<std::string>({"contours", "mask"}));
  CHECK(FieldSelection::All().valid());
  CHECK(!(FieldSelection{true, {"mask"}}.valid()));

  auto left = request(10, 20, RequestPriority::Speculative);
  auto right = request(10, 20, RequestPriority::CurrentFrame, 1,
                       AccessPattern::RandomSeek);
  right.fields = FieldSelection::Named({"mask", "contours"});
  CHECK(left.valid());
  CHECK(equivalentDataWork(left, right));
  right.generation = 2;
  CHECK(!equivalentDataWork(left, right));

  DataRangeResult result;
  result.status = DataResultStatus::Unavailable;
  CHECK(result.terminal());
  CHECK(result.publishable());
  result.status = DataResultStatus::Pending;
  CHECK(!result.terminal());
  CHECK(!result.publishable());
  result.status = DataResultStatus::ValidEmpty;
  CHECK(result.terminal());
  CHECK(result.publishable());
  result.status = DataResultStatus::Stale;
  CHECK(result.terminal());
  CHECK(!result.publishable());
  CHECK(std::string(dataResultStatusName(DataResultStatus::Ready)) == "ready");
  CHECK(std::string(requestPriorityName(RequestPriority::Inspection)) ==
        "inspection");
  CHECK(std::string(accessPatternName(AccessPattern::ReviewNavigation)) ==
        "review_navigation");
  return true;
}

bool testPriorityDeduplicationAndGenerationCancellation() {
  using namespace crimson::data;
  DataAccessQueue queue(4);
  const auto speculative =
      queue.submit(request(100, 119, RequestPriority::Speculative));
  CHECK(speculative.status == DataRequestSubmitStatus::Accepted);

  auto demand = request(100, 119, RequestPriority::CurrentFrame, 1,
                        AccessPattern::RandomSeek);
  demand.fields = FieldSelection::Named({"mask", "contours", "mask"});
  const auto promoted = queue.submit(std::move(demand));
  CHECK(promoted.status == DataRequestSubmitStatus::Promoted);
  CHECK(promoted.sequence == speculative.sequence);
  CHECK(queue.size() == 1);

  const auto scheduled = queue.popNext();
  CHECK(scheduled.has_value());
  CHECK(scheduled->request.priority == RequestPriority::CurrentFrame);
  CHECK(scheduled->request.access_pattern == AccessPattern::RandomSeek);
  CHECK(!scheduled->cancellation.cancelled());
  const auto active_duplicate = queue.submit(request(
      100, 119, RequestPriority::CurrentFrame, 1, AccessPattern::Paused));
  CHECK(active_duplicate.status == DataRequestSubmitStatus::Duplicate);
  CHECK(active_duplicate.sequence == scheduled->sequence);
  CHECK(queue.advanceGeneration(source(), 2) == 1);
  CHECK(scheduled->cancellation.cancelled());
  CHECK(queue.complete(scheduled->sequence, DataResultStatus::Stale));

  const auto stale =
      queue.submit(request(100, 119, RequestPriority::CurrentFrame, 1));
  CHECK(stale.status == DataRequestSubmitStatus::RejectedStale);
  const auto current =
      queue.submit(request(120, 139, RequestPriority::CurrentFrame, 2));
  CHECK(current.accepted());
  const auto current_work = queue.popNext();
  CHECK(current_work.has_value());
  CHECK(queue.cancelSource(source()) == 1);
  CHECK(current_work->cancellation.cancelled());
  CHECK(queue.complete(current_work->sequence, DataResultStatus::Discarded));

  const auto metrics = queue.metrics();
  CHECK(metrics.accepted == 2);
  CHECK(metrics.promotions == 1);
  CHECK(metrics.duplicates == 1);
  CHECK(metrics.rejected_stale == 1);
  CHECK(metrics.peak_pending_requests == 1);
  CHECK(metrics.peak_active_requests == 1);
  CHECK(metrics.completed_requests == 2);
  CHECK(metrics.discarded_completions == 2);
  return true;
}

bool testBoundedDemandFirstQueue() {
  using namespace crimson::data;
  DataAccessQueue queue(2);
  CHECK(queue.submit(request(0, 9, RequestPriority::Speculative)).accepted());
  CHECK(
      queue.submit(request(10, 19, RequestPriority::VisibleWindow)).accepted());
  const auto demand =
      queue.submit(request(20, 20, RequestPriority::CurrentFrame));
  CHECK(demand.status == DataRequestSubmitStatus::Accepted);
  CHECK(demand.capacity_evictions == 1);
  CHECK(queue.size() == 2);

  const auto first = queue.popNext();
  const auto second = queue.popNext();
  CHECK(first && second);
  CHECK((first->request.frames == FrameRange{20, 20}));
  CHECK((second->request.frames == FrameRange{10, 19}));
  CHECK(!queue.popNext());

  CHECK(
      queue.submit(request(30, 39, RequestPriority::CurrentFrame)).accepted());
  CHECK(queue.submit(request(40, 49, RequestPriority::Inspection)).accepted());
  const auto rejected =
      queue.submit(request(50, 59, RequestPriority::Speculative));
  CHECK(rejected.status == DataRequestSubmitStatus::RejectedCapacity);
  const auto metrics = queue.metrics();
  CHECK(metrics.capacity_evictions == 1);
  CHECK(metrics.rejected_capacity == 1);
  CHECK(metrics.pending_requests == 2);

  DataAccessQueue retained(4);
  CHECK(
      retained.submit(request(0, 0, RequestPriority::Speculative)).accepted());
  CHECK(
      retained.submit(request(1, 1, RequestPriority::Speculative)).accepted());
  CHECK(
      retained.submit(request(2, 2, RequestPriority::Speculative)).accepted());
  CHECK(retained.retainSourceRange(source(), 1, {1, 1}) == 2);
  CHECK(retained.size() == 1);
  CHECK(retained.containsSource(source()));
  const auto retained_work = retained.popNext();
  CHECK(retained_work.has_value());
  CHECK(retained.contains(retained_work->sequence));
  CHECK((retained_work->request.frames == FrameRange{1, 1}));
  CHECK(retained.complete(retained_work->sequence, DataResultStatus::Ready));
  CHECK(!retained.containsSource(source()));
  return true;
}

bool testSharedWorkerScheduler() {
  using namespace crimson::data;
  DataAccessScheduler scheduler(8, 1);
  CHECK(scheduler.running());

  std::mutex mutex;
  std::condition_variable condition;
  bool blocker_started = false;
  bool release_blocker = false;
  std::vector<int64_t> execution_order;
  const auto blocker = scheduler.submit(
      request(0, 0, RequestPriority::Inspection),
      [&](const ScheduledDataRequest &scheduled) {
        std::unique_lock<std::mutex> lock(mutex);
        blocker_started = true;
        execution_order.push_back(scheduled.request.frames.first);
        condition.notify_all();
        condition.wait(lock, [&] {
          return release_blocker || scheduled.cancellation.cancelled();
        });
        return scheduled.cancellation.cancelled() ? DataResultStatus::Stale
                                                  : DataResultStatus::Ready;
      });
  CHECK(blocker.accepted());
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return blocker_started; }));
  }

  CHECK(scheduler
            .submit(request(10, 10, RequestPriority::Speculative),
                    [&](const ScheduledDataRequest &scheduled) {
                      std::lock_guard<std::mutex> lock(mutex);
                      execution_order.push_back(scheduled.request.frames.first);
                      return DataResultStatus::Ready;
                    })
            .accepted());
  CHECK(scheduler
            .submit(request(20, 20, RequestPriority::CurrentFrame),
                    [&](const ScheduledDataRequest &scheduled) {
                      std::lock_guard<std::mutex> lock(mutex);
                      execution_order.push_back(scheduled.request.frames.first);
                      return DataResultStatus::Ready;
                    })
            .accepted());
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_blocker = true;
    condition.notify_all();
  }
  scheduler.waitUntilIdle();
  CHECK(execution_order == std::vector<int64_t>({0, 20, 10}));
  const auto ordered_metrics = scheduler.metrics();
  CHECK(ordered_metrics.worker_count == 1);
  CHECK(ordered_metrics.work_started == 3);
  CHECK(ordered_metrics.work_completed == 3);
  CHECK(ordered_metrics.queue.peak_active_requests == 1);

  bool cancelled_work_started = false;
  bool cancelled_work_observed = false;
  const auto active = scheduler.submit(
      request(30, 30, RequestPriority::CurrentFrame, 1),
      [&](const ScheduledDataRequest &scheduled) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          cancelled_work_started = true;
          condition.notify_all();
        }
        while (!scheduled.cancellation.cancelled()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        cancelled_work_observed = true;
        return DataResultStatus::Stale;
      });
  CHECK(active.accepted());
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return cancelled_work_started; }));
  }
  CHECK(scheduler
            .submit(request(31, 31, RequestPriority::Speculative, 1),
                    [](const ScheduledDataRequest &) {
                      return DataResultStatus::Ready;
                    })
            .accepted());
  CHECK(scheduler.advanceGeneration(source(), 2) == 2);
  scheduler.waitForSourceIdle(source());
  CHECK(cancelled_work_observed);
  CHECK(scheduler.metrics().queue.cancelled_requests >= 2);

  CHECK(scheduler
            .submit(request(40, 40, RequestPriority::CurrentFrame, 2),
                    [](const ScheduledDataRequest &) -> DataResultStatus {
                      throw 7;
                    })
            .accepted());
  scheduler.waitUntilIdle();
  const auto final_metrics = scheduler.metrics();
  CHECK(final_metrics.work_exceptions == 1);
  CHECK(final_metrics.queue.failed_completions == 1);
  scheduler.shutdown();
  CHECK(!scheduler.running());
  return true;
}

bool testDemandReservationAndSourceIsolation() {
  using namespace crimson::data;
  DataAccessScheduler scheduler(16, 4, 1);
  std::mutex mutex;
  std::condition_variable condition;
  bool release = false;
  bool first_speculative_started = false;
  bool second_speculative_started = false;
  size_t visible_started = 0;

  auto first_speculative = request(0, 0, RequestPriority::Speculative);
  first_speculative.source = source("speculative-a");
  CHECK(scheduler
            .submit(std::move(first_speculative),
                    [&](const ScheduledDataRequest &) {
                      std::unique_lock<std::mutex> lock(mutex);
                      first_speculative_started = true;
                      condition.notify_all();
                      condition.wait(lock, [&] { return release; });
                      return DataResultStatus::Ready;
                    })
            .accepted());
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return first_speculative_started; }));
  }

  auto second_speculative = request(1, 1, RequestPriority::Speculative);
  second_speculative.source = source("speculative-b");
  CHECK(scheduler
            .submit(std::move(second_speculative),
                    [&](const ScheduledDataRequest &) {
                      std::lock_guard<std::mutex> lock(mutex);
                      second_speculative_started = true;
                      condition.notify_all();
                      return DataResultStatus::Ready;
                    })
            .accepted());
  for (const char *run : {"visible-a", "visible-b", "visible-c"}) {
    auto visible = request(10, 20, RequestPriority::VisibleWindow);
    visible.source = source(run);
    CHECK(scheduler
              .submit(std::move(visible),
                      [&](const ScheduledDataRequest &) {
                        std::unique_lock<std::mutex> lock(mutex);
                        ++visible_started;
                        condition.notify_all();
                        condition.wait(lock, [&] { return release; });
                        return DataResultStatus::Ready;
                      })
              .accepted());
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return visible_started == 3; }));
    CHECK(!second_speculative_started);
    release = true;
    condition.notify_all();
  }
  scheduler.waitUntilIdle();
  CHECK(second_speculative_started);
  const auto reservation_metrics = scheduler.metrics();
  CHECK(reservation_metrics.queue.peak_active_requests == 4);
  CHECK(reservation_metrics.peak_active_speculative_requests == 1);
  scheduler.shutdown();

  DataAccessScheduler isolated(8, 2, 1);
  release = false;
  bool first_source_started = false;
  bool second_source_started = false;
  bool other_source_started = false;
  auto first_source = request(30, 30, RequestPriority::CurrentFrame);
  first_source.source = source("isolated");
  CHECK(isolated
            .submit(std::move(first_source),
                    [&](const ScheduledDataRequest &) {
                      std::unique_lock<std::mutex> lock(mutex);
                      first_source_started = true;
                      condition.notify_all();
                      condition.wait(lock, [&] { return release; });
                      return DataResultStatus::Ready;
                    })
            .accepted());
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return first_source_started; }));
  }
  auto same_source = request(31, 31, RequestPriority::CurrentFrame);
  same_source.source = source("isolated");
  CHECK(isolated
            .submit(std::move(same_source),
                    [&](const ScheduledDataRequest &) {
                      std::lock_guard<std::mutex> lock(mutex);
                      second_source_started = true;
                      condition.notify_all();
                      return DataResultStatus::Ready;
                    })
            .accepted());
  auto other_source = request(40, 40, RequestPriority::CurrentFrame);
  other_source.source = source("other");
  CHECK(isolated
            .submit(std::move(other_source),
                    [&](const ScheduledDataRequest &) {
                      std::lock_guard<std::mutex> lock(mutex);
                      other_source_started = true;
                      condition.notify_all();
                      return DataResultStatus::Ready;
                    })
            .accepted());
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return other_source_started; }));
    CHECK(!second_source_started);
    release = true;
    condition.notify_all();
  }
  isolated.waitUntilIdle();
  CHECK(second_source_started);
  isolated.shutdown();
  return true;
}

bool testCurrentFrameWorkerReservation() {
  using namespace crimson::data;
  DataAccessScheduler scheduler(16, 4, 1, 1);
  std::mutex mutex;
  std::condition_variable condition;
  bool release_background = false;
  size_t blocking_background_started = 0;
  bool queued_background_started = false;
  bool current_frame_started = false;

  for (const char *run : {"background-a", "background-b", "background-c"}) {
    auto background = request(0, 100, RequestPriority::VisibleWindow);
    background.source = source(run);
    CHECK(scheduler
              .submit(std::move(background),
                      [&](const ScheduledDataRequest &) {
                        std::unique_lock<std::mutex> lock(mutex);
                        ++blocking_background_started;
                        condition.notify_all();
                        condition.wait(lock,
                                       [&] { return release_background; });
                        return DataResultStatus::Ready;
                      })
              .accepted());
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return blocking_background_started == 3; }));
  }

  auto queued_background = request(101, 200, RequestPriority::VisibleWindow);
  queued_background.source = source("background-queued");
  CHECK(scheduler
            .submit(std::move(queued_background),
                    [&](const ScheduledDataRequest &) {
                      std::lock_guard<std::mutex> lock(mutex);
                      queued_background_started = true;
                      condition.notify_all();
                      return DataResultStatus::Ready;
                    })
            .accepted());

  auto current_frame = request(50, 50, RequestPriority::CurrentFrame);
  current_frame.source = source("current-frame");
  CHECK(scheduler
            .submit(std::move(current_frame),
                    [&](const ScheduledDataRequest &) {
                      std::lock_guard<std::mutex> lock(mutex);
                      current_frame_started = true;
                      condition.notify_all();
                      return DataResultStatus::Ready;
                    })
            .accepted());
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return current_frame_started; }));
    CHECK(!queued_background_started);
    release_background = true;
    condition.notify_all();
  }
  scheduler.waitUntilIdle();
  CHECK(queued_background_started);
  const auto metrics = scheduler.metrics();
  CHECK(metrics.reserved_current_frame_workers == 1);
  CHECK(metrics.peak_active_non_current_requests == 3);
  CHECK(metrics.queue.peak_active_requests == 4);
  scheduler.shutdown();

  DataAccessScheduler single_worker(4, 1, 1, 1);
  CHECK(single_worker.metrics().reserved_current_frame_workers == 0);
  CHECK(single_worker
            .submit(request(0, 1, RequestPriority::VisibleWindow),
                    [](const ScheduledDataRequest &) {
                      return DataResultStatus::Ready;
                    })
            .accepted());
  single_worker.waitUntilIdle();
  single_worker.shutdown();
  return true;
}

bool testSchedulerTimingTelemetry() {
  using namespace crimson::data;
  DataAccessScheduler scheduler(8, 1);
  std::mutex mutex;
  std::condition_variable condition;
  bool blocker_started = false;
  bool release_blocker = false;
  bool promoted_work_started = false;

  auto blocker = request(0, 0, RequestPriority::Inspection);
  blocker.source = source("timing-blocker");
  CHECK(scheduler
            .submit(std::move(blocker),
                    [&](const ScheduledDataRequest &) {
                      std::unique_lock<std::mutex> lock(mutex);
                      blocker_started = true;
                      condition.notify_all();
                      condition.wait(lock, [&] { return release_blocker; });
                      return DataResultStatus::Ready;
                    })
            .accepted());
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return blocker_started; }));
  }

  auto speculative = request(10, 10, RequestPriority::Speculative);
  speculative.source = source("timing-promoted");
  const auto initial =
      scheduler.submit(speculative, [&](const ScheduledDataRequest &) {
        std::lock_guard<std::mutex> lock(mutex);
        promoted_work_started = true;
        return DataResultStatus::Ready;
      });
  CHECK(initial.status == DataRequestSubmitStatus::Accepted);

  // The wait before promotion must not be charged to current-frame demand.
  std::this_thread::sleep_for(std::chrono::milliseconds(130));
  speculative.priority = RequestPriority::CurrentFrame;
  speculative.access_pattern = AccessPattern::RandomSeek;
  const auto promoted = scheduler.submit(
      std::move(speculative),
      [](const ScheduledDataRequest &) { return DataResultStatus::Failed; });
  CHECK(promoted.status == DataRequestSubmitStatus::Promoted);
  CHECK(promoted.sequence == initial.sequence);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_blocker = true;
    condition.notify_all();
  }
  scheduler.waitUntilIdle();
  CHECK(promoted_work_started);

  const auto metrics = scheduler.metrics();
  const size_t current = static_cast<size_t>(RequestPriority::CurrentFrame);
  const size_t inspection = static_cast<size_t>(RequestPriority::Inspection);
  const size_t speculative_priority =
      static_cast<size_t>(RequestPriority::Speculative);
  CHECK(metrics.timing_by_priority[current].started == 1);
  CHECK(metrics.timing_by_priority[current].completed == 1);
  CHECK(metrics.timing_by_priority[inspection].service_over_100_ms == 1);
  CHECK(metrics.timing_by_priority[speculative_priority].started == 0);
  CHECK(metrics.timing_by_priority[inspection].maximum_service_ms -
            metrics.timing_by_priority[current].maximum_queue_wait_ms >=
        100.0);
  CHECK(metrics.timing_by_priority[current].averageQueueWaitMs() >= 0.0);
  CHECK(metrics.timing_by_priority[current].averageServiceMs() >= 0.0);

  const auto promoted_source = std::find_if(
      metrics.timing_by_source.begin(), metrics.timing_by_source.end(),
      [](const DataAccessSourceTimingMetrics &timing) {
        return timing.source == source("timing-promoted");
      });
  CHECK(promoted_source != metrics.timing_by_source.end());
  CHECK(promoted_source->by_priority[current].started == 1);
  CHECK(promoted_source->by_priority[speculative_priority].started == 0);
  CHECK(std::is_sorted(
      metrics.timing_by_source.begin(), metrics.timing_by_source.end(),
      [](const DataAccessSourceTimingMetrics &left,
         const DataAccessSourceTimingMetrics &right) {
        return std::tie(left.source.archive, left.source.product,
                        left.source.run) < std::tie(right.source.archive,
                                                    right.source.product,
                                                    right.source.run);
      }));
  scheduler.shutdown();
  return true;
}

bool testByteBudgetedWeightedLru() {
  using namespace crimson::data;
  ByteBudgetLruCache<std::string, int> cache({100, 50, 3});
  CHECK(cache.put("current", 1, {40, 0}, RequestPriority::CurrentFrame, true)
            .admitted());
  CHECK(cache.put("prefetch", 2, {40, 0}, RequestPriority::Speculative)
            .admitted());
  const auto visible =
      cache.put("visible", 3, {40, 0}, RequestPriority::VisibleWindow);
  CHECK(visible.admitted());
  CHECK(visible.evicted_keys == std::vector<std::string>({"prefetch"}));
  CHECK(cache.contains("current"));
  CHECK(cache.contains("visible"));
  CHECK(!cache.contains("prefetch"));
  CHECK(cache.metrics().current_cpu_bytes == 80);

  const auto rejected =
      cache.put("new-prefetch", 4, {40, 0}, RequestPriority::Speculative);
  CHECK(rejected.status == DataCacheAdmissionStatus::RejectedPressure);
  CHECK(cache.contains("visible"));
  CHECK(!cache.contains("new-prefetch"));

  CHECK(cache.findAndTouch("visible") != nullptr);
  const auto replacement =
      cache.put("next-visible", 5, {40, 0}, RequestPriority::VisibleWindow);
  CHECK(replacement.admitted());
  CHECK(replacement.evicted_keys == std::vector<std::string>({"visible"}));
  CHECK(cache.contains("current"));
  CHECK(cache.contains("next-visible"));

  const auto oversize =
      cache.put("oversize", 6, {101, 0}, RequestPriority::CurrentFrame);
  CHECK(oversize.status == DataCacheAdmissionStatus::RejectedOversize);
  const auto replaced =
      cache.put("current", 7, {20, 0}, RequestPriority::CurrentFrame, true);
  CHECK(replaced.status == DataCacheAdmissionStatus::Replaced);
  CHECK(cache.metrics().current_cpu_bytes == 60);
  CHECK(*cache.peek("current") == 7);

  ByteBudgetLruCache<std::string, int> gpu_cache({10, 10, 2});
  CHECK(gpu_cache.put("old", 1, {0, 8}, RequestPriority::VisibleWindow)
            .admitted());
  const auto gpu =
      gpu_cache.put("new", 2, {0, 8}, RequestPriority::VisibleWindow);
  CHECK(gpu.admitted());
  CHECK(gpu.evicted_keys == std::vector<std::string>({"old"}));
  CHECK(gpu_cache.metrics().current_gpu_bytes == 8);
  CHECK(gpu_cache.metrics().peak_gpu_bytes == 8);

  cache.clear();
  CHECK(cache.empty());
  CHECK(cache.metrics().current_cpu_bytes == 0);
  CHECK(cache.metrics().current_items == 0);
  return true;
}

} // namespace

int main() {
  if (!testRequestAndResultContract() ||
      !testPriorityDeduplicationAndGenerationCancellation() ||
      !testBoundedDemandFirstQueue() || !testSharedWorkerScheduler() ||
      !testDemandReservationAndSourceIsolation() ||
      !testCurrentFrameWorkerReservation() || !testSchedulerTimingTelemetry() ||
      !testByteBudgetedWeightedLru()) {
    return 1;
  }
  std::cout << "data_access_contract_tests: PASS\n";
  return 0;
}
