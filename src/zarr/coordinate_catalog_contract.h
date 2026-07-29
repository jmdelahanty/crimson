#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crimson::zarr {

enum class CoordinateCatalogStage : uint8_t {
  CanonicalDetection,
  RefinedDetection,
  GeometryOnlyCrop,
};

struct CoordinateCatalogBinding {
  std::string array_contract_id;
  int array_contract_version = 0;
  std::string surface_id;
  std::string semantic_role;
  std::string legacy_coordinate_space;
};

struct CoordinateCatalogSurface {
  std::string surface_id;
  std::string domain_id;
  std::string geometry_type;
  std::vector<std::string> components;
  std::vector<std::string> component_units;
  std::string pixel_convention;
  std::string reference_extent_role;
  std::string source_camera_mapping;
  std::string descriptor_profile_id;
  std::string descriptor_overlay_status;
  bool has_descriptor = false;
};

struct CoordinateCatalogSummary {
  CoordinateCatalogStage stage = CoordinateCatalogStage::CanonicalDetection;
  std::string document_digest;
  std::vector<CoordinateCatalogBinding> bindings;
  std::vector<CoordinateCatalogSurface> surfaces;

  const CoordinateCatalogBinding *
  findBinding(std::string_view array_contract_id,
              int array_contract_version) const;
  const CoordinateCatalogSurface *
  findSurface(std::string_view surface_id) const;
};

std::string_view CoordinateCatalogStageName(CoordinateCatalogStage stage);
std::string_view ExpectedCoordinateCatalogDigest(CoordinateCatalogStage stage);

bool ValidateCoordinateCatalogEnvelope(const nlohmann::json &envelope,
                                       CoordinateCatalogStage stage,
                                       CoordinateCatalogSummary *summary,
                                       std::string *error = nullptr);

} // namespace crimson::zarr
