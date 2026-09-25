#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace crimson::loading {

enum class LoadingState : uint8_t {
  Idle,
  Running,
  Ready,
  Cancelled,
  Failed,
};

const char *loadingStateName(LoadingState state);

struct LoadingProductProgress {
  std::string product;
  std::string phase;
  LoadingState state = LoadingState::Idle;
  double elapsed_ms = 0.0;
  bool available = false;
  std::string error;
};

struct LoadingProgressSnapshot {
  std::string product;
  std::string phase = "Waiting";
  size_t completed_products = 0;
  size_t total_products = 0;
  bool running = false;
  bool cancelled = false;
  LoadingState state = LoadingState::Idle;
  std::string error;
  std::vector<LoadingProductProgress> products;

  double fraction() const;
  bool terminal() const;
};

class LoadingProgressTracker {
public:
  void start(size_t total_products, std::string phase);
  void startProduct(std::string product, std::string phase);
  void completeProduct(std::string product, std::string phase,
                       bool available, double elapsed_ms,
                       std::string error = {});
  void setPhase(std::string phase);
  void finish(std::string phase = "Ready");
  void cancel(std::string phase = "Loading cancelled");
  void fail(std::string error, std::string phase = "Loading failed");
  LoadingProgressSnapshot snapshot() const;

private:
  mutable std::mutex mutex_;
  LoadingProgressSnapshot state_;
};

} // namespace crimson::loading
