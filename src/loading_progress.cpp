#include "loading_progress.h"

#include <algorithm>
#include <utility>

namespace crimson::loading {

namespace {

LoadingProductProgress *findProduct(LoadingProgressSnapshot *snapshot,
                                    const std::string &product) {
  const auto found = std::find_if(
      snapshot->products.begin(), snapshot->products.end(),
      [&](const auto &candidate) { return candidate.product == product; });
  return found == snapshot->products.end() ? nullptr : &*found;
}

} // namespace

const char *loadingStateName(LoadingState state) {
  switch (state) {
  case LoadingState::Idle:
    return "idle";
  case LoadingState::Running:
    return "running";
  case LoadingState::Ready:
    return "ready";
  case LoadingState::Cancelled:
    return "cancelled";
  case LoadingState::Failed:
    return "failed";
  }
  return "unknown";
}

double LoadingProgressSnapshot::fraction() const {
  if (total_products == 0) {
    return terminal() ? 1.0 : 0.0;
  }
  return std::clamp(static_cast<double>(completed_products) /
                        static_cast<double>(total_products),
                    0.0, 1.0);
}

bool LoadingProgressSnapshot::terminal() const {
  return state == LoadingState::Ready || state == LoadingState::Cancelled ||
         state == LoadingState::Failed;
}

void LoadingProgressTracker::start(size_t total_products, std::string phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = {};
  state_.phase = std::move(phase);
  state_.total_products = total_products;
  state_.running = true;
  state_.state = LoadingState::Running;
}

void LoadingProgressTracker::startProduct(std::string product,
                                          std::string phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.product = product;
  state_.phase = phase;
  state_.running = true;
  state_.cancelled = false;
  state_.state = LoadingState::Running;
  auto *entry = findProduct(&state_, product);
  if (entry == nullptr) {
    state_.products.push_back(
        {std::move(product), std::move(phase), LoadingState::Running});
  } else if (entry->state != LoadingState::Ready &&
             entry->state != LoadingState::Failed) {
    entry->phase = std::move(phase);
    entry->state = LoadingState::Running;
  }
}

void LoadingProgressTracker::completeProduct(std::string product,
                                             std::string phase,
                                             bool available,
                                             double elapsed_ms,
                                             std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.product = product;
  state_.phase = phase;
  auto *entry = findProduct(&state_, product);
  const bool first_completion =
      entry == nullptr || (entry->state != LoadingState::Ready &&
                           entry->state != LoadingState::Failed);
  const LoadingState product_state =
      available || error.empty() ? LoadingState::Ready : LoadingState::Failed;
  if (entry == nullptr) {
    state_.products.push_back({std::move(product), std::move(phase),
                               product_state, elapsed_ms, available,
                               std::move(error)});
  } else {
    entry->phase = std::move(phase);
    entry->state = product_state;
    entry->elapsed_ms = elapsed_ms;
    entry->available = available;
    entry->error = std::move(error);
  }
  if (first_completion) {
    ++state_.completed_products;
  }
}

void LoadingProgressTracker::setPhase(std::string phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.phase = std::move(phase);
}

void LoadingProgressTracker::finish(std::string phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.phase = std::move(phase);
  state_.running = false;
  state_.cancelled = false;
  state_.state = LoadingState::Ready;
}

void LoadingProgressTracker::cancel(std::string phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.phase = std::move(phase);
  state_.running = false;
  state_.cancelled = true;
  state_.state = LoadingState::Cancelled;
}

void LoadingProgressTracker::fail(std::string error, std::string phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.phase = std::move(phase);
  state_.error = error.empty() ? "Loading failed" : std::move(error);
  state_.running = false;
  state_.cancelled = false;
  state_.state = LoadingState::Failed;
}

LoadingProgressSnapshot LoadingProgressTracker::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

} // namespace crimson::loading
