#pragma once

#include "loading_progress.h"
#include "session_readiness.h"

#include <cstddef>
#include <string>
#include <vector>

namespace crimson::session {

struct SessionLoadingProductPresentation {
  std::string product;
  std::string phase;
  std::string status;
  double elapsed_ms = 0.0;
  bool available = false;
  std::string error;
};

struct SessionLoadingPresentation {
  std::string title = "Loading analysis";
  std::string phase = "Preparing session";
  std::string detail;
  size_t completed_products = 0;
  size_t total_products = 0;
  double fraction = 0.0;
  bool visible = false;
  bool error_visible = false;
  bool dismissible = false;
  bool background_loading = false;
  SessionReadinessState readiness_state = SessionReadinessState::Waiting;
  std::vector<SessionLoadingProductPresentation> products;
};

const char *
sessionLoadingProductStatus(const loading::LoadingProductProgress &product);

SessionLoadingPresentation
makeSessionLoadingPresentation(const loading::LoadingProgressSnapshot &progress,
                               const SessionReadinessDecision &readiness,
                               std::string title = "Loading analysis");

} // namespace crimson::session
