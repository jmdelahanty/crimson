#include "data_access_diagnostics.h"

#include <iomanip>
#include <ostream>

namespace crimson::data {
namespace {

void writeTiming(std::ostream &output, std::string_view backend,
                 std::string_view scope, std::string_view product,
                 std::string_view run, RequestPriority priority,
                 const DataAccessTimingMetrics &timing) {
  if (timing.started == 0) {
    return;
  }

  output << '[' << backend << "DataSchedulerTiming] scope=" << scope;
  if (!product.empty()) {
    output << " product=" << product << " run=" << run;
  }
  output << " priority=" << requestPriorityName(priority)
         << " started=" << timing.started << " completed=" << timing.completed
         << std::fixed << std::setprecision(1)
         << " queue_avg_ms=" << timing.averageQueueWaitMs()
         << " queue_max_ms=" << timing.maximum_queue_wait_ms
         << " service_avg_ms=" << timing.averageServiceMs()
         << " service_max_ms=" << timing.maximum_service_ms
         << " queue_over_100ms=" << timing.queue_wait_over_100_ms
         << " queue_over_1000ms=" << timing.queue_wait_over_1000_ms
         << " queue_over_5000ms=" << timing.queue_wait_over_5000_ms
         << " service_over_100ms=" << timing.service_over_100_ms
         << " service_over_1000ms=" << timing.service_over_1000_ms
         << " service_over_5000ms=" << timing.service_over_5000_ms << '\n';
}

} // namespace

void writeDataAccessSchedulerDiagnostics(
    std::ostream &output, std::string_view backend,
    const DataAccessSchedulerMetrics &metrics) {
  const std::ios::fmtflags original_flags = output.flags();
  const std::streamsize original_precision = output.precision();
  const DataAccessQueueMetrics &queue = metrics.queue;
  output << '[' << backend << "DataScheduler] workers=" << metrics.worker_count
         << " submissions=" << queue.submissions
         << " accepted=" << queue.accepted << " duplicates=" << queue.duplicates
         << " promotions=" << queue.promotions
         << " rejected_invalid=" << queue.rejected_invalid
         << " rejected_stale=" << queue.rejected_stale
         << " rejected_capacity=" << queue.rejected_capacity
         << " cancelled=" << queue.cancelled_requests
         << " capacity_evictions=" << queue.capacity_evictions
         << " completed=" << queue.completed_requests
         << " discarded=" << queue.discarded_completions
         << " failed=" << queue.failed_completions
         << " work_started=" << metrics.work_started
         << " work_completed=" << metrics.work_completed
         << " work_exceptions=" << metrics.work_exceptions
         << " reserved_current=" << metrics.reserved_current_frame_workers
         << " peak_pending=" << queue.peak_pending_requests
         << " peak_active=" << queue.peak_active_requests
         << " peak_non_current=" << metrics.peak_active_non_current_requests
         << " peak_speculative=" << metrics.peak_active_speculative_requests
         << '\n';

  for (size_t index = 0; index < kDataRequestPriorityCount; ++index) {
    writeTiming(output, backend, "priority", {}, {},
                static_cast<RequestPriority>(index),
                metrics.timing_by_priority[index]);
  }
  for (const auto &source : metrics.timing_by_source) {
    for (size_t index = 0; index < kDataRequestPriorityCount; ++index) {
      writeTiming(output, backend, "source", source.source.product,
                  source.source.run, static_cast<RequestPriority>(index),
                  source.by_priority[index]);
    }
  }
  output.flags(original_flags);
  output.precision(original_precision);
}

} // namespace crimson::data
