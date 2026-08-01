#include "zarr/canonical_json.h"
#include "zarr/subject_mask_sampled_contour_v1_contract.h"
#include "zarr/subject_mask_v1_contract.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

json readRunManifest(const std::filesystem::path &store,
                     const std::string &group, const std::string &run) {
  const auto path = store / group / run / "zarr.json";
  std::ifstream input(path);
  require(input.good(), "Could not open run metadata: " + path.string());
  json metadata;
  input >> metadata;
  return metadata.at("attributes").at("run_manifest");
}

void updateEnvelopeDigest(json *manifest) {
  (*manifest)["payload_digest"] =
      crimson::zarr::CanonicalJsonSha256(manifest->at("payload"));
}

void expectRejected(json candidate, const json &source_manifest,
                    const crimson::zarr::SubjectMaskV1ManifestSummary &source,
                    const std::string &run, const std::string &label) {
  updateEnvelopeDigest(&candidate);
  crimson::zarr::SubjectMaskSampledContourV1Summary summary;
  std::string error;
  require(!crimson::zarr::ValidateSubjectMaskSampledContourV1Manifest(
              candidate, run, source_manifest, source, &summary, &error),
          label + " was accepted");
  require(!error.empty(), label + " failed without a diagnostic");
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 6) {
    std::cerr << "Usage: " << argv[0]
              << " SOURCE.zarr SOURCE_RUN CACHE.zarr CACHE_RUN "
                 "CACHE_MANIFEST_PAYLOAD_DIGEST\n";
    return 2;
  }
  try {
    const std::filesystem::path source_store = argv[1];
    const std::string source_run = argv[2];
    const std::filesystem::path cache_store = argv[3];
    const std::string cache_run = argv[4];
    const std::string expected_cache_digest = argv[5];
    const json source_manifest =
        readRunManifest(source_store, "refined_subject_masks_runs", source_run);
    const json cache_manifest =
        readRunManifest(cache_store, "subject_mask_cache_runs", cache_run);

    crimson::zarr::SubjectMaskV1ManifestSummary source_summary;
    std::string error;
    require(crimson::zarr::ValidateSubjectMaskV1Manifest(
                source_manifest, source_run, &source_summary, &error),
            "Dense source manifest was rejected: " + error);
    require(cache_manifest.at("payload_digest").get<std::string>() ==
                expected_cache_digest,
            "Sampled-contour manifest digest differs from the handoff");

    crimson::zarr::SubjectMaskSampledContourV1Summary cache_summary;
    require(crimson::zarr::ValidateSubjectMaskSampledContourV1Manifest(
                cache_manifest, cache_run, source_manifest, source_summary,
                &cache_summary, &error),
            "Sampled-contour manifest was rejected: " + error);
    require(cache_summary.arrays.size() == 12 &&
                cache_summary.component_sample_counts ==
                    std::vector<size_t>({128, 64, 64, 32}),
            "Validated sampled-contour inventory differs from v1");

    json candidate = cache_manifest;
    candidate["payload"]["publication"]["stage_selector_eligible"] = true;
    expectRejected(candidate, source_manifest, source_summary, cache_run,
                   "selector-eligible cache");

    candidate = cache_manifest;
    candidate["payload"]["source_refined_subject_mask_snapshot"]["authority"] =
        "sampled_contours";
    expectRejected(candidate, source_manifest, source_summary, cache_run,
                   "wrong source authority");

    candidate = cache_manifest;
    candidate["payload"]["source_refined_subject_mask_snapshot"]["run_name"] =
        "different_source";
    expectRejected(candidate, source_manifest, source_summary, cache_run,
                   "wrong source run");

    candidate = cache_manifest;
    std::swap(candidate["payload"]["components"]["labels"][0],
              candidate["payload"]["components"]["labels"][1]);
    expectRejected(candidate, source_manifest, source_summary, cache_run,
                   "reordered components");

    candidate = cache_manifest;
    candidate["payload"]["contour_profile"]["default_cache"]
             ["component_sample_counts"]["subject_body"] = 64;
    expectRejected(candidate, source_manifest, source_summary, cache_run,
                   "changed sample count");

    candidate = cache_manifest;
    auto &logical = candidate["payload"]["logical_content"];
    logical["document"]["arrays"]
           ["components/subject_body/sampled_contours/source_point_count"]
           ["dtype"] = "int64";
    logical["digest"] =
        crimson::zarr::CanonicalJsonSha256(logical.at("document"));
    expectRejected(candidate, source_manifest, source_summary, cache_run,
                   "changed source-point-count dtype");

    candidate = cache_manifest;
    candidate["payload"]["write_receipt"]["full_dense_equivalence"] = false;
    expectRejected(candidate, source_manifest, source_summary, cache_run,
                   "missing full dense equivalence");

    std::cout << "subject_mask_sampled_contour_v1_contract_gate: PASS "
              << "arrays=" << cache_summary.arrays.size()
              << " frames=" << cache_summary.frame_count
              << " rows=" << cache_summary.row_count << '\n';
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "[SubjectMaskSampledContourV1ContractGate] FAIL "
              << exception.what() << '\n';
    return 1;
  }
}
