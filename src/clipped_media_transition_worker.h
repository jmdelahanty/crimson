#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace crimson::playback {

// Runs one blocking transition stage. The owner may poll finished() without
// joining; drain() is reserved for a completed stage or an explicit session
// close/open barrier. Payloads captured by the work function remain owned by
// its caller until drain() returns.
class ClippedMediaTransitionWorker {
public:
  ClippedMediaTransitionWorker() = default;
  ClippedMediaTransitionWorker(const ClippedMediaTransitionWorker &) = delete;
  ClippedMediaTransitionWorker &operator=(const ClippedMediaTransitionWorker &) = delete;
  ~ClippedMediaTransitionWorker() { drain(); }

  void start(std::function<void()> work) {
    if (worker_.joinable()) {
      throw std::logic_error("transition worker already active");
    }
    error_.clear();
    finished_.store(false, std::memory_order_relaxed);
    worker_ = std::thread([this, work = std::move(work)] {
      try {
        work();
      } catch (const std::exception &e) {
        error_ = e.what();
      } catch (...) {
        error_ = "unknown transition worker error";
      }
      finished_.store(true, std::memory_order_release);
    });
  }

  bool active() const { return worker_.joinable(); }
  bool finished() const {
    return worker_.joinable() &&
           finished_.load(std::memory_order_acquire);
  }
  const std::string &error() const { return error_; }

  void drain() {
    if (worker_.joinable()) {
      worker_.join();
    }
    finished_.store(false, std::memory_order_relaxed);
  }

private:
  std::thread worker_;
  std::atomic<bool> finished_{false};
  std::string error_;
};

enum class ClippedMediaTransitionPoll {
  Preparing,
  Prepared,
  Joining,
  ReadyToCommit,
};

inline ClippedMediaTransitionPoll pollClippedMediaTransition(
    bool join_started, const ClippedMediaTransitionWorker &prepare,
    const ClippedMediaTransitionWorker &join) {
  if (!join_started) {
    return prepare.finished() ? ClippedMediaTransitionPoll::Prepared
                              : ClippedMediaTransitionPoll::Preparing;
  }
  return join.finished() ? ClippedMediaTransitionPoll::ReadyToCommit
                         : ClippedMediaTransitionPoll::Joining;
}

// Shutdown and reload may encounter threads already joined by a transition
// stage. Joining only live handles also makes repeated drains safe.
inline void joinLiveThreads(std::vector<std::thread> &threads) {
  for (auto &thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

} // namespace crimson::playback
