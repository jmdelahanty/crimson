#include "clipped_media_transition_worker.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using crimson::playback::ClippedMediaTransitionPoll;
using crimson::playback::ClippedMediaTransitionWorker;
using crimson::playback::pollClippedMediaTransition;
using crimson::playback::joinLiveThreads;

void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;

  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }

  void waitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, std::chrono::seconds(2),
                      [&] { return entered; }), "worker did not enter gate");
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    cv.notify_all();
  }
};

struct StageCleanup {
  Gate &prepare_gate;
  Gate &join_gate;
  ClippedMediaTransitionWorker &prepare;
  ClippedMediaTransitionWorker &join;
  ~StageCleanup() {
    prepare_gate.release();
    join_gate.release();
    prepare.drain();
    join.drain();
  }
};

struct WorkerCleanup {
  Gate &gate;
  ClippedMediaTransitionWorker &worker;
  ~WorkerCleanup() {
    gate.release();
    worker.drain();
  }
};

void waitUntilFinished(ClippedMediaTransitionWorker &worker) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  while (!worker.finished() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  check(worker.finished(), "worker did not finish");
}

void testTwoStageOwnerPolling() {
  ClippedMediaTransitionWorker prepare;
  ClippedMediaTransitionWorker join;
  Gate prepare_gate;
  Gate join_gate;
  std::string active_clip = "old";
  bool prepared = false;
  bool joined = false;
  StageCleanup cleanup{prepare_gate, join_gate, prepare, join};

  prepare.start([&] { prepare_gate.block(); prepared = true; });
  prepare_gate.waitUntilEntered();
  const auto prepare_poll_start = std::chrono::steady_clock::now();
  for (int i = 0; i < 10000; ++i) {
    check(pollClippedMediaTransition(false, prepare, join) ==
              ClippedMediaTransitionPoll::Preparing,
          "preparation gate advanced before worker completion");
    check(active_clip == "old", "preparation changed active clip");
  }
  check(std::chrono::steady_clock::now() - prepare_poll_start <
            std::chrono::seconds(1), "owner preparation polls blocked");
  prepare_gate.release();
  waitUntilFinished(prepare);
  check(prepared && pollClippedMediaTransition(false, prepare, join) ==
                        ClippedMediaTransitionPoll::Prepared,
        "completed preparation was not exposed to owner");
  prepare.drain();

  join.start([&] { join_gate.block(); joined = true; });
  join_gate.waitUntilEntered();
  const auto join_poll_start = std::chrono::steady_clock::now();
  for (int i = 0; i < 10000; ++i) {
    check(pollClippedMediaTransition(true, prepare, join) ==
              ClippedMediaTransitionPoll::Joining,
          "join gate advanced before worker completion");
    check(active_clip == "old", "join changed active clip");
  }
  check(std::chrono::steady_clock::now() - join_poll_start <
            std::chrono::seconds(1), "owner join polls blocked");
  join_gate.release();
  waitUntilFinished(join);
  check(joined && pollClippedMediaTransition(true, prepare, join) ==
                      ClippedMediaTransitionPoll::ReadyToCommit,
        "drained prepare stage hid completed join stage");
  check(active_clip == "old", "worker committed clip before owner");
  join.drain();
  active_clip = "new";
  check(active_clip == "new", "owner commit failed");
}

void testFailureAndCloseDrain() {
  ClippedMediaTransitionWorker prepare;
  ClippedMediaTransitionWorker join;
  prepare.start([] { throw std::runtime_error("prepare failed"); });
  waitUntilFinished(prepare);
  check(pollClippedMediaTransition(false, prepare, join) ==
            ClippedMediaTransitionPoll::Prepared,
        "failed preparation not returned for owner error handling");
  check(prepare.error() == "prepare failed", "worker exception was lost");
  prepare.drain();

  Gate gate;
  WorkerCleanup cleanup{gate, prepare};
  prepare.start([&] { gate.block(); });
  gate.waitUntilEntered();
  std::atomic<bool> drained{false};
  std::thread close_thread([&] {
    prepare.drain();
    drained.store(true, std::memory_order_release);
  });
  const bool drained_before_release = drained.load(std::memory_order_acquire);
  gate.release();
  close_thread.join();
  check(!drained_before_release,
        "close drained before blocked worker was released");
  check(!prepare.active(), "close left worker active");
}

void testRepeatedFinalDecoderDrain() {
  std::atomic<int> runs{0};
  std::vector<std::thread> decoder_threads;
  decoder_threads.emplace_back([&] { runs.fetch_add(1); });
  ClippedMediaTransitionWorker stage;
  stage.start([&] { joinLiveThreads(decoder_threads); });
  waitUntilFinished(stage);
  stage.drain();
  stage.drain(); // Cancel followed by final owner shutdown.
  check(!decoder_threads.front().joinable(),
        "transition join left decoder handle live");
  joinLiveThreads(decoder_threads);
  check(runs.load() == 1,
        "final shutdown repeated or failed the decoder join");
  check(!stage.active(), "repeated stage drain left a worker live");
}

} // namespace

int main() {
  try {
    testTwoStageOwnerPolling();
    testFailureAndCloseDrain();
    testRepeatedFinalDecoderDrain();
    std::cout << "clipped_media_transition_worker_tests: PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "clipped_media_transition_worker_tests: FAIL: "
              << e.what() << '\n';
    return 1;
  }
}
