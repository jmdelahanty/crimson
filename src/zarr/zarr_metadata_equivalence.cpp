#include "zarr/zarr_metadata_equivalence.h"

#include <nlohmann/json.hpp>

namespace crimson::zarr::internal {
namespace {

using json = nlohmann::json;

bool isEmptyGroupConsolidation(const json &value) {
  return value.is_null() ||
         (value.is_object() && value.size() == 3 &&
          value.value("kind", "") == "inline" &&
          value.contains("must_understand") &&
          value.at("must_understand").is_boolean() &&
          !value.at("must_understand").get<bool>() &&
          value.contains("metadata") && value.at("metadata").is_object() &&
          value.at("metadata").empty());
}

bool normalizeEmptyGroupConsolidation(json *value) {
  if (!value || !value->is_object()) {
    return false;
  }
  const auto found = value->find("consolidated_metadata");
  if (found == value->end()) {
    return true;
  }
  if (!isEmptyGroupConsolidation(*found)) {
    return false;
  }
  value->erase(found);
  return true;
}

} // namespace

bool EquivalentDirectAndConsolidatedZarrNode(const json &direct,
                                             const json &consolidated) {
  if (!direct.is_object() || !consolidated.is_object()) {
    return false;
  }
  if (direct.value("node_type", "") != "group" ||
      consolidated.value("node_type", "") != "group") {
    return direct == consolidated &&
           !direct.contains("consolidated_metadata") &&
           !consolidated.contains("consolidated_metadata");
  }
  json normalized_direct = direct;
  json normalized_consolidated = consolidated;
  return normalizeEmptyGroupConsolidation(&normalized_direct) &&
         normalizeEmptyGroupConsolidation(&normalized_consolidated) &&
         normalized_direct == normalized_consolidated;
}

} // namespace crimson::zarr::internal
