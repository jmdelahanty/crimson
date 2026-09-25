#include "async_single_flight_job.h"

#include <chrono>
#include <exception>
#include <future>
#include <utility>

namespace crimson::tasks {

namespace {

using Clock = std::chrono::steady_clock;

double durationMs(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

struct JobTimingState {
  Clock::time_point queued_at{};
  Clock::time_point started_at{};
  Clock::time_point finished_at{};
};

} // namespace

struct AsyncSingleFlightJob::Impl {
  bool active = false;
  bool closed = false;
  uint64_t next_sequence = 1;
  uint64_t active_sequence = 0;
  std::string active_label;
  std::shared_ptr<JobTimingState> timing;
  std::future<void> future;
};

bool SingleFlightJobStartOutcome::accepted() const {
  return status == SingleFlightJobStartStatus::Accepted;
}

bool SingleFlightJobCompletion::succeeded() const {
  return status == SingleFlightJobCompletionStatus::Succeeded;
}

AsyncSingleFlightJob::AsyncSingleFlightJob()
    : impl_(std::make_unique<Impl>()) {}

AsyncSingleFlightJob::~AsyncSingleFlightJob() { close(); }

SingleFlightJobStartOutcome
AsyncSingleFlightJob::start(std::string label, SingleFlightJobWork work) {
  SingleFlightJobStartOutcome outcome;
  if (label.empty() || !work) {
    outcome.reason = "A single-flight job requires a label and work function";
    return outcome;
  }
  if (impl_->closed) {
    outcome.status = SingleFlightJobStartStatus::RejectedClosed;
    outcome.reason = "The single-flight job runner is closed";
    return outcome;
  }
  if (impl_->active) {
    outcome.status = SingleFlightJobStartStatus::RejectedBusy;
    outcome.sequence = impl_->active_sequence;
    outcome.reason = "A single-flight job is already active";
    return outcome;
  }

  const uint64_t sequence = impl_->next_sequence++;
  auto timing = std::make_shared<JobTimingState>();
  timing->queued_at = Clock::now();
  try {
    impl_->future = std::async(std::launch::async,
                               [timing, work = std::move(work)]() mutable {
                                 timing->started_at = Clock::now();
                                 try {
                                   work();
                                 } catch (...) {
                                   timing->finished_at = Clock::now();
                                   throw;
                                 }
                                 timing->finished_at = Clock::now();
                               });
  } catch (const std::exception &error) {
    outcome.status = SingleFlightJobStartStatus::RejectedUnavailable;
    outcome.reason = error.what();
    return outcome;
  } catch (...) {
    outcome.status = SingleFlightJobStartStatus::RejectedUnavailable;
    outcome.reason = "The background job could not be launched";
    return outcome;
  }

  impl_->active = true;
  impl_->active_sequence = sequence;
  impl_->active_label = std::move(label);
  impl_->timing = std::move(timing);
  outcome.status = SingleFlightJobStartStatus::Accepted;
  outcome.sequence = sequence;
  return outcome;
}

std::optional<SingleFlightJobCompletion> AsyncSingleFlightJob::takeReady() {
  if (!impl_->active || !impl_->future.valid() ||
      impl_->future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::ready) {
    return std::nullopt;
  }

  SingleFlightJobCompletion completion;
  completion.sequence = impl_->active_sequence;
  completion.label = impl_->active_label;
  try {
    impl_->future.get();
    completion.status = SingleFlightJobCompletionStatus::Succeeded;
  } catch (const std::exception &error) {
    completion.error = error.what();
  } catch (...) {
    completion.error = "Background work raised an unknown exception";
  }
  if (impl_->timing != nullptr) {
    completion.queue_wait_ms =
        durationMs(impl_->timing->started_at - impl_->timing->queued_at);
    completion.service_ms =
        durationMs(impl_->timing->finished_at - impl_->timing->started_at);
  }

  impl_->active = false;
  impl_->active_sequence = 0;
  impl_->active_label.clear();
  impl_->timing.reset();
  return completion;
}

bool AsyncSingleFlightJob::active() const { return impl_->active; }

bool AsyncSingleFlightJob::closed() const { return impl_->closed; }

void AsyncSingleFlightJob::close() {
  if (impl_->closed) {
    return;
  }
  impl_->closed = true;
  if (impl_->future.valid()) {
    try {
      impl_->future.get();
    } catch (...) {
      // Shutdown deliberately discards an unobserved completion.
    }
  }
  impl_->active = false;
  impl_->active_sequence = 0;
  impl_->active_label.clear();
  impl_->timing.reset();
}

const char *singleFlightJobStartStatusName(SingleFlightJobStartStatus status) {
  switch (status) {
  case SingleFlightJobStartStatus::Accepted:
    return "accepted";
  case SingleFlightJobStartStatus::RejectedInvalid:
    return "rejected_invalid";
  case SingleFlightJobStartStatus::RejectedBusy:
    return "rejected_busy";
  case SingleFlightJobStartStatus::RejectedClosed:
    return "rejected_closed";
  case SingleFlightJobStartStatus::RejectedUnavailable:
    return "rejected_unavailable";
  }
  return "unknown";
}

} // namespace crimson::tasks
