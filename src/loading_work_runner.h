#pragma once

#include "async_single_flight_job.h"

#include <cstdint>
#include <functional>
#include <string>

namespace crimson::tasks {

enum class LoadingWorkStatus : uint8_t {
  Applied,
  Failed,
  Busy,
  Closed,
  Cancelled,
  Stale,
};

struct LoadingWorkResult {
  LoadingWorkStatus status = LoadingWorkStatus::Failed;
  std::string error;
  bool applied() const { return status == LoadingWorkStatus::Applied; }
};

const char *loadingWorkStatusName(LoadingWorkStatus status);

// The owner calls run on its UI thread. Only work runs in the background;
// heartbeat and adopt always run on the owner thread. A cancelled pump drains
// the job before returning, so work captures remain alive during shutdown.
class LoadingWorkRunner {
public:
  LoadingWorkResult run(const std::string &label, uint64_t generation,
                        SingleFlightJobWork work,
                        const std::function<bool()> &heartbeat,
                        const std::function<uint64_t()> &current_generation,
                        const std::function<void()> &adopt);
  void close();
  bool active() const;

private:
  AsyncSingleFlightJob job_;
  bool running_ = false;
};

} // namespace crimson::tasks
