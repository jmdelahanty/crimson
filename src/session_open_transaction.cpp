#include "session_open_transaction.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace crimson::session {

namespace detail {

struct SessionOpenState {
  SessionLifecycle *lifecycle = nullptr;
  loading::LoadingProgressTracker *progress = nullptr;
  mutable std::mutex mutex;
  uint64_t active_generation = 0;
  std::vector<SessionReadinessProductRule> products;
  std::unordered_set<std::string> product_names;
};

} // namespace detail

namespace {

bool isCurrent(const detail::SessionOpenState &state, uint64_t generation) {
  return generation != 0 && state.active_generation == generation &&
         state.lifecycle != nullptr && state.progress != nullptr;
}

void setError(std::string *error, std::string value) {
  if (error != nullptr) {
    *error = std::move(value);
  }
}

} // namespace

SessionOpenTransaction::SessionOpenTransaction(
    std::shared_ptr<detail::SessionOpenState> state, uint64_t generation)
    : state_(std::move(state)), generation_(generation) {}

uint64_t SessionOpenTransaction::generation() const { return generation_; }

bool SessionOpenTransaction::current() const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return isCurrent(*state_, generation_);
}

SessionOpenTransaction::operator bool() const { return current(); }

bool SessionOpenTransaction::startProduct(const std::string &product,
                                          std::string phase) const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!isCurrent(*state_, generation_) ||
      state_->product_names.count(product) == 0) {
    return false;
  }
  state_->progress->startProduct(product, std::move(phase));
  return true;
}

bool SessionOpenTransaction::completeProduct(const std::string &product,
                                             std::string phase, bool available,
                                             double elapsed_ms,
                                             std::string error) const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!isCurrent(*state_, generation_) ||
      state_->product_names.count(product) == 0) {
    return false;
  }
  state_->progress->completeProduct(product, std::move(phase), available,
                                    elapsed_ms, std::move(error));
  return true;
}

bool SessionOpenTransaction::commit(std::optional<SessionDescriptor> resolved,
                                    std::string phase,
                                    std::string *error) const {
  if (!state_) {
    setError(error, "Session-open transaction is invalid");
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!isCurrent(*state_, generation_)) {
    setError(error, "Session-open transaction is stale");
    return false;
  }

  const auto progress = state_->progress->snapshot();
  for (const auto &rule : state_->products) {
    const auto found =
        std::find_if(progress.products.begin(), progress.products.end(),
                     [&](const auto &candidate) {
                       return candidate.product == rule.product;
                     });
    if (found == progress.products.end() ||
        found->state == loading::LoadingState::Idle ||
        found->state == loading::LoadingState::Running) {
      setError(error,
               "Session product '" + rule.product + "' has not completed");
      return false;
    }
    if (rule.availability == ProductAvailabilityRequirement::Required &&
        (found->state != loading::LoadingState::Ready || !found->available)) {
      setError(error, "Required session product '" + rule.product +
                          "' is unavailable");
      return false;
    }
  }

  if (!state_->lifecycle->completeOpen(generation_, std::move(resolved))) {
    setError(error, "Session lifecycle rejected the open completion");
    return false;
  }
  state_->progress->finish(std::move(phase));
  state_->active_generation = 0;
  setError(error, {});
  return true;
}

bool SessionOpenTransaction::fail(std::string error, std::string phase) const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!isCurrent(*state_, generation_)) {
    return false;
  }
  if (!state_->lifecycle->failOpen(generation_, error)) {
    return false;
  }
  state_->progress->fail(std::move(error), std::move(phase));
  state_->active_generation = 0;
  return true;
}

bool SessionOpenTransaction::cancel(std::string phase) const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!isCurrent(*state_, generation_)) {
    return false;
  }
  if (!state_->lifecycle->failOpen(generation_, phase)) {
    return false;
  }
  state_->progress->cancel(std::move(phase));
  state_->active_generation = 0;
  return true;
}

bool SessionOpenSettlement::terminal() const {
  return state == SessionOpenSettlementState::Committed ||
         state == SessionOpenSettlementState::Failed;
}

SessionOpenSettlement
settleSessionOpenFromProgress(const SessionOpenTransaction &transaction,
                              const loading::LoadingProgressSnapshot &progress,
                              const SessionOpenReadinessBinding &binding,
                              double elapsed_ms) {
  SessionOpenSettlement settlement;
  settlement.readiness = evaluateSessionReadiness(
      progress, binding.readiness_products, binding.policy);
  if (!transaction.current()) {
    return settlement;
  }
  if (settlement.readiness.state == SessionReadinessState::Waiting) {
    settlement.state = SessionOpenSettlementState::Waiting;
    return settlement;
  }

  if (settlement.readiness.ready()) {
    if (binding.transaction_product &&
        !transaction.completeProduct(*binding.transaction_product,
                                     binding.product_ready_phase, true,
                                     elapsed_ms)) {
      settlement.state = SessionOpenSettlementState::Failed;
      settlement.error = "Failed to publish ready session product '" +
                         *binding.transaction_product + "'";
      transaction.fail(settlement.error, binding.session_failed_phase);
      return settlement;
    }

    if (!transaction.commit(binding.resolved, binding.session_ready_phase,
                            &settlement.error)) {
      settlement.state = SessionOpenSettlementState::Failed;
      transaction.fail(settlement.error, binding.session_failed_phase);
      return settlement;
    }
    settlement.state = SessionOpenSettlementState::Committed;
    return settlement;
  }

  settlement.error = settlement.readiness.reason.empty()
                         ? "Session loading failed"
                         : settlement.readiness.reason;
  if (binding.transaction_product) {
    transaction.completeProduct(*binding.transaction_product,
                                binding.product_failed_phase, false, elapsed_ms,
                                settlement.error);
  }
  transaction.fail(settlement.error, binding.session_failed_phase);
  settlement.state = SessionOpenSettlementState::Failed;
  return settlement;
}

SessionOpenCoordinator::SessionOpenCoordinator(
    SessionLifecycle &lifecycle, loading::LoadingProgressTracker &progress)
    : state_(std::make_shared<detail::SessionOpenState>()) {
  state_->lifecycle = &lifecycle;
  state_->progress = &progress;
}

SessionOpenCoordinator::~SessionOpenCoordinator() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->active_generation = 0;
  state_->lifecycle = nullptr;
  state_->progress = nullptr;
}

SessionOpenTransaction SessionOpenCoordinator::begin(SessionOpenPlan plan,
                                                     std::string *error) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::unordered_set<std::string> names;
  for (const auto &product : plan.products) {
    if (product.product.empty()) {
      setError(error, "Session product names must not be empty");
      return {};
    }
    if (!names.insert(product.product).second) {
      setError(error, "Session product names must be unique");
      return {};
    }
  }

  const uint64_t generation =
      state_->lifecycle->beginOpen(std::move(plan.requested));
  state_->products = std::move(plan.products);
  state_->product_names = std::move(names);
  state_->active_generation = generation;
  state_->progress->start(state_->products.size(), std::move(plan.phase));
  setError(error, {});
  return SessionOpenTransaction(state_, generation);
}

SessionReadinessDecision
SessionOpenCoordinator::readiness(const SessionReadinessPolicy &policy) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return evaluateSessionReadiness(state_->progress->snapshot(),
                                  state_->products, policy);
}

std::vector<SessionReadinessProductRule>
SessionOpenCoordinator::productRules() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->products;
}

} // namespace crimson::session
