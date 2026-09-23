#pragma once

#include "zarr/subject_mask_v1_contract.h"

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace crimson::zarr {

struct SubjectMaskSampledContourV1ArrayDeclaration {
  std::string path;
  std::string dtype;
  std::vector<size_t> shape;
  std::string component;
  std::string field;
  size_t sample_count = 0;
};

struct SubjectMaskSampledContourV1Summary {
  std::string run_id;
  std::string payload_digest;
  std::string manifest_digest;
  std::string metadata_digest;
  std::string logical_content_digest;
  std::string source_run_id;
  std::string source_manifest_payload_digest;
  std::string source_manifest_digest;
  size_t frame_count = 0;
  size_t row_count = 0;
  std::vector<std::string> component_labels;
  std::vector<size_t> component_sample_counts;
  std::vector<SubjectMaskSampledContourV1ArrayDeclaration> arrays;
};

bool ValidateSubjectMaskSampledContourV1Manifest(
    const nlohmann::json &manifest, std::string_view requested_run,
    const nlohmann::json &source_manifest,
    const SubjectMaskV1ManifestSummary &source_summary,
    SubjectMaskSampledContourV1Summary *summary, std::string *error = nullptr);

} // namespace crimson::zarr
