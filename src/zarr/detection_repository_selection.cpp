#include "zarr/detection_repository_selection.h"

#include "zarr/archive_context_internal.h"
#include "zarr/refined_detection_contract.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace crimson::zarr {
namespace {

void assignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

} // namespace

std::unique_ptr<CanonicalDetectionRepository> OpenSelectedDetectionRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const DetectionRepositorySelectionRequest &request,
    std::string *error_message, DetectionRepositorySelectionMetrics *metrics) {
  DetectionRepositorySelectionMetrics observed;
  if (!archive || !archive->impl_) {
    assignError(error_message, "Archive context is unavailable");
    return nullptr;
  }

  if (!request.explicit_refined_run.empty()) {
    observed.kind = DetectionRepositorySelectionKind::ExplicitRefinedV1;
    RefinedDetectionRepositoryOpenOptions options;
    options.allow_selector_ineligible =
        request.allow_selector_ineligible_benchmark;
    auto repository = OpenRefinedDetectionRepository(
        archive, request.explicit_refined_run, options, error_message,
        &observed.refined_open);
    if (metrics) {
      *metrics = observed;
    }
    return repository;
  }

  const auto parent_attributes =
      internal::ReadArchiveAttributes(*archive->impl_, "refined_detect_runs");
  observed.authority_metadata_reads = 1;
  bool authority_present = false;
  if (parent_attributes && parent_attributes->contains("authoritative_run")) {
    authority_present = true;
    if (!parent_attributes->at("authoritative_run").is_string() ||
        !parent_attributes->contains("authoritative_run_provenance")) {
      assignError(error_message,
                  "Refined detection authoritative pointer is malformed");
      if (metrics)
        *metrics = observed;
      return nullptr;
    }
    RefinedDetectionAuthoritySummary authority;
    const std::string run =
        parent_attributes->at("authoritative_run").get<std::string>();
    if (!ValidateRefinedDetectionAuthority(
            run, parent_attributes->at("authoritative_run_provenance"),
            &authority, error_message)) {
      if (metrics)
        *metrics = observed;
      return nullptr;
    }
    observed.kind =
        DetectionRepositorySelectionKind::ApprovedAuthoritativeRefinedV1;
    RefinedDetectionRepositoryOpenOptions options;
    options.authority_approved = true;
    options.expected_manifest_digest = authority.run_manifest_digest;
    auto repository =
        OpenRefinedDetectionRepository(archive, authority.run_id, options,
                                       error_message, &observed.refined_open);
    if (metrics)
      *metrics = observed;
    return repository;
  }

  if (!authority_present &&
      request.raw_fallback_policy ==
          DetectionRawFallbackPolicy::AllowOnlyWhenNoRefinedAuthority &&
      !request.canonical_raw_run.empty()) {
    observed.kind =
        DetectionRepositorySelectionKind::ExplicitlyPermittedCanonicalRaw;
    auto repository = OpenCanonicalDetectionRepository(
        archive, request.canonical_raw_run, error_message,
        &observed.canonical_open);
    if (metrics)
      *metrics = observed;
    return repository;
  }

  assignError(error_message,
              request.canonical_raw_run.empty()
                  ? "No explicit or approved refined detection run is available"
                  : "Canonical raw fallback is forbidden by selection policy");
  if (metrics)
    *metrics = observed;
  return nullptr;
}

} // namespace crimson::zarr
