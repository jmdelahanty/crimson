#include "session_readiness.h"

#include <algorithm>

namespace crimson::session {

namespace {

const loading::LoadingProductProgress *
findProduct(const loading::LoadingProgressSnapshot &progress,
            const std::string &product) {
  const auto found = std::find_if(
      progress.products.begin(), progress.products.end(),
      [&](const auto &candidate) { return candidate.product == product; });
  return found == progress.products.end() ? nullptr : &*found;
}

SessionReadinessDecision makeDecision(SessionReadinessState state,
                                      const std::string &phase,
                                      std::string reason,
                                      const SessionReadinessPolicy &policy,
                                      bool background_loading = false) {
  SessionReadinessDecision decision;
  decision.state = state;
  decision.phase = phase;
  decision.reason = std::move(reason);
  decision.background_loading = background_loading;

  switch (state) {
  case SessionReadinessState::Waiting:
    decision.show_loading_ui = true;
    decision.loading_ui_dismissible = policy.allow_background_continue;
    decision.session_interaction_enabled = policy.allow_background_continue;
    decision.playback_controls_enabled = policy.allow_background_continue;
    decision.pause_playback = !policy.allow_background_continue;
    break;
  case SessionReadinessState::Ready:
    decision.show_loading_ui = false;
    decision.loading_ui_dismissible = true;
    decision.session_interaction_enabled = true;
    decision.playback_controls_enabled = true;
    decision.pause_playback = false;
    decision.autoplay_allowed = policy.autoplay_when_ready;
    break;
  case SessionReadinessState::Blocked:
  case SessionReadinessState::Cancelled:
    decision.show_loading_ui = false;
    decision.show_error_ui = true;
    decision.loading_ui_dismissible = true;
    decision.session_interaction_enabled = true;
    decision.playback_controls_enabled = false;
    decision.pause_playback = true;
    break;
  }
  return decision;
}

} // namespace

const char *sessionReadinessStateName(SessionReadinessState state) {
  switch (state) {
  case SessionReadinessState::Waiting:
    return "waiting";
  case SessionReadinessState::Ready:
    return "ready";
  case SessionReadinessState::Blocked:
    return "blocked";
  case SessionReadinessState::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

bool SessionReadinessDecision::ready() const {
  return state == SessionReadinessState::Ready;
}

SessionReadinessDecision evaluateSessionReadiness(
    const loading::LoadingProgressSnapshot &progress,
    const std::vector<SessionReadinessProductRule> &products,
    const SessionReadinessPolicy &policy) {
  const std::string phase =
      progress.phase.empty() ? "Preparing session" : progress.phase;

  if (progress.state == loading::LoadingState::Idle && products.empty()) {
    return makeDecision(SessionReadinessState::Ready, phase, {}, policy);
  }

  if (progress.state == loading::LoadingState::Cancelled) {
    return makeDecision(SessionReadinessState::Cancelled, phase,
                        progress.error.empty() ? "Loading was cancelled"
                                               : progress.error,
                        policy);
  }
  if (progress.state == loading::LoadingState::Failed) {
    return makeDecision(
        SessionReadinessState::Blocked, phase,
        progress.error.empty() ? "Loading failed" : progress.error, policy);
  }

  for (const auto &rule : products) {
    const auto *product = findProduct(progress, rule.product);
    if (product == nullptr) {
      if (progress.terminal()) {
        return makeDecision(SessionReadinessState::Blocked, phase,
                            "Loading completed without reporting product '" +
                                rule.product + "'",
                            policy);
      }
      return makeDecision(SessionReadinessState::Waiting, phase,
                          "Waiting for " + rule.product, policy);
    }
    if (product->state == loading::LoadingState::Idle ||
        product->state == loading::LoadingState::Running) {
      return makeDecision(SessionReadinessState::Waiting, phase,
                          "Waiting for " + rule.product, policy);
    }
    if (rule.availability == ProductAvailabilityRequirement::Required &&
        (product->state != loading::LoadingState::Ready ||
         !product->available)) {
      return makeDecision(SessionReadinessState::Blocked, phase,
                          product->error.empty()
                              ? "Required product '" + rule.product +
                                    "' is unavailable"
                              : product->error,
                          policy);
    }
  }

  if (policy.wait_for_all_products) {
    if (progress.state == loading::LoadingState::Idle ||
        progress.state == loading::LoadingState::Running) {
      return makeDecision(SessionReadinessState::Waiting, phase,
                          "Loading products are still pending", policy);
    }
    if (progress.completed_products < progress.total_products) {
      return makeDecision(
          SessionReadinessState::Blocked, phase,
          "Loading finished before all declared products completed", policy);
    }
  }

  const bool background_loading =
      !policy.wait_for_all_products &&
      progress.state == loading::LoadingState::Running;
  return makeDecision(SessionReadinessState::Ready, phase, {}, policy,
                      background_loading);
}

} // namespace crimson::session
