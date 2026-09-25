#include "recording_open_workflow.h"

#include <utility>

namespace crimson::session {

RecordingOpenWorkflowController::RecordingOpenWorkflowController(
    SessionLifecycle &lifecycle, loading::LoadingProgressTracker &progress)
    : coordinator_(lifecycle, progress) {}

bool RecordingOpenWorkflowController::begin(
    SessionOpenPlan plan, SessionOpenReadinessBinding child_readiness,
    std::string *error) {
  auto transaction = coordinator_.begin(std::move(plan), error);
  if (!transaction) {
    return false;
  }
  transaction_ = std::move(transaction);
  child_readiness_ = std::move(child_readiness);
  started_ = Clock::now();
  product_started_.clear();
  return true;
}

bool RecordingOpenWorkflowController::active() const {
  return transaction_.current();
}

uint64_t RecordingOpenWorkflowController::generation() const {
  return transaction_.generation();
}

double RecordingOpenWorkflowController::elapsedMs() const {
  if (started_ == Clock::time_point{}) {
    return 0.0;
  }
  return std::chrono::duration<double, std::milli>(Clock::now() - started_)
      .count();
}

bool RecordingOpenWorkflowController::startProduct(const std::string &product,
                                                   std::string phase) {
  if (!transaction_.startProduct(product, std::move(phase))) {
    return false;
  }
  product_started_[product] = Clock::now();
  return true;
}

double RecordingOpenWorkflowController::productElapsedMs(
    const std::string &product) const {
  const auto found = product_started_.find(product);
  const auto product_start =
      found == product_started_.end() ? started_ : found->second;
  if (product_start == Clock::time_point{}) {
    return 0.0;
  }
  return std::chrono::duration<double, std::milli>(Clock::now() - product_start)
      .count();
}

bool RecordingOpenWorkflowController::completeProduct(
    const std::string &product, std::string phase, bool available,
    std::string error) {
  return transaction_.completeProduct(product, std::move(phase), available,
                                      productElapsedMs(product),
                                      std::move(error));
}

bool RecordingOpenWorkflowController::failProduct(const std::string &product,
                                                  std::string product_phase,
                                                  std::string error,
                                                  std::string session_phase) {
  const bool product_completed =
      completeProduct(product, std::move(product_phase), false, error);
  const bool session_failed =
      transaction_.fail(std::move(error), std::move(session_phase));
  return product_completed && session_failed;
}

bool RecordingOpenWorkflowController::commit(
    std::optional<SessionDescriptor> resolved, std::string phase,
    std::string failed_phase, std::string *error) {
  std::string commit_error;
  if (transaction_.commit(std::move(resolved), std::move(phase),
                          &commit_error)) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  transaction_.fail(commit_error, std::move(failed_phase));
  if (error != nullptr) {
    *error = std::move(commit_error);
  }
  return false;
}

bool RecordingOpenWorkflowController::fail(std::string error,
                                           std::string phase) {
  return transaction_.fail(std::move(error), std::move(phase));
}

bool RecordingOpenWorkflowController::cancel(std::string phase) {
  return transaction_.cancel(std::move(phase));
}

SessionOpenSettlement RecordingOpenWorkflowController::settle(
    uint64_t generation,
    const loading::LoadingProgressSnapshot &child_progress) {
  if (generation != transaction_.generation()) {
    return {};
  }
  return settleSessionOpenFromProgress(transaction_, child_progress,
                                       child_readiness_, elapsedMs());
}

SessionReadinessDecision RecordingOpenWorkflowController::readiness(
    const SessionReadinessPolicy &policy) const {
  return coordinator_.readiness(policy);
}

} // namespace crimson::session
