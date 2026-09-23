#include "session_loading_presentation.h"

#include <algorithm>
#include <utility>

namespace crimson::session {

const char *
sessionLoadingProductStatus(const loading::LoadingProductProgress &product) {
  switch (product.state) {
  case loading::LoadingState::Idle:
    return "Waiting";
  case loading::LoadingState::Running:
    return "Loading";
  case loading::LoadingState::Ready:
    return product.available ? "Ready" : "Unavailable";
  case loading::LoadingState::Cancelled:
    return "Cancelled";
  case loading::LoadingState::Failed:
    return "Unavailable";
  }
  return "Unknown";
}

SessionLoadingPresentation
makeSessionLoadingPresentation(const loading::LoadingProgressSnapshot &progress,
                               const SessionReadinessDecision &readiness,
                               std::string title) {
  SessionLoadingPresentation presentation;
  presentation.title =
      title.empty() ? std::string("Loading analysis") : std::move(title);
  presentation.phase = !progress.phase.empty()    ? progress.phase
                       : !readiness.phase.empty() ? readiness.phase
                                                  : "Preparing session";
  presentation.detail = readiness.reason;
  presentation.completed_products =
      std::min(progress.completed_products, progress.total_products);
  presentation.total_products = progress.total_products;
  presentation.fraction = progress.fraction();
  presentation.visible = readiness.show_loading_ui;
  presentation.error_visible = readiness.show_error_ui;
  presentation.dismissible = readiness.loading_ui_dismissible;
  presentation.background_loading = readiness.background_loading;
  presentation.readiness_state = readiness.state;
  presentation.products.reserve(progress.products.size());
  for (const auto &product : progress.products) {
    presentation.products.push_back(
        {product.product, product.phase, sessionLoadingProductStatus(product),
         product.elapsed_ms, product.available, product.error});
  }
  return presentation;
}

} // namespace crimson::session
