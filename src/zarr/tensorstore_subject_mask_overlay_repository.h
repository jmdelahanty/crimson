#pragma once

#include "zarr/archive_context.h"
#include "zarr/subject_mask_overlay_repository.h"

#include <memory>
#include <string>

namespace crimson::zarr {

struct SubjectMaskOverlayOpenOptions {
  std::string requested_run;
  std::string expected_manifest_payload_digest;
  bool allow_selector_ineligible = false;
  bool require_strict_v1 = false;
  std::shared_ptr<ArchiveContext> presentation_cache_archive;
  std::string presentation_cache_run;
  std::string expected_presentation_cache_manifest_payload_digest;
  bool contour_only = false;
};

std::unique_ptr<SubjectMaskOverlayRepository>
OpenSubjectMaskOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 const SubjectMaskOverlayOpenOptions &options,
                                 std::string *error_message = nullptr);

std::unique_ptr<SubjectMaskOverlayRepository>
OpenSubjectMaskOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 const std::string &requested_run = {},
                                 std::string *error_message = nullptr);

} // namespace crimson::zarr
