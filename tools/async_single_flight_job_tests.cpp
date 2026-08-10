#include "async_single_flight_job.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout =
                                        std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

bool testValidationAndSingleFlight() {
  crimson::tasks::AsyncSingleFlightJob runner;
  CHECK(!runner.start("", [] {}).accepted());
  CHECK(!runner.start("missing-work", {}).accepted());

  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();
  std::atomic<bool> started{false};
  const auto first = runner.start("first", [&] {
    started.store(true);
    release_future.wait();
  });
  CHECK(first.accepted());
  CHECK(first.sequence == 1);
  CHECK(waitUntil([&] { return started.load(); }));
  CHECK(runner.active());

  const auto busy = runner.start("second", [] {});
  CHECK(busy.status ==
        crimson::tasks::SingleFlightJobStartStatus::RejectedBusy);
  CHECK(busy.sequence == first.sequence);
  CHECK(!runner.takeReady().has_value());

  release.set_value();
  std::optional<crimson::tasks::SingleFlightJobCompletion> completion;
  CHECK(waitUntil([&] {
    completion = runner.takeReady();
    return completion.has_value();
  }));
  CHECK(completion->succeeded());
  CHECK(completion->sequence == first.sequence);
  CHECK(completion->label == "first");
  CHECK(completion->queue_wait_ms >= 0.0);
  CHECK(completion->service_ms >= 0.0);
  CHECK(!runner.active());

  const auto second = runner.start("second", [] {});
  CHECK(second.accepted());
  CHECK(second.sequence == 2);
  CHECK(waitUntil([&] { return runner.takeReady().has_value(); }));
  return true;
}

bool testExceptionAndClose() {
  crimson::tasks::AsyncSingleFlightJob runner;
  CHECK(runner.start("throws", [] { throw std::runtime_error("expected"); })
            .accepted());
  std::optional<crimson::tasks::SingleFlightJobCompletion> completion;
  CHECK(waitUntil([&] {
    completion = runner.takeReady();
    return completion.has_value();
  }));
  CHECK(!completion->succeeded());
  CHECK(completion->error == "expected");

  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();
  CHECK(runner.start("closing", [&] { release_future.wait(); }).accepted());
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    release.set_value();
  });
  runner.close();
  releaser.join();
  CHECK(runner.closed());
  CHECK(!runner.active());
  CHECK(runner.start("after-close", [] {}).status ==
        crimson::tasks::SingleFlightJobStartStatus::RejectedClosed);
  return true;
}

} // namespace

int main() {
  if (!testValidationAndSingleFlight() || !testExceptionAndClose()) {
    return 1;
  }
  std::cout << "async_single_flight_job_tests: PASS\n";
  return 0;
}
