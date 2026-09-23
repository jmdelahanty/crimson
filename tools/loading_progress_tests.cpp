#include "loading_progress.h"

#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition   \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testProgressAndUniqueCompletion() {
  crimson::loading::LoadingProgressTracker tracker;
  tracker.start(2, "Starting");
  tracker.startProduct("archive", "Resolving archive");
  tracker.completeProduct("archive", "Archive ready", true, 12.0);
  tracker.completeProduct("archive", "Archive ready", true, 12.0);
  auto snapshot = tracker.snapshot();
  CHECK(snapshot.completed_products == 1);
  CHECK(snapshot.fraction() == 0.5);
  CHECK(snapshot.products.size() == 1);

  tracker.startProduct("masks", "Loading masks");
  tracker.completeProduct("masks", "Masks unavailable", false, 5.0,
                          "missing run");
  tracker.finish("Analysis products ready");
  snapshot = tracker.snapshot();
  CHECK(snapshot.state == crimson::loading::LoadingState::Ready);
  CHECK(snapshot.completed_products == 2);
  CHECK(snapshot.fraction() == 1.0);
  CHECK(snapshot.products[1].state == crimson::loading::LoadingState::Failed);
  return true;
}

bool testThreadSafeProductsAndCancellation() {
  crimson::loading::LoadingProgressTracker tracker;
  tracker.start(2, "Starting");
  std::thread first([&] {
    tracker.startProduct("one", "One");
    tracker.completeProduct("one", "One ready", true, 1.0);
  });
  std::thread second([&] {
    tracker.startProduct("two", "Two");
    tracker.completeProduct("two", "Two ready", true, 2.0);
  });
  first.join();
  second.join();
  CHECK(tracker.snapshot().completed_products == 2);
  tracker.cancel();
  const auto cancelled = tracker.snapshot();
  CHECK(cancelled.cancelled);
  CHECK(cancelled.terminal());
  return true;
}

} // namespace

int main() {
  if (!testProgressAndUniqueCompletion() ||
      !testThreadSafeProductsAndCancellation()) {
    return EXIT_FAILURE;
  }
  std::cout << "loading_progress_tests: PASS\n";
  return EXIT_SUCCESS;
}
