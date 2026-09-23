#pragma once

#include "zarr/canonical_detection_repository.h"
#include "zarr/tensorstore_canonical_detection_repository.h"
#include "zarr/tensorstore_refined_detection_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

enum class DetectionRawFallbackPolicy : uint8_t {
  Forbid,
  AllowOnlyWhenNoRefinedAuthority,
};

struct DetectionRepositorySelectionRequest {
  std::string explicit_refined_run;
  std::string canonical_raw_run;
  DetectionRawFallbackPolicy raw_fallback_policy =
      DetectionRawFallbackPolicy::Forbid;
  bool allow_selector_ineligible_benchmark = false;
};

enum class DetectionRepositorySelectionKind : uint8_t {
  ExplicitRefinedV1,
  ApprovedAuthoritativeRefinedV1,
  ExplicitlyPermittedCanonicalRaw,
};

struct DetectionRepositorySelectionMetrics {
  DetectionRepositorySelectionKind kind =
      DetectionRepositorySelectionKind::ExplicitRefinedV1;
  size_t authority_metadata_reads = 0;
  CanonicalDetectionRepositoryOpenMetrics canonical_open;
  RefinedDetectionRepositoryOpenMetrics refined_open;
};

std::unique_ptr<CanonicalDetectionRepository> OpenSelectedDetectionRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const DetectionRepositorySelectionRequest &request,
    std::string *error_message = nullptr,
    DetectionRepositorySelectionMetrics *metrics = nullptr);

} // namespace crimson::zarr
