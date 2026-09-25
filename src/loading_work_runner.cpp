#include "loading_work_runner.h"

#include <exception>
#include <chrono>
#include <thread>

namespace crimson::tasks {

const char *loadingWorkStatusName(LoadingWorkStatus status) {
  switch (status) {
  case LoadingWorkStatus::Applied: return "applied";
  case LoadingWorkStatus::Failed: return "failed";
  case LoadingWorkStatus::Busy: return "busy";
  case LoadingWorkStatus::Closed: return "closed";
  case LoadingWorkStatus::Cancelled: return "cancelled";
  case LoadingWorkStatus::Stale: return "stale";
  }
  return "unknown";
}

LoadingWorkResult LoadingWorkRunner::run(
    const std::string &label, uint64_t generation, SingleFlightJobWork work,
    const std::function<bool()> &heartbeat,
    const std::function<uint64_t()> &current_generation,
    const std::function<void()> &adopt) {
  if (running_ || job_.active()) {
    return {LoadingWorkStatus::Busy, "A loading job is already active"};
  }
  if (job_.closed()) {
    return {LoadingWorkStatus::Closed, "The loading runner is closed"};
  }
  running_ = true;
  struct RunningGuard {
    bool &running;
    ~RunningGuard() { running = false; }
  } guard{running_};
  try {
    if (!current_generation || current_generation() != generation) {
      return {LoadingWorkStatus::Stale,
              "Loading request belongs to an older session"};
    }
    if (!heartbeat || !heartbeat()) {
      return {LoadingWorkStatus::Cancelled, "Loading was cancelled"};
    }
    if (current_generation() != generation) {
      return {LoadingWorkStatus::Stale,
              "Loading request belongs to an older session"};
    }
  } catch (const std::exception &error) {
    return {LoadingWorkStatus::Failed, error.what()};
  } catch (...) {
    return {LoadingWorkStatus::Failed, "Loading pump failed unexpectedly"};
  }
  const auto start = job_.start(label, std::move(work));
  if (!start.accepted()) {
    const auto status =
        start.status == SingleFlightJobStartStatus::RejectedBusy
            ? LoadingWorkStatus::Busy
            : start.status == SingleFlightJobStartStatus::RejectedClosed
                  ? LoadingWorkStatus::Closed
                  : LoadingWorkStatus::Failed;
    return {status, start.reason};
  }

  try {
    bool cancelled = false;
    for (;;) {
      if (!heartbeat()) {
        cancelled = true;
      }
      if (auto completion = job_.takeReady()) {
        if (cancelled) {
          return {LoadingWorkStatus::Cancelled, "Loading was cancelled"};
        }
        if (!completion->succeeded()) {
          return {LoadingWorkStatus::Failed, completion->error};
        }
        if (current_generation() != generation) {
          return {LoadingWorkStatus::Stale, "Loading result belongs to an older session"};
        }
        try {
          if (adopt) {
            adopt();
          }
        } catch (const std::exception &error) {
          return {LoadingWorkStatus::Failed, error.what()};
        } catch (...) {
          return {LoadingWorkStatus::Failed, "Loading adoption failed unexpectedly"};
        }
        return {LoadingWorkStatus::Applied, {}};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (const std::exception &error) {
    job_.close();
    return {LoadingWorkStatus::Failed, error.what()};
  } catch (...) {
    job_.close();
    return {LoadingWorkStatus::Failed, "Loading pump failed unexpectedly"};
  }
}

void LoadingWorkRunner::close() { job_.close(); }
bool LoadingWorkRunner::active() const { return running_ || job_.active(); }

} // namespace crimson::tasks
