#include "analysis_product_lifecycle.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <utility>

namespace crimson::analysis {

namespace {

template <typename Container>
bool contains(const Container &values, const std::string &value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool dependencyGraphHasCycle(
    const std::vector<ProductDependency> &dependencies) {
  std::unordered_map<std::string, std::vector<std::string>> edges;
  for (const auto &dependency : dependencies) {
    edges[dependency.product].push_back(dependency.prerequisite);
  }

  enum class Visit : uint8_t { Unvisited, Visiting, Complete };
  std::unordered_map<std::string, Visit> visits;
  std::function<bool(const std::string &)> visit =
      [&](const std::string &product) {
        const auto state = visits[product];
        if (state == Visit::Visiting) {
          return true;
        }
        if (state == Visit::Complete) {
          return false;
        }
        visits[product] = Visit::Visiting;
        const auto found = edges.find(product);
        if (found != edges.end()) {
          for (const auto &prerequisite : found->second) {
            if (visit(prerequisite)) {
              return true;
            }
          }
        }
        visits[product] = Visit::Complete;
        return false;
      };

  for (const auto &entry : edges) {
    if (visit(entry.first)) {
      return true;
    }
  }
  return false;
}

ProductRouteDecision reject(ProductRouteDisposition disposition,
                            std::string reason) {
  return {disposition, std::move(reason)};
}

} // namespace

bool ProductRouteDecision::adopt() const {
  return disposition == ProductRouteDisposition::Adopt;
}

bool ProductRouteDecision::deferred() const {
  return disposition == ProductRouteDisposition::Defer;
}

bool ProductRouteDecision::rejected() const { return !adopt() && !deferred(); }

bool AnalysisProductLifecycleController::begin(ProductLifecyclePlan plan,
                                               std::string *error) {
  auto fail = [&](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };

  if (plan.generation == 0) {
    return fail("Analysis product lifecycle generation must be non-zero");
  }
  std::set<std::pair<std::string, std::string>> unique_dependencies;
  for (const auto &dependency : plan.dependencies) {
    if (dependency.product.empty() || dependency.prerequisite.empty()) {
      return fail("Analysis product dependencies require non-empty names");
    }
    if (dependency.product == dependency.prerequisite) {
      return fail("Analysis product cannot depend on itself: " +
                  dependency.product);
    }
    if (!unique_dependencies
             .insert({dependency.product, dependency.prerequisite})
             .second) {
      return fail("Duplicate analysis product dependency: " +
                  dependency.product + " -> " + dependency.prerequisite);
    }
  }
  if (dependencyGraphHasCycle(plan.dependencies)) {
    return fail("Analysis product dependency graph contains a cycle");
  }

  generation_ = plan.generation;
  active_ = true;
  cancelled_ = false;
  dependencies_ = std::move(plan.dependencies);
  routed_products_.clear();
  deferred_products_.clear();
  states_.clear();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool AnalysisProductLifecycleController::prerequisitesSettled(
    const std::string &product) const {
  for (const auto &dependency : dependencies_) {
    if (dependency.product == product && !state(dependency.prerequisite)) {
      return false;
    }
  }
  return true;
}

ProductRouteDecision
AnalysisProductLifecycleController::route(uint64_t generation,
                                          const std::string &product) {
  if (product.empty()) {
    return reject(ProductRouteDisposition::RejectInvalid,
                  "Analysis product name must be non-empty");
  }
  if (generation != generation_) {
    return reject(ProductRouteDisposition::RejectStale,
                  "Analysis product result belongs to a stale generation");
  }
  if (cancelled_) {
    return reject(ProductRouteDisposition::RejectCancelled,
                  "Analysis product lifecycle was cancelled");
  }
  if (!active_) {
    return reject(ProductRouteDisposition::RejectInvalid,
                  "Analysis product lifecycle is not active");
  }
  if (state(product) || contains(routed_products_, product)) {
    return reject(ProductRouteDisposition::RejectDuplicate,
                  "Analysis product was already routed or completed: " +
                      product);
  }
  if (contains(deferred_products_, product)) {
    return {ProductRouteDisposition::Defer,
            "Analysis product is already waiting for prerequisites: " +
                product};
  }
  if (!prerequisitesSettled(product)) {
    deferred_products_.push_back(product);
    return {ProductRouteDisposition::Defer,
            "Analysis product is waiting for prerequisites: " + product};
  }
  routed_products_.push_back(product);
  return {ProductRouteDisposition::Adopt, {}};
}

ProductCompletionDecision AnalysisProductLifecycleController::complete(
    uint64_t generation, const std::string &product, bool available) {
  ProductCompletionDecision decision;
  if (generation != generation_) {
    decision.reason =
        "Analysis product completion belongs to a stale generation";
    return decision;
  }
  if (cancelled_ || !active_) {
    decision.reason = cancelled_ ? "Analysis product lifecycle was cancelled"
                                 : "Analysis product lifecycle is not active";
    return decision;
  }
  const auto routed =
      std::find(routed_products_.begin(), routed_products_.end(), product);
  if (routed == routed_products_.end()) {
    decision.reason =
        "Analysis product was not routed for adoption: " + product;
    return decision;
  }

  routed_products_.erase(routed);
  states_.push_back({product, available ? ProductLifecycleState::Available
                                        : ProductLifecycleState::Unavailable});
  decision.accepted = true;

  auto deferred = deferred_products_.begin();
  while (deferred != deferred_products_.end()) {
    if (prerequisitesSettled(*deferred)) {
      decision.released_products.push_back(*deferred);
      deferred = deferred_products_.erase(deferred);
    } else {
      ++deferred;
    }
  }
  std::sort(decision.released_products.begin(),
            decision.released_products.end());
  return decision;
}

bool AnalysisProductLifecycleController::cancel(uint64_t generation) {
  if (!active_ || generation != generation_) {
    return false;
  }
  active_ = false;
  cancelled_ = true;
  routed_products_.clear();
  deferred_products_.clear();
  return true;
}

uint64_t AnalysisProductLifecycleController::generation() const {
  return generation_;
}

bool AnalysisProductLifecycleController::active() const { return active_; }

bool AnalysisProductLifecycleController::cancelled() const {
  return cancelled_;
}

size_t AnalysisProductLifecycleController::deferredProductCount() const {
  return deferred_products_.size();
}

std::optional<ProductLifecycleState>
AnalysisProductLifecycleController::state(const std::string &product) const {
  const auto found =
      std::find_if(states_.begin(), states_.end(),
                   [&](const auto &entry) { return entry.first == product; });
  if (found == states_.end()) {
    return std::nullopt;
  }
  return found->second;
}

bool shouldRequestInitialFrame(bool deterministic_presentation,
                               bool loader_running) {
  return deterministic_presentation || !loader_running;
}

} // namespace crimson::analysis
