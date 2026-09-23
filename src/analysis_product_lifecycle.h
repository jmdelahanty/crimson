#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace crimson::analysis {

enum class ProductLifecycleState : uint8_t {
  Available,
  Unavailable,
};

enum class ProductRouteDisposition : uint8_t {
  Adopt,
  Defer,
  RejectInvalid,
  RejectStale,
  RejectCancelled,
  RejectDuplicate,
};

struct ProductDependency {
  std::string product;
  std::string prerequisite;
};

struct ProductLifecyclePlan {
  uint64_t generation = 0;
  std::vector<ProductDependency> dependencies;
};

struct ProductRouteDecision {
  ProductRouteDisposition disposition = ProductRouteDisposition::RejectInvalid;
  std::string reason;

  bool adopt() const;
  bool deferred() const;
  bool rejected() const;
};

struct ProductCompletionDecision {
  bool accepted = false;
  std::vector<std::string> released_products;
  std::string reason;
};

// Orders backend-owned product payloads without knowing their repository,
// decoder, buffer, or renderer types. A platform routes a payload, installs it
// when instructed, and reports the terminal installation result.
class AnalysisProductLifecycleController {
public:
  bool begin(ProductLifecyclePlan plan, std::string *error = nullptr);

  ProductRouteDecision route(uint64_t generation, const std::string &product);
  ProductCompletionDecision
  complete(uint64_t generation, const std::string &product, bool available);
  bool cancel(uint64_t generation);

  uint64_t generation() const;
  bool active() const;
  bool cancelled() const;
  size_t deferredProductCount() const;
  std::optional<ProductLifecycleState> state(const std::string &product) const;

private:
  bool prerequisitesSettled(const std::string &product) const;

  uint64_t generation_ = 0;
  bool active_ = false;
  bool cancelled_ = false;
  std::vector<ProductDependency> dependencies_;
  std::vector<std::string> routed_products_;
  std::vector<std::string> deferred_products_;
  std::vector<std::pair<std::string, ProductLifecycleState>> states_;
};

bool shouldRequestInitialFrame(bool deterministic_presentation,
                               bool loader_running);

} // namespace crimson::analysis
