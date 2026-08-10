#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace crimson::tasks {

enum class SingleFlightJobStartStatus : uint8_t {
  Accepted,
  RejectedInvalid,
  RejectedBusy,
  RejectedClosed,
  RejectedUnavailable,
};

struct SingleFlightJobStartOutcome {
  SingleFlightJobStartStatus status =
      SingleFlightJobStartStatus::RejectedInvalid;
  uint64_t sequence = 0;
  std::string reason;

  bool accepted() const;
};

enum class SingleFlightJobCompletionStatus : uint8_t {
  Succeeded,
  Failed,
};

struct SingleFlightJobCompletion {
  SingleFlightJobCompletionStatus status =
      SingleFlightJobCompletionStatus::Failed;
  uint64_t sequence = 0;
  std::string label;
  std::string error;
  double queue_wait_ms = 0.0;
  double service_ms = 0.0;

  bool succeeded() const;
};

using SingleFlightJobWork = std::function<void()>;

// Executes at most one immediately launched background job. The owner polls
// completion from one thread and must call close before destroying resources
// captured by the work function. Running work is deliberately not cancelled.
class AsyncSingleFlightJob {
public:
  AsyncSingleFlightJob();
  ~AsyncSingleFlightJob();

  AsyncSingleFlightJob(const AsyncSingleFlightJob &) = delete;
  AsyncSingleFlightJob &operator=(const AsyncSingleFlightJob &) = delete;

  SingleFlightJobStartOutcome start(std::string label,
                                    SingleFlightJobWork work);
  std::optional<SingleFlightJobCompletion> takeReady();

  bool active() const;
  bool closed() const;
  void close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char *singleFlightJobStartStatusName(SingleFlightJobStartStatus status);

} // namespace crimson::tasks
