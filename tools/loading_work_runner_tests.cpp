#include "loading_work_runner.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "   \
                << #condition << '\n';                                         \
      return 1;                                                                \
    }                                                                          \
  } while (false)

int main() {
  using crimson::tasks::LoadingWorkRunner;
  using crimson::tasks::LoadingWorkStatus;
  using namespace std::chrono_literals;
  const auto ui_thread = std::this_thread::get_id();
  uint64_t generation = 1;
  int heartbeats = 0;
  int applied = 0;
  std::atomic<bool> worker_done{false};
  std::atomic<int> observed_heartbeats{0};
  LoadingWorkRunner runner;

  auto success = runner.run(
      "slow archive", generation,
      [&] {
        if (std::this_thread::get_id() == ui_thread) {
          throw std::runtime_error("work ran on UI thread");
        }
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (observed_heartbeats.load() < 10 &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(1ms);
        }
        if (observed_heartbeats.load() < 10) {
          throw std::runtime_error("UI heartbeat stopped during loading");
        }
        worker_done = true;
      },
      [&] {
        if (std::this_thread::get_id() != ui_thread) {
          throw std::runtime_error("heartbeat left UI thread");
        }
        ++heartbeats;
        ++observed_heartbeats;
        std::this_thread::sleep_for(1ms);
        return true;
      },
      [&] { return generation; },
      [&] {
        if (std::this_thread::get_id() != ui_thread || !worker_done) {
          throw std::runtime_error("adoption ran before work or off UI thread");
        }
        ++applied;
      });
  CHECK(success.applied());
  CHECK(heartbeats >= 10);
  CHECK(applied == 1);

  auto failure = runner.run(
      "failed archive", generation,
      [] { throw std::runtime_error("read failed"); },
      [] { std::this_thread::sleep_for(1ms); return true; },
      [&] { return generation; }, [&] { ++applied; });
  CHECK(failure.status == LoadingWorkStatus::Failed);
  CHECK(failure.error == "read failed" && applied == 1);

  bool started_invalid_work = false;
  auto invalid = runner.run(
      "", generation, [&] { started_invalid_work = true; },
      [] { return true; }, [&] { return generation; }, [&] { ++applied; });
  CHECK(invalid.status == LoadingWorkStatus::Failed);
  CHECK(!started_invalid_work && applied == 1);

  auto adoption_failure = runner.run(
      "adoption fails", generation, [] {},
      [] { std::this_thread::sleep_for(1ms); return true; },
      [&] { return generation; },
      [] { throw std::runtime_error("adoption failed"); });
  CHECK(adoption_failure.status == LoadingWorkStatus::Failed);
  CHECK(adoption_failure.error == "adoption failed");

  bool started_stale_work = false;
  auto superseded_before_start = runner.run(
      "superseded before launch", generation,
      [&] { started_stale_work = true; },
      [&] { ++generation; return true; },
      [&] { return generation; }, [&] { ++applied; });
  CHECK(superseded_before_start.status == LoadingWorkStatus::Stale);
  CHECK(!started_stale_work && applied == 1);

  bool started_cancelled_work = false;
  auto cancelled_before_start = runner.run(
      "closed before launch", generation,
      [&] { started_cancelled_work = true; },
      [] { return false; }, [&] { return generation; }, [&] { ++applied; });
  CHECK(cancelled_before_start.status == LoadingWorkStatus::Cancelled);
  CHECK(!started_cancelled_work && applied == 1);

  int stale_heartbeats = 0;
  auto stale = runner.run(
      "replaced archive", generation, [] { std::this_thread::sleep_for(10ms); },
      [&] {
        if (++stale_heartbeats == 2) {
          ++generation;
        }
        std::this_thread::sleep_for(1ms);
        return true;
      },
      [&] { return generation; }, [&] { ++applied; });
  CHECK(stale.status == LoadingWorkStatus::Stale && applied == 1);

  std::atomic<int> cancel_heartbeats{0};
  auto cancelled = runner.run(
      "closing archive", generation,
      [&] {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (cancel_heartbeats.load() < 3 &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(1ms);
        }
        if (cancel_heartbeats.load() < 3) {
          throw std::runtime_error("cancellation stopped pumping events");
        }
      },
      [&] { return ++cancel_heartbeats == 1; },
      [&] { return generation; }, [&] { ++applied; });
  CHECK(cancelled.status == LoadingWorkStatus::Cancelled && applied == 1);
  CHECK(cancel_heartbeats.load() >= 3);
  CHECK(!runner.active());
  auto after_cancel = runner.run(
      "retry", generation, [] {},
      [] { std::this_thread::sleep_for(1ms); return true; },
      [&] { return generation; }, [&] { ++applied; });
  CHECK(after_cancel.applied() && applied == 2);
  runner.close();
  auto closed = runner.run("closed", generation, [] {}, [] { return true; },
                           [&] { return generation; }, [&] { ++applied; });
  CHECK(closed.status == LoadingWorkStatus::Closed && applied == 2);

  LoadingWorkRunner reopened;
  auto reopened_result = reopened.run(
      "new session", generation, [] {},
      [] { std::this_thread::sleep_for(1ms); return true; },
      [&] { return generation; }, [&] { ++applied; });
  CHECK(reopened_result.applied() && applied == 3);

  LoadingWorkRunner busy_runner;
  auto outer = busy_runner.run(
      "outer", generation, [] { std::this_thread::sleep_for(10ms); },
      [&] {
        const auto busy = busy_runner.run(
            "inner", generation, [] {}, [] { return true; },
            [&] { return generation; }, [&] { ++applied; });
        if (busy.status != LoadingWorkStatus::Busy) {
          throw std::runtime_error("nested work was not rejected as busy");
        }
        std::this_thread::sleep_for(1ms);
        return true;
      },
      [&] { return generation; }, [&] { ++applied; });
  CHECK(outer.applied() && applied == 4);

  LoadingWorkRunner adopt_busy_runner;
  auto adopt_outer = adopt_busy_runner.run(
      "adopt outer", generation, [] {},
      [] { std::this_thread::sleep_for(1ms); return true; },
      [&] { return generation; },
      [&] {
        const auto nested = adopt_busy_runner.run(
            "adopt inner", generation, [] {}, [] { return true; },
            [&] { return generation; }, [&] { ++applied; });
        if (nested.status != LoadingWorkStatus::Busy) {
          throw std::runtime_error("nested adoption work was not rejected");
        }
        ++applied;
      });
  CHECK(adopt_outer.applied() && applied == 5);

  std::cout << "loading_work_runner_tests: PASS\n";
  return 0;
}
