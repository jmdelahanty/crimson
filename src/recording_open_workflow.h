#pragma once

#include "session_open_transaction.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace crimson::session {

// Owns the portable orchestration for one recording/session open. Platform
// shells still perform I/O and own decoders, repositories, and GPU resources.
class RecordingOpenWorkflowController {
public:
  RecordingOpenWorkflowController(SessionLifecycle &lifecycle,
                                  loading::LoadingProgressTracker &progress);

  bool begin(SessionOpenPlan plan,
             SessionOpenReadinessBinding child_readiness = {},
             std::string *error = nullptr);

  bool active() const;
  uint64_t generation() const;
  double elapsedMs() const;

  bool startProduct(const std::string &product, std::string phase);
  bool completeProduct(const std::string &product, std::string phase,
                       bool available, std::string error = {});
  bool failProduct(const std::string &product, std::string product_phase,
                   std::string error,
                   std::string session_phase = "Session unavailable");

  bool commit(std::optional<SessionDescriptor> resolved = std::nullopt,
              std::string phase = "Session ready",
              std::string failed_phase = "Session unavailable",
              std::string *error = nullptr);
  bool fail(std::string error, std::string phase = "Session unavailable");
  bool cancel(std::string phase = "Session opening cancelled");

  SessionOpenSettlement
  settle(uint64_t generation,
         const loading::LoadingProgressSnapshot &child_progress);
  SessionReadinessDecision
  readiness(const SessionReadinessPolicy &policy = {}) const;

private:
  using Clock = std::chrono::steady_clock;

  double productElapsedMs(const std::string &product) const;

  SessionOpenCoordinator coordinator_;
  SessionOpenTransaction transaction_;
  SessionOpenReadinessBinding child_readiness_;
  Clock::time_point started_{};
  std::unordered_map<std::string, Clock::time_point> product_started_;
};

} // namespace crimson::session
