#include "zarr/coordinate_catalog_contract.h"

#include "zarr/canonical_json.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

using json = nlohmann::json;

struct StageExpectation {
  std::string_view name;
  std::string_view digest;
  size_t binding_count;
  size_t surface_count;
};

StageExpectation expectation(CoordinateCatalogStage stage) {
  switch (stage) {
  case CoordinateCatalogStage::CanonicalDetection:
    return {"canonical_detection",
            "337613bd6e5f283eef9d6a89c14766d50c5b6863dea584f7568b90bb1d936733",
            3, 3};
  case CoordinateCatalogStage::RefinedDetection:
    return {"refined_detection",
            "75656615ecd32a215f6b4148a01c9ef75e96b8d7aa6bf9fb8a7d21757fa7a2ed",
            6, 3};
  case CoordinateCatalogStage::GeometryOnlyCrop:
    return {"geometry_only_crop",
            "e9ce640761ee1de4a6edd72695968bd66ae2fcbdd09d7d2c902450904f6ddfec",
            7, 7};
  }
  return {};
}

void assignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

bool exactKeys(const json &value,
               std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  return std::all_of(
      expected.begin(), expected.end(),
      [&](std::string_view key) { return value.contains(std::string(key)); });
}

bool nonemptyString(const json &value, std::string_view key) {
  const auto found = value.find(std::string(key));
  return found != value.end() && found->is_string() &&
         !found->get_ref<const std::string &>().empty();
}

bool stringList(const json &value, std::vector<std::string> *output) {
  if (!value.is_array() || value.empty() || !output) {
    return false;
  }
  output->clear();
  output->reserve(value.size());
  for (const auto &item : value) {
    if (!item.is_string() || item.get_ref<const std::string &>().empty()) {
      return false;
    }
    output->push_back(item.get<std::string>());
  }
  return true;
}

bool validSemanticRole(std::string_view role) {
  return role == "authoritative_numeric_surface" ||
         role == "exact_derived_numeric_surface" ||
         role == "crop_placement_surface" || role == "sampled_spatial_surface";
}

bool validMapping(std::string_view mapping) {
  return mapping == "direct_source_camera_continuous_pixels" ||
         mapping == "scale_by_source_camera_extent" ||
         mapping == "rowwise_roi_to_source_camera_transform" ||
         mapping == "not_positional_geometry";
}

bool readBinding(const json &value, CoordinateCatalogBinding *binding) {
  if (!binding ||
      !exactKeys(value, {"schema_id", "schema_version", "array_contract_id",
                         "array_contract_version", "surface_id",
                         "semantic_role", "legacy_coordinate_space"}) ||
      value.value("schema_id", "") != "palette.array_coordinate_binding" ||
      !value.at("schema_version").is_number_integer() ||
      value.at("schema_version").get<int>() != 1 ||
      !value.at("array_contract_version").is_number_integer() ||
      value.at("array_contract_version").get<int>() <= 0 ||
      !nonemptyString(value, "array_contract_id") ||
      !nonemptyString(value, "surface_id") ||
      !nonemptyString(value, "semantic_role") ||
      !nonemptyString(value, "legacy_coordinate_space")) {
    return false;
  }
  binding->array_contract_id = value.at("array_contract_id").get<std::string>();
  binding->array_contract_version =
      value.at("array_contract_version").get<int>();
  binding->surface_id = value.at("surface_id").get<std::string>();
  binding->semantic_role = value.at("semantic_role").get<std::string>();
  binding->legacy_coordinate_space =
      value.at("legacy_coordinate_space").get<std::string>();
  return validSemanticRole(binding->semantic_role);
}

bool readSurface(const json &value, CoordinateCatalogSurface *surface) {
  if (!surface ||
      !exactKeys(value,
                 {"schema_id", "schema_version", "surface_id", "domain_id",
                  "geometry_type", "components", "component_units",
                  "pixel_convention", "reference_extent_role",
                  "source_camera_mapping", "descriptor_profile_id",
                  "descriptor_overlay_status"}) ||
      value.value("schema_id", "") != "palette.coordinate_surface_contract" ||
      !value.at("schema_version").is_number_integer() ||
      value.at("schema_version").get<int>() != 1 ||
      !nonemptyString(value, "surface_id") ||
      !nonemptyString(value, "domain_id") ||
      !nonemptyString(value, "geometry_type") ||
      !nonemptyString(value, "pixel_convention") ||
      !nonemptyString(value, "reference_extent_role") ||
      !nonemptyString(value, "source_camera_mapping") ||
      !stringList(value.at("components"), &surface->components) ||
      !stringList(value.at("component_units"), &surface->component_units) ||
      surface->components.size() != surface->component_units.size()) {
    return false;
  }
  surface->surface_id = value.at("surface_id").get<std::string>();
  surface->domain_id = value.at("domain_id").get<std::string>();
  surface->geometry_type = value.at("geometry_type").get<std::string>();
  surface->pixel_convention = value.at("pixel_convention").get<std::string>();
  surface->reference_extent_role =
      value.at("reference_extent_role").get<std::string>();
  surface->source_camera_mapping =
      value.at("source_camera_mapping").get<std::string>();
  if (!validMapping(surface->source_camera_mapping)) {
    return false;
  }
  const auto &profile = value.at("descriptor_profile_id");
  const auto &overlay = value.at("descriptor_overlay_status");
  if (profile.is_null() && overlay.is_null()) {
    surface->has_descriptor = false;
    return surface->source_camera_mapping == "not_positional_geometry";
  }
  if (!profile.is_string() || profile.get_ref<const std::string &>().empty() ||
      !overlay.is_string() || overlay.get_ref<const std::string &>().empty()) {
    return false;
  }
  surface->descriptor_profile_id = profile.get<std::string>();
  surface->descriptor_overlay_status = overlay.get<std::string>();
  surface->has_descriptor = true;
  return surface->source_camera_mapping != "not_positional_geometry";
}

} // namespace

const CoordinateCatalogBinding *
CoordinateCatalogSummary::findBinding(std::string_view array_contract_id,
                                      int array_contract_version) const {
  const auto found =
      std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding) {
        return binding.array_contract_id == array_contract_id &&
               binding.array_contract_version == array_contract_version;
      });
  return found == bindings.end() ? nullptr : &*found;
}

const CoordinateCatalogSurface *
CoordinateCatalogSummary::findSurface(std::string_view surface_id) const {
  const auto found =
      std::find_if(surfaces.begin(), surfaces.end(), [&](const auto &surface) {
        return surface.surface_id == surface_id;
      });
  return found == surfaces.end() ? nullptr : &*found;
}

std::string_view CoordinateCatalogStageName(CoordinateCatalogStage stage) {
  return expectation(stage).name;
}

std::string_view ExpectedCoordinateCatalogDigest(CoordinateCatalogStage stage) {
  return expectation(stage).digest;
}

bool ValidateCoordinateCatalogEnvelope(const json &envelope,
                                       CoordinateCatalogStage stage,
                                       CoordinateCatalogSummary *summary,
                                       std::string *error) {
  try {
    const auto expected = expectation(stage);
    if (expected.name.empty() ||
        !exactKeys(envelope, {"schema_id", "schema_version", "digest_algorithm",
                              "digest", "document"}) ||
        envelope.value("schema_id", "") !=
            "palette.persisted_coordinate_catalog" ||
        !envelope.at("schema_version").is_number_integer() ||
        envelope.at("schema_version").get<int>() != 1 ||
        envelope.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        !envelope.at("document").is_object()) {
      assignError(error, "Coordinate catalog envelope is incompatible");
      return false;
    }
    const std::string digest = envelope.value("digest", "");
    if (!IsLowerSha256(digest) || digest != expected.digest ||
        CanonicalJsonSha256(envelope.at("document")) != digest) {
      assignError(error,
                  "Coordinate catalog digest or frozen identity differs");
      return false;
    }
    const auto &document = envelope.at("document");
    if (!exactKeys(document,
                   {"schema_id", "schema_version", "bindings", "surfaces"}) ||
        document.value("schema_id", "") != "palette.array_coordinate_catalog" ||
        !document.at("schema_version").is_number_integer() ||
        document.at("schema_version").get<int>() != 1 ||
        !document.at("bindings").is_array() ||
        !document.at("surfaces").is_array() ||
        document.at("bindings").size() != expected.binding_count ||
        document.at("surfaces").size() != expected.surface_count) {
      assignError(error, "Coordinate catalog document is incompatible");
      return false;
    }

    CoordinateCatalogSummary candidate;
    candidate.stage = stage;
    candidate.document_digest = digest;
    std::set<std::pair<std::string, int>> binding_keys;
    for (const auto &value : document.at("bindings")) {
      CoordinateCatalogBinding binding;
      if (!readBinding(value, &binding) ||
          !binding_keys
               .emplace(binding.array_contract_id,
                        binding.array_contract_version)
               .second) {
        assignError(
            error,
            "Coordinate catalog contains an invalid or duplicate binding");
        return false;
      }
      candidate.bindings.push_back(std::move(binding));
    }
    std::set<std::string> surface_ids;
    for (const auto &value : document.at("surfaces")) {
      CoordinateCatalogSurface surface;
      if (!readSurface(value, &surface) ||
          !surface_ids.insert(surface.surface_id).second) {
        assignError(
            error,
            "Coordinate catalog contains an invalid or duplicate surface");
        return false;
      }
      candidate.surfaces.push_back(std::move(surface));
    }
    std::set<std::string> referenced_surfaces;
    for (const auto &binding : candidate.bindings) {
      if (!candidate.findSurface(binding.surface_id)) {
        assignError(error, "Coordinate binding references an absent surface");
        return false;
      }
      referenced_surfaces.insert(binding.surface_id);
    }
    if (referenced_surfaces != surface_ids) {
      assignError(error, "Coordinate catalog contains an unbound surface");
      return false;
    }
    if (summary) {
      *summary = std::move(candidate);
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error,
                "Invalid coordinate catalog: " + std::string(exception.what()));
    return false;
  }
}

} // namespace crimson::zarr
