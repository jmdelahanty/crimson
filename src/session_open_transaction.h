#pragma once

#include "loading_progress.h"
#include "session_lifecycle.h"
#include "session_readiness.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace crimson::session {

struct SessionOpenPlan {
  SessionDescriptor requested;
  std::string phase = "Opening session";
  std::vector<SessionReadinessProductRule> products;
};

namespace detail {
struct SessionOpenState;
}

class SessionOpenTransaction {
public:
  SessionOpenTransaction() = default;

  uint64_t generation() const;
  bool current() const;
  explicit operator bool() const;

  bool startProduct(const std::string &product, std::string phase) const;
  bool completeProduct(const std::string &product, std::string phase,
                       bool available, double elapsed_ms,
                       std::string error = {}) const;
  bool commit(std::optional<SessionDescriptor> resolved = std::nullopt,
              std::string phase = "Session ready",
              std::string *error = nullptr) const;
  bool fail(std::string error, std::string phase = "Session unavailable") const;
  bool cancel(std::string phase = "Session opening cancelled") const;

private:
  friend class SessionOpenCoordinator;
  SessionOpenTransaction(std::shared_ptr<detail::SessionOpenState> state,
                         uint64_t generation);

  std::shared_ptr<detail::SessionOpenState> state_;
  uint64_t generation_ = 0;
};

enum class SessionOpenSettlementState : uint8_t {
  Inactive,
  Waiting,
  Committed,
  Failed,
};

struct SessionOpenReadinessBinding {
  std::optional<std::string> transaction_product;
  std::vector<SessionReadinessProductRule> readiness_products;
  SessionReadinessPolicy policy;
  std::optional<SessionDescriptor> resolved;
  std::string product_ready_phase = "Product ready";
  std::string product_failed_phase = "Product unavailable";
  std::string session_ready_phase = "Session ready";
  std::string session_failed_phase = "Session unavailable";
};

struct SessionOpenSettlement {
  SessionReadinessDecision readiness;
  SessionOpenSettlementState state = SessionOpenSettlementState::Inactive;
  std::string error;

  bool terminal() const;
};

SessionOpenSettlement
settleSessionOpenFromProgress(const SessionOpenTransaction &transaction,
                              const loading::LoadingProgressSnapshot &progress,
                              const SessionOpenReadinessBinding &binding,
                              double elapsed_ms);

// Coordinates logical state only. Callers retain ownership of repositories,
// decoders, worker threads, windows, and GPU resources.
class SessionOpenCoordinator {
public:
  SessionOpenCoordinator(SessionLifecycle &lifecycle,
                         loading::LoadingProgressTracker &progress);
  ~SessionOpenCoordinator();

  SessionOpenCoordinator(const SessionOpenCoordinator &) = delete;
  SessionOpenCoordinator &operator=(const SessionOpenCoordinator &) = delete;
  SessionOpenCoordinator(SessionOpenCoordinator &&) = delete;
  SessionOpenCoordinator &operator=(SessionOpenCoordinator &&) = delete;

  SessionOpenTransaction begin(SessionOpenPlan plan,
                               std::string *error = nullptr);
  SessionReadinessDecision
  readiness(const SessionReadinessPolicy &policy = {}) const;
  std::vector<SessionReadinessProductRule> productRules() const;

private:
  std::shared_ptr<detail::SessionOpenState> state_;
};

} // namespace crimson::session
