#pragma once

#include "loading_progress.h"

#include <cstdint>
#include <string>
#include <vector>

namespace crimson::session {

enum class ProductAvailabilityRequirement : uint8_t {
  Optional,
  Required,
};

struct SessionReadinessProductRule {
  std::string product;
  ProductAvailabilityRequirement availability =
      ProductAvailabilityRequirement::Optional;
};

struct SessionReadinessPolicy {
  bool wait_for_all_products = true;
  bool allow_background_continue = false;
  bool autoplay_when_ready = false;
};

enum class SessionReadinessState : uint8_t {
  Waiting,
  Ready,
  Blocked,
  Cancelled,
};

const char *sessionReadinessStateName(SessionReadinessState state);

struct SessionReadinessDecision {
  SessionReadinessState state = SessionReadinessState::Waiting;
  bool show_loading_ui = true;
  bool show_error_ui = false;
  bool loading_ui_dismissible = false;
  bool session_interaction_enabled = false;
  bool playback_controls_enabled = false;
  bool pause_playback = true;
  bool autoplay_allowed = false;
  bool background_loading = false;
  std::string phase;
  std::string reason;

  bool ready() const;
};

SessionReadinessDecision evaluateSessionReadiness(
    const loading::LoadingProgressSnapshot &progress,
    const std::vector<SessionReadinessProductRule> &products,
    const SessionReadinessPolicy &policy = {});

} // namespace crimson::session
